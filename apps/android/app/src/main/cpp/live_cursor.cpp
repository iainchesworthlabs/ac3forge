// The encode loop: this app's first real "live cursor" - the first place in
// the whole ac3forge project that drives AtmosEncoder::encode_frame() from a
// live, externally-set position every frame rather than from an authored
// KeyframePath/OrbitPath or a file. Modeled directly on ac3cli's run_live
// (apps/cli/main.cpp) - same per-frame shape (build placement, encode_frame,
// IEC61937-wrap, PassthroughSink::submit with retry+sleep) - but self-paced
// by wall clock instead of run_live's "block on the capture ring buffer"
// mechanism, because this app synthesizes its own object audio; there is no
// upstream producer to drain.
//
// kObjects objects: kInteractiveObjects (currently 1) that a pre-planned
// trajectory carries around the room on its own, with input from
// InputController.kt biasing them off that course - held input pushes,
// releasing it lets the object spring back onto the trajectory (see
// LiveCursorState::deflect_selected/advance below) - plus kAmbientObjects
// that follow their own trajectory untouched by input at all, for the sound
// mixing/interaction a single moving voice can't demonstrate on its own. The
// encode loop calls LiveCursorState::advance() once per frame - this is the
// actual "live cursor" the file is named for.
//
// Audible even unsigned: AtmosEncoder pans every object into the
// transmitted 5.1 bed (see atmos.hpp's header comment) - a legacy/non-JOC
// decode still hears it panned across the fixed channel layout, it just
// cannot reconstruct the object as a separate height-rendered source. The
// object signer (a per-frame pass keyed on the bundled signing.key asset) is
// what closes that gap; see run_loop()'s emit_objects
// (ac3shield::signing_available()) for what an unsigned build does instead.
//
// Real-time viability history: this was briefly a pre-encode-then-loop-a-
// buffer diagnostic, because AtmosEncoder::encode_frame() measured at
// ~266ms/frame on this Shield's SoC - traced (Tracy) to the forward MDCT
// recomputing std::cos() fresh inside an O(N^2) loop instead of using a
// precomputed table the way the inverse transform already did (see
// src/forge/src/core/mdct.cpp's ForwardCosTable). Fixed there, not worked
// around here - this is the straight per-frame loop again.

#include <jni.h>

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ac3/analysis/levels.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/io/object_strip.hpp"
#include "ac3/latency.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "ac3/audio/passthrough.hpp"
#include "shield_signing_hook.hpp"

namespace {

constexpr char kLogTag[] = "ac3forge.shield.live_cursor";
constexpr int kInteractiveObjects = 1;
constexpr int kAmbientObjects = 2;
constexpr int kObjects = kInteractiveObjects + kAmbientObjects;
constexpr int kSceneCount = 5;
constexpr double kSampleRate = 48000.0;
// The interactive lead at A4, the two ambient objects a major third and a
// perfect fifth above it (C#5, E5) - an A major triad rather than an
// arbitrary/dissonant set of tones, so the "sound interaction/mixing" the
// ambient objects exist for is pleasant to actually listen to as they and
// the lead move past each other. Ambient objects sit quieter than the lead
// so it stays the clear focus of the demo, but loud enough to still be
// heard clearly - two rounds of real-device testing on a Shield + AVR each
// found the previous level still needed the receiver driven meaningfully
// hotter than normal listening level. These gains are deliberately chosen
// alongside soft_limit() below rather than kept safely under a linear 1.0
// ceiling on their own - see that function's own comment for why a limiter
// was the only way to get materially louder without just clipping.
constexpr std::array<double, kObjects> kToneHz{440.0, 554.365, 659.255};
constexpr std::array<double, kObjects> kToneGain{0.95, 0.30, 0.30};

// A soft-knee limiter, applied to every object's final sample below - pure
// linear gain increases run straight into a hard ceiling (any sample over
// +-1.0 hard-clips, an audible crackle, since this encoder does no internal
// limiting of its own), so getting meaningfully louder without distortion
// needs the peaks themselves shaped, not just a bigger multiplier. Fully
// transparent (identity) below kLimiterThreshold; only the excess above it
// is compressed, smoothly approaching but never reaching +-1.0 - ordinary
// low-level content is completely unaffected, only genuine peaks are
// tamed. tanh() only runs for samples that actually exceed the threshold,
// not on every sample.
constexpr float kLimiterThreshold = 0.72f;
float soft_limit(float x) {
    const float sign = x < 0.0f ? -1.0f : 1.0f;
    const float ax = std::abs(x);
    if (ax <= kLimiterThreshold) {
        return x;
    }
    const float excess = (ax - kLimiterThreshold) / (1.0f - kLimiterThreshold);
    const float compressed = kLimiterThreshold + (1.0f - kLimiterThreshold) * std::tanh(excess);
    return sign * std::min(compressed, 0.999f);
}

// One object's pre-planned path: a circular orbit centred on the room's
// exact middle - oamd.hpp's (0.5, 0.5, 0) - which is also where the JOC/VBAP
// render implicitly assumes the listener sits, so every object's lap carries
// it both in front of AND behind that point rather than staying confined to
// the front half of the room. Height bobs independently and more slowly, so
// the path is a gentle tilted ellipse in 3-space rather than a flat circle.
// Distinct rate/phase/radius per object keeps the three visually and
// audibly distinguishable rather than moving in lockstep.
// What an object's path through the room actually looks like. The demo used
// to have exactly one answer to this - a circle with a slow height bob - so
// it had exactly one thing to show, forever. These are the shapes a scene can
// ask for; the parameters below mean slightly different things per shape,
// which is noted on each.
enum class Shape : std::uint8_t {
    // Circle in x/y about the room centre, with an independent height bob.
    kOrbit,
    // Front-to-back and back again, rising to a peak as it passes over the
    // listening position: the "did you hear it go over you" pass. radius is
    // the lateral wander, height_amp the apex height.
    kFlyover,
    // Fixed above the listener, pure vertical sweep. Isolates the height axis
    // completely - the one shape where nothing else is moving to distract.
    kElevator,
    // Circle like kOrbit, but pinned near the ceiling: height_amp is the base
    // height rather than a bob amplitude.
    kOverhead,
    // Hard front/back alternation with no height at all, for A/B-ing speaker
    // placement rather than for listening to.
    kPingPong,
};

struct TrajectoryParams {
    Shape shape = Shape::kOrbit;
    double rate_hz;         // path repetitions per second
    double phase_rad;
    double radius;          // xy radius about the room centre, room units
    double height_amp;      // z amplitude or base height, room units ([-1,1])
    double height_rate_hz;  // z bob revolutions per second (kOrbit/kElevator)
    // Per-scene voice level, multiplied onto kToneGain. 0 silences the object
    // without stopping it moving - which is how a scene "has fewer objects"
    // without the encoder being rebuilt for a different object count.
    double gain_scale = 1.0;
};

struct Scene {
    std::string_view name;
    // One line of "what to listen for", shown with the name when the scene
    // changes. The demo has always been able to show where a sound is; it has
    // never told anyone what it wanted them to notice.
    std::string_view hint;
    std::array<TrajectoryParams, kObjects> objects;
};

// height_amp is close to the full [-1,1] range for the lead (0.85, not the
// original 0.5) so its bob genuinely swings up near the ceiling and down
// near the floor rather than only ever reaching halfway - real-device
// testing found the original range read as barely-there movement rather
// than a source passing overhead. Lap rate is also faster (12s, not 20s) so
// the motion is perceptible within a short listening window rather than
// requiring a patient, full-lap wait to notice.
// The object COUNT is fixed at kObjects for every scene, deliberately:
// AtmosEncoder takes its object count at construction, so varying it would
// mean rebuilding the encoder mid-stream - per-object QMF filterbank
// allocation on a thread holding a 32ms deadline. A scene that wants fewer
// voices silences them with gain_scale instead and leaves them moving.
constexpr std::array<Scene, kSceneCount> kScenes{{
    // 0 - what the demo has always done. Still the best one to hand someone a
    // controller on, so it stays first.
    {"Orbit",
     "The lead laps the room - push it off course and let go",
     {{
         {Shape::kOrbit, 1.0 / 12.0, 0.0, 0.45, 0.85, 1.0 / 25.0, 1.0},
         {Shape::kOrbit, 1.0 / 33.0, 2.0 * std::numbers::pi / 3.0, 0.28, 0.45, 1.0 / 53.0, 1.0},
         {Shape::kOrbit, 1.0 / 27.0, 4.0 * std::numbers::pi / 3.0, 0.28, -0.45, 1.0 / 47.0, 1.0},
     }}},
    // 1 - one voice, passing over the listening position. The ambients drop
    // right down rather than out: something still has to hold the room while
    // the lead is away at the far wall.
    {"Flyover",
     "Front to back, straight over your head - listen for it passing",
     {{
         {Shape::kFlyover, 1.0 / 9.0, 0.0, 0.10, 0.90, 0.0, 1.0},
         {Shape::kOrbit, 1.0 / 41.0, 2.0 * std::numbers::pi / 3.0, 0.34, -0.20, 1.0 / 61.0, 0.35},
         {Shape::kOrbit, 1.0 / 37.0, 4.0 * std::numbers::pi / 3.0, 0.34, -0.25, 1.0 / 59.0, 0.35},
     }}},
    // 2 - everything in the ceiling plane at once. The scene that sells height
    // channels, because nothing is down at ear level to compare against.
    {"Overhead",
     "Everything is above you - the height channels are doing all of this",
     {{
         {Shape::kOverhead, 1.0 / 19.0, 0.0, 0.38, 0.80, 1.0 / 23.0, 1.0},
         {Shape::kOverhead, 1.0 / 29.0, 2.0 * std::numbers::pi / 3.0, 0.30, 0.72, 1.0 / 31.0, 0.7},
         {Shape::kOverhead, 1.0 / 23.0, 4.0 * std::numbers::pi / 3.0, 0.24, 0.88, 1.0 / 37.0, 0.7},
     }}},
    // 3 - the height axis on its own, nothing else moving or sounding.
    {"Elevator",
     "One voice, straight up and down over your seat - nothing else moving",
     {{
         {Shape::kElevator, 0.0, 0.0, 0.0, 0.92, 1.0 / 8.0, 1.0},
         {Shape::kOrbit, 1.0 / 43.0, 2.0 * std::numbers::pi / 3.0, 0.28, 0.0, 1.0 / 61.0, 0.0},
         {Shape::kOrbit, 1.0 / 47.0, 4.0 * std::numbers::pi / 3.0, 0.28, 0.0, 1.0 / 59.0, 0.0},
     }}},
    // 4 - deliberately unmusical. This one is for checking a room, not for
    // enjoying: front wall, back wall, nothing in between and no height.
    {"Front / back",
     "Hard front-to-back with no height - for checking your speaker placement",
     {{
         {Shape::kPingPong, 1.0 / 4.0, 0.0, 0.0, 0.0, 0.0, 1.0},
         {Shape::kOrbit, 1.0 / 43.0, 2.0 * std::numbers::pi / 3.0, 0.28, 0.0, 1.0 / 61.0, 0.0},
         {Shape::kOrbit, 1.0 / 47.0, 4.0 * std::numbers::pi / 3.0, 0.28, 0.0, 1.0 / 59.0, 0.0},
     }}},
}};

// Which scene the demo is playing. Written from Kotlin (a keypress, or the
// guided tour's own timer), read by the encode thread.
std::atomic<int> g_scene{0};

// How long a scene change takes to complete. Positions are jumped between
// otherwise, and a 32ms step from one side of the room to the other is an
// abrupt pan rather than a move. Long enough to read as a transition, short
// enough not to feel like waiting.
constexpr double kSceneBlendSeconds = 0.9;

// Path recording: hold the room's own clock and remember where the lead
// object actually went, then fly that path forever. A parametric orbit is
// something the demo does TO you; a path you flew by hand is something you
// did, and watching your own gesture come round again is a different kind of
// convincing.
//
// One entry per encode frame (32ms), capped at two minutes - well past any
// gesture anyone performs live, and 3750 Positions is ~90KB, which is nothing
// next to the frame buffers this loop already holds.
constexpr int kMaxRecordFrames = 3750;
constexpr double kFrameSeconds = static_cast<double>(ac3::kSamplesPerFrame) / kSampleRate;

enum class RecordState : std::int32_t { kIdle = 0, kRecording = 1, kPlaying = 2 };

// 0 at the start of a blend, 1 at the end, with the ends eased so the
// transition neither starts nor stops abruptly.
double smooth_step(double x) {
    const double c = std::clamp(x, 0.0, 1.0);
    return c * c * (3.0 - 2.0 * c);
}

// 0 -> 1 -> 0 over one period, for the shapes that travel out and back rather
// than around. A sawtooth would be simpler but wraps, and a wrap is an object
// teleporting from the back wall to the front.
double triangle01(double phase) {
    const double frac = phase - std::floor(phase);
    return frac < 0.5 ? frac * 2.0 : (1.0 - frac) * 2.0;
}

ac3::oba::Position trajectory_position(int scene, int obj, double time_s) {
    const auto& p = kScenes[static_cast<std::size_t>(scene)]
                        .objects[static_cast<std::size_t>(obj)];
    const double angle = 2.0 * std::numbers::pi * p.rate_hz * time_s + p.phase_rad;
    const double height_angle =
        2.0 * std::numbers::pi * p.height_rate_hz * time_s + p.phase_rad;
    switch (p.shape) {
        case Shape::kFlyover: {
            // y runs front (0) to back (1) and back again; height peaks as it
            // crosses the listening position, so it arrives low, passes over,
            // and leaves low.
            const double travel = triangle01(p.rate_hz * time_s + p.phase_rad / (2.0 * std::numbers::pi));
            const double y = 0.03 + 0.94 * travel;
            return {.x = 0.5 + p.radius * std::sin(angle * 0.5),
                    .y = y,
                    .z = p.height_amp * std::sin(std::numbers::pi * travel)};
        }
        case Shape::kElevator:
            return {.x = 0.5, .y = 0.5, .z = p.height_amp * std::sin(height_angle)};
        case Shape::kOverhead:
            // height_amp is a base height here, with a small bob around it -
            // the point of the scene is that nothing drops back to ear level.
            return {.x = 0.5 + p.radius * std::sin(angle),
                    .y = 0.5 - p.radius * std::cos(angle),
                    .z = std::clamp(p.height_amp + 0.12 * std::sin(height_angle), -1.0, 1.0)};
        case Shape::kPingPong: {
            const double travel = triangle01(p.rate_hz * time_s + p.phase_rad / (2.0 * std::numbers::pi));
            return {.x = 0.5, .y = 0.05 + 0.90 * travel, .z = 0.0};
        }
        case Shape::kOrbit:
        default:
            return {.x = 0.5 + p.radius * std::sin(angle),
                    .y = 0.5 - p.radius * std::cos(angle),
                    .z = p.height_amp * std::sin(height_angle)};
    }
}

// The position an object is at, accounting for a scene change still in
// progress. `from` is the scene being left; once the blend is complete the
// caller stops passing one.
ac3::oba::Position blended_position(int scene, int from, double blend, int obj, double time_s) {
    const auto to_pos = trajectory_position(scene, obj, time_s);
    if (blend >= 1.0 || scene == from) {
        return to_pos;
    }
    const auto from_pos = trajectory_position(from, obj, time_s);
    const double w = smooth_step(blend);
    return {.x = from_pos.x + (to_pos.x - from_pos.x) * w,
            .y = from_pos.y + (to_pos.y - from_pos.y) * w,
            .z = from_pos.z + (to_pos.z - from_pos.z) * w};
}

// Distance-based loudness: without this, an object sounded exactly as loud
// swinging past the listener at the room centre as it did out at the far
// edge of its orbit - correct panning direction, but no actual sense of
// "coming toward me" versus "far away". Inverse-square-ish, clamped with a
// floor so the far end of an orbit (~1 room-unit out, given the widened
// trajectory radii/heights above) is quieter but never silent - going
// fully silent would fight the "isolate and track it" point of the
// pause/mute feature rather than complement it.
constexpr double kDistanceFalloffK = 1.0;
constexpr double kDistanceAttenFloor = 0.4;
double distance_attenuation(const ac3::oba::Position& pos) {
    const double dx = pos.x - 0.5;
    const double dy = pos.y - 0.5;
    const double dz = pos.z;  // z is already centred on the listener's ear height
    const double dist_sq = dx * dx + dy * dy + dz * dz;
    const double atten = 1.0 / (1.0 + kDistanceFalloffK * dist_sq);
    return std::max(atten, kDistanceAttenFloor);
}

// The lead object's voice: a plain sine, however correctly panned, is a
// genuinely bad choice for demonstrating precise 3D localization by ear -
// real-device testing confirmed it as "muddy", not a discrete point source.
// Two independent reasons, not one:
//  - A single continuous tone with no onsets gives the ear a weak azimuth
//    (interaural time/level difference) cue - there is no transient to lock
//    onto, only a slowly-panning steady drone.
//  - Elevation localization in human hearing is driven almost entirely by
//    pinna-filtered high-frequency/broadband spectral cues - a 440Hz sine
//    has essentially no energy up there at all, so "hear it go overhead"
//    cannot work no matter how correct the encoder's height panning is.
// A rhythmic, sharpened amplitude envelope (kLeadRotorHz, a rotor-blade-like
// "thump" rather than a smooth tremolo) mixed with broadband noise addresses
// both: real onset transients for azimuth, and real high-frequency content
// for elevation. Ambient objects deliberately stay plain sines - a
// contrasting, non-percussive texture that's easy to tune out, so the lead
// reads as the one distinct, locatable voice by timbre alone, not just by
// having asked the listener to trust the panning.
constexpr double kLeadRotorHz = 5.0;
constexpr double kLeadRotorSharpness = 3.0;  // higher = shorter, more percussive pulses
constexpr float kLeadToneMix = 0.7f;
constexpr float kLeadNoiseMix = 0.3f;

// Cheap, deterministic PRNG (xorshift32) for the lead's noise texture - a
// fixed seed rather than time-seeded, since the exact sequence never
// matters, only that it sounds like noise; avoids <random>'s per-sample
// engine overhead in what is otherwise a tight, allocation-free hot loop.
std::uint32_t g_noise_state = 0x9e3779b9u;
float next_noise_sample() {
    g_noise_state ^= g_noise_state << 13;
    g_noise_state ^= g_noise_state >> 17;
    g_noise_state ^= g_noise_state << 5;
    return static_cast<float>(static_cast<std::int32_t>(g_noise_state)) * (1.0f / 2147483648.0f);
}

// The lead object's bundled voice: a seamlessly-looping, offline-rendered
// rotorcraft sample (rotor thump + tail rotor + engine drone + blade-slap
// noise + rumble - see tools/gen_lead_voice.py-style generation, richer than
// this tight real-time loop could afford per-sample on this SoC) rather than
// the live tone+noise synthesis above. Set once from Kotlin
// (MainActivity.onCreate, before nativeStartLiveCursor) via
// nativeSetAssetManager - a plain AAssetManager*, not wrapped in any
// lifetime-tracking type, since the Java-side AssetManager (and therefore
// this pointer) outlives the whole process once set. A raw pointer, not a
// GlobalRef-held jobject: AAssetManager_fromJava's returned native handle
// stays valid independent of any JNI local/global ref bookkeeping.
std::atomic<AAssetManager*> g_asset_manager{nullptr};
constexpr char kLeadVoiceAsset[] = "lead_voice_48k_mono_s16le.raw";

// Loads kLeadVoiceAsset fully into memory as [-1,1] float samples. Returns an
// empty vector (never throws/aborts) if no AAssetManager has been set yet, if
// the asset is missing from this particular build/packaging, or if the read
// comes back short - run_loop() below treats an empty result as "fall back
// to the live rotor+noise synthesis", not as a fatal error, since a missing
// bundled asset is a real, recoverable packaging-boundary condition, not
// something internal code can just assume never happens.
std::vector<float> load_lead_voice_sample() {
    AAssetManager* mgr = g_asset_manager.load(std::memory_order_relaxed);
    if (mgr == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "no AAssetManager set - lead object will use live-synthesized voice");
        return {};
    }
    AAsset* asset = AAssetManager_open(mgr, kLeadVoiceAsset, AASSET_MODE_BUFFER);
    if (asset == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "asset '%s' not found - lead object will use live-synthesized voice",
                            kLeadVoiceAsset);
        return {};
    }
    const off_t length = AAsset_getLength(asset);
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(length) / sizeof(std::int16_t));
    const int read_bytes = AAsset_read(asset, pcm.data(), static_cast<std::size_t>(length));
    AAsset_close(asset);
    if (read_bytes != length || pcm.empty()) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "asset '%s' read incomplete (%d/%lld bytes) - lead object will use "
                            "live-synthesized voice",
                            kLeadVoiceAsset, read_bytes, static_cast<long long>(length));
        return {};
    }
    std::vector<float> samples(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        samples[i] = static_cast<float>(pcm[i]) * (1.0f / 32768.0f);
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "loaded lead voice asset: %zu samples (%.2fs)",
                        samples.size(), static_cast<double>(samples.size()) / kSampleRate);
    return samples;
}

std::thread g_worker;
std::atomic_bool g_stop_requested{false};
std::atomic_bool g_running{false};
// run_loop()'s own start_time, published so nativeGetFutureLeadTrajectory()
// (a JNI call from the UI thread, not the encode-loop thread) can compute
// "now" in the exact same time base trajectory_position() uses without
// touching anything mutex-protected - trajectory_position() is a pure
// function of time, so this is the only state that call actually needs.
std::atomic<std::int64_t> g_start_time_ns{0};

// Live stream stats + the ambient-mute control, both read/written from
// different threads (RoomView.kt's UI-thread poll, the remote's play/pause
// keys, the encode-loop thread that updates the stats every frame) - plain
// atomics, not LiveCursorState's mutex, since every field here is
// independent and there is nothing that needs to stay consistent ACROSS
// fields the way a whole ObjectPlacement does.
struct StreamStats {
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> bursts_submitted{0};
    std::atomic<std::uint64_t> bursts_rendered{0};
    std::atomic<std::uint64_t> underruns{0};
    std::atomic_bool signed_stream{false};
    // Set by the remote's pause/play keys (InputController.kt) - muting the
    // two ambient objects' audio (not their position/trajectory, which keeps
    // advancing in the background) so a listener can isolate the lead
    // object's sound and hear its own movement without the ambient wash on
    // top of it. "Restart everything that was paused" on play is therefore
    // just un-muting: the ambient objects never stopped moving, only sounding.
    std::atomic_bool ambient_muted{false};
    // How long the last encode_frame() call actually took - for the on-
    // screen "encode headroom" readout (real-time viability was a genuine,
    // previously-hit problem on this SoC - see this file's own header - so
    // showing the live number rather than just asserting it stays fast is
    // worth doing).
    std::atomic<float> encode_ms{0.0f};
    // AC-3 coded order (L, C, R, Ls, Rs, LFE) - see AtmosEncoder::bed()'s own
    // comment. RMS level per channel of the REAL, actually-encoded 5.1 bed,
    // not an approximation from the room-position math - see run_loop()'s
    // own comment on where this is computed. For the speaker-activity meter.
    std::array<std::atomic<float>, 6> channel_levels{};
    // The encoder's own end-to-end latency for this configuration, in ms:
    // object_latency_ms is what a decoder reconstructing the objects hears,
    // bed_latency_ms what a legacy 5.1 decoder hears. Constant for a run
    // (they depend on the config, not the content) but not compile-time
    // constant here, since joc_domain decides whether the object path pays a
    // second transform - see AtmosEncoder::latency(). Published so the
    // dashboard can state the figure instead of the demo quietly implying
    // the dot and the sound are simultaneous.
    std::atomic<float> object_latency_ms{0.0F};
    std::atomic<float> bed_latency_ms{0.0F};

    // Where the bed's energy actually sits on the loudspeaker ring - Gerzon's
    // energy vector over the REAL encoded bed, not the room-position maths.
    // That distinction is the whole point: the room panels plot where the
    // demo asked the object to be, and this shows where a 5.1 decoder's own
    // speakers will actually put it. Azimuth is degrees counterclockwise from
    // front; magnitude runs 1 (all energy at one speaker) to 0 (no direction).
    std::atomic<float> energy_azimuth_deg{0.0F};
    std::atomic<float> energy_magnitude{0.0F};

    // BS.1770 integrated loudness of the bed, and the dialnorm it implies.
    // Measured and DISPLAYED only - see the loudness block in run_loop() for
    // why this stream is deliberately not patched with it.
    std::atomic<float> integrated_lkfs{0.0F};
    std::atomic_bool loudness_valid{false};
    std::atomic<int> implied_dialnorm{0};

    // OBJECTS OFF: strips the object layer out of every access unit before it
    // is wrapped, live. See the strip block in run_loop().
    std::atomic_bool objects_off{false};
    std::atomic<std::uint32_t> stripped_bytes_per_frame{0};

    // Zeroes everything a *previous* run left behind. Without this, a stopped
    // loop froze its last values on screen rather than going dark: the meters
    // held whatever the final frame happened to be, the frame/burst counters
    // kept counting from the old total when the loop restarted, and
    // `signed_stream` still claimed "Atmos (signed)" for a stream that was no
    // longer being produced at all. A stopped demo should look stopped.
    void reset() {
        frames.store(0, std::memory_order_relaxed);
        bursts_submitted.store(0, std::memory_order_relaxed);
        bursts_rendered.store(0, std::memory_order_relaxed);
        underruns.store(0, std::memory_order_relaxed);
        signed_stream.store(false, std::memory_order_relaxed);
        encode_ms.store(0.0F, std::memory_order_relaxed);
        object_latency_ms.store(0.0F, std::memory_order_relaxed);
        bed_latency_ms.store(0.0F, std::memory_order_relaxed);
        energy_azimuth_deg.store(0.0F, std::memory_order_relaxed);
        energy_magnitude.store(0.0F, std::memory_order_relaxed);
        integrated_lkfs.store(0.0F, std::memory_order_relaxed);
        loudness_valid.store(false, std::memory_order_relaxed);
        implied_dialnorm.store(0, std::memory_order_relaxed);
        stripped_bytes_per_frame.store(0, std::memory_order_relaxed);
        // objects_off is deliberately NOT reset, for the same reason
        // ambient_muted is not: it is a control the presenter set, not a
        // measurement of this run.
        for (auto& level : channel_levels) {
            level.store(0.0F, std::memory_order_relaxed);
        }
        // ambient_muted is deliberately NOT reset: it is a user preference
        // set from the remote, not a measurement of this run.
    }
};

StreamStats& stream_stats() {
    static StreamStats s;
    return s;
}

// Input-driven bias for one interactive object: how far its actual position
// currently sits from where trajectory_position() says it "should" be. Kept
// separate from the trajectory itself (rather than, say, directly nudging an
// absolute position) precisely so it can decay independently every frame -
// see advance() below - which is what makes "release the stick and it drifts
// back onto its course" work without InputController.kt ever having to tell
// native input has stopped.
struct Deflection {
    double x = 0.0, y = 0.0, z = 0.0;
};

// How far held input can push an object off its trajectory before the clamp
// in deflect_selected() stops it - the "bounding box" the deflection is
// limited by. xy is tighter than z: the xy trajectory radius is already up
// to 0.45 room-units (kTrajectory[0]), so 0.45 more still comfortably clears
// the walls once combined and clamped again in advance() below (real-device
// testing found the original, tighter 0.35 made a deliberate push feel
// underwhelming); z has even more headroom since the trajectory's own
// height bob, even widened above, only reaches +-0.85.
constexpr double kMaxDeflectXy = 0.45;
constexpr double kMaxDeflectZ = 0.75;
// Per-encode-frame multiplicative decay applied to a deflection whether or
// not fresh input arrived this frame - the actual "spring-back". Frames are
// kSamplesPerFrame/48000 = 32ms apart; this value is exp(-frame_s / tau) for
// a tau of 1.5s, so a released deflection falls to ~1/e of its size in 1.5s
// and is effectively gone (~5%) after about 4.5s - unhurried enough to watch
// happen, quick enough not to feel unresponsive.
constexpr double kDeflectionDecayPerFrame = 0.9789;

// The live cursor itself: kObjects positions (kInteractiveObjects driven by
// a trajectory plus a decaying input deflection, the rest purely by their
// own trajectory) and which interactive object input currently targets.
// Written by the JNI functions at the bottom of this file (called from
// Kotlin's input-handling thread, roughly once per animation frame - see
// InputController.kt); advance() is called once per encode frame by
// run_loop() below, on the encode thread. A plain mutex, not
// atomics-per-field: this is nowhere near a contended hot path (one
// advance() plus at most a few deflect_selected() calls per ~16-32ms), and a
// mutex keeps a whole Deflection/ObjectPlacement's fields consistent with
// each other, which per-field atomics would not.
class LiveCursorState {
public:
    // Advances every object to `time_s`: the trajectory alone for ambient
    // objects, trajectory-plus-decaying-deflection for interactive ones.
    // Called once per encode frame - this is the only place deflection_
    // decays, so the spring-back happens on its own every frame regardless
    // of whether any input arrived.
    std::array<ac3::oba::ObjectPlacement, kObjects> advance(double time_s, int scene, int from,
                                                            double blend) {
        std::lock_guard lock(mutex_);
        for (int i = 0; i < kInteractiveObjects; ++i) {
            // A played-back recording REPLACES the scene's trajectory for the
            // lead, but deflection still applies on top of it - the recorded
            // path behaves exactly like any other course, including being
            // pushable and springing back to itself.
            const auto base =
                (i == 0 && record_state_ == RecordState::kPlaying && !recorded_.empty())
                    ? recorded_[static_cast<std::size_t>(
                          static_cast<std::int64_t>((time_s - playback_start_s_) / kFrameSeconds) %
                          static_cast<std::int64_t>(recorded_.size()))]
                    : blended_position(scene, from, blend, i, time_s);
            auto& defl = deflection_[static_cast<std::size_t>(i)];
            // Clamp to oamd.hpp's Position contract on top of the
            // deflection's own bounding-box clamp in deflect_selected():
            // that one keeps the BIAS itself bounded, this one keeps the
            // final trajectory+bias position inside the room even right at
            // the trajectory's own extremes (x,y in [0,1], z in [-1,1] - see
            // src/forge/include/ac3/oba/oamd.hpp).
            placements_[static_cast<std::size_t>(i)] = {
                .position = {.x = std::clamp(base.x + defl.x, 0.0, 1.0),
                            .y = std::clamp(base.y + defl.y, 0.0, 1.0),
                            .z = std::clamp(base.z + defl.z, -1.0, 1.0)},
                .gain = 1.0,
            };
            defl.x *= kDeflectionDecayPerFrame;
            defl.y *= kDeflectionDecayPerFrame;
            defl.z *= kDeflectionDecayPerFrame;

            // Record the FINAL placed position, deflection and clamps
            // included - what gets replayed is where the object actually
            // went, not where the trajectory alone would have put it.
            if (i == 0 && record_state_ == RecordState::kRecording) {
                if (recorded_.size() < static_cast<std::size_t>(kMaxRecordFrames)) {
                    recorded_.push_back(placements_[0].position);
                } else {
                    // Out of room: keep what was captured and start playing it
                    // rather than silently recording nothing further.
                    playback_start_s_ = time_s;
                    record_state_ = RecordState::kPlaying;
                }
            }
        }
        for (int i = kInteractiveObjects; i < kObjects; ++i) {
            placements_[static_cast<std::size_t>(i)] = {
                .position = blended_position(scene, from, blend, i, time_s), .gain = 1.0};
        }
        return placements_;
    }

    // Instantly zeroes the selected object's deflection, rather than waiting
    // out kDeflectionDecayPerFrame's own ~1.5s time constant - called from a
    // long-press of A/center (InputController.kt), replacing what used to be
    // "cycle selected object" there (a no-op with a single interactive
    // object anyway) with something a presenter can actually use: a crisp,
    // on-demand "and... reset" moment instead of watching it drift back.
    void snap_selected() {
        std::lock_guard lock(mutex_);
        deflection_[static_cast<std::size_t>(selected_)] = Deflection{};
    }

    // Adds (dx, dy, dz) to the selected interactive object's deflection,
    // clamped to the bounding box around its trajectory - see
    // InputController.kt for how dx/dy/dz are derived from the stick/D-pad
    // each animation frame.
    void deflect_selected(double dx, double dy, double dz) {
        std::lock_guard lock(mutex_);
        auto& defl = deflection_[static_cast<std::size_t>(selected_)];
        defl.x = std::clamp(defl.x + dx, -kMaxDeflectXy, kMaxDeflectXy);
        defl.y = std::clamp(defl.y + dy, -kMaxDeflectXy, kMaxDeflectXy);
        defl.z = std::clamp(defl.z + dz, -kMaxDeflectZ, kMaxDeflectZ);
    }

    int cycle_selected() {
        std::lock_guard lock(mutex_);
        selected_ = (selected_ + 1) % kInteractiveObjects;
        return selected_;
    }

    int selected() const {
        std::lock_guard lock(mutex_);
        return selected_;
    }

    // For the room visualization: the placements advance() last computed,
    // without advancing anything - RoomView polls this far more often
    // (every UI vsync) than the encode loop actually produces new frames.
    std::array<ac3::oba::ObjectPlacement, kObjects> snapshot() const {
        std::lock_guard lock(mutex_);
        return placements_;
    }

    /**
     * Cycles idle -> recording -> playing -> idle, returning the new state.
     *
     * Stopping a recording goes straight to playing rather than back to idle:
     * the gesture was performed to be watched, and making the user press
     * again to see it is a beat of dead air in a live demo.
     */
    RecordState toggle_record(double time_s) {
        std::lock_guard lock(mutex_);
        switch (record_state_) {
            case RecordState::kIdle:
                recorded_.clear();
                recorded_.reserve(kMaxRecordFrames);
                record_state_ = RecordState::kRecording;
                break;
            case RecordState::kRecording:
                // A recording too short to be a path at all is discarded
                // rather than looped as a twitch.
                if (recorded_.size() < 8) {
                    recorded_.clear();
                    record_state_ = RecordState::kIdle;
                } else {
                    playback_start_s_ = time_s;
                    record_state_ = RecordState::kPlaying;
                }
                break;
            case RecordState::kPlaying:
                recorded_.clear();
                record_state_ = RecordState::kIdle;
                break;
        }
        return record_state_;
    }

    RecordState record_state() const {
        std::lock_guard lock(mutex_);
        return record_state_;
    }

    std::size_t recorded_frames() const {
        std::lock_guard lock(mutex_);
        return recorded_.size();
    }

private:
    mutable std::mutex mutex_;
    std::array<Deflection, kInteractiveObjects> deflection_{};
    std::array<ac3::oba::ObjectPlacement, kObjects> placements_{};
    int selected_ = 0;

    RecordState record_state_ = RecordState::kIdle;
    std::vector<ac3::oba::Position> recorded_;
    double playback_start_s_ = 0.0;
};

LiveCursorState& live_cursor_state() {
    static LiveCursorState state;
    return state;
}

void run_loop() {
    // Every counter and level below belongs to THIS run - see
    // StreamStats::reset(). Must happen before signed_stream is set a few
    // lines down.
    stream_stats().reset();
    // Load the bundled signing key (if this build carries one) before deciding
    // whether to emit the object container: the same AAssetManager the lead
    // voice uses is already set by now (MainActivity.onCreate registers it
    // before nativeStartLiveCursor). A build without the key asset leaves
    // signing unavailable - see shield_signing_hook.hpp.
    ac3shield::init_signing(g_asset_manager.load(std::memory_order_relaxed));
    // Without a key, an emitted-but-unsigned container would be the hard-refusal
    // case AtmosConfig::emit_object_metadata's own comment warns about, not a
    // graceful 5.1 fallback - omit it entirely instead. Only a build carrying
    // the signing key asset ever sets this true.
    const bool emit_objects = ac3shield::signing_available();
    stream_stats().signed_stream.store(emit_objects, std::memory_order_relaxed);
    ac3::oba::AtmosEncoder encoder({.bitrate_kbps = 448, .emit_object_metadata = emit_objects},
                                   kObjects);
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "object container: %s", emit_objects ? "objects (signed)" : "bed51 (omitted, unsigned build)");
    {
        // Asked once, here: both figures are a property of the configuration
        // this encoder was just built with, not of any frame.
        const auto object_ms = ac3::latency_ms(encoder.latency(), ac3::SampleRate::k48000);
        const auto bed_ms = ac3::latency_ms(encoder.bed_latency(), ac3::SampleRate::k48000);
        stream_stats().object_latency_ms.store(static_cast<float>(object_ms),
                                               std::memory_order_relaxed);
        stream_stats().bed_latency_ms.store(static_cast<float>(bed_ms), std::memory_order_relaxed);
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "encoder latency: objects %.1fms (%d samples), bed %.1fms (%d samples)",
                            object_ms, encoder.latency().total_samples(), bed_ms,
                            encoder.bed_latency().total_samples());
    }
    ac3::audio::PassthroughSink sink;
    auto started = sink.start("", 48000, ac3::audio::BitstreamFormat::kEac3);
    if (!started) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "PassthroughSink::start failed: %s",
                            std::string(ac3::audio::describe(started.error())).c_str());
        return;
    }

    // A real programme meter over the encoded bed, replacing an `rms * 4.0`
    // clamp whose only justification was "without some boost the meter would
    // barely move". This is dB-scaled with PPM ballistics and a peak hold, so
    // the bars mean the same thing the desktop tools' bars mean.
    //
    // rms_integration_ms is shortened well below the 300ms default: 300ms is
    // roughly ten frames at 32ms, and the soundfield arrow computed from these
    // levels would visibly trail a fast pan - the exact opposite of the cue it
    // exists to give. 80ms still reads as a level rather than a twitch.
    ac3::analysis::LevelMeter level_meter(ac3::Acmod::k3_2, /*lfe=*/true,
                                          static_cast<std::uint32_t>(kSampleRate),
                                          ac3::analysis::MeterBallistics{
                                              .rms_integration_ms = 80.0,
                                          });
    // BS.1770 over the same bed. Measured for display only - see where it is
    // published below.
    ac3::meta::LoudnessMeter loudness_meter(ac3::SampleRate::k48000, ac3::Acmod::k3_2,
                                            /*lfe=*/true);
    // Reused every frame for both meters rather than rebuilt: bed() hands back
    // a span of vectors, and both meters want a span of spans.
    std::vector<std::span<const float>> bed_views;
    bed_views.reserve(6);

    ac3::iec61937::Eac3BurstPacker packer;
    std::array<std::vector<float>, kObjects> tones;
    for (auto& tone : tones) {
        tone.resize(ac3::kSamplesPerFrame);
    }
    std::array<double, kObjects> phase{};
    std::array<double, kObjects> phase_step{};
    for (int obj = 0; obj < kObjects; ++obj) {
        phase_step[static_cast<std::size_t>(obj)] =
            2.0 * std::numbers::pi * kToneHz[static_cast<std::size_t>(obj)] / kSampleRate;
    }
    // Only the lead (kInteractiveObjects == 1) has a rotor envelope; see
    // kLeadRotorHz's own comment.
    double rotor_phase = 0.0;
    const double rotor_step = 2.0 * std::numbers::pi * kLeadRotorHz / kSampleRate;

    // The lead's bundled voice, if this build has one packaged and an
    // AAssetManager was registered before this thread started (see
    // load_lead_voice_sample()'s own comment). Empty means "use the live
    // rotor+tone+noise synthesis below instead" - never a fatal condition.
    const std::vector<float> lead_sample = load_lead_voice_sample();
    std::size_t lead_sample_pos = 0;

    // Wall-clock frame pacing, not a producer to drain (see header comment):
    // one AC-3 frame is exactly kSamplesPerFrame/48000 seconds.
    const auto frame_period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(ac3::kSamplesPerFrame / kSampleRate));
    // Scene state belongs to this thread alone: g_scene is the only thing
    // crossing a thread boundary, and the blend it triggers is derived here
    // rather than shared, so nothing else has to be synchronised.
    int active_scene = std::clamp(g_scene.load(std::memory_order_relaxed), 0, kSceneCount - 1);
    int blend_from_scene = active_scene;
    double blend_start_s = -kSceneBlendSeconds;  // already finished

    auto next_deadline = std::chrono::steady_clock::now();
    // Elapsed wall-clock time since the loop started is the trajectory's own
    // clock (trajectory_position's time_s) - a monotonic clock, not a sample
    // counter, so a stall/resync below (falling behind and skipping ahead)
    // moves the trajectory forward with it rather than the encoded audio and
    // the visible motion drifting apart.
    const auto start_time = std::chrono::steady_clock::now();
    g_start_time_ns.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(start_time.time_since_epoch())
            .count(),
        std::memory_order_relaxed);

    g_running.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "encode loop started (%d interactive + %d ambient objects)",
                        kInteractiveObjects, kAmbientObjects);

    std::uint64_t frames = 0;
    while (!g_stop_requested.load(std::memory_order_acquire)) {
        // Placement is advanced FIRST, before the tone synthesis below reads
        // it for distance_attenuation() - this frame's positions have to be
        // known before this frame's samples can reflect how far each object
        // currently sits from the listener. time_s is computed here too
        // (moved up from after synthesis) for the same reason: the position
        // this loop iteration encodes should be timestamped from the START
        // of the work it does, not after several hundred microseconds of
        // sample generation have already elapsed.
        const double time_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();

        // A scene change starts a blend from wherever the objects currently
        // are. Changing again mid-blend restarts from the scene being left
        // rather than compounding blends - the visible result of a fast
        // double-press is one move, not two overlapping ones.
        const int requested = std::clamp(g_scene.load(std::memory_order_relaxed), 0, kSceneCount - 1);
        if (requested != active_scene) {
            blend_from_scene = active_scene;
            blend_start_s = time_s;
            active_scene = requested;
            __android_log_print(ANDROID_LOG_INFO, kLogTag, "scene -> %d (%.*s)", active_scene,
                                static_cast<int>(kScenes[static_cast<std::size_t>(active_scene)].name.size()),
                                kScenes[static_cast<std::size_t>(active_scene)].name.data());
        }
        const double blend = (time_s - blend_start_s) / kSceneBlendSeconds;

        const auto placement =
            live_cursor_state().advance(time_s, active_scene, blend_from_scene, blend);

        const bool ambient_muted = stream_stats().ambient_muted.load(std::memory_order_relaxed);
        for (int obj = 0; obj < kObjects; ++obj) {
            auto& tone = tones[static_cast<std::size_t>(obj)];
            auto& ph = phase[static_cast<std::size_t>(obj)];
            const auto step = phase_step[static_cast<std::size_t>(obj)];
            const bool is_lead = obj < kInteractiveObjects;
            // Muting only silences the ambient objects' audio, never the
            // lead's - see StreamStats::ambient_muted's own comment.
            // distance_attenuation() reads THIS frame's own placement -
            // real position-based loudness, not merely correct panning
            // direction; see that function's own comment.
            // The scene's own level for this voice, blended in alongside the
            // position so a scene that silences an object fades it rather
            // than cutting it - a hard gain step between frames is a click.
            const double scene_gain = [&] {
                const double to_gain =
                    kScenes[static_cast<std::size_t>(active_scene)]
                        .objects[static_cast<std::size_t>(obj)].gain_scale;
                if (blend >= 1.0 || blend_from_scene == active_scene) {
                    return to_gain;
                }
                const double from_gain =
                    kScenes[static_cast<std::size_t>(blend_from_scene)]
                        .objects[static_cast<std::size_t>(obj)].gain_scale;
                return from_gain + (to_gain - from_gain) * smooth_step(blend);
            }();
            const auto tone_gain =
                kToneGain[static_cast<std::size_t>(obj)] * scene_gain *
                ((!is_lead && ambient_muted) ? 0.0 : 1.0) *
                distance_attenuation(placement[static_cast<std::size_t>(obj)].position);
            if (is_lead && !lead_sample.empty()) {
                // The bundled asset already has its own rotor/tail-rotor/
                // engine/blade-slap structure baked in offline - no live
                // envelope or noise mixing needed here, just loop playback
                // through the same tone_gain/soft_limit chain every voice
                // goes through.
                for (std::size_t n = 0; n < tone.size(); ++n) {
                    tone[n] = soft_limit(static_cast<float>(tone_gain) * lead_sample[lead_sample_pos]);
                    lead_sample_pos = (lead_sample_pos + 1) % lead_sample.size();
                }
            } else if (is_lead) {
                for (std::size_t n = 0; n < tone.size(); ++n) {
                    // See kLeadRotorHz's own comment for why this is a
                    // rhythmic, sharpened envelope over a tone+noise mix
                    // rather than a bare sine. Floor raised to 0.55 (from an
                    // earlier 0.35): real-device testing asked for more
                    // loudness twice over, and a shallower dip between
                    // pulses raises the average level while still leaving a
                    // clearly audible ~5dB swing for the percussive "thump"
                    // itself - most of the extra loudness instead comes from
                    // kToneGain and soft_limit() below.
                    const double pulse = std::max(0.0, std::sin(rotor_phase));
                    const double envelope = 0.55 + 0.45 * std::pow(pulse, kLeadRotorSharpness);
                    const float voice = kLeadToneMix * static_cast<float>(std::sin(ph)) +
                                        kLeadNoiseMix * next_noise_sample();
                    tone[n] = soft_limit(static_cast<float>(tone_gain * envelope) * voice);
                    ph += step;
                    rotor_phase += rotor_step;
                }
                rotor_phase = std::fmod(rotor_phase, 2.0 * std::numbers::pi);
            } else {
                for (std::size_t n = 0; n < tone.size(); ++n) {
                    tone[n] = soft_limit(static_cast<float>(tone_gain * std::sin(ph)));
                    ph += step;
                }
            }
            // Keep the running phase bounded - it only ever feeds sin(), so
            // this cannot audibly discontinue the waveform (sin is 2*pi
            // periodic), it just stops an unbounded double from slowly
            // losing precision over a long-running session.
            ph = std::fmod(ph, 2.0 * std::numbers::pi);
        }
        const std::vector<std::span<const float>> views(tones.begin(), tones.end());

        // Timed for the on-screen "encode headroom" readout - this loop's
        // own real-time viability was a genuine, previously-hit problem on
        // this SoC (see this file's own header comment on the MDCT fix), so
        // showing the live number is worth more here than it would be in
        // most encode loops.
        const auto encode_start = std::chrono::steady_clock::now();
        auto unit = encoder.encode_frame(views, placement);
        const auto encode_elapsed_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                       encode_start)
                .count();
        stream_stats().encode_ms.store(static_cast<float>(encode_elapsed_ms),
                                       std::memory_order_relaxed);
        if (!unit) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "encode_frame failed: %d",
                                static_cast<int>(unit.error()));
            break;
        }

        // For the speaker-activity meter: RMS level of each of the six REAL
        // bed channels this frame actually encoded (AtmosEncoder::bed(), AC-3
        // coded order L/C/R/Ls/Rs/LFE) - not a guess derived from the room
        // position math, the literal audio a legacy decoder hears. x4.0 is a
        // fixed display gain (typical per-channel RMS with three panned
        // objects sits well under 1.0; without some boost the meter would
        // barely move) - visual only, has no effect on the encoded audio.
        {
            const auto bed = encoder.bed();
            bed_views.clear();
            for (std::size_t ch = 0; ch < bed.size() && ch < 6; ++ch) {
                bed_views.emplace_back(bed[ch]);
            }
            level_meter.process(bed_views);
            loudness_meter.push(bed_views);

            // meter_fraction puts a dB level on the same 0..1 scale the bars
            // already draw, and on the same scale the desktop meters use.
            const auto levels = level_meter.levels();
            for (std::size_t ch = 0; ch < levels.size() && ch < 6; ++ch) {
                const auto fraction = ac3::analysis::meter_fraction(levels[ch].rms_db);
                stream_stats().channel_levels[ch].store(static_cast<float>(fraction),
                                                        std::memory_order_relaxed);
            }

            const auto vector = ac3::analysis::energy_vector(levels, ac3::Acmod::k3_2);
            stream_stats().energy_azimuth_deg.store(static_cast<float>(vector.azimuth_deg),
                                                    std::memory_order_relaxed);
            stream_stats().energy_magnitude.store(static_cast<float>(vector.magnitude),
                                                  std::memory_order_relaxed);

            // Integrated loudness is cheap to read (it is accumulated by
            // push() above); loudness_range() is NOT - it allocates and sorts
            // on every call - so it is deliberately not asked for here, on the
            // encode thread, on every frame.
            if (const auto lkfs = loudness_meter.integrated_lkfs()) {
                stream_stats().integrated_lkfs.store(static_cast<float>(*lkfs),
                                                     std::memory_order_relaxed);
                stream_stats().implied_dialnorm.store(ac3::meta::dialnorm_from_lkfs(*lkfs),
                                                      std::memory_order_relaxed);
                stream_stats().loudness_valid.store(true, std::memory_order_relaxed);
            }
        }

        // Right after encode, before IEC61937 wrapping - a no-op (returns
        // false) on any build without the signing key asset; see
        // shield_signing_hook.hpp. This is the ONLY thing standing between
        // "objects panned into the bed, audible but not reconstructable" and
        // "a real Dolby-licensed decoder actually unlocks the objects" - see
        // [[joc-decoder-auth-gate]].
        (void)ac3shield::maybe_sign_atmos_unit(unit->bytes);

        // push() returns expected<optional<vector<byte>>, WrapError>: the
        // outer expected is a hard wrap error (should not happen with our
        // own encoder's output); the inner optional is empty only until
        // blocks_pending_ reaches 6 - this encoder's frames carry all 6
        // blocks each (kSamplesPerFrame/256), so in practice one push() per
        // encode_frame() yields one burst immediately, not an accumulation
        // across several frames.
        // OBJECTS OFF: strip the object layer out of this access unit before
        // it is wrapped, live, while everything else about the stream stays
        // put. What the receiver does with that is the point of the whole
        // demo - a licensed decoder drops from Atmos to plain DD+ on its own
        // front panel and back again, on a toggle, with the per-frame byte
        // cost of the object layer on screen next to it.
        //
        // AFTER signing, not before: the signature covers the container this
        // removes, so stripping first would leave a signature over bytes that
        // are no longer there. Removing the whole container outright is the
        // safe direction - an unsigned-but-present container is the hard
        // refusal case AtmosConfig::emit_object_metadata warns about.
        //
        // A strip failure falls back to the UNSTRIPPED unit rather than
        // `break`ing the loop the way the surrounding error paths do: this is
        // a presentation toggle, and the correct response to "could not strip"
        // is to keep playing the stream we already have.
        std::span<const std::byte> unit_bytes{unit->bytes};
        std::vector<std::byte> stripped_storage;
        if (stream_stats().objects_off.load(std::memory_order_relaxed)) {
            auto stripped = ac3::io::strip_objects(unit->bytes);
            if (stripped) {
                stream_stats().stripped_bytes_per_frame.store(
                    static_cast<std::uint32_t>(stripped->bytes_removed),
                    std::memory_order_relaxed);
                stripped_storage = std::move(stripped->bytes);
                unit_bytes = stripped_storage;
            } else if (frames % 96 == 0) {  // ~3s apart, not once per frame
                __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                    "strip_objects failed (%s) - sending the unstripped unit",
                                    std::string(ac3::io::describe(stripped.error())).c_str());
            }
        } else {
            stream_stats().stripped_bytes_per_frame.store(0, std::memory_order_relaxed);
        }

        const auto push_result = packer.push(unit_bytes);
        if (!push_result) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "iec61937 wrap failed: %d",
                                static_cast<int>(push_result.error()));
            break;
        }
        if (*push_result && !(*push_result)->empty()) {
            if (frames == 0) {
                __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                    "first burst ready: %zu bytes (expect %zu)",
                                    (*push_result)->size(), ac3::iec61937::kEac3BurstBytes);
            }
            int retry_count = 0;
            while (!sink.submit(**push_result)) {
                if (g_stop_requested.load(std::memory_order_acquire)) {
                    break;
                }
                if (++retry_count == 250) {  // ~500ms of retrying one burst
                    __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                        "submit() has retried %d times on frame %llu - "
                                        "AudioTrack is not draining",
                                        retry_count, static_cast<unsigned long long>(frames));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        } else if (frames == 0) {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                "first frame produced no burst yet (accumulating blocks)");
        }

        // Published every frame, not just the 48-frame logcat interval below
        // - RoomView.kt's on-screen stats readout polls this once per vsync
        // and should read live, not update in visible 1.5s jumps.
        {
            const auto stats = sink.stats();
            stream_stats().frames.store(frames, std::memory_order_relaxed);
            stream_stats().bursts_submitted.store(stats.bursts_submitted, std::memory_order_relaxed);
            stream_stats().bursts_rendered.store(stats.bursts_rendered, std::memory_order_relaxed);
            stream_stats().underruns.store(stats.underruns, std::memory_order_relaxed);
            if (frames % 48 == 0) {  // roughly every 1.5s at 32ms/frame
                __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                    "frame %llu: bursts submitted=%llu rendered=%llu underruns=%llu",
                                    static_cast<unsigned long long>(frames),
                                    static_cast<unsigned long long>(stats.bursts_submitted),
                                    static_cast<unsigned long long>(stats.bursts_rendered),
                                    static_cast<unsigned long long>(stats.underruns));
            }
        }
        ++frames;

        const auto now = std::chrono::steady_clock::now();
        if (now > next_deadline + frame_period) {
            // Running behind by more than one whole frame: resync to now
            // rather than let sleep_until race to catch up on a backlog
            // that will just keep growing (and each catch-up frame would
            // still submit as fast as possible, which is fine for
            // PassthroughSink but pointless if the receiver already lost
            // lock from the earlier gap).
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                "frame %llu: encode loop fell behind, resyncing",
                                static_cast<unsigned long long>(frames));
            next_deadline = now;
        }
        next_deadline += frame_period;
        std::this_thread::sleep_until(next_deadline);
    }

    sink.stop();
    // Clearing the trajectory clock is what makes
    // nativeGetFutureLeadTrajectory report "no run in progress" rather than
    // computing a path from a start time that is no longer meaningful - a
    // stopped loop used to leave the waiting screen drawing a phantom orbit
    // phased off whenever the device happened to boot.
    g_start_time_ns.store(0, std::memory_order_relaxed);
    g_running.store(false, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "encode loop stopped");
}

}  // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeStartLiveCursor(JNIEnv* /*env*/, jclass /*clazz*/) {
    if (g_running.load(std::memory_order_acquire)) {
        return JNI_TRUE;
    }
    if (g_worker.joinable()) {
        // A previous run's thread exited (g_running false) but was never
        // joined - stop() below handles the normal path; this only matters
        // if start() is called again without an intervening stop().
        g_worker.join();
    }
    g_stop_requested.store(false, std::memory_order_relaxed);
    g_worker = std::thread(run_loop);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeStopLiveCursor(JNIEnv* /*env*/, jclass /*clazz*/) {
    g_stop_requested.store(true, std::memory_order_release);
    if (g_worker.joinable()) {
        g_worker.join();
    }
}

// Whether the encode loop is actually up, not just "was nativeStartLiveCursor
// called" - the two can disagree: run_loop() returns immediately, before
// setting g_running, if PassthroughSink::start() fails (e.g. no receiver
// currently accepting E-AC3, HDMI not negotiated yet), and
// nativeStartLiveCursor() itself returns true as soon as the worker thread
// is SPAWNED, not once it has actually succeeded - it has no way to wait for
// that without blocking the caller. MainActivity's own receiver-availability
// polling reads this to notice a start that silently failed (as opposed to
// one that's still spinning up) and retry, rather than requiring the user to
// force-restart the app once their AVR is actually on - see MainActivity's
// own reconcileReceiverState().
extern "C" JNIEXPORT jboolean JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeIsLiveCursorRunning(JNIEnv* /*env*/, jclass /*clazz*/) {
    return g_running.load(std::memory_order_acquire) ? JNI_TRUE : JNI_FALSE;
}

// The running-total underrun count (StreamStats::underruns) - how
// MainActivity's reconcileReceiverState() detects a receiver disappearing
// WHILE the encode loop is already streaming, without ever calling
// AudioTrack.isDirectPlaybackSupported() again while a direct AudioTrack is
// open: that call blocks indefinitely against an actively-playing direct
// track on the same route (confirmed hanging on real hardware - audio
// policy manager lock contention, not a bug in the probe itself). A rising
// underrun count while running is a safe, purely-numeric signal instead -
// see MainActivity's own comment for how it's used.
extern "C" JNIEXPORT jlong JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeGetUnderrunCount(JNIEnv* /*env*/, jclass /*clazz*/) {
    return static_cast<jlong>(stream_stats().underruns.load(std::memory_order_relaxed));
}

// Called once from MainActivity.onCreate, before nativeStartLiveCursor - see
// g_asset_manager's own comment. AAssetManager_fromJava's returned handle is
// tied to the passed-in AssetManager object's lifetime; MainActivity passes
// its own Context.getAssets() result, which lives for the whole process, so
// no GlobalRef/cleanup is needed here.
extern "C" JNIEXPORT void JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeSetAssetManager(JNIEnv* env, jclass /*clazz*/,
                                                              jobject asset_manager) {
    g_asset_manager.store(AAssetManager_fromJava(env, asset_manager), std::memory_order_relaxed);
}

// Called from InputController.kt's animation ticker, roughly once per UI
// frame (~16ms) - NOT once per raw MotionEvent/KeyEvent, so a stick or held
// D-pad direction biases the object smoothly rather than in per-event jumps.
// dx/dy/dz are already scaled by the caller (stick magnitude x speed x
// elapsed time, or a D-pad direction x speed x elapsed time); this function
// only clamps the resulting deflection to its bounding box - see
// LiveCursorState::deflect_selected. The object itself keeps following its
// trajectory throughout; this only biases it off that course, and the bias
// decays back to zero on its own once input stops (LiveCursorState::advance).
extern "C" JNIEXPORT void JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeDeflectSelectedObject(JNIEnv* /*env*/,
                                                                   jclass /*clazz*/, jfloat dx,
                                                                   jfloat dy, jfloat dz) {
    live_cursor_state().deflect_selected(dx, dy, dz);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeCycleSelectedObject(JNIEnv* /*env*/,
                                                                 jclass /*clazz*/) {
    return live_cursor_state().cycle_selected();
}

// Called from InputController.kt's long-press handling. See
// LiveCursorState::snap_selected's own comment.
extern "C" JNIEXPORT void JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeSnapSelectedToCourse(JNIEnv* /*env*/,
                                                                  jclass /*clazz*/) {
    live_cursor_state().snap_selected();
}

// For the room visualization (a later pass): one flat array, 4 floats per
// object (x, y, z, 1.0-if-selected-else-0.0), kObjects*4 long.
extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeGetObjectState(JNIEnv* env, jclass /*clazz*/) {
    const auto placement = live_cursor_state().snapshot();
    const int selected = live_cursor_state().selected();

    jfloatArray result = env->NewFloatArray(kObjects * 4);
    if (result == nullptr) {
        return nullptr;
    }
    std::array<jfloat, kObjects * 4> flat{};
    for (int obj = 0; obj < kObjects; ++obj) {
        flat[static_cast<std::size_t>(obj * 4 + 0)] =
            static_cast<jfloat>(placement[static_cast<std::size_t>(obj)].position.x);
        flat[static_cast<std::size_t>(obj * 4 + 1)] =
            static_cast<jfloat>(placement[static_cast<std::size_t>(obj)].position.y);
        flat[static_cast<std::size_t>(obj * 4 + 2)] =
            static_cast<jfloat>(placement[static_cast<std::size_t>(obj)].position.z);
        flat[static_cast<std::size_t>(obj * 4 + 3)] = obj == selected ? 1.0f : 0.0f;
    }
    env->SetFloatArrayRegion(result, 0, kObjects * 4, flat.data());
    return result;
}

// Called from InputController.kt's play/pause key handling. See
// StreamStats::ambient_muted's own comment for what this actually does
// (mutes the ambient objects' audio, not their motion).
extern "C" JNIEXPORT void JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeSetAmbientMuted(JNIEnv* /*env*/, jclass /*clazz*/,
                                                              jboolean muted) {
    stream_stats().ambient_muted.store(muted != JNI_FALSE, std::memory_order_relaxed);
}

// For the room visualization's 3D trail view: `sample_count` (x,y,z) points
// along the LEAD object's own pre-planned trajectory - deliberately its base
// path only, with no deflection - starting now and running `seconds_ahead`
// seconds into the future. Deflection is excluded on purpose: it decays back
// to zero on its own (LiveCursorState::advance) and future input can't be
// known, so the only honest "path ahead" to show is the course the object
// is actually heading back toward, not a guess at what a listener might do
// with the stick next. trajectory_position() is a pure function of time -
// this needs none of LiveCursorState's mutex-protected state, only
// g_start_time_ns to translate "now" into the same elapsed-seconds time
// base run_loop() itself uses.
extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeGetFutureLeadTrajectory(JNIEnv* env,
                                                                     jclass /*clazz*/,
                                                                     jfloat seconds_ahead,
                                                                     jint sample_count) {
    if (sample_count <= 0) {
        return env->NewFloatArray(0);
    }
    const auto start_ns = g_start_time_ns.load(std::memory_order_relaxed);
    if (start_ns == 0) {
        // No run in progress (never started, or stopped) - there is no
        // "ahead" to plot. An empty array, not a path computed against a
        // zero epoch, which would be the whole boot-clock elapsed time.
        return env->NewFloatArray(0);
    }
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const double now_s = static_cast<double>(now_ns - start_ns) / 1.0e9;

    jfloatArray result = env->NewFloatArray(sample_count * 3);
    if (result == nullptr) {
        return nullptr;
    }
    std::vector<jfloat> flat(static_cast<std::size_t>(sample_count) * 3);
    const int steps = sample_count > 1 ? sample_count - 1 : 1;
    for (int i = 0; i < sample_count; ++i) {
        const double t = now_s + (static_cast<double>(i) / static_cast<double>(steps)) *
                                     static_cast<double>(seconds_ahead);
        // The scene as it is NOW: a blend in progress is deliberately ignored
        // here, since this draws where the object is heading, and where it is
        // heading is the scene it is heading into.
        const auto pos = trajectory_position(
            std::clamp(g_scene.load(std::memory_order_relaxed), 0, kSceneCount - 1), 0, t);
        flat[static_cast<std::size_t>(i) * 3 + 0] = static_cast<jfloat>(pos.x);
        flat[static_cast<std::size_t>(i) * 3 + 1] = static_cast<jfloat>(pos.y);
        flat[static_cast<std::size_t>(i) * 3 + 2] = static_cast<jfloat>(pos.z);
    }
    env->SetFloatArrayRegion(result, 0, sample_count * 3, flat.data());
    return result;
}

// Whether the ambient voices are muted. A getter as well as a setter because
// three places now change it - the remote's transport keys, the settings
// panel and the phone remote - and a mirror of this in Kotlin would drift the
// moment any two of them were used in one session.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeGetAmbientMuted(JNIEnv* /*env*/, jclass /*clazz*/) {
    return stream_stats().ambient_muted.load(std::memory_order_relaxed) ? JNI_TRUE : JNI_FALSE;
}

// Path recording: idle -> recording -> playing -> idle. Returns the new
// state (0/1/2, matching RecordState). The encode loop's own clock is what
// timestamps a recording, so this needs the loop to be running; with it
// stopped this is a no-op that stays idle.
extern "C" JNIEXPORT jint JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeToggleRecording(JNIEnv* /*env*/, jclass /*clazz*/) {
    const auto start_ns = g_start_time_ns.load(std::memory_order_relaxed);
    if (start_ns == 0) {
        return static_cast<jint>(RecordState::kIdle);
    }
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const double time_s = static_cast<double>(now_ns - start_ns) / 1.0e9;
    return static_cast<jint>(live_cursor_state().toggle_record(time_s));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeGetRecordState(JNIEnv* /*env*/, jclass /*clazz*/) {
    return static_cast<jint>(live_cursor_state().record_state());
}

// Scene selection. See kScenes - the demo used to have one path through the
// room and nothing else to show.
extern "C" JNIEXPORT void JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeSetScene(JNIEnv* /*env*/, jclass /*clazz*/,
                                                       jint scene) {
    // Wrapped rather than clamped: this is reached by "next"/"previous" keys
    // and by the guided tour, and all three want to come round again.
    const int wrapped = ((scene % kSceneCount) + kSceneCount) % kSceneCount;
    g_scene.store(wrapped, std::memory_order_relaxed);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeGetScene(JNIEnv* /*env*/, jclass /*clazz*/) {
    return g_scene.load(std::memory_order_relaxed);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeGetSceneCount(JNIEnv* /*env*/, jclass /*clazz*/) {
    return kSceneCount;
}

// name and hint for one scene, tab-separated - one call rather than two, and
// they are only ever wanted together (the overlay shows both).
extern "C" JNIEXPORT jstring JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeGetSceneText(JNIEnv* env, jclass /*clazz*/,
                                                           jint scene) {
    const int wrapped = ((scene % kSceneCount) + kSceneCount) % kSceneCount;
    const auto& s = kScenes[static_cast<std::size_t>(wrapped)];
    std::string text(s.name);
    text += '\t';
    text += s.hint;
    return env->NewStringUTF(text.c_str());
}

// OBJECTS OFF, from the remote/controller. See the strip block in run_loop().
extern "C" JNIEXPORT void JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeSetObjectsOff(JNIEnv* /*env*/, jclass /*clazz*/,
                                                            jboolean off) {
    stream_stats().objects_off.store(off == JNI_TRUE, std::memory_order_relaxed);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeGetObjectsOff(JNIEnv* /*env*/, jclass /*clazz*/) {
    return stream_stats().objects_off.load(std::memory_order_relaxed) ? JNI_TRUE : JNI_FALSE;
}

// Two floats: the energy vector's azimuth in degrees counterclockwise from
// front, and its magnitude in [0,1]. For the top-down panel's soundfield
// arrow - see StreamStats::energy_azimuth_deg.
extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeGetSoundfieldVector(JNIEnv* env, jclass /*clazz*/) {
    auto& s = stream_stats();
    const std::array<jfloat, 2> flat{s.energy_azimuth_deg.load(std::memory_order_relaxed),
                                     s.energy_magnitude.load(std::memory_order_relaxed)};
    jfloatArray result = env->NewFloatArray(2);
    if (result == nullptr) {
        return nullptr;
    }
    env->SetFloatArrayRegion(result, 0, 2, flat.data());
    return result;
}

// The measured BS.1770 loudness of the bed, and the dialnorm it implies.
// Empty until the meter's first gated 400ms block has passed.
extern "C" JNIEXPORT jstring JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeGetLoudnessText(JNIEnv* env, jclass /*clazz*/) {
    auto& s = stream_stats();
    if (!s.loudness_valid.load(std::memory_order_relaxed)) {
        return env->NewStringUTF("");
    }
    char buf[96];
    std::snprintf(buf, sizeof buf, "%.1f LKFS  ->  dialnorm %d",
                  static_cast<double>(s.integrated_lkfs.load(std::memory_order_relaxed)),
                  s.implied_dialnorm.load(std::memory_order_relaxed));
    return env->NewStringUTF(buf);
}

// For the on-screen stats overlay (RoomView.kt) - a single formatted line
// rather than several numeric fields, since there is exactly one caller and
// building the string here avoids Kotlin needing its own copy of the same
// formatting logic.
extern "C" JNIEXPORT jstring JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeGetStreamStatsText(JNIEnv* env, jclass /*clazz*/) {
    auto& s = stream_stats();
    // Frame count dropped and "lost" only appended when actually nonzero -
    // the 3D track card that hosts this text got considerably narrower once
    // top-down/elevation moved beside it instead of behind it (see
    // RoomView.kt's layout rebalance), and the full "frame N | bursts ...
    // (0 lost) | ..." string no longer fit at any font size worth reading -
    // confirmed clipped on a real device screenshot. Frame count was the
    // least useful field to a viewer anyway; "0 lost" is the expected,
    // non-event case and not worth the width when it's the common case.
    const auto underruns = s.underruns.load(std::memory_order_relaxed);
    char loss_buf[24] = "";
    if (underruns > 0) {
        std::snprintf(loss_buf, sizeof loss_buf, " (%llu lost)",
                      static_cast<unsigned long long>(underruns));
    }
    // The object path's latency, which is what the moving dot on screen is
    // actually ahead of. Shown so the demo states the figure rather than
    // implying the plot and the sound are simultaneous; the plot is
    // deliberately NOT shifted by it, because the encoder's own budget is
    // only the part of the pipeline this app can measure - the AudioTrack
    // queue and the receiver's own decode add more, and silently correcting
    // for one term of three would be a different kind of wrong.
    const auto object_lat_ms = s.object_latency_ms.load(std::memory_order_relaxed);

    // What the object layer is costing, only while it is being taken away -
    // the number is meaningless otherwise, and the line has no width to spare.
    // Recording state is worth a word on screen: a demo that is silently
    // capturing, or silently replaying rather than following its own scene, is
    // confusing in exactly the way this readout exists to prevent.
    const char* record_buf = "";
    switch (live_cursor_state().record_state()) {
        case RecordState::kRecording: record_buf = " | REC"; break;
        case RecordState::kPlaying:   record_buf = " | LOOPING YOUR PATH"; break;
        case RecordState::kIdle:      break;
    }
    char strip_buf[40] = "";
    if (s.objects_off.load(std::memory_order_relaxed)) {
        std::snprintf(strip_buf, sizeof strip_buf, " | OBJECTS OFF (-%u B/frame)",
                      s.stripped_bytes_per_frame.load(std::memory_order_relaxed));
    }
    char buf[288];
    std::snprintf(buf, sizeof buf,
                  "bursts %llu/%llu%s | encode %.1f/32ms | enc lat %.0fms | %s%s%s%s",
                  static_cast<unsigned long long>(s.bursts_rendered.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(s.bursts_submitted.load(std::memory_order_relaxed)),
                  loss_buf,
                  static_cast<double>(s.encode_ms.load(std::memory_order_relaxed)),
                  static_cast<double>(object_lat_ms),
                  s.signed_stream.load(std::memory_order_relaxed) ? "Atmos (signed)"
                                                                   : "5.1 bed (unsigned)",
                  s.ambient_muted.load(std::memory_order_relaxed) ? " | ambient muted" : "",
                  strip_buf, record_buf);
    return env->NewStringUTF(buf);
}

// For the speaker-activity meter (RoomView.kt / a small dedicated channel-
// meter view) - 6 floats, AC-3 coded order (L, C, R, Ls, Rs, LFE), see
// StreamStats::channel_levels's own comment for what they represent.
extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeGetChannelLevels(JNIEnv* env, jclass /*clazz*/) {
    jfloatArray result = env->NewFloatArray(6);
    if (result == nullptr) {
        return nullptr;
    }
    std::array<jfloat, 6> flat{};
    auto& s = stream_stats();
    for (std::size_t ch = 0; ch < 6; ++ch) {
        flat[ch] = s.channel_levels[ch].load(std::memory_order_relaxed);
    }
    env->SetFloatArrayRegion(result, 0, 6, flat.data());
    return result;
}

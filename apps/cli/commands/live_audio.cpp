#include "live_audio.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <fmt/base.h>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "../exit_codes.hpp"
#include "../support.hpp"
#include "ac3/analysis/levels.hpp"
#include "ac3/audio/capture.hpp"
#include "ac3/audio/live_positions.hpp"
#include "ac3/audio/monitor.hpp"
#include "ac3/audio/passthrough.hpp"
#include "ac3/audio/resampler.hpp"
#include "ac3/audio/spatial.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/oba/scene.hpp"
#include "ac3/audio/watchdog.hpp"
#include "ac3/encoder/assignment.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/iec61937/iec61937.hpp"
#include "recording_sink.hpp"

namespace ac3cli::commands {

namespace plan = ac3::plan;

int run_monitor(std::string_view in_path, int device_index, const Options& meta) {
    const auto stream = read_elementary_stream(in_path);
    if (stream.empty()) {
        return kExitInput;
    }
    if (!apply_object_verification(stream, meta)) {
        return kExitInput;
    }
    const auto bsid = ac3::stream_bsid(stream);
    if (!bsid) {
        fmt::println(stderr, "error: {} is too short to hold a syncframe", in_path);
        return kExitInput;
    }
    const bool eac3 = *bsid > 8;

    std::string device_id;
    std::string device_name = "default endpoint";
    // 0 means the backend cannot say - see RenderDeviceInfo::channels, and
    // the fold decision below, which treats unknown as "leave it alone".
    std::uint16_t device_channels = 0;
    if (device_index >= 0) {
        const auto devices = ac3::audio::enumerate_render_devices();
        if (!devices) {
            fmt::println(stderr, "error: {}", ac3::audio::describe(devices.error()));
            return kExitUnavailable;
        }
        if (static_cast<std::size_t>(device_index) >= devices->size()) {
            fmt::println(stderr, "error: device index {} out of range (see 'ac3cli outputs')",
                         device_index);
            return kExitUsage;
        }
        const auto& chosen = (*devices)[static_cast<std::size_t>(device_index)];
        device_id = chosen.id;
        device_name = chosen.name;
        device_channels = chosen.channels;
    } else {
        // The default endpoint has no index to look up, so it is found by the
        // is_default flag the enumeration already sets - the same endpoint an
        // empty device_id opens below. A failed enumeration is not an error
        // here: it only costs the fold decision its input.
        if (const auto devices = ac3::audio::enumerate_render_devices()) {
            for (const auto& candidate : *devices) {
                if (candidate.is_default) {
                    device_channels = candidate.channels;
                    break;
                }
            }
        }
    }

    // §7.8: what the decoders should fold to before anything is played. An
    // explicit channels=/downmix= always wins; otherwise a device that renders
    // fewer channels than the programme gets a real §7.8 fold instead of
    // whatever the platform's shared-mode mixer would average it down to.
    //
    // MonitorSink opens in SHARED mode, so a 5.1 programme on a stereo
    // endpoint is not refused - it is silently mixed by the OS, with no
    // cmixlev/surmixlev and no §7.8.1 normalisation. That is exactly the case
    // worth catching, and the only way to catch it is to notice the width
    // beforehand: RenderDeviceInfo::channels is 0 on any backend that cannot
    // say, and 0 leaves the audio alone.
    ac3::OutputConfig output = meta.output;
    if (output.target == ac3::DownmixTarget::kAsCoded && device_channels > 0) {
        const auto scanned = ac3::io::scan(stream);
        if (scanned && scanned->channels > static_cast<int>(device_channels)) {
            output.target = device_channels == 1 ? ac3::DownmixTarget::kMono
                                                 : ac3::DownmixTarget::kLoRo;
            fmt::println("  {} channels on a {}-channel output: folding to {} (§7.8)",
                         scanned->channels, device_channels,
                         output.target == ac3::DownmixTarget::kMono ? "mono" : "Lo/Ro stereo");
        }
    }

    ac3::audio::MonitorSink sink;
    std::uint64_t units_played = 0;
    auto play = [&](std::span<const float> interleaved) {
        while (!sink.submit(interleaved)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    };

    if (eac3) {
        const auto units = ac3::split_access_units(stream);
        if (!units || units->empty()) {
            fmt::println(stderr, "error: {} is not a valid E-AC-3 stream", in_path);
            return kExitInput;
        }
        // Heap-allocated (PREfast's C6262, alert #9): Eac3Decoder grew
        // several KB of per-block scratch members (alert #63's fix), which
        // pushed this one-shot stack declaration over the threshold - same
        // pattern as PR #50.
        auto decoder = std::make_unique<ac3::Eac3Decoder>(
            ac3::DecoderConfig{.drc_scale = meta.drc_scale,
                               .fast_imdct = meta.fast_imdct,
                               .heavy_compression = meta.p.heavy.has_value(),
                               .output = output,
                               .concealment = meta.concealment,
                               .fast_mdct = meta.fast_mdct});
        std::vector<std::size_t> order;
        for (const auto& unit : *units) {
            const auto decoded = decoder->decode_access_unit(unit);
            if (!decoded) {
                fmt::println(stderr, "error: decode failed (code {})",
                             static_cast<int>(decoded.error()));
                return kExitInput;
            }
            if (!decoded->has_value()) {
                // §3.7: held back pending transient pre-noise processing
                // (Eac3Decoder::decode_access_unit's own doc comment) - live
                // monitoring just waits for the next unit to catch up rather
                // than draining decoder.flush() mid-stream.
                continue;
            }
            const auto& out = **decoded;
            if (order.empty()) {
                // Dual mono has no Table E2.5 location to order by - `layout`
                // is left empty for exactly that case - so Ch1/Ch2 monitor in
                // coded order, same as everywhere else this comes up (see
                // plan::monitor_order's own comment). A fold has already put
                // its own channels in their own order (L then R, or the one
                // mono channel) with `layout` left at the rendered BED's full
                // layout - monitor_order alone would size the permutation off
                // that unfolded layout rather than the folded channel count,
                // so the fold case stays identity here too.
                if (out.acmod == ac3::Acmod::kDualMono ||
                    output.target != ac3::DownmixTarget::kAsCoded) {
                    order.resize(out.channels.size());
                    for (std::size_t i = 0; i < order.size(); ++i) {
                        order[i] = i;
                    }
                } else {
                    order = plan::monitor_order(std::span{out.layout.items}.first(
                                                    static_cast<std::size_t>(out.layout.count)),
                                                out.channels.size());
                }
                const auto started = sink.start(device_id, sample_rate_hz(out.sample_rate),
                                                static_cast<std::uint16_t>(order.size()));
                if (!started) {
                    fmt::println(stderr, "error: {}", ac3::audio::describe(started.error()));
                    return kExitUnavailable;
                }
                status_println(status_stream(), "monitoring {} ({} channels, {} Hz) on \"{}\"…",
                               in_path, order.size(), sample_rate_hz(out.sample_rate),
                               device_name);
                // Same object-count line run_decode_eac3 reports (see
                // report_decoded_objects) - this path still only plays the
                // 5.1 bed (this function's own header comment), so it says
                // so rather than implying object playback is coming.
                if (out.object_metadata) {
                    // Guarded by the if above; clang-tidy's
                    // bugprone-unchecked-optional-access doesn't trace the
                    // guard through into a multi-argument fmt::println call,
                    // the same false positive print_channel_summary(*meter)
                    // elsewhere in this file works around - binding once here
                    // instead of repeating out.object_metadata-> twice sidesteps it.
                    const auto& metadata = *out.object_metadata;  // NOLINT(bugprone-unchecked-optional-access)
                    fmt::println(
                        "  {} dynamic objects + the bed's LFE = {} objects, OAMD present{}",
                        metadata.objects.size(), ac3::oba::object_count(metadata.program),
                        out.object_audio.empty()
                            ? " (JOC audio not reconstructed)"
                            : ", JOC audio reconstructed (bed-only playback here; see "
                              "'ac3cli decode' with objects_dir to export it)");
                }
            }
            play(interleave_reordered(out.channels, order));
            ++units_played;
        }
    } else {
        const auto frames = ac3::split_frames(stream);
        if (!frames || frames->empty()) {
            fmt::println(stderr, "error: {} is not a valid AC-3 stream", in_path);
            return kExitInput;
        }
        ac3::FrameDecoder decoder{
            ac3::DecoderConfig{.drc_scale = meta.drc_scale,
                               .fast_imdct = meta.fast_imdct,
                               .heavy_compression = meta.p.heavy.has_value(),
                               .output = output,
                               .concealment = meta.concealment}};
        std::vector<std::size_t> order;
        for (const auto& frame : *frames) {
            const auto decoded = decoder.decode_frame(frame);
            if (!decoded) {
                fmt::println(stderr, "error: {}: {}", in_path, ac3::describe(decoded.error()));
                return kExitInput;
            }
            if (order.empty()) {
                if (output.target != ac3::DownmixTarget::kAsCoded &&
                    decoded->acmod != ac3::Acmod::kDualMono) {
                    order.resize(decoded->channels.size());
                    for (std::size_t i = 0; i < order.size(); ++i) {
                        order[i] = i;
                    }
                } else {
                    order = ac3::io::wav_channel_order(decoded->acmod, decoded->lfe);
                }
                const auto started = sink.start(device_id, sample_rate_hz(decoded->sample_rate),
                                                static_cast<std::uint16_t>(order.size()));
                if (!started) {
                    fmt::println(stderr, "error: {}", ac3::audio::describe(started.error()));
                    return kExitUnavailable;
                }
                status_println(status_stream(), "monitoring {} ({} channels, {} Hz) on \"{}\"…",
                               in_path, order.size(), sample_rate_hz(decoded->sample_rate),
                               device_name);
            }
            play(interleave_reordered(decoded->channels, order));
            ++units_played;
        }
    }

    while (sink.stats().frames_rendered < sink.stats().frames_submitted) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto stats = sink.stats();
    sink.stop();
    status_println(status_stream(), "played {} {}, {} underruns", units_played,
                   eac3 ? "access units" : "frames", stats.underruns);
    return kExitOk;
}

namespace {

// WAVEFORMATEXTENSIBLE SPEAKER_LOW_FREQUENCY (ksmedia.h) - the one bed
// channel run_spatial feeds the sink as a static object. Hardcoded for the
// same reason ac3::audio's own Windows backend hardcodes its SPEAKER_* bits
// rather than pulling in mmreg.h: the value is part of the public ABI and
// has never changed.
constexpr std::uint32_t kSpeakerLowFrequency = 0x8;

// TS 103 420 §4.2.1's room-anchored cube (x,y in [0,1]; z in [-1,1] about ear
// height - see ac3::oba::scene.hpp's Orientation comment and
// bed_label_position's "front wall at y=0, ceiling at z=+1, sides at x=0 and
// 1", and the GUI's ObjectInspectorDialog.qml plan/elevation views, which
// draw the same cube the same way) to ISpatialAudioObject::SetPosition's
// listener-relative, right-handed metres: +x right, +y up, +z BEHIND the
// listener (Microsoft Learn, "Render spatial sound using spatial audio
// objects").
//
// OAMD's cube carries no absolute size, so the metre scale below is a
// plausible small-room half-extent, not a measured one - what is NOT a
// guess is the axis correspondence, and the listener sits at the room's
// centre facing the front (screen) wall, the same reference point
// ac3::oba::Orientation::rotate already treats as "centred". Moving away
// from centre still moves an object further away in the right direction;
// only the absolute distance is approximate.
struct SpatialXyz {
    float x;
    float y;
    float z;
};

SpatialXyz to_windows_spatial(const ac3::oba::Position& p) {
    constexpr float kHalfWidthM = 2.0F;  // left/right wall distance from centre
    constexpr float kHalfDepthM = 2.0F;  // front/rear wall distance from centre
    constexpr float kHeightM = 1.0F;     // ceiling/floor distance from ear height
    return {.x = (static_cast<float>(p.x) - 0.5F) * kHalfWidthM,
            .y = static_cast<float>(p.z) * kHeightM,
            .z = (static_cast<float>(p.y) - 0.5F) * kHalfDepthM};
}

}  // namespace

int run_spatial(std::string_view in_path, int device_index, const Options& meta) {
    const auto stream = read_all(in_path);
    if (stream.empty()) {
        fmt::println(stderr, "error: cannot read {}", in_path);
        return kExitInput;
    }
    if (!apply_object_verification(stream, meta)) {
        return kExitInput;
    }
    const auto bsid = ac3::stream_bsid(stream);
    if (!bsid) {
        fmt::println(stderr, "error: {} is too short to hold a syncframe", in_path);
        return kExitInput;
    }
    if (*bsid <= 8) {
        fmt::println(stderr,
                     "error: spatial rendering needs the object layer, which only E-AC-3 "
                     "carries - 'ac3cli monitor' plays a plain AC-3 bed");
        return kExitInput;
    }

    std::string device_id;
    std::string device_name = "default endpoint";
    if (device_index >= 0) {
        const auto devices = ac3::audio::enumerate_render_devices();
        if (!devices) {
            fmt::println(stderr, "error: {}", ac3::audio::describe(devices.error()));
            return kExitUnavailable;
        }
        if (static_cast<std::size_t>(device_index) >= devices->size()) {
            fmt::println(stderr, "error: device index {} out of range (see 'ac3cli outputs')",
                         device_index);
            return kExitUsage;
        }
        const auto& chosen = (*devices)[static_cast<std::size_t>(device_index)];
        device_id = chosen.id;
        device_name = chosen.name;
    }

    // Refused up front, before any decoding: a spatial format has to be
    // enabled on the chosen endpoint for this command to do anything this
    // project cannot already do with 'monitor', and the roadmap calls for
    // exactly this clean, named refusal rather than a generic failure.
    const auto capability = ac3::audio::probe_spatial_capability(device_id);
    if (!capability) {
        fmt::println(stderr, "error: {}", ac3::audio::describe(capability.error()));
        return kExitUnavailable;
    }
    if (capability->max_dynamic_objects == 0) {
        fmt::println(stderr, "error: {}", capability->reason);
        return kExitUnavailable;
    }

    const auto units = ac3::split_access_units(stream);
    if (!units || units->empty()) {
        fmt::println(stderr, "error: {} is not a valid E-AC-3 stream", in_path);
        return kExitInput;
    }
    auto decoder = std::make_unique<ac3::Eac3Decoder>(
        ac3::DecoderConfig{.drc_scale = meta.drc_scale,
                           .fast_imdct = meta.fast_imdct,
                           .heavy_compression = meta.p.heavy.has_value(),
                           .output = meta.output,
                           .concealment = meta.concealment});

    ac3::audio::SpatialObjectSink sink;
    bool started = false;
    std::uint64_t units_played = 0;
    std::vector<ac3::audio::DynamicObjectUpdate> dynamic_updates;
    std::vector<ac3::audio::StaticObjectUpdate> static_updates;

    for (const auto& unit : *units) {
        const auto decoded = decoder->decode_access_unit(unit);
        if (!decoded) {
            fmt::println(stderr, "error: decode failed (code {})",
                         static_cast<int>(decoded.error()));
            return kExitInput;
        }
        if (!decoded->has_value()) {
            // §3.7: held back pending transient pre-noise processing - see
            // run_monitor's identical handling above.
            continue;
        }
        const auto& out = **decoded;

        if (!started) {
            const bool has_lfe =
                out.object_metadata && ac3::oba::has_lfe(out.object_metadata->program);
            const auto started_result =
                sink.start(device_id, sample_rate_hz(out.sample_rate),
                          has_lfe ? kSpeakerLowFrequency : 0U,
                          static_cast<std::uint32_t>(out.object_audio.size()));
            if (!started_result) {
                fmt::println(stderr, "error: {}", ac3::audio::describe(started_result.error()));
                return kExitUnavailable;
            }
            started = true;
            status_println(status_stream(), "spatial: {} dynamic object(s){} on \"{}\"…",
                           out.object_audio.size(),
                           has_lfe ? " + the bed's LFE (static)" : "", device_name);
        }

        dynamic_updates.clear();
        if (out.object_metadata) {
            const auto positions = ac3::oba::describe_objects(*out.object_metadata);
            for (std::size_t i = 0; i < out.object_audio.size() && i < positions.size(); ++i) {
                const auto xyz = to_windows_spatial(positions[i].position);
                dynamic_updates.push_back(
                    {.pcm = out.object_audio[i],
                     .x = xyz.x,
                     .y = xyz.y,
                     .z = xyz.z,
                     .gain = static_cast<float>(std::pow(10.0, positions[i].gain_db / 20.0))});
            }
        }
        static_updates.clear();
        if (out.object_metadata && ac3::oba::has_lfe(out.object_metadata->program) &&
            !out.channels.empty()) {
            // Table 5.8's coded order puts the LFE last regardless of acmod
            // (ac3::DecodedAccessUnit::channels' own doc comment) - never a
            // JOC output (§6.3.2.2), so it only ever exists here.
            static_updates.push_back(
                {.pcm = out.channels.back(), .channel = kSpeakerLowFrequency});
        }

        while (!sink.submit(dynamic_updates, static_updates)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        ++units_played;
    }

    while (sink.stats().updates_rendered < sink.stats().updates_submitted) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto stats = sink.stats();
    sink.stop();
    status_println(status_stream(),
                   "played {} access units, {} active dynamic objects, {} underruns",
                   units_played, stats.active_dynamic_objects, stats.underruns);
    return kExitOk;
}

// The slot budget for a live object session, allocated ONCE here so a slot
// bound later cannot change the stream's object count mid-session - a decoder
// reads the count from the first access unit's OAMD and never re-reads it.
//
// Three ways to arrive at it, in priority order: an explicit map= binds
// capture channels to slots in map= order (object_slots_from_assignment, the
// same function atmos-encode builds its objects with, so a given map= means
// the same objects either way); objects= alone sets the budget and binds the
// first N captured channels one-to-one; neither leaves one slot per captured
// channel, which is what `live mode=atmos` has always done.
//
// Returns nullopt with the reason already printed.
std::optional<std::vector<ObjectSlot>> resolve_object_slots(
    const Options& meta, std::size_t master_channels, std::size_t slave_channels) {
    const std::size_t combined = master_channels + slave_channels;
    std::vector<ObjectSlot> slots;

    if (meta.map_spec) {
        // The two capture devices are the two sources, concatenated in the
        // order `live` already concatenates their channels - so map=0.N
        // addresses the master and map=1.N the slave, matching what 'devices'
        // and capture2= already number.
        std::vector<plan::SourceShape> shapes;
        shapes.push_back({.channels = master_channels, .label = "capture"});
        if (slave_channels > 0) {
            shapes.push_back({.channels = slave_channels, .label = "capture2"});
        }
        plan::Assignment assignment;
        if (!plan::parse_assignment(*meta.map_spec, shapes, assignment)) {
            fmt::println(stderr, "error: bad map= spec ({})", plan::kAssignmentSyntax);
            return std::nullopt;
        }
        slots = object_slots_from_assignment(assignment, shapes);
        if (slots.empty()) {
            fmt::println(stderr,
                         "error: map= names no obj/objm destination, so this session would "
                         "carry no objects at all - use 'live mode=channels' for a plain "
                         "channel session");
            return std::nullopt;
        }
    } else {
        const std::size_t count =
            meta.live_objects.value_or(std::min<std::size_t>(combined, 15));
        slots.resize(std::min<std::size_t>(count, 15));
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (i < combined) {
                slots[i].taps.emplace_back(i, 1.0);
            }
        }
    }

    // objects= is the budget, whatever map= filled: a smaller budget than the
    // assignment needs is refused rather than silently truncated (the GUI
    // refuses the same way - see EncoderController's own "Refused rather than
    // truncated" note), a larger one allocates the extra slots unbound.
    if (meta.live_objects) {
        if (slots.size() > *meta.live_objects) {
            fmt::println(stderr,
                         "error: map= assigns {} objects but objects={} allows {} - raise the "
                         "budget or assign fewer",
                         slots.size(), *meta.live_objects, *meta.live_objects);
            return std::nullopt;
        }
        slots.resize(*meta.live_objects);
    }
    if (slots.empty() || slots.size() > 15) {
        fmt::println(stderr,
                     "error: 1 to 15 object slots (the bed's LFE is the 16th, and TS 103 420 "
                     "8.3.2.2 caps the total at 16); this session resolved {}",
                     slots.size());
        return std::nullopt;
    }
    return slots;
}

int run_live(std::string_view out_path, int capture_device, std::uint32_t seconds,
            std::uint32_t bitrate, int monitor_device, int passthrough_device,
            std::string_view mode, const Options& meta) {
    if (mode != "channels" && mode != "atmos") {
        fmt::println(stderr, "error: mode is 'channels' (default) or 'atmos'");
        return kExitUsage;
    }
    const bool atmos = mode == "atmos";

    // positions= only means anything once objects exist to drive - a pure
    // input-shape conflict, so it is refused before any device I/O rather
    // than after: the answer does not depend on what hardware is present.
    if (meta.positions && !atmos) {
        fmt::println(stderr,
                     "error: positions= drives object placement, which only 'live mode=atmos' "
                     "has");
        return kExitUsage;
    }

    const auto devices = ac3::audio::enumerate_devices();
    if (!devices) {
        fmt::println(stderr, "error: {}", ac3::audio::describe(devices.error()));
        return kExitUnavailable;
    }
    if (capture_device < 0 || static_cast<std::size_t>(capture_device) >= devices->size()) {
        fmt::println(stderr, "error: capture device index {} out of range (see 'ac3cli devices')",
                     capture_device);
        return kExitUsage;
    }
    const auto& device = (*devices)[static_cast<std::size_t>(capture_device)];

    // capture2=: a second, independently-clocked device. Range-checked the
    // same way as the master above - a bad index refuses the whole command
    // rather than silently falling back to a single-device session.
    if (meta.capture2 && (*meta.capture2 < 0 ||
                          static_cast<std::size_t>(*meta.capture2) >= devices->size())) {
        fmt::println(stderr,
                     "error: capture2 device index {} out of range (see 'ac3cli devices')",
                     *meta.capture2);
        return kExitUsage;
    }
    const ac3::audio::DeviceInfo* device2 =
        meta.capture2 ? &(*devices)[static_cast<std::size_t>(*meta.capture2)] : nullptr;

    ac3::SampleRate sr{};
    switch (device.sample_rate) {
        case 48000: sr = ac3::SampleRate::k48000; break;
        case 44100: sr = ac3::SampleRate::k44100; break;
        case 32000: sr = ac3::SampleRate::k32000; break;
        default:
            fmt::println(stderr,
                         "error: \"{}\" runs at {} Hz; AC-3/E-AC-3 need 32, 44.1 or 48 kHz",
                         device.name, device.sample_rate);
            return kExitUnavailable;
    }

    // capture2's own rate only has to be a legal AC-3 rate itself - it does
    // NOT need to match the master's, since the resampler's nominal-
    // conversion side is exactly what absorbs a 44.1/48 kHz mismatch between
    // the two devices.
    double nominal_ratio = 1.0;
    if (device2) {
        switch (device2->sample_rate) {
            case 48000:
            case 44100:
            case 32000: break;
            default:
                fmt::println(stderr,
                             "error: capture2 \"{}\" runs at {} Hz; AC-3/E-AC-3 need 32, 44.1 "
                             "or 48 kHz",
                             device2->name, device2->sample_rate);
                return kExitUnavailable;
        }
        nominal_ratio =
            static_cast<double>(device.sample_rate) / static_cast<double>(device2->sample_rate);
    }

    // Object mode's stream shape is fixed by TS 103 420 - a 5.1 E-AC-3 bed
    // plus the object layer - so layout=/codec= only describe a channel-mode
    // session. Asking for both is a contradiction worth refusing rather than
    // silently ignoring one of them.
    if (atmos && (!meta.take_layout.empty() || meta.take_codec)) {
        fmt::println(stderr,
                     "error: layout=/codec= describe a channel session; mode=atmos always "
                     "encodes the TS 103 420 5.1 E-AC-3 bed plus its object layer");
        return kExitUsage;
    }
    std::optional<TakePlan> take;
    if (!atmos) {
        take = resolve_take_plan(meta, bitrate, sr);
        if (!take) {
            return kExitUsage;
        }
    }
    const bool eac3 = atmos || take->eac3;

    ac3::audio::Capture capture;
    const auto started = capture.start(device.id, device.kind);
    if (!started) {
        fmt::println(stderr, "error: {}", ac3::audio::describe(started.error()));
        return kExitUnavailable;
    }
    const auto channels = capture.channels();
    const auto rate_hz = capture.sample_rate();

    // Clock-master model: capture paces the session exactly as before -
    // nothing about its own timing changes below. capture2, when present, is
    // a second, independently-clocked device whose stream gets resampled
    // into lockstep with capture's pacing every frame, then appended after
    // capture's own channels.
    ac3::audio::Capture capture2;
    std::size_t capture2_channels = 0;
    std::optional<ac3::audio::DriftResampler> slave_resampler;
    std::optional<ac3::audio::ClockDriftEstimator> slave_drift;
    std::vector<float> slave_scratch;
    std::size_t slave_scratch_valid_frames = 0;
    std::vector<float> slave_out;
    const auto status = status_stream();
    if (device2) {
        const auto started2 = capture2.start(device2->id, device2->kind);
        if (!started2) {
            fmt::println(stderr, "error: {}", ac3::audio::describe(started2.error()));
            return kExitUnavailable;
        }
        capture2_channels = capture2.channels();
        slave_resampler.emplace(capture2_channels);
        slave_drift.emplace(nominal_ratio, static_cast<std::size_t>(ac3::kSamplesPerFrame));
        slave_scratch.resize(4 * static_cast<std::size_t>(ac3::kSamplesPerFrame) *
                             capture2_channels);
        slave_out.resize(static_cast<std::size_t>(ac3::kSamplesPerFrame) * capture2_channels);
        slave_resampler->reset();
        status_println(status, "capture2: \"{}\", {} ch @ {} Hz (nominal ratio {:.6f})",
                       device2->name, device2->channels, device2->sample_rate, nominal_ratio);
    }

    // Object mode: every slot is one object, bound to capture channels by
    // map= or one-to-one by default, against a budget fixed here (roadmap
    // IO9 - `live` used to pan exactly one object per capture channel with no
    // way to say otherwise). Channel mode: the captured channels are routed
    // onto take's coded channels by direction, the same plan::route model
    // 'encode' and the GUI's own live session use.
    std::vector<ObjectSlot> slots;
    if (atmos) {
        auto resolved = resolve_object_slots(meta, channels, capture2_channels);
        if (!resolved) {
            return kExitUsage;
        }
        slots = std::move(*resolved);
    } else if (meta.map_spec) {
        fmt::println(stderr,
                     "error: map= binds capture channels to OBJECT slots, which only "
                     "'live mode=atmos' has; a channel session places its channels by "
                     "direction onto layout= instead");
        return kExitUsage;
    }
    const std::size_t nobjects = slots.size();

    // positions=: a real live object-position source (roadmap UX4) instead
    // of the built-in synthetic orbit below. Built here, right after
    // nobjects is fixed and before anything else in this session (capture,
    // encoders, the output file) opens, so a bind failure is refused early
    // rather than discovered mid-session. Fatal (kExitUnavailable) rather
    // than a warning the way monitor=/passthrough= are: those are OUTPUT
    // legs whose absence still leaves a correct recording, but positions=
    // is an INPUT - silently falling back to the orbit because a port was
    // already in use would put a different scene in the file than the one
    // asked for, discovered only at playback.
    std::unique_ptr<ac3::audio::LivePositionSource> position_source;
    std::optional<ac3::oba::SceneCursor> position_cursor;
    if (atmos && meta.positions) {
        position_source = std::make_unique<ac3::audio::LivePositionSource>(nobjects);
        const auto bound = position_source->start(meta.positions->bind, meta.positions->port);
        if (!bound) {
            fmt::println(stderr, "error: positions={}:{}:{} - {}", meta.positions->scheme,
                         meta.positions->bind, meta.positions->port,
                         ac3::audio::describe(bound.error()));
            return kExitUnavailable;
        }
        // One default automation point per slot: the orbit's own t=0
        // position, so an object nothing has addressed yet holds where the
        // orbit would have started it - still spread apart from its
        // siblings, which is the reason the orbit spreads them at all (JOC
        // cannot separate co-located objects) - and this session's own
        // gain law, so switching positions= on changes WHERE objects start,
        // never how loud they are.
        std::vector<ac3::oba::SceneObject> objects(nobjects);
        for (std::size_t i = 0; i < nobjects; ++i) {
            const double angle =
                2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(nobjects);
            const double height =
                nobjects == 1 ? 0.5
                             : -1.0 + 2.0 * static_cast<double>(i) /
                                          static_cast<double>(nobjects - 1);
            objects[i].automation.push_back(
                {.time_s = 0.0,
                 .position = {.x = 0.5 + 0.5 * std::sin(angle),
                             .y = 0.5 - 0.5 * std::cos(angle),
                             .z = height},
                 .gain = 0.7 / std::sqrt(static_cast<double>(nobjects)),
                 .lfe_send = i == 0 ? 0.2 : 0.0});
        }
        auto scene = ac3::oba::ObjectScene::create(std::move(objects));
        if (!scene) {
            fmt::println(stderr, "error: internal: {}", scene.error().message);
            return kExitUsage;
        }
        position_cursor.emplace(std::move(*scene));
        status_println(status,
                       "positions: OSC on {}:{}, objects 0-{} (/object/<n>/xyz|gain|lfe|release)",
                       meta.positions->bind, position_source->local_port(), nobjects - 1);
    }

    const auto channel_plan = atmos ? plan::ChannelPlan{} : plan::resolve(take->plan);
    std::optional<plan::Routing> routing;
    if (!atmos) {
        routing = plan::route(channel_plan, channels, meta.p.cmixlev, meta.p.surmixlev);
        if (!routing) {
            fmt::println(stderr, "error: {} capture channels - {}", channels,
                         plan::describe(plan::PlanError::kNoSourceLayout));
            return kExitUsage;
        }
    }
    // Two different counts, both needed. `coded_channels` is what the encoder
    // is fed and what the meter shows; `rendered_channels` is what a decoder
    // hands back, which is fewer wherever a dependent REPLACES a bed channel
    // (7.1 renders 8 speakers from 10 coded) - so it is what the monitor sink
    // is opened with, since interleave_reordered below produces exactly that
    // many. An object session's bed is 5.1 either way.
    const std::size_t coded_channels =
        atmos ? 6 : static_cast<std::size_t>(routing->coded_channels);
    const std::size_t rendered_channels = atmos ? 6 : static_cast<std::size_t>(
                                                          take->rendered_channels);
    const auto bed_acmod = atmos ? ac3::Acmod::k3_2 : channel_plan.bed_acmod;
    const bool bed_lfe = atmos ? true : channel_plan.bed_lfe;
    const std::size_t bed_channels =
        static_cast<std::size_t>(ac3::fullbw_channel_count(bed_acmod) + (bed_lfe ? 1 : 0));

    auto resolve_render_device = [&](int index) -> std::optional<ac3::audio::RenderDeviceInfo> {
        if (index < 0) {
            return ac3::audio::RenderDeviceInfo{};  // empty id: default endpoint
        }
        const auto render_devices = ac3::audio::enumerate_render_devices(rate_hz);
        if (!render_devices || static_cast<std::size_t>(index) >= render_devices->size()) {
            return std::nullopt;
        }
        return (*render_devices)[static_cast<std::size_t>(index)];
    };

    ac3::audio::MonitorSink monitor_sink;
    bool monitoring = false;
    if (monitor_device != -2) {
        const auto target = resolve_render_device(monitor_device);
        if (!target) {
            fmt::println(stderr, "warning: monitor device index {} out of range; monitoring off",
                         monitor_device);
        } else {
            const auto mstarted = monitor_sink.start(
                target->id, rate_hz, static_cast<std::uint16_t>(rendered_channels));
            if (!mstarted) {
                fmt::println(stderr, "warning: monitor unavailable: {}",
                             ac3::audio::describe(mstarted.error()));
            } else {
                monitoring = true;
                status_println(status, "monitoring on \"{}\"",
                               target->name.empty() ? "default endpoint" : target->name);
            }
        }
    }

    // The parallel downmix leg (roadmap IO9, mirroring the GUI's
    // wants_downmix_leg): when the stream needs E-AC-3 but the chosen
    // receiver only bitstreams AC-3, an independent AC-3 encode of the bed
    // the main plan has ALREADY computed goes to the receiver, so a capped
    // downmix reaches it instead of a refusal. The file still carries the
    // full stream. downmix=off keeps the old plain refusal.
    ac3::audio::PassthroughSink passthrough_sink;
    bool passing_through = false;
    bool downmix_leg = false;
    if (passthrough_device != -2) {
        const auto target = resolve_render_device(passthrough_device);
        if (!target) {
            fmt::println(stderr,
                         "warning: passthrough device index {} out of range; passthrough off",
                         passthrough_device);
        } else {
            // An empty id is the default endpoint, whose capabilities were
            // never probed - it is taken at its word, exactly as before.
            const bool known = !target->id.empty();
            const bool takes_eac3 = !known || target->supports_eac3_passthrough;
            const bool takes_ac3 = !known || target->supports_ac3_passthrough;
            downmix_leg = eac3 && !takes_eac3 && takes_ac3 && meta.downmix_leg;
            const bool leg_eac3 = eac3 && !downmix_leg;
            if (!(leg_eac3 ? takes_eac3 : takes_ac3)) {
                fmt::println(stderr,
                             "warning: \"{}\" does not accept {} over IEC 61937; passthrough "
                             "off{}",
                             target->name, leg_eac3 ? "E-AC-3" : "AC-3",
                             eac3 && takes_ac3 && !meta.downmix_leg
                                 ? " (drop downmix=off to send it a capped 5.1 AC-3 leg)"
                                 : "");
            } else {
                const auto format = leg_eac3 ? ac3::audio::BitstreamFormat::kEac3
                                             : ac3::audio::BitstreamFormat::kAc3;
                const auto pstarted = passthrough_sink.start(target->id, rate_hz, format);
                if (!pstarted) {
                    fmt::println(stderr, "warning: passthrough unavailable: {}",
                                 ac3::audio::describe(pstarted.error()));
                } else {
                    passing_through = true;
                    status_println(status, "passthrough ({}) on \"{}\"{}",
                                   leg_eac3 ? "E-AC-3" : "AC-3",
                                   target->name.empty() ? "default endpoint" : target->name,
                                   downmix_leg ? " - parallel 5.1 downmix leg; the file still "
                                                 "carries the full stream"
                                               : "");
                }
            }
        }
    }
    downmix_leg = downmix_leg && passing_through;

    // Heap-allocated: each carries several KB of MDCT/delay history state,
    // and this function only constructs them once, at session start, not per
    // audio frame (PREfast's C6262) - same pattern as EncoderController's
    // runLiveSession, the GUI's equivalent of this function.
    std::unique_ptr<ac3::FrameEncoder> ac3_encoder;
    std::unique_ptr<ac3::eac3::AccessUnitEncoder> eac3_encoder;
    std::unique_ptr<ac3::oba::AtmosEncoder> atmos_encoder;
    if (atmos) {
        atmos_encoder = std::make_unique<ac3::oba::AtmosEncoder>(
            ac3::oba::AtmosConfig{.sample_rate = sr, .bitrate_kbps = bitrate,
                                  .dialnorm = meta.p.dialnorm, .num_bands_idx = 4,
                                  .fast_mdct = meta.fast_mdct, .joc_domain = meta.joc_domain},
            static_cast<int>(nobjects));
    } else if (take->eac3) {
        eac3_encoder =
            std::make_unique<ac3::eac3::AccessUnitEncoder>(plan::eac3_config(take->plan));
    } else {
        ac3_encoder = std::make_unique<ac3::FrameEncoder>(plan::ac3_config(take->plan));
    }
    // The receiver leg's own encoder, fed the bed the main plan already
    // computed - which IS a self-sufficient fold-down of the whole programme
    // (plan::route's own guarantee, and TS 103 420's for an object bed), so
    // there is no separate 7.8 fold to compute here. Built only when the leg
    // is actually running, unlike the GUI's (whose receiver can be hot-swapped
    // mid-session; ac3cli's cannot).
    std::unique_ptr<ac3::FrameEncoder> downmix_encoder;
    if (downmix_leg) {
        downmix_encoder = std::make_unique<ac3::FrameEncoder>(
            ac3::EncoderConfig{.sample_rate = sr,
                               .bitrate_kbps = ac3::clamp_to_legal_ac3_bitrate(bitrate),
                               .dialnorm = meta.p.dialnorm,
                               .acmod = bed_acmod,
                               .lfe = bed_lfe,
                               .fast_mdct = meta.fast_mdct,
                               .cmixlev = meta.p.cmixlev,
                               .surmixlev = meta.p.surmixlev});
    }
    auto ac3_monitor_decoder = std::make_unique<ac3::FrameDecoder>();
    // Heap-allocated (PREfast's C6262, alert #89): Eac3Decoder's per-block
    // scratch members pushed this stack declaration over the threshold, same
    // as the two decoders just above - same pattern as
    // examples/atmos_objects.cpp (PR #295).
    auto eac3_monitor_decoder = std::make_unique<ac3::Eac3Decoder>();
    ac3::iec61937::Eac3BurstPacker eac3_packer;

    // Object mode meters the 5.1 bed (matching encodeObjects/run_atmos_encode
    // - what a legacy decoder hears); channel mode meters the routed coded
    // channels. Getting this wrong doesn't just mislabel a column - the wrong
    // acmod also changes how many channels the meter reports.
    ac3::analysis::LevelMeter meter{bed_acmod, bed_lfe, rate_hz,
                                    static_cast<int>(coded_channels)};
    const std::uint64_t target_frames =
        (static_cast<std::uint64_t>(seconds) * rate_hz + ac3::kSamplesPerFrame - 1) /
        ac3::kSamplesPerFrame;

    RecordingSink sink;
    {
        const auto config =
            atmos ? RecordingSink::Config{.container = meta.container,
                                          .eac3 = true,
                                          .sample_rate = rate_hz,
                                          .channels = 6}
                  : take_sink_config(meta, *take, rate_hz);
        if (const auto why = sink.open(std::string{out_path}, config); !why.empty()) {
            fmt::println(stderr, "error: {}: {}", out_path, why);
            return kExitOutput;
        }
    }

    std::vector<float> interleaved(static_cast<std::size_t>(ac3::kSamplesPerFrame) * channels);
    // Object mode fills one block per SLOT; channel mode one per captured
    // channel, routed into `coded_block` below. Sized for whichever is
    // running, never both.
    std::vector<std::vector<float>> block(atmos ? nobjects : channels,
                                          std::vector<float>(ac3::kSamplesPerFrame, 0.0F));
    std::vector<std::vector<float>> coded_block(
        atmos ? 0 : coded_channels, std::vector<float>(ac3::kSamplesPerFrame, 0.0F));
    std::vector<std::span<const float>> views(atmos ? nobjects : channels);
    std::vector<std::span<float>> coded_out(atmos ? 0 : coded_channels);
    std::vector<std::span<const float>> coded_views(atmos ? 0 : coded_channels);
    for (std::size_t ch = 0; ch < block.size(); ++ch) {
        views[ch] = block[ch];
    }
    for (std::size_t ch = 0; ch < coded_block.size(); ++ch) {
        coded_out[ch] = coded_block[ch];
        coded_views[ch] = coded_block[ch];
    }
    // Separate from `views`/`coded_views`: the bed the receiver leg and the
    // meter read is a fixed `bed_channels` wide, which either of those can be
    // narrower than - reusing one for both risked (and in an earlier version
    // of this loop, did) an out-of-bounds write.
    std::vector<std::span<const float>> bed_views(bed_channels);
    std::vector<ac3::oba::ObjectPlacement> placement(nobjects);

    // Both capture devices are watched: a session that keeps running on a
    // vanished device reads as healthy with nothing coming in (see
    // ac3::audio::SilenceWatchdog, and run_record's own use of it). The slave
    // gets its own watchdog because its drain is non-blocking - it can be
    // legitimately empty on any given frame, just not for seconds on end.
    ac3::audio::SilenceWatchdog watchdog{meta.watchdog};
    ac3::audio::SilenceWatchdog slave_watchdog{meta.watchdog};
    const auto session_start = std::chrono::steady_clock::now();
    watchdog.reset(session_start);
    slave_watchdog.reset(session_start);
    const bool watching = meta.watchdog.count() > 0;
    bool device_lost = false;
    bool lost_is_slave = false;
    bool encode_failed = false;

    // Roadmap IO3's capture-side half, as it applies here. Unlike 'record',
    // a live session has nothing useful to do with a bitstream: it mixes,
    // resamples a second device into lockstep, meters, monitors and can pan
    // objects, none of which mean anything applied to burst data. So this
    // detects and stops rather than switching modes - the alternative is a
    // whole session's output that is noise, discovered at the end of it.
    // Costs nothing after the first quarter-second.
    ac3::iec61937::PassthroughDetector passthrough_probe;

    std::uint64_t n0 = 0;
    std::uint64_t frames_written = 0;
    for (std::uint64_t f = 0; f < target_frames; ++f) {
        std::size_t filled = 0;
        while (filled < interleaved.size()) {
            const auto got = capture.buffer()->read(
                std::span{interleaved}.subspan(filled, interleaved.size() - filled));
            filled += got;
            const auto read_at = std::chrono::steady_clock::now();
            watchdog.on_read(got, read_at);
            if (got == 0) {
                if (watching && watchdog.timed_out(read_at)) {
                    device_lost = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        if (device_lost) {
            break;
        }
        if (!passthrough_probe.decided()) {
            passthrough_probe.push(interleaved, static_cast<std::uint16_t>(channels));
            if (const auto type = passthrough_probe.detected()) {
                capture.stop();
                fmt::println("");
                fmt::println(stderr,
                             "error: \"{}\" is bitstreaming {} over IEC 61937, not delivering "
                             "PCM - a live encode of it would be noise",
                             device.name,
                             *type == ac3::iec61937::BurstDataType::kEac3 ? "Dolby Digital Plus"
                                                                         : "Dolby Digital");
                fmt::println(stderr,
                             "  'ac3cli record <out.ec3> <seconds> 0 {}' records the elementary "
                             "stream instead, and 'ac3cli unspdif' recovers one from a capture "
                             "already saved as a WAV.",
                             capture_device);
                return kExitInput;
            }
        }
        if (slave_resampler.has_value() && slave_drift.has_value()) {
            // Opportunistic, non-blocking drain: whatever capture2 has ready
            // right now joins the scratch FIFO's tail. Unlike the master's
            // read above, this never waits - a slave that is momentarily
            // behind just leaves the resampler's next render() with less to
            // work from, which is exactly the drift the estimator is
            // steering against, not a stall to block the session on.
            //
            // Guarded on slave_resampler/slave_drift's own has_value() rather
            // than device2 (always in lockstep with it by construction, both
            // populated together right after capture2 opens) so clang-tidy's
            // bugprone-unchecked-optional-access can actually see the
            // invariant instead of having to trust a same-lockstep but
            // type-unrelated raw pointer.
            const std::size_t capacity_frames = slave_scratch.size() / capture2_channels;
            const std::size_t free_frames = capacity_frames - slave_scratch_valid_frames;
            if (free_frames > 0) {
                const auto got = capture2.buffer()->read(std::span{slave_scratch}.subspan(
                    slave_scratch_valid_frames * capture2_channels,
                    free_frames * capture2_channels));
                const auto read_at = std::chrono::steady_clock::now();
                slave_watchdog.on_read(got, read_at);
                slave_scratch_valid_frames += got / capture2_channels;
                if (watching && got == 0 && slave_watchdog.timed_out(read_at)) {
                    device_lost = true;
                    lost_is_slave = true;
                    break;
                }
            }
            slave_drift->update(slave_scratch_valid_frames);
            slave_resampler->set_ratio(slave_drift->ratio());
            const auto consumed = slave_resampler->render(
                std::span{slave_scratch}.first(slave_scratch_valid_frames * capture2_channels),
                slave_scratch_valid_frames, std::span{slave_out},
                static_cast<std::size_t>(ac3::kSamplesPerFrame));
            const std::size_t remaining_frames = slave_scratch_valid_frames - consumed;
            std::copy(slave_scratch.begin() + static_cast<std::ptrdiff_t>(
                                                   consumed * capture2_channels),
                     slave_scratch.begin() + static_cast<std::ptrdiff_t>(
                                                  slave_scratch_valid_frames * capture2_channels),
                     slave_scratch.begin());
            slave_scratch_valid_frames = remaining_frames;
        }
        // A tap addresses the COMBINED capture space: 0..channels-1 is the
        // master (interleaved), channels..combined-1 is the slave
        // (slave_out, index shifted back down by `channels`) - devices are
        // sources concatenated after one another, the same space the GUI's
        // own slot binding addresses.
        const auto tap_sample = [&](std::size_t flat, int i) {
            const auto sample = static_cast<std::size_t>(i);
            if (flat < channels) {
                return interleaved[sample * channels + flat];
            }
            const std::size_t local = flat - channels;
            if (local < capture2_channels) {
                return slave_out[sample * capture2_channels + local];
            }
            return 0.0F;
        };
        if (atmos) {
            for (std::size_t slot = 0; slot < nobjects; ++slot) {
                for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                    float sum = 0.0F;
                    for (const auto& [flat, gain] : slots[slot].taps) {
                        sum += tap_sample(flat, i) * static_cast<float>(gain);
                    }
                    block[slot][static_cast<std::size_t>(i)] = sum;
                }
            }
        } else {
            for (std::size_t ch = 0; ch < channels; ++ch) {
                for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                    block[ch][static_cast<std::size_t>(i)] = tap_sample(ch, i);
                }
            }
        }
        n0 += ac3::kSamplesPerFrame;

        std::vector<std::byte> unit_bytes;
        if (atmos) {
            // Objects orbit at their own rate and start spread around the
            // ring, matching run_atmos exactly - the position is recomputed
            // from elapsed time every frame rather than fixed once, which
            // used to be described as "the hook a real live position source
            // drops into once one exists" (see live_audio.hpp's own header):
            // positions= is that source now, sampled through the same
            // SceneCursor seam ac3::oba::SceneCursor was built for (roadmap
            // UX4), at the frame-end time `t` either path already needs.
            const double t = static_cast<double>(n0) / static_cast<double>(rate_hz);
            if (position_source) {
                position_source->drain_into(*position_cursor, t);
                position_cursor->sample_into(t, placement);
            } else {
                for (std::size_t i = 0; i < nobjects; ++i) {
                    const double rate =
                        1.0 / (6.0 * (1.0 + 0.31 * static_cast<double>(i)));
                    const double phase =
                        2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(nobjects);
                    const double angle = 2.0 * std::numbers::pi * rate * t + phase;
                    const double height =
                        nobjects == 1 ? 0.5
                                     : -1.0 + 2.0 * static_cast<double>(i) /
                                                  static_cast<double>(nobjects - 1);
                    placement[i] = {.position = {.x = 0.5 + 0.5 * std::sin(angle),
                                                 .y = 0.5 - 0.5 * std::cos(angle),
                                                 .z = height},
                                    .gain = 0.7 / std::sqrt(static_cast<double>(nobjects)),
                                    .lfe_send = i == 0 ? 0.2 : 0.0};
                }
            }
            const auto unit = atmos_encoder->encode_frame(views, placement);
            if (!unit) {
                fmt::println(stderr, "error: cannot encode {} objects at {} kbps",
                             nobjects, bitrate);
                encode_failed = true;
                break;
            }
            for (std::size_t ch = 0; ch < bed_channels; ++ch) {
                bed_views[ch] = std::span{atmos_encoder->bed()[ch]};
            }
            meter.process(bed_views);
            unit_bytes = unit->bytes;
        } else {
            plan::render(*routing, views, coded_out, ac3::kSamplesPerFrame);
            meter.process(coded_views);
            // The independent substream's channels come first in coded order
            // (plan::coded_channels' own contract), so the bed the receiver
            // leg wants is the front of what was just routed.
            for (std::size_t ch = 0; ch < bed_channels; ++ch) {
                bed_views[ch] = coded_views[ch];
            }
            if (take->eac3) {
                const auto unit = eac3_encoder->encode_access_unit(coded_views);
                if (!unit) {
                    fmt::println(stderr, "error: the encoder cannot express this configuration");
                    encode_failed = true;
                    break;
                }
                unit_bytes = unit->bytes;
            } else {
                auto frame = ac3_encoder->encode_frame(coded_views);
                if (!frame) {
                    fmt::println(stderr, "error: bitrate must be a legal AC-3 rate");
                    encode_failed = true;
                    break;
                }
                unit_bytes = std::move(*frame);
            }
        }

        if (monitoring) {
            std::optional<std::vector<float>> to_play;
            if (eac3) {
                const auto decoded = eac3_monitor_decoder->decode_access_unit(unit_bytes);
                // 3.7: decoded->has_value() is false exactly when this
                // access unit is being held back pending transient
                // pre-noise processing (decode_access_unit's own doc
                // comment) - live monitoring just waits for the next one.
                if (decoded && decoded->has_value()) {
                    const auto order =
                        plan::monitor_order(std::span{(*decoded)->layout.items}.first(
                                                static_cast<std::size_t>((*decoded)->layout.count)),
                                            (*decoded)->channels.size());
                    to_play = interleave_reordered((*decoded)->channels, order);
                }
            } else {
                const auto decoded = ac3_monitor_decoder->decode_frame(unit_bytes);
                if (decoded) {
                    const auto order = ac3::io::wav_channel_order(decoded->acmod, decoded->lfe);
                    to_play = interleave_reordered(decoded->channels, order);
                }
            }
            if (to_play) {
                while (!monitor_sink.submit(*to_play)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(4));
                }
            }
        }

        if (passing_through) {
            std::optional<std::vector<std::byte>> burst;
            if (downmix_leg) {
                // The capped receiver leg: an independent AC-3 encode of the
                // bed the main plan has already computed. The main encode
                // above (unit_bytes) is untouched and still reaches the
                // meters, the monitor and the file exactly as it always has.
                const auto leg_frame = downmix_encoder->encode_frame(bed_views);
                if (leg_frame) {
                    if (const auto wrapped = ac3::iec61937::wrap_frame(*leg_frame)) {
                        burst = *wrapped;
                    }
                }
            } else if (eac3) {
                auto packed = eac3_packer.push(unit_bytes);
                if (packed && *packed) {
                    burst = std::move(**packed);
                }
            } else {
                if (const auto wrapped = ac3::iec61937::wrap_frame(unit_bytes)) {
                    burst = *wrapped;
                }
            }
            if (burst) {
                while (!passthrough_sink.submit(*burst)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(4));
                }
            }
        }

        if (const auto why = sink.push(unit_bytes); !why.empty()) {
            fmt::println(stderr, "error: {}: {}", out_path, why);
            std::ignore = sink.close();
            return kExitOutput;
        }
        ++frames_written;
        print_live_meter(meter, static_cast<double>(frames_written * ac3::kSamplesPerFrame) /
                                    rate_hz);
    }
    status_println(status);

    capture.stop();
    if (device2) {
        capture2.stop();
    }
    if (position_source) {
        position_source->stop();
    }
    if (monitoring) {
        while (monitor_sink.stats().frames_rendered < monitor_sink.stats().frames_submitted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        monitor_sink.stop();
    }
    if (passing_through) {
        while (passthrough_sink.stats().bursts_rendered <
               passthrough_sink.stats().bursts_submitted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto pstats = passthrough_sink.stats();
        passthrough_sink.stop();
        status_println(status, "passthrough: {} bursts submitted, {} rendered, {} underruns",
                       pstats.bursts_submitted, pstats.bursts_rendered, pstats.underruns);
    }
    const auto stats = capture.stats();
    // Finalized whether or not the session ended early: every unit already
    // pushed is on disk and playable, which is the whole reason a take
    // streams rather than accumulating (roadmap IO9). A close() complaint is
    // reported either way, but a lost device is the more useful diagnosis of
    // the two and wins the exit code - a session that captured nothing before
    // the device vanished ends as a device failure, not as a disk one.
    const auto close_problem = sink.close();
    if (!close_problem.empty() && !device_lost) {
        fmt::println(stderr, "error: {}: {}", out_path, close_problem);
        return kExitOutput;
    }
    if (device_lost) {
        fmt::println(stderr,
                     "error: \"{}\" stopped delivering audio for {} ms; the session was stopped "
                     "and what had already been written is kept (watchdog=0 disables this){}",
                     lost_is_slave && device2 != nullptr ? device2->name : device.name,
                     meta.watchdog.count(),
                     close_problem.empty() ? "" : " - " + close_problem);
        return kExitRuntime;
    }
    status_println(status, "wrote {} {} ({} kbps, {}) to {}{}", frames_written,
                   eac3 ? "E-AC-3 access units" : "AC-3 frames", bitrate,
                   atmos ? std::string{"5.1 bed + objects"} : take->label, out_path,
                   container_note(meta.container));
    if (atmos) {
        std::size_t bound = 0;
        for (const auto& slot : slots) {
            bound += slot.taps.empty() ? std::size_t{0} : std::size_t{1};
        }
        status_println(status,
                       "  {} object slots, {} bound to captured channels, {} carried silent",
                       nobjects, bound, nobjects - bound);
    }
    if (position_source) {
        const auto position_stats = position_source->stats();
        status_println(status, "positions: {} datagrams, {} updates applied, {} dropped",
                       position_stats.datagrams, position_stats.updates_applied,
                       position_stats.packets_rejected + position_stats.messages_dropped);
    }
    status_println(status, "captured {} frames, {} silence-filled, {} dropped",
                   stats.frames_captured, stats.frames_silence_filled, stats.frames_dropped);
    if (slave_drift.has_value()) {
        status_println(status, "capture2 drift: {:+.1f} ppm", slave_drift->drift_ppm());
    }
    print_channel_summary(meter, status);
    return encode_failed ? kExitUsage : kExitOk;
}

}  // namespace ac3cli::commands

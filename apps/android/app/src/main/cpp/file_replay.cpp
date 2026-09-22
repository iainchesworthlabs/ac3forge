// A diagnostic-only playback path, separate from live_cursor.cpp's synthesized
// encode loop: streams a real, already-encoded AC-3/E-AC-3 file (e.g. an
// audio track pulled straight out of a commercial Dolby Atmos demo trailer's
// MKV, no re-encoding at all) through the exact same PassthroughSink this
// app's own encoder output goes through. Mirrors ac3cli's run_play
// (apps/cli/main.cpp) almost exactly - same split_access_units/BurstPacker/
// submit-with-retry shape - just triggered over JNI instead of a CLI arg.
//
// Purpose: isolate "is this app's AudioTrack passthrough configuration
// correct" from "is our own encoder's Atmos/JOC output correct" - a real,
// known-good Dolby-encoded Atmos stream either lights up the receiver's Atmos
// indicator through this exact same code path, or it doesn't, independent of
// anything ac3::forge or the quarantine signer did.

#include <jni.h>

#include <android/log.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "ac3/decoder/decoder.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/iec61937/iec61937.hpp"
#include "ac3/audio/passthrough.hpp"

namespace {

constexpr char kLogTag[] = "ac3forge.shield.file_replay";

// Set by nativeStopFileReplay, cleared when play_file returns. Both wait
// loops below used to be `while (!condition) sleep();` with no exit but
// success: a receiver switched off, switched to another input, or simply
// never draining leaves this thread spinning at 4ms forever, holding the sink
// open, with no way to reach it - the Activity's own teardown could not stop
// it and neither could anything else. live_cursor.cpp's equivalent loop has
// always checked its stop flag; this one had none to check.
std::atomic<bool> g_replay_stop{false};

// Independently of an explicit stop, a wait that makes no progress at all for
// this long is a stall, not slowness: at 32ms of audio per burst, a healthy
// sink accepts or renders something several times a second. Long enough to
// ride out an AVR's own re-lock after an input switch (typically well under a
// second), short enough that a dead receiver ends the replay rather than
// pinning a thread until the process dies.
constexpr auto kNoProgressTimeout = std::chrono::seconds(5);

// ac3::split_access_units groups syncframes by reading strmtyp from each
// frame's own header - correct for a stream whose independent substream uses
// bsid 16 (every generation this project's own encoder and, apparently, most
// test material produces), but strmtyp only lives at that byte position in a
// genuine Annex E (bsid 11-16) frame. A real commercial disc encountered here
// authors its independent substream at bsid 6 (spec-legal - see below) with
// its Atmos-carrying dependent substream right behind it at bsid 16; for the
// bsid-6 frame, the byte split_access_units reads as strmtyp is really part
// of that frame's crc1 - essentially random per-frame content - so roughly a
// quarter of the time it *happens* to alias to "dependent" and swallows the
// next several access units into one runaway group (confirmed against this
// exact file: 176 of 480 groups corrupted this way, the largest merging 24
// frames into one 122880-byte non-burst). bsid itself is not random - it is
// the frame's own declared identity, in the same fixed position for exactly
// this reason (A/52 Annex E: "bsid sits at bit 40 in both generations") - so
// grouping on "does this frame's bsid fall in the E-AC-3 dependent range"
// instead is deterministic and, empirically, correct throughout this file.
// Kept local to this diagnostic rather than changed in the shared decoder:
// ac3::split_access_units is exercised well beyond this one file/tool, and
// this project's own encoder never emits a non-16 leading bsid, so nothing
// here should risk a broader, less-tested change to that shared code path.
std::expected<std::vector<std::span<const std::byte>>, ac3::DecodeError> group_by_bsid(
    std::span<const std::byte> stream) {
    const auto frames = ac3::split_frames(stream);
    if (!frames) return std::unexpected(frames.error());

    std::vector<std::span<const std::byte>> units;
    std::size_t start = 0;
    std::size_t offset = 0;
    for (const auto& frame : *frames) {
        const auto bsid = std::to_integer<int>(frame[5]) >> 3;
        const bool begins_unit = !(bsid >= ac3::eac3::kMinDecodableBsid && bsid <= ac3::eac3::kBsid);
        if (begins_unit && offset != start) {
            units.push_back(stream.subspan(start, offset - start));
            start = offset;
        }
        offset += frame.size();
    }
    if (offset != start) {
        units.push_back(stream.subspan(start, offset - start));
    }
    return units;
}

std::vector<std::byte> read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const auto size = in.tellg();
    if (size <= 0) return {};
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), size);
    // A short read left the untouched tail as zero-initialized garbage that
    // still looks like a full-size buffer to every caller - silently
    // returning it produced a bitstream that was byte-correct up to the
    // short point and corrupt after, which read as a parse failure deep in
    // the file rather than the read failure it actually was.
    if (in.gcount() != size) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "%s: short read (%lld of %lld bytes)", path.c_str(),
                            static_cast<long long>(in.gcount()), static_cast<long long>(size));
        return {};
    }
    return data;
}

bool play_file(const std::string& path) {
    const auto stream = read_all(path);
    if (stream.empty()) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "cannot read %s", path.c_str());
        return false;
    }

    // Always E-AC-3, unlike ac3cli's run_play (which also handles plain
    // .ac3): this diagnostic exists specifically to play real commercial
    // Dolby Atmos/DD+ content (see file header). group_by_bsid, not
    // ac3::split_access_units - see that function's own comment for why.
    const auto split = group_by_bsid(stream);
    if (!split) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s: frame split failed: %s", path.c_str(),
                            std::string(ac3::describe(split.error())).c_str());
        return false;
    }
    if (split->empty()) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s: no access units found", path.c_str());
        return false;
    }
    const auto& units = *split;
    const std::uint32_t content_rate = ac3::sample_rate_hz(
        static_cast<ac3::SampleRate>(std::to_integer<std::uint32_t>(units[0][4]) >> 6));

    ac3::audio::PassthroughSink sink;
    const auto started = sink.start("", content_rate, ac3::audio::BitstreamFormat::kEac3);
    if (!started) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "PassthroughSink::start failed: %s",
                            std::string(ac3::audio::describe(started.error())).c_str());
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "streaming %zu access units from %s (%u Hz, carrier 4x that)",
                        units.size(), path.c_str(), content_rate);

    ac3::iec61937::Eac3BurstPacker eac3_packer;
    for (const auto& unit : units) {
        auto result = eac3_packer.push(unit);
        if (!result) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "burst wrap failed");
            return false;
        }
        if (!*result) continue;  // accumulating; nothing to submit yet
        const auto& burst = **result;
        auto blocked_since = std::chrono::steady_clock::now();
        while (!sink.submit(burst)) {
            if (g_replay_stop.load(std::memory_order_relaxed)) {
                __android_log_print(ANDROID_LOG_INFO, kLogTag, "stop requested - ending replay");
                sink.stop();
                return false;
            }
            if (std::chrono::steady_clock::now() - blocked_since > kNoProgressTimeout) {
                __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                                    "sink accepted nothing for %llds - giving up",
                                    static_cast<long long>(kNoProgressTimeout.count()));
                sink.stop();
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }
    // Same treatment for the drain: a sink that stops rendering mid-drain
    // would otherwise hold this thread here forever, after every burst has
    // already been submitted.
    auto last_progress = std::chrono::steady_clock::now();
    auto last_rendered = sink.stats().bursts_rendered;
    while (sink.stats().bursts_rendered < sink.stats().bursts_submitted) {
        if (g_replay_stop.load(std::memory_order_relaxed)) {
            __android_log_print(ANDROID_LOG_INFO, kLogTag, "stop requested during drain");
            break;
        }
        const auto rendered = sink.stats().bursts_rendered;
        if (rendered != last_rendered) {
            last_rendered = rendered;
            last_progress = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - last_progress > kNoProgressTimeout) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                                "drain stalled at %llu/%llu bursts - giving up",
                                static_cast<unsigned long long>(rendered),
                                static_cast<unsigned long long>(sink.stats().bursts_submitted));
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto stats = sink.stats();
    sink.stop();
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "done: submitted=%llu rendered=%llu underruns=%llu",
                        static_cast<unsigned long long>(stats.bursts_submitted),
                        static_cast<unsigned long long>(stats.bursts_rendered),
                        static_cast<unsigned long long>(stats.underruns));
    return true;
}

}  // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ac3forge_shield_NativeBridge_nativePlayEac3File(JNIEnv* env, jclass /*clazz*/,
                                                          jstring jpath) {
    const char* raw = env->GetStringUTFChars(jpath, nullptr);
    const std::string path(raw != nullptr ? raw : "");
    if (raw != nullptr) env->ReleaseStringUTFChars(jpath, raw);
    g_replay_stop.store(false, std::memory_order_relaxed);
    const bool ok = play_file(path);
    g_replay_stop.store(false, std::memory_order_relaxed);
    return ok ? JNI_TRUE : JNI_FALSE;
}

// Asks a replay in progress to end at its next wait point. Safe to call when
// nothing is playing (the flag is reset at the start of every replay) and
// safe to call from the main thread - it never blocks on the replay thread,
// which is the whole reason this is a flag rather than a join.
extern "C" JNIEXPORT void JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeStopFileReplay(JNIEnv* /*env*/, jclass /*clazz*/) {
    g_replay_stop.store(true, std::memory_order_relaxed);
}

// Decode AC-3 or E-AC-3 from wherever the bytes are and play it - the wiring
// around ac3forge::Player, which is where the work happens.
//
// The difference between this and the i2s_player example beside it is where the
// audio comes from, and that difference is the entire point. That one decodes a
// bitstream linked into its own image, which is fine for showing the codec
// works and is not how anything real gets its audio. This one never has the
// whole stream in memory: a fetch task reads it in blocks from wherever it is,
// a ring buffer holds what has arrived, and a decode task on the other core
// frames it with ac3::io::AccessUnitAccumulator, decodes whatever complete
// access units come out, and writes them to the sink.
//
// Both tasks, the ring and the decoders are the component's
// (esp-idf/ac3forge/include/ac3forge/player.hpp); what is left here is what an
// integrator's own firmware would have to write too. BOTH ENDS ARE SEAMS, and
// CMake resolves both - see byte_source.hpp and audio_sink.hpp. This file
// mentions neither a partition nor I2S.
//
//   source/partition/  flash. The default, and the first one CI runs.
//   source/fatfs/      a FAT volume in flash; the SD source's file layer, runnable.
//   source/sd/         an SD card over SDMMC.
//   source/http/       an HTTP body, over WiFi or QEMU's Ethernet.
//
//   sink/i2s/          a stereo DAC.
//   sink/tdm/          multi-channel on one data line.
//   sink/capture/      converts and checks; what CI runs.
//   sink/null/         counts frames.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ac3/core/tables.hpp"
#include "ac3forge/player.hpp"

#include "audio_sink.hpp"
#include "byte_source.hpp"

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint64_t kFrameDurationUs = 32000;  // §5.3.2: 1,536 samples at 48 kHz
constexpr std::size_t kOutputChannels = 2;

// From Kconfig, ints so they arrive as plain constants rather than through
// preprocessor conditionals - see main/Kconfig.projbuild. kMaxLaps of 0 plays
// until the source cannot rewind; CI sets a small number so the run ends with
// a verdict. kReportEveryFrames of 0 reports only at the end of a pass.
constexpr std::uint32_t kMaxLaps = CONFIG_AC3FORGE_EXAMPLE_MAX_LAPS;
constexpr std::uint64_t kReportEveryFrames = CONFIG_AC3FORGE_EXAMPLE_REPORT_EVERY_FRAMES;

BaseType_t core_from_kconfig(int value) { return value < 0 ? tskNO_AFFINITY : value; }

// The source seam as the player's ByteSource. The seam's functions are what
// CMake resolved to a directory; this is the adapter, and it is all of it.
class SeamSource final : public ac3forge::ByteSource {
   public:
    std::size_t read(std::span<std::byte> dst) override { return player::source_read(dst); }
    bool rewind() override { return player::source_rewind(); }
};

// The sink seam as the player's PcmSink, with a level meter in front of it.
//
// The meter is what turns result=pass from "some units decoded without
// returning an error" - which a stream decoding to silence satisfies - into an
// end-to-end check: the RMS of what was actually sent, per channel, scaled by
// 1e6 the way apps/baremetal/probe.cpp reports levels. The player reports it
// and does not judge it; what the levels should be is a property of the stream,
// so CI holds the expectation. Written from the decode task, read from app_main
// after the run has ended.
class MeteredSink final : public ac3forge::PcmSink {
   public:
    void write(std::span<const std::span<const float>> channels) override {
        for (std::size_t ch = 0; ch < channels.size() && ch < kOutputChannels; ++ch) {
            for (const float sample : channels[ch]) {
                sum_squares_[ch] += static_cast<double>(sample) * static_cast<double>(sample);
            }
        }
        samples_ += channels.empty() ? 0 : channels[0].size();
        player::sink_write(channels);
    }

    void report() const {
        for (std::size_t ch = 0; ch < kOutputChannels; ++ch) {
            const double rms = samples_ == 0 ? 0.0
                                             : std::sqrt(sum_squares_[ch] /
                                                         static_cast<double>(samples_));
            std::printf("stream.rms[%u]=%ld\n", static_cast<unsigned>(ch),
                        static_cast<long>((rms * 1e6) + 0.5));
        }
    }

   private:
    std::array<double, kOutputChannels> sum_squares_{};
    std::size_t samples_ = 0;
};

// realtime_permille is decode time against the audio time it produced: 1000 is
// exactly real time and anything at or above it cannot play without gaps. The
// worst SINGLE frame matters as much as the average, because the sink's queue
// only absorbs a spike that small - it says how deep. ring_low is the least the
// ring ever held when the decoder came for more: zero means the decoder waited
// on the source at least once, and how far above zero it stays is the margin
// the ring's depth is buying.
// ring_low prints as "-" until the player has measured it: a stream shorter
// than the ring, or one that has just begun, has nothing to say about buffering,
// and a zero there would read as a stall.
void print_ring_low(const ac3forge::PlayerStats& s) {
    if (s.ring_low_valid) {
        std::printf("%lu", static_cast<unsigned long>(s.ring_low_water));
    } else {
        std::printf("-");
    }
}

void report_timing(const char* label, unsigned long value, const ac3forge::PlayerStats& s) {
    const std::uint64_t permille =
        s.frames_played > 0 ? (s.decode_us * 1000) / (kFrameDurationUs * s.frames_played) : 0;
    std::printf("%s=%lu frames=%lu us_per_frame=%lu worst_frame_us=%lu realtime_permille=%lu "
                "resync=%lu ring_low=",
                label, value, static_cast<unsigned long>(s.frames_played),
                static_cast<unsigned long>(s.frames_played > 0 ? s.decode_us / s.frames_played : 0),
                static_cast<unsigned long>(s.worst_frame_us), static_cast<unsigned long>(permille),
                static_cast<unsigned long>(s.resync_bytes));
    print_ring_low(s);
    std::printf(" heap_free=%lu\n",
                static_cast<unsigned long>(
                    heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

void describe(const ac3forge::StreamInfo& info) {
    std::printf("stream: %s acmod=%d channels=%d substreams=%d dialnorm=-%d objects=%s, folded to "
                "%u\n",
                info.eac3 ? "E-AC-3" : "AC-3", info.acmod, info.channels, info.substreams,
                info.dialnorm, info.objects ? "yes" : "no",
                static_cast<unsigned>(kOutputChannels));
}

}  // namespace

extern "C" void app_main() {
    std::printf("ac3forge stream_player: AC-3 or E-AC-3, folded to stereo\n");

    if (!player::source_open()) {
        std::printf("result=fail\n");
        return;
    }
    // kOutputChannels, not the coded count: the decoder folds to stereo before
    // it returns (PlayerConfig's default decoder settings). A player that wanted
    // 5.1 out would ask for kAsCoded and open the sink with six.
    if (!player::sink_open(kSampleRate, static_cast<int>(kOutputChannels))) {
        return;
    }

    SeamSource source;
    MeteredSink sink;
    ac3forge::PlayerConfig config;
    config.output_channels = kOutputChannels;
    config.ring_bytes = CONFIG_AC3FORGE_EXAMPLE_RING_BYTES;
    config.ring_in_psram = CONFIG_AC3FORGE_EXAMPLE_RING_IN_PSRAM != 0;
    config.fetch_core = core_from_kconfig(CONFIG_AC3FORGE_EXAMPLE_FETCH_CORE);
    config.decode_core = core_from_kconfig(CONFIG_AC3FORGE_EXAMPLE_DECODE_CORE);
    config.max_passes = kMaxLaps;

    ac3forge::Player player{config, source, sink};
    if (!player.start()) {
        std::printf("result=fail\n");
        return;
    }

    // Everything from here is reporting. The player runs on its own two tasks;
    // this task wakes ten times a second to say what they did.
    bool described = false;
    std::int64_t started_us = 0;
    std::uint32_t passes_seen = 0;
    std::uint64_t next_report = kReportEveryFrames;
    unsigned long reports = 0;
    for (;;) {
        const bool done = player.wait(pdMS_TO_TICKS(100));
        const auto stats = player.stats();
        if (!described) {
            if (const auto info = player.stream()) {
                started_us = esp_timer_get_time();
                describe(*info);
                described = true;
            }
        }
        // The pass's own figures, taken by the decode task as the pass ended,
        // not this task's later view of them. If more than one pass completed
        // since the last wake - which only a sink with no pacing manages - the
        // earlier ones carry the latest snapshot.
        while (passes_seen < stats.passes) {
            ++passes_seen;
            report_timing("lap", passes_seen, player.last_pass());
        }
        if (kReportEveryFrames != 0 && stats.frames_played >= next_report) {
            ++reports;
            report_timing("progress", reports, stats);
            player::sink_report();
            next_report += kReportEveryFrames;
        }
        if (done) {
            break;
        }
    }

    // The verdict. stream.audio_ms against stream.wall_ms is the
    // whole-pipeline real-time check: a player that kept up spent as long
    // playing as the audio lasted, one that stalled spent longer by exactly
    // the silence it inserted, and a sink with no peripheral runs ahead of the
    // clock. The sink's own line says where.
    const auto stats = player.stats();
    if (stats.failed) {
        std::printf("error: %s failed (%d)\n", stats.failure, stats.error);
    } else {
        std::printf("stream: %s ended (%s)\n", player::source_name(), stats.failure);
    }
    sink.report();
    player::sink_report();
    const std::int64_t wall_us = started_us == 0 ? 0 : esp_timer_get_time() - started_us;
    std::printf("stream.units=%lu stream.held=%lu stream.resync_bytes=%lu stream.sink=%s "
                "stream.sink_frames=%lu stream.source=%s stream.fetched=%lu stream.ring_low=",
                static_cast<unsigned long>(stats.frames_played),
                static_cast<unsigned long>(stats.frames_held),
                static_cast<unsigned long>(stats.resync_bytes), player::sink_name(),
                static_cast<unsigned long>(player::sink_frames_written()), player::source_name(),
                static_cast<unsigned long>(stats.fetched_bytes));
    print_ring_low(stats);
    std::printf(" stream.audio_ms=%lu stream.wall_ms=%lu\n",
                static_cast<unsigned long>((stats.frames_played * kFrameDurationUs) / 1000),
                static_cast<unsigned long>(wall_us / 1000));
    std::printf("result=%s\n", (stats.frames_played > 0 && !stats.failed) ? "pass" : "fail");
    player.stop();
    vTaskDelay(pdMS_TO_TICKS(200));
}

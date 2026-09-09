// Decode AC-3 out of a flash partition and play it, a block at a time.
//
// The difference between this and the i2s_player example beside it is where the
// audio comes from, and that difference is the entire point. That one decodes a
// bitstream linked into its own image, which is fine for showing the codec
// works and is not how anything real gets its audio. This one never has the
// whole stream in memory: it reads 2 KB at a time out of a partition, hands
// those bytes to ac3::io::AccessUnitAccumulator, and decodes whatever complete
// access units come back.
//
// WHY THAT NEEDS A CLASS AT ALL. ac3::split_frames and ac3::split_access_units
// take a span over the entire stream. Nothing streaming can produce one: an SD
// card, an HTTP body and this partition all arrive in pieces, and on a part with
// 280 KB of RAM a whole file is not going to be resident anyway. The accumulator
// is the same boundary rule applied incrementally, over a buffer this file owns
// - so the decode allocates nothing for framing.
//
// BOTH ENDS ARE SEAMS, and CMake resolves both - see byte_source.hpp and
// audio_sink.hpp. This file mentions neither a partition nor I2S: it reads
// bytes, frames them, decodes them and writes audio, and every question about
// WHERE is answered somewhere else.
//
//   source/partition/  flash. The default, and the only one CI can run.
//   source/sd/         an SD card over SDMMC.
//   source/http/       an HTTP body over WiFi.
//
//   sink/i2s/          a stereo DAC.
//   sink/tdm/          multi-channel on one data line.
//   sink/null/         counts frames; what CI builds.

#include <algorithm>
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

#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/io/stream_accumulator.hpp"

#include "audio_sink.hpp"
#include "byte_source.hpp"

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint64_t kFrameDurationUs = 32000;  // §5.3.2: 1,536 samples at 48 kHz
constexpr std::size_t kOutputChannels = 2;

// The accumulator's working buffer. kRecommendedBuffer is 16 KB, which holds an
// independent substream plus three dependents; a plain AC-3 stream would fit in
// kMinimumBuffer, but an example should show the size that copes with a stream
// whose shape is not known in advance.
//
// How much is read into it per call is the SOURCE's business - see
// byte_source.hpp. It is deliberately less than the buffer everywhere, so the
// accumulator's "need more input" path runs on a real device and not only in
// its unit tests: a read size that always happened to contain a whole frame
// would hide every framing bug there is.
alignas(4) std::array<std::byte, ac3::io::kRecommendedBuffer> g_stream_buffer{};

// From Kconfig, an int so it arrives as a plain constant rather than through a
// preprocessor conditional - see main/Kconfig.projbuild. 0 plays forever, which
// is what a demo on a board should do; CI sets a small number so the run ends
// with a verdict.
constexpr std::uint32_t kMaxLaps = CONFIG_AC3FORGE_EXAMPLE_MAX_LAPS;

// Caller-owned PCM, the shape an embedded integrator has: one block, sized
// once, reused every frame. Six channels because the fixture is 5.1 - the fold
// to stereo happens IN this storage, so it has to be wide enough for the coded
// programme going in, not for the two coming out.
constexpr std::size_t kCodedChannels = 6;
std::array<std::array<float, ac3::kSamplesPerFrame>, kCodedChannels> g_pcm{};
std::array<std::span<float>, kCodedChannels> g_pcm_spans{};
// The same storage again as const spans, which is what the sink takes. Built
// once per frame rather than held, because the count depends on what the
// decoder folded to.
std::array<std::span<const float>, kCodedChannels> g_out_views{};

// --- what actually came out --------------------------------------------------
// Sum of squares per output channel, over the whole run.
//
// The player REPORTS the level and does not judge it: what the levels should be
// is a property of the stream, and a player carrying expectations for one
// particular file would be a fixture wearing an example's clothes. CI holds the
// expectation, and records where the reference values came from.
//
// Worth having because of what the verdict used to mean: "some units decoded
// without returning an error". A stream that decoded to silence, or to
// full-scale noise, satisfied that completely.
//
// Scaled by 1e6 and rounded - the same form apps/baremetal/probe.cpp reports
// its own levels in, so the two read the same way.
std::array<double, kCodedChannels> g_sum_squares{};
std::size_t g_sample_count = 0;

void accumulate_levels(int channels) {
    for (int ch = 0; ch < channels; ++ch) {
        const auto uch = static_cast<std::size_t>(ch);
        for (const float sample : g_pcm[uch]) {
            g_sum_squares[uch] += static_cast<double>(sample) * static_cast<double>(sample);
        }
    }
    g_sample_count += ac3::kSamplesPerFrame;
}

std::int32_t rms_scaled(std::size_t channel) {
    if (g_sample_count == 0) {
        return 0;
    }
    const double rms = std::sqrt(g_sum_squares[channel] / static_cast<double>(g_sample_count));
    return static_cast<std::int32_t>((rms * 1e6) + 0.5);
}

void report_levels() {
    for (std::size_t ch = 0; ch < kOutputChannels; ++ch) {
        std::printf("stream.rms[%u]=%ld\n", static_cast<unsigned>(ch),
                    static_cast<long>(rms_scaled(ch)));
    }
}

void play_frame(int channels) {
    for (int ch = 0; ch < channels; ++ch) {
        g_out_views[static_cast<std::size_t>(ch)] = g_pcm[static_cast<std::size_t>(ch)];
    }
    player::sink_write(std::span<const std::span<const float>>{
        g_out_views.data(), static_cast<std::size_t>(channels)});
}

}  // namespace

extern "C" void app_main() {
    std::printf("ac3forge stream_player: AC-3 from a flash partition, folded to stereo\n");

    for (std::size_t ch = 0; ch < kCodedChannels; ++ch) {
        g_pcm_spans[ch] = std::span<float>(g_pcm[ch]);
    }

    if (!player::source_open()) {
        std::printf("result=fail\n");
        return;
    }

    // kOutputChannels, not the coded count: the decoder folds to stereo before
    // it returns (see the OutputConfig below). A player that wanted 5.1 out
    // would ask for kAsCoded and open the sink with six.
    if (!player::sink_open(kSampleRate, static_cast<int>(kOutputChannels))) {
        return;
    }

    // kLine is §7.7.1's operating mode - dialnorm normalisation plus the full
    // transmitted range - which is what a decoder feeding an amplifier does.
    // Without it, playback level follows whatever the encoder's dialnorm was and
    // two streams play back at two different loudnesses.
    ac3::FrameDecoder decoder{{.output = {.target = ac3::DownmixTarget::kLoRo,
                                          .mode = ac3::OperatingMode::kLine}}};
    ac3::io::AccessUnitAccumulator accumulator{g_stream_buffer};

    std::uint64_t decode_us = 0;
    std::uint64_t worst_frame_us = 0;
    std::uint64_t played = 0;
    std::uint32_t laps = 0;

    for (;;) {
        const auto unit = accumulator.next();

        if (unit.status == ac3::io::AccessUnitAccumulator::Status::kNeedMoreInput) {
            const auto got = player::source_read(accumulator.writable());
            if (got == 0) {
                // Not "wait" - there will never be more. finish() is what closes
                // the last access unit, whose end is otherwise only found by
                // reading the start of a successor that is not coming.
                accumulator.finish();
            } else {
                accumulator.commit(got);
            }
            continue;
        }

        if (unit.status == ac3::io::AccessUnitAccumulator::Status::kEndOfStream) {
            // Loop. A real player would stop, or fetch the next track; this
            // rewinds so the example keeps making a noise, and re-arms the
            // accumulator because finish() is a one-way switch.
            ++laps;
            const std::uint64_t permille =
                played > 0 ? (decode_us * 1000) / (kFrameDurationUs * played) : 0;
            std::printf("lap=%lu frames=%lu us_per_frame=%lu worst_frame_us=%lu "
                        "realtime_permille=%lu resync=%lu heap_free=%lu\n",
                        static_cast<unsigned long>(laps), static_cast<unsigned long>(played),
                        static_cast<unsigned long>(played > 0 ? decode_us / played : 0),
                        static_cast<unsigned long>(worst_frame_us),
                        static_cast<unsigned long>(permille),
                        static_cast<unsigned long>(accumulator.resynchronised_bytes()),
                        static_cast<unsigned long>(heap_caps_get_free_size(
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
            if (kMaxLaps != 0 && laps >= kMaxLaps) {
                // The verdict CI gates on. Every unit the accumulator produced
                // decoded, and it produced them by reading the partition a
                // block at a time - which is the whole claim.
                report_levels();
                player::sink_report();
                std::printf("stream.units=%lu stream.resync_bytes=%lu stream.sink=%s "
                            "stream.sink_frames=%lu stream.source=%s\n",
                            static_cast<unsigned long>(played),
                            static_cast<unsigned long>(accumulator.resynchronised_bytes()),
                            player::sink_name(),
                            static_cast<unsigned long>(player::sink_frames_written()),
                            player::source_name());
                std::printf("result=%s\n", played > 0 ? "pass" : "fail");
                vTaskDelay(pdMS_TO_TICKS(200));
                return;
            }
            // Some sources cannot go back. A socket has delivered what it
            // delivered; re-requesting the URL would be a new stream, not a
            // rewind, and the decoder's overlap-add state would carry across
            // the seam as a click. Stopping is the honest answer.
            if (!player::source_rewind()) {
                std::printf("stream: %s cannot rewind, stopping\n", player::source_name());
                report_levels();
                player::sink_report();
                std::printf("result=%s\n", played > 0 ? "pass" : "fail");
                vTaskDelay(pdMS_TO_TICKS(200));
                return;
            }
            accumulator = ac3::io::AccessUnitAccumulator{g_stream_buffer};
            continue;
        }

        if (unit.status != ac3::io::AccessUnitAccumulator::Status::kUnit) {
            std::printf("error: stream stopped (status=%d scan_error=%d)\n",
                        static_cast<int>(unit.status), static_cast<int>(accumulator.error()));
            std::printf("result=fail\n");
            return;
        }

        const std::int64_t started = esp_timer_get_time();
        const auto decoded = decoder.decode_frame_into(unit.bytes, g_pcm_spans);
        const std::uint64_t elapsed = static_cast<std::uint64_t>(esp_timer_get_time() - started);
        if (!decoded) {
            std::printf("error: decode failed (%d)\n", static_cast<int>(decoded.error()));
            std::printf("result=fail\n");
            return;
        }
        decode_us += elapsed;
        if (elapsed > worst_frame_us) {
            worst_frame_us = elapsed;
        }
        ++played;

        accumulate_levels(static_cast<int>(kOutputChannels));
        play_frame(static_cast<int>(kOutputChannels));
    }
}

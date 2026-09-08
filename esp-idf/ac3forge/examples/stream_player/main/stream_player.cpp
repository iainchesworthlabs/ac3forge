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
// SWAPPING THE SOURCE. read_block() below is the only function that knows where
// the bytes come from. An SD card is the same function over f_read(); HTTP is
// the same function over esp_http_client_read(). Nothing else in this file
// changes, which is the shape worth copying.
//
// SWAPPING THE SINK is the same idea at the other end, except CMake does it -
// see audio_sink.hpp. This file never mentions I2S.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/io/stream_accumulator.hpp"

#include "audio_sink.hpp"

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint64_t kFrameDurationUs = 32000;  // §5.3.2: 1,536 samples at 48 kHz
constexpr std::size_t kOutputChannels = 2;

// How much is read from the partition per call. 2 KB is deliberately smaller
// than a syncframe (1,792 bytes at 448 kbit/s is close, and E-AC-3 can reach
// 4,096) so the accumulator's "need more input" path is exercised on a real
// device rather than only in its unit tests - a block size that always happened
// to contain a whole frame would hide every framing bug there is.
constexpr std::size_t kReadBlock = 2048;

// From Kconfig, an int so it arrives as a plain constant rather than through a
// preprocessor conditional - see main/Kconfig.projbuild. 0 plays forever, which
// is what a demo on a board should do; CI sets a small number so the run ends
// with a verdict.
constexpr std::uint32_t kMaxLaps = CONFIG_AC3FORGE_EXAMPLE_MAX_LAPS;

// How many bytes of the partition are actually audio. Supplied by the build
// from the file's own size (see main/CMakeLists.txt): the partition is 256 KB
// and the sample is a fraction of that, and without a length the player would
// read a quarter of a megabyte of erased flash every lap while the accumulator
// skipped all of it looking for a sync word.
//
// A real source knows this - Content-Length, a file size, a directory entry. A
// raw partition does not, so the build says.
constexpr std::size_t kStreamBytes = AC3FORGE_STREAM_BYTES;

// The accumulator's working buffer. kRecommendedBuffer is 16 KB, which holds an
// independent substream plus three dependents; this sample is plain AC-3 and
// would fit in kMinimumBuffer, but an example should show the size that copes
// with a stream whose shape is not known in advance.
alignas(4) std::array<std::byte, ac3::io::kRecommendedBuffer> g_stream_buffer{};

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

const esp_partition_t* g_audio = nullptr;
std::size_t g_read_offset = 0;

// The ONLY function that knows where bytes come from. Returns 0 at end of
// stream. Replace its body to read from anywhere else.
std::size_t read_block(std::span<std::byte> dst) {
    // Bounded by the AUDIO length, not the partition's - see kStreamBytes.
    const std::size_t limit = std::min(kStreamBytes, static_cast<std::size_t>(g_audio->size));
    if (g_audio == nullptr || g_read_offset >= limit) {
        return 0;
    }
    // Every term cast to std::size_t first: esp_partition_t::size is a uint32_t
    // and mixing it into a braced std::min with size_t values is a deduction
    // failure rather than a promotion.
    const std::size_t remaining = limit - g_read_offset;
    const std::size_t want = std::min({dst.size(), kReadBlock, remaining});
    if (esp_partition_read(g_audio, g_read_offset, dst.data(), want) != ESP_OK) {
        return 0;
    }
    g_read_offset += want;
    return want;
}

// Hands the decoder's own planar channels straight to the sink, which owns the
// interleave and the sample format - see audio_sink.hpp for why that split is
// where it is.
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

    g_audio = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                       static_cast<esp_partition_subtype_t>(0x40), "audio");
    if (g_audio == nullptr) {
        std::printf("error: no 'audio' partition - check partitions.csv is the table in use\n");
        return;
    }
    std::printf("stream: partition '%s' at 0x%lx, %lu bytes of audio in %lu\n", g_audio->label,
                static_cast<unsigned long>(g_audio->address),
                static_cast<unsigned long>(kStreamBytes),
                static_cast<unsigned long>(g_audio->size));

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
            const auto got = read_block(accumulator.writable());
            if (got == 0) {
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
                std::printf("stream.units=%lu stream.resync_bytes=%lu stream.sink=%s "
                            "stream.sink_frames=%lu\n",
                            static_cast<unsigned long>(played),
                            static_cast<unsigned long>(accumulator.resynchronised_bytes()),
                            player::sink_name(),
                            static_cast<unsigned long>(player::sink_frames_written()));
                std::printf("result=%s\n", played > 0 ? "pass" : "fail");
                vTaskDelay(pdMS_TO_TICKS(200));
                return;
            }
            g_read_offset = 0;
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

        play_frame(static_cast<int>(kOutputChannels));
    }
}

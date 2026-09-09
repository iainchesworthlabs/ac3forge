// Decode AC-3 or E-AC-3 out of a byte source and play it, a block at a time.
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
#include <expected>
#include <optional>
#include <span>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/io/elementary.hpp"
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

// From Kconfig, ints so they arrive as plain constants rather than through
// preprocessor conditionals - see main/Kconfig.projbuild. kMaxLaps of 0 plays
// forever, which is what a demo on a board should do; CI sets a small number so
// the run ends with a verdict. kReportEveryFrames of 0 reports only at the end
// of a pass; a source that cannot rewind makes exactly one.
constexpr std::uint32_t kMaxLaps = CONFIG_AC3FORGE_EXAMPLE_MAX_LAPS;
constexpr std::uint64_t kReportEveryFrames = CONFIG_AC3FORGE_EXAMPLE_REPORT_EVERY_FRAMES;

// Caller-owned PCM, the shape an embedded integrator has: one block, sized
// once, reused every frame. The fold to stereo happens IN this storage, so it
// has to be wide enough for the coded programme going in, not for the two
// coming out.
//
// Eight, not §E3.8.2's cap of sixteen and not the six a 5.1 stream needs: the
// decoder asserts a span for every channel the programme renders, so this is
// the widest layout the example accepts - 7.1 - and provisioning for wider
// would put 6 KB of .bss per channel behind a stream nothing here plays. The
// same reasoning, and the same number, as apps/baremetal/probe.cpp.
constexpr std::size_t kMaxChannels = 8;
std::array<std::array<float, ac3::kSamplesPerFrame>, kMaxChannels> g_pcm{};
std::array<std::span<float>, kMaxChannels> g_pcm_spans{};
// The same storage again as const spans, which is what the sink takes. Built
// once per frame rather than held, because the count depends on what the
// decoder folded to.
std::array<std::span<const float>, kMaxChannels> g_out_views{};

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
std::array<double, kMaxChannels> g_sum_squares{};
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

// The running figures, in one place so the end-of-pass line and the periodic
// one cannot drift apart.
struct Timing {
    std::uint64_t decode_us = 0;
    std::uint64_t worst_frame_us = 0;
    std::uint64_t played = 0;  // units that produced audio
    std::uint64_t held = 0;    // units the decoder held back (§3.7), see below
};

// realtime_permille is decode time against the audio time it produced: 1000 is
// exactly real time and anything at or above it cannot play without gaps. The
// worst SINGLE frame matters as much as the average, because the sink's queue
// only absorbs a spike that small - it says how deep.
void report_timing(const char* label, std::uint64_t value, const Timing& t,
                   std::size_t resync_bytes) {
    const std::uint64_t permille =
        t.played > 0 ? (t.decode_us * 1000) / (kFrameDurationUs * t.played) : 0;
    std::printf("%s=%lu frames=%lu us_per_frame=%lu worst_frame_us=%lu realtime_permille=%lu "
                "resync=%lu heap_free=%lu\n",
                label, static_cast<unsigned long>(value), static_cast<unsigned long>(t.played),
                static_cast<unsigned long>(t.played > 0 ? t.decode_us / t.played : 0),
                static_cast<unsigned long>(t.worst_frame_us), static_cast<unsigned long>(permille),
                static_cast<unsigned long>(resync_bytes),
                static_cast<unsigned long>(
                    heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

// What the first unit said about the stream, printed once.
struct UnitInfo {
    bool eac3 = false;
    int acmod = 0;
    int channels = 0;
    int substreams = 0;
    int dialnorm = 0;
    bool objects = false;
};

void describe(const UnitInfo& info) {
    std::printf("stream: %s acmod=%d channels=%d substreams=%d dialnorm=-%d objects=%s, folded to "
                "%u\n",
                info.eac3 ? "E-AC-3" : "AC-3", info.acmod, info.channels, info.substreams,
                info.dialnorm, info.objects ? "yes" : "no",
                static_cast<unsigned>(kOutputChannels));
}

// Two decoders, constructed on the first access unit and chosen per unit by
// its header - and the reason is a defect, not a design.
//
// Eac3Decoder accepts an AC-3 syncframe as one access unit of one substream
// (§E2.3.1.2), which is the shape a player wants: one object, both
// generations. It does not survive a fold. Its decode_ac3_core() builds the
// inner FrameDecoder from the whole DecoderConfig, output stage included, so
// an AC-3 core reaches the §E3.8.2 assembly already folded to two channels
// and the assembly refuses it as kInvalidStream - which is how this example's
// own CI sample failed on 2026-09-10. The fix is one line in the library
// (build the core's decoder with `output` cleared; the assembled programme is
// folded once, afterwards) and belongs to the decoder core's owner; see
// planning/esp32-player.md. Until it lands, a player that folds hands a unit
// that IS one AC-3 syncframe to FrameDecoder itself. A legacy core followed
// by Annex E dependents is an E-AC-3 access unit and goes the other way.
struct Decoders {
    std::optional<ac3::FrameDecoder> ac3;
    std::optional<ac3::Eac3Decoder> eac3;
};

// True when the unit produced audio, false when the decoder held it back
// (§3.7's transient pre-noise processing releases each frame one call late).
std::expected<bool, ac3::DecodeError> decode_unit(Decoders& decoders,
                                                  const ac3::DecoderConfig& config,
                                                  std::span<const std::byte> unit,
                                                  UnitInfo* info) {
    const auto header = ac3::io::read_frame_header(unit);
    const bool one_ac3_syncframe = header.has_value() &&
                                   header->kind == ac3::io::StreamKind::kAc3 &&
                                   header->bytes == unit.size();
    if (one_ac3_syncframe) {
        if (!decoders.ac3.has_value()) {
            decoders.ac3.emplace(config);
        }
        const auto decoded = decoders.ac3->decode_frame_into(unit, g_pcm_spans);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        if (info != nullptr) {
            *info = {.eac3 = false,
                     .acmod = static_cast<int>(decoded->acmod),
                     .channels = header->coded_channels(),
                     .substreams = 1,
                     .dialnorm = decoded->dialnorm,
                     .objects = false};
        }
        return true;
    }
    if (!decoders.eac3.has_value()) {
        decoders.eac3.emplace(config);
    }
    const auto decoded = decoders.eac3->decode_access_unit_into(unit, g_pcm_spans);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    if (!decoded->has_value()) {
        return false;
    }
    if (info != nullptr) {
        const auto& au = **decoded;
        *info = {.eac3 = true,
                 .acmod = static_cast<int>(au.acmod),
                 .channels = au.layout.count,
                 .substreams = au.substream_count,
                 .dialnorm = au.dialnorm,
                 .objects = au.object_metadata.has_value()};
    }
    return true;
}

// The verdict block, at the end of a pass or of the stream. wall_ms against
// audio_ms is the whole-pipeline real-time check: a player that kept up spent
// as long playing as the audio lasted, and one that stalled spent longer, by
// exactly the silence it inserted. The sink's own line says where.
void report_end(const Timing& t, std::size_t resync_bytes, std::int64_t started_us) {
    report_levels();
    player::sink_report();
    const std::int64_t wall_us = esp_timer_get_time() - started_us;
    std::printf("stream.units=%lu stream.held=%lu stream.resync_bytes=%lu stream.sink=%s "
                "stream.sink_frames=%lu stream.source=%s stream.audio_ms=%lu stream.wall_ms=%lu\n",
                static_cast<unsigned long>(t.played), static_cast<unsigned long>(t.held),
                static_cast<unsigned long>(resync_bytes), player::sink_name(),
                static_cast<unsigned long>(player::sink_frames_written()), player::source_name(),
                static_cast<unsigned long>((t.played * kFrameDurationUs) / 1000),
                static_cast<unsigned long>(wall_us / 1000));
    std::printf("result=%s\n", t.played > 0 ? "pass" : "fail");
}

}  // namespace

extern "C" void app_main() {
    std::printf("ac3forge stream_player: AC-3 or E-AC-3, folded to stereo\n");

    for (std::size_t ch = 0; ch < kMaxChannels; ++ch) {
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

    // One configuration for whichever decoder a unit needs - see Decoders.
    //
    // kLine is §7.7.1's operating mode - dialnorm normalisation plus the full
    // transmitted range - which is what a decoder feeding an amplifier does.
    // Without it, playback level follows whatever the encoder's dialnorm was and
    // two streams play back at two different loudnesses.
    //
    // skip_object_reconstruction: an Atmos stream's bed IS the complete mix -
    // JOC extracts objects OUT of it (TS 103 420 §6), so a stereo fold of the
    // bed loses nothing a stereo DAC could render. Reconstructing them would
    // cost this part about 10 ms of every 32 ms frame and a 147 KB
    // ReconstructionState (docs/platforms/esp32.md), for objects with nowhere to
    // go. A player with a renderer behind it turns this off.
    const ac3::DecoderConfig config{.output = {.target = ac3::DownmixTarget::kLoRo,
                                               .mode = ac3::OperatingMode::kLine},
                                    .skip_object_reconstruction = true};
    Decoders decoders;
    ac3::io::AccessUnitAccumulator accumulator{g_stream_buffer};

    Timing timing;
    std::uint32_t laps = 0;
    std::uint64_t reports = 0;
    std::int64_t started_us = 0;  // set when the first unit decodes

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
            report_timing("lap", laps, timing, accumulator.resynchronised_bytes());
            if (kMaxLaps != 0 && laps >= kMaxLaps) {
                // The verdict CI gates on. Every unit the accumulator produced
                // decoded, and it produced them by reading the partition a
                // block at a time - which is the whole claim.
                report_end(timing, accumulator.resynchronised_bytes(), started_us);
                vTaskDelay(pdMS_TO_TICKS(200));
                return;
            }
            // Some sources cannot go back. A socket has delivered what it
            // delivered; re-requesting the URL would be a new stream, not a
            // rewind, and the decoder's overlap-add state would carry across
            // the seam as a click. Stopping is the honest answer.
            if (!player::source_rewind()) {
                std::printf("stream: %s cannot rewind, stopping\n", player::source_name());
                report_end(timing, accumulator.resynchronised_bytes(), started_us);
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

        UnitInfo info;
        const std::int64_t started = esp_timer_get_time();
        const auto decoded =
            decode_unit(decoders, config, unit.bytes, timing.played == 0 ? &info : nullptr);
        const std::uint64_t elapsed = static_cast<std::uint64_t>(esp_timer_get_time() - started);
        if (!decoded) {
            std::printf("error: decode failed (%d)\n", static_cast<int>(decoded.error()));
            std::printf("result=fail\n");
            return;
        }
        timing.decode_us += elapsed;
        if (elapsed > timing.worst_frame_us) {
            timing.worst_frame_us = elapsed;
        }
        // A held-back unit is not an error: a stream using §3.7's transient
        // pre-noise processing releases each frame's PCM one call late, so
        // there is nothing to play THIS call. Counted, so a run's frame total
        // still reconciles with what the source delivered.
        if (!*decoded) {
            ++timing.held;
            continue;
        }
        if (timing.played == 0) {
            started_us = esp_timer_get_time();
            describe(info);
        }
        ++timing.played;

        accumulate_levels(static_cast<int>(kOutputChannels));
        play_frame(static_cast<int>(kOutputChannels));

        if (kReportEveryFrames != 0 && timing.played % kReportEveryFrames == 0) {
            ++reports;
            report_timing("progress", reports, timing, accumulator.resynchronised_bytes());
            player::sink_report();
        }
    }
}

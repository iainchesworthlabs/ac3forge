// A sink with no peripheral that still checks the conversion.
//
// WHY THIS IS NOT THE NULL SINK. The null sink counts frames; it establishes
// that the player looped and the decode did not error. This one runs the SAME
// conversion the real sinks run - interleave_16 and interleave_24in32 from
// ../../interleave.hpp, not copies of them - into a buffer, and then checks
// what came out.
//
// That is the difference between mocking a boundary and skipping it. The
// peripheral is unmockable: QEMU has no I2S and no TDM, so the blocking write
// cannot happen. Everything on THIS side of the write is ordinary code that
// runs perfectly well, and until this sink existed none of it was exercised on
// target at all - the conversion was host-tested only, and the sinks' own use
// of it (slot counts, buffer sizing, channel indexing) was not tested anywhere.
//
// WHAT IT CHECKS, and why each is worth a line:
//
//   * Every 24-in-32 slot has a clear low byte. The DAC takes the top 24 bits,
//     so a sample scaled to 32-bit rather than shifted would sound correct in a
//     host test comparing floats and be wrong on the wire.
//   * Padding slots are exactly zero. A 5.1 programme on an 8-slot bus leaves
//     two, and skipping them rather than zeroing them plays whatever the
//     previous frame left in the DMA buffer.
//   * Carried slots are not all zero, so "everything is zero" cannot pass the
//     two checks above by being vacuously correct.
//   * The RMS of the CONVERTED integers, which the player's own float RMS is
//     compared against - a conversion that halved every sample would satisfy
//     every structural check and fail this one.

#include "audio_sink.hpp"

#include <array>
#include <cmath>
#include <cstdio>

#include "ac3/core/tables.hpp"

#include "interleave.hpp"

namespace player {
namespace {

std::uint64_t g_frames = 0;
int g_channels = 0;
std::size_t g_slots = 0;

// Both shapes, because this sink stands in for both real ones and which it is
// standing in for is a build option. 49,152 bytes for the wider of the two, at
// namespace scope for the same reason the real sinks keep theirs there.
constexpr std::size_t kMaxSlots = 8;
std::array<std::int32_t, ac3::kSamplesPerFrame * kMaxSlots> g_tdm{};
std::array<std::int16_t, ac3::kSamplesPerFrame * 2> g_stereo{};

// Accumulated over the run rather than checked per frame: a fault that only
// appears on one frame in six still moves these, and reporting once keeps the
// console out of the way of the timing figures.
std::uint64_t g_low_byte_set = 0;
std::uint64_t g_padding_nonzero = 0;
std::uint64_t g_carried_nonzero = 0;
double g_sum_squares = 0.0;
std::uint64_t g_samples = 0;

constexpr bool kTdm = CONFIG_AC3FORGE_EXAMPLE_CAPTURE_TDM != 0;

}  // namespace

bool sink_open(std::uint32_t sample_rate, int channels) {
    if (channels <= 0 || static_cast<std::size_t>(channels) > kMaxSlots) {
        std::printf("error: the capture sink takes 1 to %u channels, asked for %d\n",
                    static_cast<unsigned>(kMaxSlots), channels);
        return false;
    }
    g_channels = channels;
    g_slots = kTdm ? static_cast<std::size_t>(CONFIG_AC3FORGE_EXAMPLE_TDM_SLOTS)
                   : static_cast<std::size_t>(channels);
    if (g_slots > kMaxSlots || static_cast<std::size_t>(channels) > g_slots) {
        std::printf("error: %d channels do not fit %u slots\n", channels,
                    static_cast<unsigned>(g_slots));
        return false;
    }
    std::printf("sink: capture %lu Hz %s x%d in %u slots (no peripheral, no pacing)\n",
                static_cast<unsigned long>(sample_rate), kTdm ? "24-in-32" : "16-bit", channels,
                static_cast<unsigned>(g_slots));
    return true;
}

void sink_write(std::span<const std::span<const float>> channels) {
    if (kTdm) {
        const auto padding =
            interleave_24in32(channels, g_slots, ac3::kSamplesPerFrame,
                              std::span<std::int32_t>{g_tdm.data(),
                                                      ac3::kSamplesPerFrame * g_slots});
        for (std::size_t frame = 0; frame < ac3::kSamplesPerFrame; ++frame) {
            const std::size_t base = frame * g_slots;
            for (std::size_t slot = 0; slot < g_slots; ++slot) {
                const std::int32_t value = g_tdm[base + slot];
                if ((value & 0xFF) != 0) {
                    ++g_low_byte_set;
                }
                const bool is_padding = slot >= g_slots - padding;
                if (is_padding && value != 0) {
                    ++g_padding_nonzero;
                } else if (!is_padding && value != 0) {
                    ++g_carried_nonzero;
                }
                if (!is_padding) {
                    // Back to the same scale the float RMS is in: the slot is
                    // 24-bit left-justified in 32, so shift down and divide by
                    // the 24-bit maximum.
                    const double sample =
                        static_cast<double>(value >> 8) / static_cast<double>(kPcm24Max);
                    g_sum_squares += sample * sample;
                    ++g_samples;
                }
            }
        }
    } else {
        interleave_16(channels, ac3::kSamplesPerFrame, g_stereo);
        for (std::size_t i = 0; i < ac3::kSamplesPerFrame * 2; ++i) {
            const std::int16_t value = g_stereo[i];
            if (value != 0) {
                ++g_carried_nonzero;
            }
            const double sample = static_cast<double>(value) / 32767.0;
            g_sum_squares += sample * sample;
            ++g_samples;
        }
    }
    ++g_frames;
}

const char* sink_name() { return kTdm ? "capture-tdm" : "capture-i2s"; }

std::uint64_t sink_frames_written() { return g_frames; }

void sink_report() {
    const double rms = g_samples > 0 ? std::sqrt(g_sum_squares / static_cast<double>(g_samples))
                                     : 0.0;
    std::printf("capture.low_byte_set=%lu capture.padding_nonzero=%lu "
                "capture.carried_nonzero=%lu capture.rms=%ld\n",
                static_cast<unsigned long>(g_low_byte_set),
                static_cast<unsigned long>(g_padding_nonzero),
                static_cast<unsigned long>(g_carried_nonzero),
                static_cast<long>((rms * 1e6) + 0.5));
}

}  // namespace player

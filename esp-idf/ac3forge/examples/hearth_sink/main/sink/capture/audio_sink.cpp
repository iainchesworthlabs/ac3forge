// A sink with no peripheral that still checks the conversion.
//
// WHY THIS IS NOT THE NULL SINK. The null sink counts blocks; it establishes
// that the player looped and the decode did not error. This one runs the SAME
// conversion the real sinks run - interleave_16 and interleave_24in32 from
// ac3forge/interleave.hpp, not copies of them - into a buffer, and then checks
// what came out.
//
// That is the difference between mocking a boundary and skipping it. The
// peripheral is unmockable: QEMU has no I2S and no TDM, so the blocking write
// cannot happen. Everything on THIS side of the write is ordinary code that
// runs perfectly well, and until this sink existed none of it was exercised on
// target at all - the conversion was host-tested only, and the sinks' own use
// of it (slot counts, buffer sizing, channel indexing) was not tested anywhere.
//
// Its TDM frame follows CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS as the i2s sink's
// does: 24-in-32 slots through interleave_24in32 at 32, 16-bit slots through
// interleave_16in16 at 16.
//
// WHAT IT CHECKS, and why each is worth a line:
//
//   * Every 24-in-32 slot has a clear low byte. The DAC takes the top 24 bits,
//     so a sample scaled to 32-bit rather than shifted would sound correct in a
//     host test comparing floats and be wrong on the wire. (A 16-bit slot has
//     no spare bits, so this count stays zero at that width.)
//   * Padding slots are exactly zero. A 5.1 programme on an 8-slot bus leaves
//     two, and skipping them rather than zeroing them plays whatever the
//     previous block left in the DMA buffer.
//   * Carried slots are not all zero, so "everything is zero" cannot pass the
//     two checks above by being vacuously correct.
//   * The RMS of the CONVERTED integers, which the player's own float RMS is
//     compared against - a conversion that halved every sample would satisfy
//     every structural check and fail this one.

#include "audio_sink.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "ac3/core/tables.hpp"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ac3forge/interleave.hpp"
#include "ac3forge/playout.hpp"

namespace player {
namespace {

std::uint64_t g_writes = 0;
int g_channels = 0;
std::size_t g_slots = 0;

// A timed write (a Sendspin stream) either says nothing about time, and runs
// as fast as it is given blocks, or is paced by a DAC that is not there
// (CONFIG_AC3FORGE_EXAMPLE_CAPTURE_PACED). Unpaced is what CI compares levels
// with: every decoded sample is written, none padded, skipped or slewed, and
// the emulator's speed cannot make a burst late.
constexpr bool kPaced = CONFIG_AC3FORGE_EXAMPLE_CAPTURE_PACED != 0;
// The ring a paced write stands in for: twelve buffers of 256 frames, the
// depth the board's network shape runs its I2S sink at.
constexpr std::uint32_t kVirtualDescriptors = 12;
ac3forge::VirtualDac g_dac;

constexpr bool kTdm = CONFIG_AC3FORGE_EXAMPLE_CAPTURE_TDM != 0;
constexpr bool kSlotBits16 = CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS == 16;
// Standing in for the stereo sink, convert the way it is configured to: 32-bit
// slots through the same 24-in-32 path the TDM bus uses, two slots wide, or
// 16-bit. The default is 32, so the default CI shape checks the conversion
// the default board shape runs.
constexpr bool kWide = !kSlotBits16;

// Both widths, because this sink stands in for both real shapes and which it
// is standing in for is a build option - but only the configured width's
// block has any size: 16 KB of 24-in-32 slots, or 8 KB of 16-bit ones (two
// slots of them for the stereo pair outside TDM). At namespace scope for the
// same reason the real sinks keep theirs there.
constexpr std::size_t kMaxSlots = 16;
std::array<std::int32_t, ac3::kSamplesPerBlock * (kWide ? kMaxSlots : 0)> g_tdm{};
std::array<std::int16_t, ac3::kSamplesPerBlock * (kWide ? 0 : (kTdm ? kMaxSlots : 2))> g_narrow{};

// Accumulated over the run rather than checked per block: a fault that only
// appears on one block in six still moves these, and reporting once keeps the
// console out of the way of the timing figures.
std::uint64_t g_low_byte_set = 0;
std::uint64_t g_padding_nonzero = 0;
std::uint64_t g_carried_nonzero = 0;
double g_sum_squares = 0.0;
std::uint64_t g_samples = 0;

}  // namespace

bool sink_open(std::uint32_t sample_rate, int channels) {
    if (channels <= 0 || static_cast<std::size_t>(channels) > kMaxSlots) {
        std::printf("error: the capture sink takes 1 to %u channels, asked for %d\n",
                    static_cast<unsigned>(kMaxSlots), channels);
        return false;
    }
    if (!kTdm && channels > 2) {
        std::printf("error: the stereo capture takes 1 or 2 channels, asked for %d - set "
                    "CONFIG_AC3FORGE_EXAMPLE_CAPTURE_TDM=1 for a wider layout\n",
                    channels);
        return false;
    }
    g_channels = channels;
    g_slots = kTdm ? static_cast<std::size_t>(CONFIG_AC3FORGE_EXAMPLE_TDM_SLOTS) : 2;
    if (g_slots > kMaxSlots || static_cast<std::size_t>(channels) > g_slots) {
        std::printf("error: %d channels do not fit %u slots\n", channels,
                    static_cast<unsigned>(g_slots));
        return false;
    }
    g_dac.open(kVirtualDescriptors, ac3::kSamplesPerBlock, sample_rate);
    std::printf("sink: capture %lu Hz %s x%d in %u slots (no peripheral, %s)\n",
                static_cast<unsigned long>(sample_rate), kWide ? "24-in-32" : "16-bit", channels,
                static_cast<unsigned>(g_slots), kPaced ? "timed writes paced as a DAC would" : "no pacing");
    return true;
}

void sink_write(std::span<const std::span<const float>> channels) {
    if (channels.empty()) {
        return;
    }
    std::size_t frames = channels[0].size();
    if (frames > ac3::kSamplesPerBlock) {
        frames = ac3::kSamplesPerBlock;
    }
    // The level in integers, per write, and in double once per write: the
    // slots already hold integers, their squares sum exactly in 64 bits over a
    // block (256 x 12 x 2^46 is under 2^58), and the soft-float library is
    // called once instead of four times a sample - which cost more than the
    // decode on a 12-slot bus.
    std::int64_t block_sum = 0;
    if (kTdm && !kWide) {
        // 16-bit TDM: the same padding and level checks on 16-bit slots.
        const auto padding = ac3forge::interleave_16in16(
            channels, g_slots, frames, std::span<std::int16_t>{g_narrow.data(), frames * g_slots});
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const std::size_t base = frame * g_slots;
            for (std::size_t slot = 0; slot < g_slots; ++slot) {
                const std::int16_t value = g_narrow[base + slot];
                const bool is_padding = slot >= g_slots - padding;
                if (is_padding && value != 0) {
                    ++g_padding_nonzero;
                } else if (!is_padding && value != 0) {
                    ++g_carried_nonzero;
                }
                if (!is_padding) {
                    const std::int64_t sample = value;
                    block_sum += sample * sample;
                    ++g_samples;
                }
            }
        }
        g_sum_squares += static_cast<double>(block_sum) / (32767.0 * 32767.0);
    } else if (kTdm) {
        const auto padding = ac3forge::interleave_24in32(
            channels, g_slots, frames, std::span<std::int32_t>{g_tdm.data(), frames * g_slots});
        for (std::size_t frame = 0; frame < frames; ++frame) {
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
                    // The slot is 24-bit left-justified in 32: shift down to
                    // the sample, and scale by the 24-bit maximum once, below.
                    const std::int64_t sample = value >> 8;
                    block_sum += sample * sample;
                    ++g_samples;
                }
            }
        }
        constexpr double kFullScale = static_cast<double>(ac3forge::kPcm24Max);
        g_sum_squares += static_cast<double>(block_sum) / (kFullScale * kFullScale);
    } else {
        // A mono layout to both slots, as the i2s sink does, converted the way
        // that sink is configured to: 24-in-32 when its slots are 32 bits wide
        // (the default), 16-bit when they are not. Until 2026-09-10 this
        // always took the 16-bit path, so CI checked a conversion the default
        // board shape does not run.
        const std::array<std::span<const float>, 2> pair = {
            channels[0], channels.size() > 1 ? channels[1] : channels[0]};
        if (kWide) {
            ac3forge::interleave_24in32(pair, 2, frames,
                                        std::span<std::int32_t>{g_tdm.data(), frames * 2});
            for (std::size_t i = 0; i < frames * 2; ++i) {
                const std::int32_t value = g_tdm[i];
                if ((value & 0xFF) != 0) {
                    ++g_low_byte_set;
                }
                if (value != 0) {
                    ++g_carried_nonzero;
                }
                const std::int64_t sample = value >> 8;
                block_sum += sample * sample;
                ++g_samples;
            }
            constexpr double kFullScale = static_cast<double>(ac3forge::kPcm24Max);
            g_sum_squares += static_cast<double>(block_sum) / (kFullScale * kFullScale);
        } else {
            ac3forge::interleave_16(pair, frames,
                                    std::span<std::int16_t>{g_narrow.data(), frames * 2});
            for (std::size_t i = 0; i < frames * 2; ++i) {
                const std::int16_t value = g_narrow[i];
                if (value != 0) {
                    ++g_carried_nonzero;
                }
                const std::int64_t sample = value;
                block_sum += sample * sample;
                ++g_samples;
            }
            g_sum_squares += static_cast<double>(block_sum) / (32767.0 * 32767.0);
        }
    }
    ++g_writes;
}

std::optional<ac3forge::PlayoutWrite> sink_write_timed(std::span<const std::span<const float>> channels) {
    if (g_channels == 0) {
        return std::nullopt;
    }
    sink_write(channels);
    if (!kPaced) {
        return std::nullopt;
    }
    const ac3forge::VirtualDac::Write written = g_dac.write(esp_timer_get_time());
    const std::int64_t wait_us = written.return_us - esp_timer_get_time();
    if (wait_us > 0) {
        vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS((wait_us + 999) / 1000)));
    }
    return ac3forge::PlayoutWrite{.play_us = written.play_us, .late = false, .gap = written.gap};
}

const char* sink_name() { return kTdm ? "capture-tdm" : "capture-i2s"; }

// The bus this sink stands in for, open or not: a player asks before its
// first stream how many outputs it may use.
int sink_slots() {
    return static_cast<int>(kTdm ? std::min<std::size_t>(CONFIG_AC3FORGE_EXAMPLE_TDM_SLOTS, kMaxSlots) : 2);
}

// Its slots are fixed by the build, and it has no second line to wire.
int sink_max_slots() { return sink_slots(); }

bool sink_second_line_possible() { return false; }

// Built for one width and checked against it (kWide above): this sink's whole
// job is to convert exactly as the i2s sink does and check the result, so the
// width is a property of the shape CI built, not something to change under it.
int sink_slot_bits() { return CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS; }

bool sink_set_slot_bits(int bits) { return bits == CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS; }

std::uint64_t sink_frames_written() { return g_writes; }

// A new play's samples are checked from zero, so the line sink_report() prints
// is about that play, as the player's own levels beside it are.
void sink_begin_play() {
    g_low_byte_set = 0;
    g_padding_nonzero = 0;
    g_carried_nonzero = 0;
    g_sum_squares = 0.0;
    g_samples = 0;
}

void sink_report() {
    const double rms = g_samples > 0 ? std::sqrt(g_sum_squares / static_cast<double>(g_samples))
                                     : 0.0;
    std::printf("capture.slots=%u capture.channels=%d capture.low_byte_set=%lu "
                "capture.padding_nonzero=%lu capture.carried_nonzero=%lu capture.rms=%ld\n",
                static_cast<unsigned>(g_slots), g_channels,
                static_cast<unsigned long>(g_low_byte_set),
                static_cast<unsigned long>(g_padding_nonzero),
                static_cast<unsigned long>(g_carried_nonzero),
                static_cast<long>((rms * 1e6) + 0.5));
}

}  // namespace player

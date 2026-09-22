#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ac3forge/slot_conversion.hpp"

// Planar float to interleaved fixed-point, in the two shapes the sinks need.
//
// In the component rather than in an example, because every sink converts
// through it - the streaming example's I2S and TDM sinks, its capture sink,
// and whatever an integrator writes - and free of ESP-IDF, because this is the
// part of a sink that can be TESTED. Everything else in one is peripheral setup
// and a blocking write, neither of which does anything without a DAC on the
// other end; this is arithmetic and indexing, and indexing is where the bugs
// are. tests/io/test_interleave.cpp builds it on the host.
//
// It is library code with a temporary home: planning/esp32-player.md hands it
// over to src/forge as ac3::io::interleave, and this copy goes when that lands.
//
// SLOTS ARE FIXED WIDTH AND MUST ALL BE WRITTEN. A TDM frame carries `slots`
// samples whatever the programme has, so a 5.1 stream on an 8-slot bus has two
// slots with nothing to put in them. They are ZEROED, not skipped: the DMA
// buffer is reused, so whatever the previous frame left in slots 6 and 7 is what
// the DAC clocks out next time - two channels of stale audio that nobody is
// listening for and everybody can hear.
//
// TWO WAYS TO CONVERT A SAMPLE, ONE CHOSEN PER PART. to_pcm16 and
// to_slot_24in32 scale, clip and truncate in float: a handful of instructions on
// a part with a floating-point unit, such as the ESP32-S3, and four calls into
// the software floating-point routines on a part without one, such as the
// ESP32-C6. to_pcm16_from_bits and to_slot_24in32_from_bits (at the bottom of
// this file) compute the same integers from the float's IEEE-754 bits with
// integer operations only. Every interleave here takes the conversion as a
// template argument, FloatConversion or BitConversion, and defaults to
// SlotConversion, which ac3forge/slot_conversion.hpp names. There are two copies
// of that header, under conversion/float/ and conversion/bits/, and the
// component's CMakeLists.txt puts one of them on the include path from
// CONFIG_SOC_CPU_HAS_FPU. A sink calls interleave_16in16 and gets its part's
// conversion; the host tests name each one.

namespace ac3forge {

// 24-bit in a 32-bit slot, which is what a TDM DAC (a PCM3168A, say) expects and
// what the ESP32-S3's I2S produces with a 32-bit slot width.
//
// The sample is left-justified: the DAC takes the top 24 bits of the slot and
// ignores the rest, so the value is scaled to 24-bit and then shifted up by 8.
// Scaling to 32-bit directly and letting the low byte fall off would work too
// and is harder to check against a datasheet.
inline constexpr std::int32_t kPcm24Max = 8388607;  // 2^23 - 1

[[nodiscard]] inline std::int32_t to_slot_24in32(float sample) {
    const float scaled = sample * static_cast<float>(kPcm24Max);
    // Clipped rather than wrapped. §7.8's normalisation bounds a fold by the
    // loudest coded sample, so an out-of-range value should not arrive - but a
    // wrapped sample turns a peak into full-scale noise of the opposite sign,
    // which is the loudest thing the system can produce, and two comparisons is
    // a cheap way not to.
    if (scaled >= static_cast<float>(kPcm24Max)) {
        return kPcm24Max << 8;
    }
    if (scaled <= static_cast<float>(-kPcm24Max)) {
        return -kPcm24Max << 8;
    }
    return static_cast<std::int32_t>(scaled) << 8;
}

// Writes `frames` TDM frames of `slots` samples each into `out`, taking the
// first channels.size() slots from `channels` and zeroing the rest.
//
// `out` must hold frames * slots entries; each span in `channels` must hold at
// least `frames`. Both are the caller's to size - this allocates nothing, which
// is the point of it being usable from the minimum-footprint profile.
//
// Returns the number of slots zeroed per frame, which is only worth having
// because it is what a caller asserts on: a 5.1 programme on an 8-slot bus
// should report 2, and a configuration that quietly dropped channels would
// report something else.
template <typename Conversion = SlotConversion>
inline std::size_t interleave_24in32(std::span<const std::span<const float>> channels,
                                     std::size_t slots, std::size_t frames,
                                     std::span<std::int32_t> out) {
    const std::size_t used = channels.size() < slots ? channels.size() : slots;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::size_t base = frame * slots;
        for (std::size_t slot = 0; slot < used; ++slot) {
            out[base + slot] = Conversion::slot_24in32(channels[slot][frame]);
        }
        for (std::size_t slot = used; slot < slots; ++slot) {
            out[base + slot] = 0;
        }
    }
    return slots - used;
}

// --- 16-bit slots: standard I2S's stereo pair, and TDM -----------------------

// 32767 rather than 32768 as the scale, so +1.0 maps to full scale and does not
// need the clamp to catch it. Clipped rather than wrapped, for the same reason
// to_slot_24in32 is: a wrapped sample turns a peak into full-scale noise of the
// opposite sign, which is the loudest thing the system can produce.
[[nodiscard]] inline std::int16_t to_pcm16(float sample) {
    constexpr float kScale = 32767.0F;
    const float scaled = sample * kScale;
    if (scaled >= kScale) {
        return 32767;
    }
    if (scaled <= -kScale) {
        return -32767;
    }
    return static_cast<std::int16_t>(scaled);
}

// Writes `frames` stereo pairs into `out`, which must hold frames * 2 entries.
//
// Two channels exactly, because standard I2S carries two slots - there is no
// padding case here and nothing to zero. A caller with more channels than that
// wants a TDM bus: interleave_16in16 or interleave_24in32.
template <typename Conversion = SlotConversion>
inline void interleave_16(std::span<const std::span<const float>> channels, std::size_t frames,
                          std::span<std::int16_t> out) {
    const auto left = channels[0];
    const auto right = channels[1];
    for (std::size_t frame = 0; frame < frames; ++frame) {
        out[frame * 2] = Conversion::pcm16(left[frame]);
        out[(frame * 2) + 1] = Conversion::pcm16(right[frame]);
    }
}

// interleave_24in32's TDM layout at 16 bits a slot: `frames` frames of `slots`
// samples each into `out`, the first channels.size() slots from `channels` and
// the rest zeroed, for the reason the top of this file gives.
//
// The width that puts eight channels on one line. An ESP32-S3 or ESP32-C6 TDM
// frame holds at most 128 bits (ac3forge/sink_plan.hpp), which is four 32-bit
// slots or eight of these, at 16 bits a sample where the 32-bit slots carry 24.
//
// `out` must hold frames * slots entries and each span in `channels` at least
// `frames`. Returns the number of slots zeroed per frame, as interleave_24in32
// does, so a second line's caller can pass it the channels past the first
// line's and assert on the padding the same way.
template <typename Conversion = SlotConversion>
inline std::size_t interleave_16in16(std::span<const std::span<const float>> channels,
                                     std::size_t slots, std::size_t frames,
                                     std::span<std::int16_t> out) {
    const std::size_t used = channels.size() < slots ? channels.size() : slots;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::size_t base = frame * slots;
        for (std::size_t slot = 0; slot < used; ++slot) {
            out[base + slot] = Conversion::pcm16(channels[slot][frame]);
        }
        for (std::size_t slot = used; slot < slots; ++slot) {
            out[base + slot] = 0;
        }
    }
    return slots - used;
}

// --- The same two conversions from the float's bits --------------------------
//
// For a part with no floating-point unit. The results are to_pcm16's and
// to_slot_24in32's for every float that is not a NaN - all 4,278,190,082 such bit
// patterns were compared on the host - and a NaN gives 0, where the float forms
// convert a NaN to an integer, which C++ leaves undefined.
//
// What the float forms compute. A finite float is M x 2^(E - 23), with M its
// 24-bit mantissa (the stored 23-bit fraction and the implicit leading one) and
// E its unbiased exponent. `sample * kScale` is the exact product
// P = M x (2^b - 1), in units of 2^(E - 23), rounded to 24 significant bits to
// nearest with ties to even: IEEE-754's default rounding, which a
// single-precision multiply does in hardware and in a software library alike (b
// is 15 for a 16-bit slot, 23 for 24-in-32). The comparisons then clip at the
// scale, and the cast truncates toward zero. Taking the cases in turn:
//
//   - |sample| >= 1, an infinity included, is at or past full scale and clips.
//     Below 1 the rounded product never reaches the scale: the largest float
//     under 1 gives 32767 - 2^-9 at 16 bits, which truncates to 32766.
//   - |sample| < 2^(-1 - b) gives a product under a half, which truncates to 0,
//     as do zero and the subnormals.
//   - Between the two, the integer is P rounded at the float's last place and
//     shifted down by 23 - E. For that integer, rounding to nearest with ties to
//     even is the same as adding half of the last place: the two differ only on
//     a tie whose kept part is even, and rounding such a tie up could only change
//     the integer by carrying through every bit from the last place to the units,
//     where the last place holds a zero. The last place follows from the length
//     of P, which is 24 + b bits when the stored fraction is at least
//     ceil(2^23 / (2^b - 1)) - 257 at 16 bits, 2 at 24 - and 23 + b bits
//     otherwise. With A = P >> (b - 2) = 4M - ceil(M / 2^(b - 2)), which is exact
//     and fits in 26 bits, the integer is (A + 2) >> (25 - b - E) for the longer
//     product and (A + 1) >> (25 - b - E) for the shorter: 32-bit shifts and
//     additions and one comparison, with no multiply.
//
// Leaving the rounding out does not give the same integers: truncating the exact
// product comes out one step nearer zero for 33,278 of the 16-bit inputs and
// 8,388,608 of the 24-in-32 ones, those whose product lies less than half of the
// float's last place below an integer, where the multiply rounds up onto it.
namespace detail {

// |sample| x (2^ScaleBits - 1) as the float forms produce it, before the sign:
// see above. ScaleBits is 15 or 23.
template <int ScaleBits>
[[nodiscard]] constexpr std::int32_t scaled_magnitude_from_bits(std::uint32_t bits) {
    constexpr auto b = static_cast<std::uint32_t>(ScaleBits);
    const std::uint32_t biased = (bits >> 23) & 0xFFU;
    const std::uint32_t fraction = bits & 0x7FFFFFU;
    // Outside 2^(-1 - b) <= |sample| < 1, in one unsigned comparison.
    if (biased - (126U - b) > b) {
        if (biased < 127U) {
            return 0;
        }
        const bool nan = biased == 255U && fraction != 0;
        return nan ? 0 : (std::int32_t{1} << ScaleBits) - 1;
    }
    constexpr std::uint32_t kLongerFrom = ((1U << 23) + (1U << b) - 2U) / ((1U << b) - 1U);
    const std::uint32_t mantissa = fraction | 0x800000U;
    std::uint32_t top = (mantissa << 2) - ((mantissa + (1U << (b - 2)) - 1U) >> (b - 2));
    top += fraction >= kLongerFrom ? 2U : 1U;
    // The comparison above holds the shift between 26 - b and 26.
    return static_cast<std::int32_t>(top >> (152U - b - biased));
}

}  // namespace detail

[[nodiscard]] constexpr std::int32_t to_slot_24in32_from_bits(float sample) {
    const auto bits = std::bit_cast<std::uint32_t>(sample);
    const std::int32_t magnitude = detail::scaled_magnitude_from_bits<23>(bits);
    return ((bits >> 31) != 0 ? -magnitude : magnitude) * 256;
}

[[nodiscard]] constexpr std::int16_t to_pcm16_from_bits(float sample) {
    const auto bits = std::bit_cast<std::uint32_t>(sample);
    const std::int32_t magnitude = detail::scaled_magnitude_from_bits<15>(bits);
    return static_cast<std::int16_t>((bits >> 31) != 0 ? -magnitude : magnitude);
}

// The interleaves' template argument: one per way of converting, named by
// ac3forge/slot_conversion.hpp for the part being built.
struct FloatConversion {
    [[nodiscard]] static std::int16_t pcm16(float sample) { return to_pcm16(sample); }
    [[nodiscard]] static std::int32_t slot_24in32(float sample) { return to_slot_24in32(sample); }
};

struct BitConversion {
    [[nodiscard]] static constexpr std::int16_t pcm16(float sample) {
        return to_pcm16_from_bits(sample);
    }
    [[nodiscard]] static constexpr std::int32_t slot_24in32(float sample) {
        return to_slot_24in32_from_bits(sample);
    }
};

}  // namespace ac3forge

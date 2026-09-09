#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// Planar float to interleaved fixed-point, in the two shapes the sinks need.
//
// Its own header, free of ESP-IDF, for one reason: this is the part of a sink
// that can be TESTED. Everything else in one is peripheral setup and a blocking
// write, neither of which does anything without a DAC on the other end; this is
// arithmetic and indexing, and indexing is where the bugs are.
//
// Both real sinks convert through here, and so does sink/capture/ - which is
// what lets CI check the conversion ON TARGET without a peripheral, rather than
// only on the host. A capture sink that did its own conversion would be
// checking a copy of the code instead of the code.
//
// See tests/io/test_interleave.cpp, which builds this on the host.
//
// SLOTS ARE FIXED WIDTH AND MUST ALL BE WRITTEN. A TDM frame carries `slots`
// samples whatever the programme has, so a 5.1 stream on an 8-slot bus has two
// slots with nothing to put in them. They are ZEROED, not skipped: the DMA
// buffer is reused, so whatever the previous frame left in slots 6 and 7 is what
// the DAC clocks out next time - two channels of stale audio that nobody is
// listening for and everybody can hear.

namespace player {

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
inline std::size_t interleave_24in32(std::span<const std::span<const float>> channels,
                                     std::size_t slots, std::size_t frames,
                                     std::span<std::int32_t> out) {
    const std::size_t used = channels.size() < slots ? channels.size() : slots;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::size_t base = frame * slots;
        for (std::size_t slot = 0; slot < used; ++slot) {
            out[base + slot] = to_slot_24in32(channels[slot][frame]);
        }
        for (std::size_t slot = used; slot < slots; ++slot) {
            out[base + slot] = 0;
        }
    }
    return slots - used;
}

// --- 16-bit stereo, which is what standard I2S carries ---------------------

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
// wants interleave_24in32 and a TDM bus.
inline void interleave_16(std::span<const std::span<const float>> channels, std::size_t frames,
                          std::span<std::int16_t> out) {
    const auto left = channels[0];
    const auto right = channels[1];
    for (std::size_t frame = 0; frame < frames; ++frame) {
        out[frame * 2] = to_pcm16(left[frame]);
        out[(frame * 2) + 1] = to_pcm16(right[frame]);
    }
}

}  // namespace player

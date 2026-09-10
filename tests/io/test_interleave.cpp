// The TDM interleave, tested on the host.
//
// This header lives in an ESP-IDF example and is built here anyway, deliberately:
// it is the only part of a TDM sink that can be checked without a DAC on the
// other end, and it is the part where the bugs are. Peripheral setup either
// works on hardware or does not; indexing a planar-to-interleaved transform with
// slot padding is arithmetic, and arithmetic can be wrong quietly.
//
// The header includes nothing from ESP-IDF, which is what makes this possible
// and is worth keeping true.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <span>
#include <vector>

#include "ac3forge/interleave.hpp"

namespace {

std::vector<std::span<const float>> views(const std::vector<std::vector<float>>& planes) {
    std::vector<std::span<const float>> out;
    for (const auto& plane : planes) {
        out.emplace_back(plane);
    }
    return out;
}

}  // namespace

TEST_CASE("interleave lays channels out slot by slot", "[io][interleave]") {
    // Distinct per channel AND per frame, so a transposed index shows up as a
    // wrong value rather than passing by symmetry.
    const std::vector<std::vector<float>> planes = {
        {0.1F, 0.2F, 0.3F},
        {0.4F, 0.5F, 0.6F},
    };
    const auto channels = views(planes);

    std::array<std::int32_t, 6> out{};
    const auto zeroed = ac3forge::interleave_24in32(channels, 2, 3, out);
    REQUIRE(zeroed == 0);

    for (std::size_t frame = 0; frame < 3; ++frame) {
        for (std::size_t ch = 0; ch < 2; ++ch) {
            CAPTURE(frame, ch);
            REQUIRE(out[(frame * 2) + ch] ==
                    ac3forge::to_slot_24in32(planes[ch][frame]));
        }
    }
}

TEST_CASE("interleave zeroes the slots a 5.1 programme does not fill", "[io][interleave]") {
    // The case the sink exists to get right: six channels on an eight-slot bus.
    // Slots 6 and 7 have nothing to carry and MUST be written, because the DMA
    // buffer is reused and whatever was there last frame is what the DAC clocks
    // out otherwise.
    constexpr std::size_t kSlots = 8;
    constexpr std::size_t kFrames = 4;
    std::vector<std::vector<float>> planes;
    for (int ch = 0; ch < 6; ++ch) {
        planes.emplace_back(kFrames, 0.5F);
    }
    const auto channels = views(planes);

    // Pre-filled with a value that is not zero, standing in for the previous
    // frame's contents. If the function skipped the unused slots instead of
    // zeroing them, this is what would reach the DAC.
    std::vector<std::int32_t> out(kSlots * kFrames, 0x7FFFFF00);
    const auto zeroed = ac3forge::interleave_24in32(channels, kSlots, kFrames, out);
    REQUIRE(zeroed == 2);

    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        CAPTURE(frame);
        for (std::size_t slot = 0; slot < 6; ++slot) {
            REQUIRE(out[(frame * kSlots) + slot] != 0);
        }
        REQUIRE(out[(frame * kSlots) + 6] == 0);
        REQUIRE(out[(frame * kSlots) + 7] == 0);
    }
}

TEST_CASE("interleave never writes past the slot count", "[io][interleave]") {
    // More channels than slots is a misconfiguration, not a crash: the extra
    // channels are dropped and nothing is written out of bounds.
    std::vector<std::vector<float>> planes;
    for (int ch = 0; ch < 8; ++ch) {
        planes.emplace_back(2, 0.25F);
    }
    const auto channels = views(planes);

    std::array<std::int32_t, 4> out{};  // 2 slots x 2 frames
    const auto zeroed = ac3forge::interleave_24in32(channels, 2, 2, out);
    REQUIRE(zeroed == 0);
    for (const auto slot : out) {
        REQUIRE(slot != 0);
    }
}

TEST_CASE("slot conversion is 24-bit left-justified in 32", "[io][interleave]") {
    // The low byte is always clear: a DAC takes the top 24 bits of the slot, so
    // the sample is scaled to 24-bit and shifted up rather than scaled to 32.
    REQUIRE((ac3forge::to_slot_24in32(0.5F) & 0xFF) == 0);
    REQUIRE(ac3forge::to_slot_24in32(0.0F) == 0);

    // Full scale maps to the 24-bit maximum, shifted - not to INT32_MAX, and
    // not wrapped.
    REQUIRE(ac3forge::to_slot_24in32(1.0F) == (ac3forge::kPcm24Max << 8));
    REQUIRE(ac3forge::to_slot_24in32(-1.0F) == (-ac3forge::kPcm24Max << 8));

    // Clipped, not wrapped. A wrapped sample turns a peak into full-scale noise
    // of the opposite sign, which is the loudest sound the system can make.
    REQUIRE(ac3forge::to_slot_24in32(4.0F) == (ac3forge::kPcm24Max << 8));
    REQUIRE(ac3forge::to_slot_24in32(-4.0F) == (-ac3forge::kPcm24Max << 8));

    // Monotonic across the range that matters, so a sign or shift error shows.
    REQUIRE(ac3forge::to_slot_24in32(-0.5F) < 0);
    REQUIRE(ac3forge::to_slot_24in32(0.5F) > 0);
    REQUIRE(ac3forge::to_slot_24in32(0.25F) < ac3forge::to_slot_24in32(0.75F));
}

// The TDM interleave, tested on the host.
//
// This header lives in the ESP-IDF component and is built here anyway, deliberately:
// it is the only part of a TDM sink that can be checked without a DAC on the
// other end, and it is the part where the bugs are. Peripheral setup either
// works on hardware or does not; indexing a planar-to-interleaved transform with
// slot padding is arithmetic, and arithmetic can be wrong quietly.
//
// The header includes nothing from ESP-IDF, which is what makes this possible
// and is worth keeping true.
//
// Each interleave case runs twice, once per conversion: FloatConversion, which
// a part with a floating-point unit compiles in, and BitConversion, which a
// part without one does (see the top of interleave.hpp). Both are checked
// against to_pcm16 and to_slot_24in32 themselves, the float forms, because the
// two conversions are meant to give the same integers and the float forms are
// what the sinks were written and measured against.

#include <array>
#include <bit>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#include "ac3forge/interleave.hpp"
#include "ac3forge/slot_conversion.hpp"

namespace {

std::vector<std::span<const float>> views(const std::vector<std::vector<float>>& planes) {
    std::vector<std::span<const float>> out;
    for (const auto& plane : planes) {
        out.emplace_back(plane);
    }
    return out;
}

bool is_nan_bits(std::uint32_t bits) {
    return ((bits >> 23) & 0xFFU) == 0xFFU && (bits & 0x7FFFFFU) != 0;
}

// Both bit conversions against both float ones over many bit patterns, counted
// rather than asserted one at a time: millions of REQUIREs and CAPTUREs cost
// seconds, and a failure only needs one pattern to reproduce. A NaN is skipped:
// the float forms' conversion of it is undefined, and the bits forms' answer
// has a case of its own below.
struct Comparison {
    std::uint64_t compared = 0;
    std::uint64_t pcm16_differ = 0;
    std::uint64_t slot_differ = 0;
    std::uint32_t first_differing = 0;

    void check(std::uint32_t bits) {
        if (is_nan_bits(bits)) {
            return;
        }
        const auto sample = std::bit_cast<float>(bits);
        const bool pcm16 = ac3forge::to_pcm16_from_bits(sample) != ac3forge::to_pcm16(sample);
        const bool slot =
            ac3forge::to_slot_24in32_from_bits(sample) != ac3forge::to_slot_24in32(sample);
        if ((pcm16 || slot) && pcm16_differ + slot_differ == 0) {
            first_differing = bits;
        }
        pcm16_differ += pcm16 ? 1U : 0U;
        slot_differ += slot ? 1U : 0U;
        ++compared;
    }

    void require_none_differ() const {
        CAPTURE(compared, first_differing);
        REQUIRE(compared > 0);
        REQUIRE(pcm16_differ == 0);
        REQUIRE(slot_differ == 0);
    }
};

}  // namespace

TEMPLATE_TEST_CASE("interleave lays channels out slot by slot", "[io][interleave]",
                   ac3forge::FloatConversion, ac3forge::BitConversion) {
    // Distinct per channel AND per frame, so a transposed index shows up as a
    // wrong value rather than passing by symmetry.
    const std::vector<std::vector<float>> planes = {
        {0.1F, 0.2F, 0.3F},
        {0.4F, 0.5F, 0.6F},
    };
    const auto channels = views(planes);

    std::array<std::int32_t, 6> out{};
    const auto zeroed = ac3forge::interleave_24in32<TestType>(channels, 2, 3, out);
    REQUIRE(zeroed == 0);

    for (std::size_t frame = 0; frame < 3; ++frame) {
        for (std::size_t ch = 0; ch < 2; ++ch) {
            CAPTURE(frame, ch);
            REQUIRE(out[(frame * 2) + ch] ==
                    ac3forge::to_slot_24in32(planes[ch][frame]));
        }
    }
}

TEMPLATE_TEST_CASE("interleave zeroes the slots a 5.1 programme does not fill",
                   "[io][interleave]", ac3forge::FloatConversion, ac3forge::BitConversion) {
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
    const auto zeroed = ac3forge::interleave_24in32<TestType>(channels, kSlots, kFrames, out);
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

TEMPLATE_TEST_CASE("interleave never writes past the slot count", "[io][interleave]",
                   ac3forge::FloatConversion, ac3forge::BitConversion) {
    // More channels than slots is a misconfiguration, not a crash: the extra
    // channels are dropped and nothing is written out of bounds.
    std::vector<std::vector<float>> planes;
    for (int ch = 0; ch < 8; ++ch) {
        planes.emplace_back(2, 0.25F);
    }
    const auto channels = views(planes);

    std::array<std::int32_t, 4> out{};  // 2 slots x 2 frames
    const auto zeroed = ac3forge::interleave_24in32<TestType>(channels, 2, 2, out);
    REQUIRE(zeroed == 0);
    for (const auto slot : out) {
        REQUIRE(slot != 0);
    }
}

TEMPLATE_TEST_CASE("16-bit TDM lays channels out slot by slot and zeroes the rest",
                   "[io][interleave]", ac3forge::FloatConversion, ac3forge::BitConversion) {
    // Seven-point-one-shaped: eight slots, distinct values per channel and per
    // frame so a transposed index reads as a wrong value.
    constexpr std::size_t kSlots = 8;
    constexpr std::size_t kFrames = 3;
    std::vector<std::vector<float>> planes;
    for (std::size_t ch = 0; ch < kSlots; ++ch) {
        std::vector<float> plane;
        for (std::size_t frame = 0; frame < kFrames; ++frame) {
            plane.push_back(0.05F * static_cast<float>(ch + 1) + 0.01F * static_cast<float>(frame));
        }
        planes.push_back(plane);
    }
    const auto channels = views(planes);

    std::vector<std::int16_t> out(kSlots * kFrames, 0);
    REQUIRE(ac3forge::interleave_16in16<TestType>(channels, kSlots, kFrames, out) == 0);
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        for (std::size_t ch = 0; ch < kSlots; ++ch) {
            CAPTURE(frame, ch);
            REQUIRE(out[(frame * kSlots) + ch] == ac3forge::to_pcm16(planes[ch][frame]));
        }
    }

    // A 5.1 programme on the same eight slots: the last two are written as
    // zeros over whatever the reused buffer held, as interleave_24in32 does.
    const auto six = std::span<const std::span<const float>>{channels}.subspan(0, 6);
    std::vector<std::int16_t> padded(kSlots * kFrames, 0x7F00);
    REQUIRE(ac3forge::interleave_16in16<TestType>(six, kSlots, kFrames, padded) == 2);
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        CAPTURE(frame);
        for (std::size_t slot = 0; slot < 6; ++slot) {
            REQUIRE(padded[(frame * kSlots) + slot] == ac3forge::to_pcm16(planes[slot][frame]));
        }
        REQUIRE(padded[(frame * kSlots) + 6] == 0);
        REQUIRE(padded[(frame * kSlots) + 7] == 0);
    }
}

TEMPLATE_TEST_CASE("16-bit TDM takes a later line's channels from a subspan", "[io][interleave]",
                   ac3forge::FloatConversion, ac3forge::BitConversion) {
    // How a second line would call it: the channels past line 0's, into a
    // frame of the same width, the unused tail zeroed.
    std::vector<std::vector<float>> planes;
    for (int ch = 0; ch < 10; ++ch) {
        planes.emplace_back(2, 0.1F * static_cast<float>(ch + 1));
    }
    const auto channels = views(planes);
    const auto rest = std::span<const std::span<const float>>{channels}.subspan(8, 2);

    std::array<std::int16_t, 16> out{};
    out.fill(0x1234);
    REQUIRE(ac3forge::interleave_16in16<TestType>(rest, 8, 2, out) == 6);
    for (std::size_t frame = 0; frame < 2; ++frame) {
        CAPTURE(frame);
        REQUIRE(out[frame * 8] == ac3forge::to_pcm16(planes[8][frame]));
        REQUIRE(out[(frame * 8) + 1] == ac3forge::to_pcm16(planes[9][frame]));
        for (std::size_t slot = 2; slot < 8; ++slot) {
            REQUIRE(out[(frame * 8) + slot] == 0);
        }
    }
}

TEMPLATE_TEST_CASE("16-bit TDM never writes past the slot count", "[io][interleave]",
                   ac3forge::FloatConversion, ac3forge::BitConversion) {
    std::vector<std::vector<float>> planes;
    for (int ch = 0; ch < 8; ++ch) {
        planes.emplace_back(2, 0.25F);
    }
    const auto channels = views(planes);

    // Two slots of two frames, with a guard entry after them that must survive.
    std::array<std::int16_t, 5> out{};
    out[4] = 0x5A5A;
    REQUIRE(ac3forge::interleave_16in16<TestType>(channels, 2, 2,
                                                  std::span<std::int16_t>{out.data(), 4}) == 0);
    for (std::size_t i = 0; i < 4; ++i) {
        REQUIRE(out[i] == ac3forge::to_pcm16(0.25F));
    }
    REQUIRE(out[4] == 0x5A5A);
}

TEMPLATE_TEST_CASE("16-bit stereo pairs interleave left then right", "[io][interleave]",
                   ac3forge::FloatConversion, ac3forge::BitConversion) {
    const std::vector<std::vector<float>> planes = {
        {0.125F, -0.25F, 0.375F},
        {-0.5F, 0.625F, -0.75F},
    };
    const auto channels = views(planes);

    std::array<std::int16_t, 6> out{};
    ac3forge::interleave_16<TestType>(channels, 3, out);
    for (std::size_t frame = 0; frame < 3; ++frame) {
        CAPTURE(frame);
        REQUIRE(out[frame * 2] == ac3forge::to_pcm16(planes[0][frame]));
        REQUIRE(out[(frame * 2) + 1] == ac3forge::to_pcm16(planes[1][frame]));
    }
}

TEST_CASE("an interleave with no conversion named uses the part's", "[io][interleave]") {
    // What the sinks do: call the interleave with no template argument and get
    // the conversion ac3forge/slot_conversion.hpp names. On the host that is the
    // float one (tests/CMakeLists.txt), and a part without an FPU gets the other
    // from the component's CMakeLists.txt.
    STATIC_REQUIRE(std::is_same_v<ac3forge::SlotConversion, ac3forge::FloatConversion>);

    const std::vector<std::vector<float>> planes = {{0.3F, -0.7F}, {0.9F, -0.1F}};
    const auto channels = views(planes);
    std::array<std::int16_t, 4> out{};
    REQUIRE(ac3forge::interleave_16in16(channels, 2, 2, out) == 0);
    REQUIRE(out[0] == ac3forge::to_pcm16(0.3F));
    REQUIRE(out[3] == ac3forge::to_pcm16(-0.1F));
}

TEMPLATE_TEST_CASE("16-bit conversion is full scale at +-1 and clipped past it",
                   "[io][interleave]", ac3forge::FloatConversion, ac3forge::BitConversion) {
    REQUIRE(TestType::pcm16(0.0F) == 0);
    REQUIRE(TestType::pcm16(1.0F) == 32767);
    REQUIRE(TestType::pcm16(-1.0F) == -32767);
    REQUIRE(TestType::pcm16(4.0F) == 32767);
    REQUIRE(TestType::pcm16(-4.0F) == -32767);
    REQUIRE(TestType::pcm16(-0.5F) < 0);
    REQUIRE(TestType::pcm16(0.25F) < TestType::pcm16(0.75F));
}

TEMPLATE_TEST_CASE("slot conversion is 24-bit left-justified in 32", "[io][interleave]",
                   ac3forge::FloatConversion, ac3forge::BitConversion) {
    // The low byte is always clear: a DAC takes the top 24 bits of the slot, so
    // the sample is scaled to 24-bit and shifted up rather than scaled to 32.
    REQUIRE((TestType::slot_24in32(0.5F) & 0xFF) == 0);
    REQUIRE(TestType::slot_24in32(0.0F) == 0);

    // Full scale maps to the 24-bit maximum, shifted - not to INT32_MAX, and
    // not wrapped.
    REQUIRE(TestType::slot_24in32(1.0F) == (ac3forge::kPcm24Max << 8));
    REQUIRE(TestType::slot_24in32(-1.0F) == (-ac3forge::kPcm24Max << 8));

    // Clipped, not wrapped. A wrapped sample turns a peak into full-scale noise
    // of the opposite sign, which is the loudest sound the system can make.
    REQUIRE(TestType::slot_24in32(4.0F) == (ac3forge::kPcm24Max << 8));
    REQUIRE(TestType::slot_24in32(-4.0F) == (-ac3forge::kPcm24Max << 8));

    // Monotonic across the range that matters, so a sign or shift error shows.
    REQUIRE(TestType::slot_24in32(-0.5F) < 0);
    REQUIRE(TestType::slot_24in32(0.5F) > 0);
    REQUIRE(TestType::slot_24in32(0.25F) < TestType::slot_24in32(0.75F));
}

TEST_CASE("the conversions from bits equal the float ones at every output step",
          "[io][interleave]") {
    // Where the two could disagree is where a truncation lands: a product just
    // below an integer, which the float multiply may round up onto it. So every
    // 16-bit step k / 32767, and every 61st 24-bit step k / 8388607, is checked
    // with the three floats either side of it, both signs.
    Comparison comparison;
    const auto around = [&comparison](float centre) {
        const auto bits = std::bit_cast<std::uint32_t>(centre);
        for (std::uint32_t offset = 0; offset <= 6; ++offset) {
            const std::uint32_t pattern = bits + offset - 3;
            comparison.check(pattern);
            comparison.check(pattern | 0x80000000U);
        }
    };
    for (int k = 1; k <= 32767; ++k) {
        around(static_cast<float>(k) / 32767.0F);
    }
    for (int k = 1; k <= ac3forge::kPcm24Max; k += 61) {
        around(static_cast<float>(k) / static_cast<float>(ac3forge::kPcm24Max));
    }
    // The largest 24-bit step below full scale, which the stride does not land on.
    around(static_cast<float>(ac3forge::kPcm24Max - 1) / static_cast<float>(ac3forge::kPcm24Max));
    comparison.require_none_differ();
}

TEST_CASE("the conversions from bits equal the float ones across the float range",
          "[io][interleave]") {
    // A fixed sequence of bit patterns from every exponent, sign and mantissa -
    // the host run behind interleave.hpp's comment compared all 2^32 - and each
    // exponent's lowest and highest mantissa.
    Comparison comparison;
    std::uint32_t state = 0x9E3779B9U;
    for (int i = 0; i < (1 << 20); ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        comparison.check(state);
    }
    for (std::uint32_t biased = 0; biased < 255; ++biased) {
        for (const std::uint32_t mantissa : {0U, 1U, 0x400000U, 0x7FFFFEU, 0x7FFFFFU}) {
            const std::uint32_t bits = (biased << 23) | mantissa;
            comparison.check(bits);
            comparison.check(bits | 0x80000000U);
        }
    }
    comparison.require_none_differ();
}

TEST_CASE("the conversions from bits handle the edges of the float format", "[io][interleave]") {
    const auto from = [](std::uint32_t bits) { return std::bit_cast<float>(bits); };

    // A NaN of either sign, quiet or signalling, is silence.
    for (const std::uint32_t nan : {0x7FC00000U, 0xFFC00000U, 0x7F800001U, 0xFFFFFFFFU}) {
        CAPTURE(nan);
        REQUIRE(ac3forge::to_pcm16_from_bits(from(nan)) == 0);
        REQUIRE(ac3forge::to_slot_24in32_from_bits(from(nan)) == 0);
    }

    // An infinity is past full scale and clips, as it does in float.
    REQUIRE(ac3forge::to_pcm16_from_bits(from(0x7F800000U)) == 32767);
    REQUIRE(ac3forge::to_pcm16_from_bits(from(0xFF800000U)) == -32767);
    REQUIRE(ac3forge::to_slot_24in32_from_bits(from(0x7F800000U)) == (ac3forge::kPcm24Max << 8));
    REQUIRE(ac3forge::to_slot_24in32_from_bits(from(0xFF800000U)) == (-ac3forge::kPcm24Max << 8));

    // Negative zero and the subnormals are zero.
    for (const std::uint32_t tiny : {0x80000000U, 0x00000001U, 0x007FFFFFU, 0x807FFFFFU}) {
        CAPTURE(tiny);
        REQUIRE(ac3forge::to_pcm16_from_bits(from(tiny)) == 0);
        REQUIRE(ac3forge::to_slot_24in32_from_bits(from(tiny)) == 0);
    }

    // The largest float below 1 does not reach full scale.
    REQUIRE(ac3forge::to_pcm16_from_bits(from(0x3F7FFFFFU)) == 32766);
    REQUIRE(ac3forge::to_slot_24in32_from_bits(from(0x3F7FFFFFU)) == (8388606 << 8));

    // The first inputs, counting up, where truncating the exact product would be
    // one step short: 2^-15 + 2^-30 times 32767 is 1 - 2^-30, and
    // (2^-23 + 2^-46) times 8388607 is 1 - 2^-46, both of which a float
    // multiply rounds to exactly 1.
    REQUIRE(ac3forge::to_pcm16_from_bits(from(0x38000100U)) == 1);
    REQUIRE(ac3forge::to_pcm16(from(0x38000100U)) == 1);
    REQUIRE(ac3forge::to_slot_24in32_from_bits(from(0x34000001U)) == 256);
    REQUIRE(ac3forge::to_slot_24in32(from(0x34000001U)) == 256);

    // Integer arithmetic, so a constant expression too.
    STATIC_REQUIRE(ac3forge::to_pcm16_from_bits(0.5F) == 16383);
    STATIC_REQUIRE(ac3forge::to_slot_24in32_from_bits(-0.5F) == (-4194303 << 8));
}

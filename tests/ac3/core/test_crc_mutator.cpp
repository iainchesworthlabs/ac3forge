#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "crc_mutator.hpp"

// fuzz/crc_mutator.hpp's re-stamping half, held to the only thing that
// actually matters about it: after corrupting a byte inside a syncframe, does
// the decoder stop rejecting the frame for its CRC?
//
// The mutator is a fuzzing-only facility, but its correctness is not a
// fuzzing-only question - a re-stamp that silently produced the wrong crc1
// would leave every mutation dying at the checksum exactly as before, and
// the only symptom would be a fuzzing run that quietly explored less than it
// claimed to. crc1 in particular is the easy one to get wrong: it precedes
// the region it protects and has to be solved for through a GF(2) inverse,
// not recomputed. So it is checked here, in the portable test binary that
// runs on every platform, rather than only implied by a coverage number from
// a Clang-only build.
//
// Only restamp_syncframe_crcs is exercised. crc_repairing_mutate calls
// libFuzzer's own LLVMFuzzerMutate, which exists only in a -fsanitize=fuzzer
// link; it is an inline function this translation unit never calls, so
// nothing here needs that symbol.

namespace {

std::vector<float> tone(int samples) {
    std::vector<float> out(static_cast<std::size_t>(samples));
    for (int n = 0; n < samples; ++n) {
        // A cheap non-silent, non-constant signal: silence encodes to a frame
        // whose mantissa area is mostly padding, which is a poor place to
        // corrupt a byte and claim the decode was reached.
        out[static_cast<std::size_t>(n)] = 0.4f * static_cast<float>(((n * 37) % 101) - 50) / 50.0f;
    }
    return out;
}

// Corrupts one byte a third of the way into the frame. Inside crc1's region
// (A/52 §7.10.1's first 5/8) deliberately, and crc2 covers the whole frame,
// so BOTH AC-3 CRC words are wrong afterwards - repairing only crc2, or
// recomputing crc1 as if it were an ordinary trailing CRC, would leave the
// frame rejected.
void corrupt_third(std::span<std::byte> frame) {
    frame[frame.size() / 3] ^= std::byte{0x5A};
}

}  // namespace

TEST_CASE("restamp_syncframe_crcs makes a corrupted AC-3 frame pass the CRC check again",
          "[crc16][fuzz]") {
    ac3::FrameEncoder encoder{{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0}};
    const auto left = tone(ac3::kSamplesPerFrame);
    const auto right = tone(ac3::kSamplesPerFrame);
    const std::array<std::span<const float>, 2> channels{left, right};
    const auto encoded = encoder.encode_frame(channels);
    REQUIRE(encoded.has_value());

    std::vector<std::byte> frame = *encoded;
    corrupt_third(frame);

    ac3::FrameDecoder before;
    const auto rejected = before.decode_frame(frame);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error() == ac3::DecodeError::kBadCrc);

    ac3fuzz::restamp_syncframe_crcs(frame);

    // Same corrupted payload, only the two CRC words rewritten - so whatever
    // the decoder makes of the frame now, it is no longer refusing it at the
    // checksum, which is the whole point.
    ac3::FrameDecoder after;
    const auto result = after.decode_frame(frame);
    if (!result.has_value()) {
        CHECK(result.error() != ac3::DecodeError::kBadCrc);
    }
}

TEST_CASE("restamp_syncframe_crcs makes a corrupted E-AC-3 frame pass the CRC check again",
          "[crc16][fuzz]") {
    ac3::eac3::FrameEncoder encoder{{.bitrate_kbps = 192}};
    const auto left = tone(ac3::kSamplesPerFrame);
    const auto right = tone(ac3::kSamplesPerFrame);
    const std::array<std::span<const float>, 2> channels{left, right};
    const auto encoded = encoder.encode_frame(channels);
    REQUIRE(encoded.has_value());

    std::vector<std::byte> frame = *encoded;
    corrupt_third(frame);

    ac3::Eac3Decoder before;
    const auto rejected = before.decode_access_unit(frame);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error() == ac3::DecodeError::kBadCrc);

    ac3fuzz::restamp_syncframe_crcs(frame);

    ac3::Eac3Decoder after;
    const auto result = after.decode_access_unit(frame);
    if (!result.has_value()) {
        CHECK(result.error() != ac3::DecodeError::kBadCrc);
    }
}

TEST_CASE("restamp_syncframe_crcs leaves bytes that are not a syncframe alone", "[crc16][fuzz]") {
    // The walk stops at the first thing it cannot size, rather than writing
    // somewhere it guessed at - a mutation that destroyed the sync word must
    // not have CRC words stamped into arbitrary offsets of whatever follows.
    std::vector<std::byte> junk(64, std::byte{0xAB});
    const std::vector<std::byte> before = junk;
    ac3fuzz::restamp_syncframe_crcs(junk);
    CHECK(junk == before);

    // ... and a frame whose declared size runs past the buffer is left alone
    // too, for the same reason (ac3::split_frames calls this kTruncated).
    std::vector<std::byte> truncated{std::byte{0x0B}, std::byte{0x77}, std::byte{0x00},
                                     std::byte{0x20}, std::byte{0x00}, std::byte{0x58}};
    const std::vector<std::byte> truncated_before = truncated;
    ac3fuzz::restamp_syncframe_crcs(truncated);
    CHECK(truncated == truncated_before);
}

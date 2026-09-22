#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include "ac3/encoder/silent_frame.hpp"
#include "ac3/io/elementary.hpp"
#include "container_input.hpp"

namespace {

using Bytes = std::vector<std::byte>;

}  // namespace

// Roadmap IO2's own regression (tools/ci/fuzz_encoder_space.py's
// REGRESSION_SEEDS, seed 3600083275727211684): AC-3 at 48 kbit/s and 48 kHz
// codes exactly 192-byte frames - one of MPEG-TS's own three packet grid
// strides - so a stream whose frames repeat a byte at that same offset every
// frame (a low-entropy signal encodes near-identically frame to frame) looks
// exactly like a packet grid to a check that only counts recurrence. This
// builds that collision directly rather than relying on a fuzz seed landing
// on it: a real, syntactically-valid AC-3 header (so sniff_container's own
// elementary-stream check has something genuine to find) padded with 0x47 at
// a fixed 192-byte stride for several "frames" in a row.
TEST_CASE("sniff_container does not mistake a repetitive elementary stream for MPEG-TS",
          "[containers][io2]") {
    // The smallest legal AC-3 frame at 48 kHz (Table 5.18's lowest rung,
    // 32 kbit/s) is 64 words = 128 bytes - ac3::io::read_frame_header only
    // ever reads the syncinfo/bsi header (well under 128 bytes for a plain
    // 2/0 layout), so everything past it is free to overwrite.
    const auto frame = ac3::build_silent_stereo_frame({.bitrate_kbps = 32});
    REQUIRE(frame.has_value());
    REQUIRE(ac3::io::read_frame_header(*frame).has_value());

    constexpr std::size_t kStride = 192;    // one of the three grid strides
    constexpr int kRepeats = 8;             // past kTsSyncRuns's own 5
    Bytes stream(kStride * kRepeats, std::byte{0x00});
    // The real header goes at the very start, untouched - what makes this a
    // genuine elementary stream and not just an arbitrary byte pattern. The
    // 0x47 recurrence starts one stride later, at i=1, so it never
    // overwrites the sync word this test depends on: the grid still needs
    // only 5 consecutive hits, and i=1..7 already gives it 7.
    std::ranges::copy(*frame, stream.begin());
    for (int i = 1; i < kRepeats; ++i) {
        stream[static_cast<std::size_t>(i) * kStride] = std::byte{0x47};
    }

    CHECK(ac3::apps::sniff_container(stream) == ac3::apps::ContainerKind::kUnknown);
}

TEST_CASE("sniff_container still finds a real MPEG-TS packet grid", "[containers][io2]") {
    // The negative case beside the one above: a plain 0x47 recurrence with
    // no valid frame header anywhere in it (the read_frame_header check
    // fails immediately on all-zero bytes) is still read as a transport
    // stream - the fix narrows the false positive, it does not disable the
    // grid check.
    constexpr std::size_t kStride = 188;
    constexpr int kRepeats = 6;
    Bytes stream(kStride * kRepeats, std::byte{0x00});
    for (int i = 0; i < kRepeats; ++i) {
        stream[static_cast<std::size_t>(i) * kStride] = std::byte{0x47};
    }

    CHECK(ac3::apps::sniff_container(stream) == ac3::apps::ContainerKind::kMpegTs);
}

TEST_CASE("elementary_stream_from_bytes leaves a bare elementary stream untouched",
          "[containers][io2]") {
    const auto frame = ac3::build_silent_stereo_frame({.bitrate_kbps = 192});
    REQUIRE(frame.has_value());

    const auto result = ac3::apps::elementary_stream_from_bytes(*frame);
    CHECK(result.error.empty());
    CHECK(result.bytes == *frame);
}

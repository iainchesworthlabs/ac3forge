#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "ac3/sendspin/frames.hpp"

// Transport-mode framing. Fragmentation is where the specification and
// aiosendspin 9.1.1 (Music Assistant's server) differ on the wire, so each form
// is checked byte for byte against how its own text or source builds a frame,
// and the reassembler against every malformed sequence the specification makes
// fatal.

namespace {

using ac3::sendspin::Dialect;
using ac3::sendspin::FrameError;
using ac3::sendspin::kMaxFramePlaintext;
using ac3::sendspin::Reassembler;
namespace message_id = ac3::sendspin::message_id;

std::vector<std::uint8_t> make_message(std::uint8_t id, std::size_t payload_bytes) {
    std::vector<std::uint8_t> message(1 + payload_bytes);
    message[0] = id;
    for (std::size_t i = 1; i < message.size(); ++i) {
        message[i] = static_cast<std::uint8_t>((i * 131) ^ (i >> 8));
    }
    return message;
}

std::vector<std::vector<std::uint8_t>> split(std::span<const std::uint8_t> message,
                                             Dialect dialect) {
    std::vector<std::vector<std::uint8_t>> frames;
    std::vector<std::uint8_t> frame(kMaxFramePlaintext);
    const std::size_t count = ac3::sendspin::frame_count(message.size(), dialect);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t written = ac3::sendspin::write_frame(message, i, frame, dialect);
        frames.emplace_back(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(written));
    }
    CHECK(ac3::sendspin::write_frame(message, count, frame, dialect) == 0);
    return frames;
}

// aiosendspin 9.1.1's noise/wire.py _fragment(), transcribed.
std::vector<std::vector<std::uint8_t>> aiosendspin_fragment(std::span<const std::uint8_t> message) {
    const std::size_t first_cap = kMaxFramePlaintext - 2;
    const std::size_t cont_cap = kMaxFramePlaintext - 1;
    const std::span<const std::uint8_t> data = message.subspan(1);
    std::vector<std::vector<std::uint8_t>> frames;
    std::vector<std::uint8_t> first{2, message[0]};
    first.insert(first.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(first_cap));
    frames.push_back(std::move(first));
    const std::span<const std::uint8_t> rest = data.subspan(first_cap);
    for (std::size_t at = 0; at < rest.size(); at += cont_cap) {
        const std::size_t n = std::min(cont_cap, rest.size() - at);
        const bool last = at + n == rest.size();
        std::vector<std::uint8_t> frame{static_cast<std::uint8_t>(last ? 3 : 2)};
        frame.insert(frame.end(), rest.begin() + static_cast<std::ptrdiff_t>(at),
                     rest.begin() + static_cast<std::ptrdiff_t>(at + n));
        frames.push_back(std::move(frame));
    }
    return frames;
}

std::vector<std::uint8_t> reassemble(Reassembler& reassembler,
                                     const std::vector<std::vector<std::uint8_t>>& frames) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const Reassembler::Result result = reassembler.push(frames[i]);
        REQUIRE(result.error == FrameError::kNone);
        CHECK(result.message.empty() == (i + 1 < frames.size()));
        if (!result.message.empty()) {
            out.assign(result.message.begin(), result.message.end());
        }
    }
    return out;
}

}  // namespace

TEST_CASE("frames: the size limits follow from Noise's 65,535 bytes", "[sendspin][frames]") {
    CHECK(ac3::sendspin::kMaxFramePlaintext == 65519);
    CHECK(ac3::sendspin::kMaxFramePayload == 65518);
    CHECK(ac3::sendspin::frame_count(1, Dialect::kSpecification) == 1);
    CHECK(ac3::sendspin::frame_count(65519, Dialect::kSpecification) == 1);
    CHECK(ac3::sendspin::frame_count(65520, Dialect::kSpecification) == 2);
    CHECK(ac3::sendspin::frame_count(65520, Dialect::kAiosendspin911) == 2);
    // The specification's first frame carries 65,516 data bytes and later ones
    // 65,517; aiosendspin's 65,517 and 65,518.
    CHECK(ac3::sendspin::frame_count(1 + 65516 + 65517, Dialect::kSpecification) == 2);
    CHECK(ac3::sendspin::frame_count(1 + 65516 + 65517 + 1, Dialect::kSpecification) == 3);
    CHECK(ac3::sendspin::frame_count(1 + 65517 + 65518, Dialect::kAiosendspin911) == 2);
    CHECK(ac3::sendspin::frame_count(1 + 65517 + 65518 + 1, Dialect::kAiosendspin911) == 3);
}

TEST_CASE("frames: a message that fits goes out as itself", "[sendspin][frames]") {
    const std::vector<std::uint8_t> message = make_message(message_id::kJson, 65518);
    const auto frames = split(message, Dialect::kSpecification);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0] == message);

    Reassembler reassembler(1 << 20);
    const Reassembler::Result result = reassembler.push(message);
    CHECK(result.error == FrameError::kNone);
    CHECK(result.message.data() == message.data());
    CHECK(result.message.size() == message.size());
}

TEST_CASE("frames: the specification's form, header by header", "[sendspin][frames]") {
    const std::vector<std::uint8_t> message = make_message(message_id::kArtworkFirst, 200000);
    const auto frames = split(message, Dialect::kSpecification);
    REQUIRE(frames.size() == 4);
    CHECK(frames[0][0] == 1);
    CHECK(frames[0][1] == 0x02);
    CHECK(frames[0][2] == message_id::kArtworkFirst);
    CHECK(frames[1][0] == 1);
    CHECK(frames[1][1] == 0x00);
    CHECK(frames[3][0] == 1);
    CHECK(frames[3][1] == 0x01);
    for (const auto& frame : frames) {
        CHECK(frame.size() <= kMaxFramePlaintext);
    }
    CHECK(frames[0].size() == kMaxFramePlaintext);
    CHECK(frames[1].size() == kMaxFramePlaintext);

    Reassembler reassembler(1 << 20);
    CHECK(reassemble(reassembler, frames) == message);
    CHECK_FALSE(reassembler.in_flight());
}

TEST_CASE("frames: aiosendspin 9.1.1's form matches its own splitter", "[sendspin][frames]") {
    for (const std::size_t payload : {std::size_t{65519}, std::size_t{65518 + 65518},
                                      std::size_t{300001}}) {
        INFO(payload);
        const std::vector<std::uint8_t> message = make_message(message_id::kJson, payload);
        const auto ours = split(message, Dialect::kAiosendspin911);
        CHECK(ours == aiosendspin_fragment(message));

        Reassembler reassembler(1 << 20);
        CHECK(reassemble(reassembler, ours) == message);
    }
}

TEST_CASE("frames: both forms reassemble on one connection, one after the other",
          "[sendspin][frames]") {
    Reassembler reassembler(1 << 20);
    const std::vector<std::uint8_t> a = make_message(message_id::kJson, 70000);
    const std::vector<std::uint8_t> b = make_message(message_id::kPlayerAudio, 90000);
    CHECK(reassemble(reassembler, split(a, Dialect::kSpecification)) == a);
    CHECK(reassemble(reassembler, split(b, Dialect::kAiosendspin911)) == b);
    const std::vector<std::uint8_t> small = make_message(message_id::kJson, 10);
    CHECK(reassembler.push(small).message.size() == small.size());
}

TEST_CASE("frames: a fragmented message in a single frame is delivered", "[sendspin][frames]") {
    Reassembler reassembler(1 << 20);
    const std::vector<std::uint8_t> frame{1, 0x03, message_id::kJson, '{', '}'};
    const Reassembler::Result result = reassembler.push(frame);
    CHECK(result.error == FrameError::kNone);
    CHECK(std::vector<std::uint8_t>(result.message.begin(), result.message.end()) ==
          std::vector<std::uint8_t>{message_id::kJson, '{', '}'});
}

TEST_CASE("frames: every malformed sequence is fatal and leaves the reassembler empty",
          "[sendspin][frames]") {
    const auto push_all = [](Reassembler& r, const std::vector<std::vector<std::uint8_t>>& frames) {
        FrameError last = FrameError::kNone;
        for (const auto& frame : frames) {
            last = r.push(frame).error;
            if (last != FrameError::kNone) {
                break;
            }
        }
        return last;
    };
    const auto check = [&](const std::vector<std::vector<std::uint8_t>>& frames, FrameError want) {
        Reassembler r(1 << 20);
        CHECK(push_all(r, frames) == want);
        CHECK_FALSE(r.in_flight());
    };

    check({{}}, FrameError::kEmpty);
    check({{1}}, FrameError::kTruncatedFragment);
    check({{1, 0x02}}, FrameError::kTruncatedFragment);
    check({{2}}, FrameError::kTruncatedFragment);
    check({{1, 0x04, 0, 'x'}}, FrameError::kReservedFlags);
    check({{1, 0x80, 'x'}}, FrameError::kReservedFlags);
    check({{1, 0x00, 'x'}}, FrameError::kContinuationWithNone);
    check({{1, 0x01, 'x'}}, FrameError::kContinuationWithNone);
    check({{3, 'x'}}, FrameError::kContinuationWithNone);
    check({{1, 0x02, 0, 'a'}, {1, 0x02, 0, 'b'}}, FrameError::kFirstWhileInFlight);
    check({{1, 0x02, 0, 'a'}, {0, 'b'}}, FrameError::kInterleaved);
    check({{2, 0, 'a'}, {4, 'b'}}, FrameError::kInterleaved);
    check({{1, 0x02, 0, 'a'}, {2, 'b'}}, FrameError::kMixedForms);
    check({{1, 0x02, 0, 'a'}, {3, 'b'}}, FrameError::kMixedForms);
    check({{2, 0, 'a'}, {1, 0x01, 'b'}}, FrameError::kMixedForms);
    check({{1, 0x03, 1, 'a'}}, FrameError::kBadOriginalId);
    check({{1, 0x03, 2, 'a'}}, FrameError::kBadOriginalId);
    check({{2, 3, 'a'}}, FrameError::kBadOriginalId);
}

TEST_CASE("frames: the reassembly limit counts the whole message", "[sendspin][frames]") {
    const std::vector<std::uint8_t> message = make_message(message_id::kJson, 100000);
    {
        Reassembler exact(message.size());
        CHECK(reassemble(exact, split(message, Dialect::kSpecification)) == message);
    }
    Reassembler tight(message.size() - 1);
    const auto frames = split(message, Dialect::kSpecification);
    CHECK(tight.push(frames[0]).error == FrameError::kNone);
    CHECK(tight.push(frames[1]).error == FrameError::kTooLarge);
    CHECK_FALSE(tight.in_flight());

    Reassembler tiny(4);
    CHECK(tiny.push(std::vector<std::uint8_t>{0, 1, 2, 3, 4}).error == FrameError::kTooLarge);
}

TEST_CASE("frames: a delivered message stays valid until the next push", "[sendspin][frames]") {
    Reassembler reassembler(1 << 20);
    const std::vector<std::uint8_t> message = make_message(message_id::kJson, 80000);
    const auto frames = split(message, Dialect::kSpecification);
    Reassembler::Result result;
    for (const auto& frame : frames) {
        result = reassembler.push(frame);
    }
    REQUIRE(result.message.size() == message.size());
    CHECK(std::equal(result.message.begin(), result.message.end(), message.begin()));
    reassembler.reset();
    CHECK_FALSE(reassembler.in_flight());
}

// ac4::SyncFrameSplitter: the sync frames of a stream that arrives in pieces,
// held to what ac4::scan() finds in the whole stream at once.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"

namespace {

namespace fs = std::filesystem;

std::vector<std::byte> read_leg(const std::string& leg) {
    const fs::path path = fs::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / leg / "dee.ac4";
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    for (const unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

// A frame the splitter handed over, copied out of its storage.
struct Split {
    std::size_t offset = 0;
    std::uint16_t sync_word = 0;
    std::vector<std::byte> raw;
    std::optional<bool> crc_ok;

    bool operator==(const Split&) const = default;
};

struct Outcome {
    std::vector<Split> frames;
    bool truncated = false;
    bool too_small = false;
    std::size_t skipped = 0;
};

// Feeds `stream` to a splitter over `capacity` bytes of storage in pieces of
// `piece`, then finishes it.
Outcome split(std::span<const std::byte> stream, std::size_t piece,
              std::size_t capacity = ac4::kSplitterRecommendedBuffer) {
    std::vector<std::byte> storage(capacity);
    ac4::SyncFrameSplitter splitter{storage};
    Outcome out;
    std::size_t fed = 0;
    for (int guard = 0; guard < 10'000'000; ++guard) {
        const ac4::SyncFrameSplitter::Result next = splitter.next();
        switch (next.status) {
            case ac4::SyncFrameSplitter::Status::kFrame:
                out.frames.push_back(
                    Split{.offset = next.frame.offset,
                          .sync_word = next.frame.sync_word,
                          .raw = {next.frame.raw_ac4_frame.begin(), next.frame.raw_ac4_frame.end()},
                          .crc_ok = next.frame.crc_ok});
                continue;
            case ac4::SyncFrameSplitter::Status::kNeedMoreInput: {
                if (fed == stream.size()) {
                    splitter.finish();
                    continue;
                }
                const std::span<std::byte> space = splitter.writable();
                REQUIRE_FALSE(space.empty());
                const std::size_t n = std::min({piece, space.size(), stream.size() - fed});
                std::copy_n(stream.begin() + static_cast<std::ptrdiff_t>(fed), n, space.begin());
                splitter.commit(n);
                fed += n;
                continue;
            }
            case ac4::SyncFrameSplitter::Status::kTruncated:
                out.truncated = true;
                continue;
            case ac4::SyncFrameSplitter::Status::kBufferTooSmall:
                out.too_small = true;
                break;
            case ac4::SyncFrameSplitter::Status::kEndOfStream:
                break;
        }
        break;
    }
    out.skipped = splitter.resynchronised_bytes();
    return out;
}

std::vector<Split> scanned(std::span<const std::byte> stream, std::size_t shift = 0) {
    std::vector<Split> out;
    for (const ac4::SyncFrame& frame : ac4::scan(stream).frames) {
        out.push_back(Split{.offset = frame.offset + shift,
                            .sync_word = frame.sync_word,
                            .raw = {frame.raw_ac4_frame.begin(), frame.raw_ac4_frame.end()},
                            .crc_ok = frame.crc_ok});
    }
    return out;
}

}  // namespace

TEST_CASE("SyncFrameSplitter hands over the frames scan finds whatever the pieces",
          "[ac4][splitter]") {
    for (const std::string leg : {"ac4-stereo-64", "ac4-51-music-384", "ac4-ims-music-64-2997"}) {
        CAPTURE(leg);
        const std::vector<std::byte> stream = read_leg(leg);
        const std::vector<Split> expected = scanned(stream);
        REQUIRE(expected.size() > 10);
        for (const std::size_t piece : {std::size_t{1}, std::size_t{3}, std::size_t{7},
                                        std::size_t{64}, std::size_t{1000}, std::size_t{65536}}) {
            CAPTURE(piece);
            const Outcome out = split(stream, piece);
            CHECK(out.frames == expected);
            CHECK_FALSE(out.truncated);
            CHECK_FALSE(out.too_small);
            CHECK(out.skipped == 0);
        }
    }
}

TEST_CASE("SyncFrameSplitter skips what is not a frame and counts it", "[ac4][splitter]") {
    const std::vector<std::byte> stream = read_leg("ac4-stereo-64");
    // Before the stream: a byte, then a sync word whose three-byte frame is
    // not followed by another, then more bytes that are not a frame.
    const std::vector<std::byte> junk =
        bytes_of({0x00, 0xAC, 0x40, 0x00, 0x03, 0x11, 0x22, 0x33, 0x44, 0xAC});
    std::vector<std::byte> joined = junk;
    joined.insert(joined.end(), stream.begin(), stream.end());
    for (const std::size_t piece : {std::size_t{1}, std::size_t{5}, std::size_t{4096}}) {
        CAPTURE(piece);
        const Outcome out = split(joined, piece);
        CHECK(out.frames == scanned(stream, junk.size()));
        CHECK(out.skipped == junk.size());
        CHECK_FALSE(out.truncated);
    }
    // Between two frames.
    const ac4::ScanResult frames = ac4::scan(stream);
    const std::size_t second = frames.frames[1].offset;
    std::vector<std::byte> interrupted(stream.begin(),
                                       stream.begin() + static_cast<std::ptrdiff_t>(second));
    interrupted.insert(interrupted.end(), junk.begin(), junk.end());
    interrupted.insert(interrupted.end(), stream.begin() + static_cast<std::ptrdiff_t>(second),
                       stream.end());
    const Outcome out = split(interrupted, 11);
    REQUIRE(out.frames.size() == frames.frames.size());
    CHECK(out.frames.front().offset == 0);
    CHECK(out.frames[1].offset == second + junk.size());
    CHECK(out.frames[1].raw == scanned(stream)[1].raw);
    CHECK(out.skipped == junk.size());
}

TEST_CASE("SyncFrameSplitter drops a partial frame at the end and refuses storage too small",
          "[ac4][splitter]") {
    const std::vector<std::byte> stream = read_leg("ac4-stereo-64");
    const std::vector<Split> expected = scanned(stream);
    const std::vector<std::byte> cut(stream.begin(), stream.end() - 5);
    const Outcome partial = split(cut, 100);
    CHECK(partial.truncated);
    REQUIRE(partial.frames.size() == expected.size() - 1);
    CHECK(std::equal(partial.frames.begin(), partial.frames.end(), expected.begin()));

    const Outcome small = split(stream, 100, 64);
    CHECK(small.too_small);
    CHECK(small.frames.empty());
}

TEST_CASE("SyncFrameSplitter reads an escaped frame size and checks the CRC", "[ac4][splitter]") {
    // frame_size 0xFFFF escapes to 24 bits (Annex G.3.1): a frame of 70 000
    // bytes, then one with a CRC word (0xAC41) whose payload was altered.
    std::vector<std::byte> stream = bytes_of({0xAC, 0x40, 0xFF, 0xFF, 0x01, 0x11, 0x70});
    stream.resize(stream.size() + 70'000, std::byte{0x5A});
    const std::vector<std::byte> good = read_leg("ac4-stereo-64");
    const ac4::ScanResult first = ac4::scan(good);
    REQUIRE(first.frames.front().sync_word == 0xAC41);
    const std::size_t end = first.frames.size() > 1 ? first.frames[1].offset : good.size();
    std::vector<std::byte> crc_frame(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(end));
    REQUIRE(crc_frame.size() > 10);
    // at(), not [], which GCC 16's -Wnull-dereference takes to reach an empty
    // vector's null data().
    crc_frame.at(10) ^= std::byte{0x01};
    stream.insert(stream.end(), crc_frame.begin(), crc_frame.end());

    const Outcome out = split(stream, 1000, 80'000);
    REQUIRE(out.frames.size() == 2);
    CHECK(out.frames[0].raw.size() == 70'000);
    CHECK_FALSE(out.frames[0].crc_ok.has_value());
    CHECK(out.frames[1].offset == 7 + 70'000);
    REQUIRE(out.frames[1].crc_ok.has_value());
    CHECK_FALSE(*out.frames[1].crc_ok);
    CHECK(out.frames == scanned(stream));
}

// ac4::Decoder's frame-to-frame behaviour, on the committed DEE streams: what
// a frame whose table of contents does not parse returns, and when I-frame
// configuration carried between frames is kept or forgotten.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"

namespace {

namespace fs = std::filesystem;

std::vector<std::byte> read_stream(const std::string& leg) {
    const fs::path path = fs::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / leg / "dee.ac4";
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

// Each sync frame's raw_ac4_frame, copied so a test can alter it.
std::vector<std::vector<std::byte>> raw_frames(const std::vector<std::byte>& stream) {
    std::vector<std::vector<std::byte>> frames;
    for (const ac4::SyncFrame& frame : ac4::scan(stream).frames) {
        frames.emplace_back(frame.raw_ac4_frame.begin(), frame.raw_ac4_frame.end());
    }
    return frames;
}

// sequence_counter is the 10 bits after the 2-bit bitstream_version (Part 1
// Table 4), so it spans the first two bytes of every frame here.
void set_sequence_counter(std::vector<std::byte>& frame, int counter) {
    const auto first = std::to_integer<unsigned>(frame[0]);
    const auto second = std::to_integer<unsigned>(frame[1]);
    const auto value = static_cast<unsigned>(counter);
    frame[0] = static_cast<std::byte>((first & 0xC0U) | ((value >> 4U) & 0x3FU));
    frame[1] = static_cast<std::byte>(((value & 0x0FU) << 4U) | (second & 0x0FU));
}

// Whether the frame's single channel-coded substream is an I-frame
// (b_audio_ndot).
bool is_iframe(const std::vector<std::byte>& frame) {
    const auto parsed = ac4::parse_raw_frame(frame);
    REQUIRE(parsed.has_value());
    const auto& chan = parsed->toc.substream_groups.at(0).substreams.at(0).chan;
    REQUIRE(chan.has_value());
    return !chan->b_iframe.empty() && chan->b_iframe.front();
}

std::optional<ac4::DecodeError> audio_refusal(const ac4::FrameReport& report) {
    for (const ac4::SubstreamReport& substream : report.substreams) {
        if (substream.kind == ac4::SubstreamReport::Kind::kAudio) {
            return substream.refused;
        }
    }
    FAIL("the frame has no audio substream");
    return std::nullopt;
}

// The first frame after `from` that is not an I-frame.
std::size_t next_non_iframe(const std::vector<std::vector<std::byte>>& frames, std::size_t from) {
    for (std::size_t k = from + 1; k < frames.size(); ++k) {
        if (!is_iframe(frames[k])) {
            return k;
        }
    }
    FAIL("no frame after the first I-frame depends on it");
    return 0;
}

}  // namespace

TEST_CASE("ac4::Decoder fails a frame whose table of contents does not parse", "[ac4dec]") {
    ac4::Decoder decoder;
    const std::vector<std::byte> nothing;
    const auto empty = decoder.parse(nothing);
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error() == ac4::DecodeError::kInvalidToc);

    // bitstream_version 3 with a variable_bits(2) extension: above 2, which
    // neither part defines.
    const std::vector<std::byte> version_3{std::byte{0xC4}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    const auto unsupported = decoder.parse(version_3);
    REQUIRE_FALSE(unsupported.has_value());
    CHECK(unsupported.error() == ac4::DecodeError::kInvalidToc);
    CHECK_FALSE(ac4::describe(ac4::DecodeError::kInvalidToc).empty());
}

TEST_CASE("ac4::Decoder carries I-frame configuration while sequence_counter continues", "[ac4dec]") {
    // ASPX stereo: a frame that is not an I-frame needs the aspx_config() of
    // the last I-frame.
    const auto frames = raw_frames(read_stream("ac4-20-speech-128"));
    REQUIRE(frames.size() > 2);
    REQUIRE(is_iframe(frames[0]));
    const std::size_t dependent = next_non_iframe(frames, 0);

    SECTION("in order") {
        ac4::Decoder decoder;
        for (std::size_t k = 0; k <= dependent; ++k) {
            const auto report = decoder.parse(frames[k]);
            REQUIRE(report.has_value());
            CHECK_FALSE(audio_refusal(*report).has_value());
        }
    }

    SECTION("across the wrap from 1020 to 1") {
        auto first = frames[0];
        auto later = frames[dependent];
        set_sequence_counter(first, 1020);
        set_sequence_counter(later, 1);
        ac4::Decoder decoder;
        REQUIRE(decoder.parse(first).has_value());
        const auto report = decoder.parse(later);
        REQUIRE(report.has_value());
        // A-SPX's previous border would differ had frames been skipped, but
        // the configuration is still the one the I-frame sent, so the frame
        // is read, not refused for want of an I-frame.
        CHECK(audio_refusal(*report) != ac4::DecodeError::kMissingIFrame);
    }
}

TEST_CASE("ac4::Decoder forgets I-frame configuration at a change of source", "[ac4dec]") {
    const auto frames = raw_frames(read_stream("ac4-20-speech-128"));
    REQUIRE(frames.size() > 2);
    REQUIRE(is_iframe(frames[0]));
    const std::size_t dependent = next_non_iframe(frames, 0);

    auto first = frames[0];
    auto later = frames[dependent];
    set_sequence_counter(first, 100);

    SECTION("a counter that jumps") {
        set_sequence_counter(later, 300);
    }
    SECTION("the splice mark, 0") {
        set_sequence_counter(later, 0);
    }

    ac4::Decoder decoder;
    const auto configured = decoder.parse(first);
    REQUIRE(configured.has_value());
    CHECK_FALSE(audio_refusal(*configured).has_value());
    const auto report = decoder.parse(later);
    REQUIRE(report.has_value());
    CHECK(audio_refusal(*report) == ac4::DecodeError::kMissingIFrame);
}

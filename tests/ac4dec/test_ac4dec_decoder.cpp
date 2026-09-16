// ac4::Decoder's frame-to-frame behaviour, on the committed DEE streams: what
// a frame whose table of contents does not parse returns, and when I-frame
// configuration carried between frames is kept or forgotten.

#include <algorithm>
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

class Bits {
   public:
    void put(std::uint32_t value, int n) {
        for (int i = n - 1; i >= 0; --i) {
            bits_.push_back(((value >> static_cast<unsigned>(i)) & 1U) != 0);
        }
    }

    [[nodiscard]] std::vector<std::byte> bytes() const {
        std::vector<std::byte> out((bits_.size() + 7) / 8, std::byte{0});
        for (std::size_t i = 0; i < bits_.size(); ++i) {
            if (bits_[i]) {
                out[i / 8] |= static_cast<std::byte>(0x80U >> (i % 8));
            }
        }
        return out;
    }

   private:
    std::vector<bool> bits_;
};

// One presentation, one channel-coded group, one substream whose own
// substream_index and whose group's hsf_ext_substream_index are the SAME
// value - a self-reference no real encoder would write, but a fuzzed stream
// can, and out.contains()'s first-claim-wins in assign_v1() (decoder.cpp)
// means the two transcriptions must resolve it identically. Shares its
// TOC-level shape with tests/ac4/test_ac4_presentation_configs.cpp's
// frame_with(), an independent bit writer proven against that suite.
std::vector<std::byte> self_referencing_hsf_ext_frame() {
    Bits w;
    w.put(2, 2);   // bitstream_version
    w.put(1, 10);  // sequence_counter
    w.put(0, 1);   // b_wait_frames
    w.put(1, 1);   // fs_index: 48 kHz
    w.put(13, 4);  // frame_rate_index 13: multiply/fractions info read nothing
    w.put(1, 1);   // b_iframe_global
    w.put(1, 1);   // b_single_presentation -> n_presentations = 1
    w.put(0, 1);   // b_payload_base
    w.put(0, 1);   // b_program_id

    // Presentation 0, single group.
    w.put(1, 1);  // b_single_substream_group
    w.put(0, 1);  // presentation_version terminator -> 0
    w.put(0, 3);  // md_compat
    w.put(0, 1);  // b_presentation_id
    w.put(0, 2);  // emdf_info: version
    w.put(0, 3);  //   key_id
    w.put(0, 1);  //   b_payloads_substream_info
    w.put(0, 2);  //   emdf_reserved primary
    w.put(0, 2);  //   emdf_reserved secondary
    w.put(0, 1);  // b_presentation_filter
    w.put(0, 3);  // ac4_sgi_specifier(): group 0
    w.put(0, 1);  // b_pre_virtualized
    w.put(0, 1);  // b_add_emdf_substreams
    w.put(0, 1);  // b_alternative
    w.put(1, 1);  // b_pres_ndot
    w.put(1, 2);  // presentation substream_index = 1

    // Group 0: single channel-coded substream, stereo, b_hsf_ext set.
    w.put(1, 1);     // b_substreams_present
    w.put(1, 1);     // b_hsf_ext
    w.put(1, 1);     // b_single_substream
    w.put(1, 1);     // b_channel_coded
    w.put(0b10, 2);  // channel_mode: stereo
    w.put(0, 1);     // b_sf_multiplier
    w.put(0, 1);     // b_bitrate_info
    w.put(1, 1);     // b_audio_ndot
    w.put(0, 2);     // substream_index = 0 (this channel's own index)
    w.put(0, 2);     // hsf_ext_substream_index = 0 - the self-reference
    w.put(0, 1);     // b_content_type

    // substream_index_table(): 2 substreams (the group's, the presentation's).
    w.put(2, 2);
    for (int s = 0; s < 2; ++s) {
        w.put(0, 1);                              // b_more_bits
        w.put(static_cast<std::uint32_t>(4 + s), 10);  // substream_size
    }
    auto data = w.bytes();
    data.resize(data.size() + 32, std::byte{0});
    return data;
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

TEST_CASE("a substream that is both a channel and its own HSF extension resolves to audio",
          "[ac4dec]") {
    ac4::Decoder decoder;
    const auto report = decoder.parse(self_referencing_hsf_ext_frame());
    REQUIRE(report.has_value());
    const auto it = std::find_if(report->substreams.begin(), report->substreams.end(),
                                  [](const ac4::SubstreamReport& s) { return s.index == 0; });
    REQUIRE(it != report->substreams.end());
    CHECK(it->kind == ac4::SubstreamReport::Kind::kAudio);
}

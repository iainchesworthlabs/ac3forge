// ac4::Decoder's frame-to-frame behaviour, on the committed DEE streams: what
// a frame whose table of contents does not parse returns, and when I-frame
// configuration carried between frames is kept or forgotten.

#include <algorithm>
#include <array>
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

// One presentation, one channel-coded group, one mono substream at 96 kHz
// (sf_multiplier 0) whose group's hsf_ext_substream_index names a SEPARATE
// substream carrying ac4_hsf_ext_substream() content - unlike
// self_referencing_hsf_ext_frame() above, this exercises the actual content
// decode (parse_sf_data()'s extended asf_section_data(), parse_sf_hsf_data()),
// not just assignment resolution. Chosen to be as bit-minimal as this
// substream shape allows: frame_len_base 512 (frame_rate_index 10, so
// asf_transform_info() reads one 2-bit transf_length, not two), the whole
// frame one window group (every scale_factor_grouping bit set to 1),
// max_sfb[0] = 5 out of num_sfb_48(128) = 14, and one asf_section_data()
// section spanning the whole extended range [0, 15) with sect_cb 0 (silence)
// - which asf_section_data() itself splits at num_sfb_48 = 14 into a core
// half [0, 14) and an extension-only half [14, 15), matching Table 39
// (ERRATA.md's own new entry). Codebook 0 throughout means neither half's
// spectral/scalefac/snf functions read another bit, so the whole point - the
// interleaving itself, and one genuine HSF-only band existing at all - is
// exercised without needing real Huffman-coded content.
//
// `ext_index_lower` swaps which of the two substream indices is which - the
// decoder's own assignment map is ordered by index (decoder.cpp's pre-pass
// looks the extension up by its hsf_ext_substream_index rather than relying
// on map iteration order to reach the owner first), and nothing in the
// syntax orders an owner before its extension, so a fuzzed (or, in
// principle, a real) stream naming its extension at a lower index than its
// own must resolve identically.
std::vector<std::byte> hsf_ext_two_substream_frame(bool ext_index_lower = false) {
    Bits w;
    w.put(2, 2);   // bitstream_version
    w.put(1, 10);  // sequence_counter
    w.put(0, 1);   // b_wait_frames
    w.put(1, 1);   // fs_index: 48 kHz
    w.put(10, 4);  // frame_rate_index 10: frame_len_base = 512
    w.put(1, 1);   // b_iframe_global
    w.put(1, 1);   // b_single_presentation -> n_presentations = 1
    w.put(0, 1);   // b_payload_base
    w.put(0, 1);   // b_program_id

    // Presentation 0, single group.
    w.put(1, 1);  // b_single_substream_group
    w.put(0, 1);  // presentation_version terminator -> 0
    w.put(0, 3);  // md_compat
    w.put(0, 1);  // b_presentation_id
    // frame_rate_index 10 falls to frame_rate_multiply_info()'s default (no
    // bits), but frame_rate_fractions_info() reads b_frame_rate_fraction for
    // every index 10 to 12 - unlike frame_rate_index 13, which self_
    // referencing_hsf_ext_frame() above uses precisely because both read
    // nothing there.
    w.put(0, 1);  // b_frame_rate_fraction
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
    w.put(2, 2);  // presentation substream_index = 2

    // Group 0: single channel-coded substream, mono, 96 kHz, HSF-linked to a
    // separate substream.
    w.put(1, 1);     // b_substreams_present
    w.put(1, 1);     // b_hsf_ext
    w.put(1, 1);     // b_single_substream
    w.put(1, 1);     // b_channel_coded
    w.put(0b0, 1);   // channel_mode: mono (Table 56's 1-bit code)
    w.put(1, 1);     // b_sf_multiplier
    w.put(0, 1);     // sf_multiplier: 0 -> 96 kHz
    w.put(0, 1);     // b_bitrate_info
    w.put(1, 1);     // b_audio_ndot (I-frame)
    const int owner_index = ext_index_lower ? 1 : 0;
    const int ext_index = ext_index_lower ? 0 : 1;
    w.put(static_cast<std::uint32_t>(owner_index), 2);  // substream_index (this channel's own)
    w.put(static_cast<std::uint32_t>(ext_index), 2);    // hsf_ext_substream_index (a separate substream)
    w.put(0, 1);     // b_content_type

    // substream_index_table(): 3 substreams, indexed as owner_index/ext_index
    // above name them, then the presentation last.
    w.put(3, 2);
    const std::array<int, 3> sizes = ext_index_lower ? std::array<int, 3>{1, 9, 2}
                                                     : std::array<int, 3>{9, 1, 2};
    for (const int size : sizes) {
        w.put(0, 1);                                   // b_more_bits
        w.put(static_cast<std::uint32_t>(size), 10);  // substream_size
    }
    auto toc = w.bytes();

    // Substream 0 (owner): single_channel_element(), mono_codec_mode SIMPLE.
    Bits o;
    o.put(5, 15);   // audio_size_value = 5 bytes
    o.put(0, 1);    // b_more_bits
    // audio_data_chan(): single_channel_element()
    o.put(0, 1);    // mono_codec_mode: SIMPLE
    o.put(0, 1);    // spec_frontend: ASF
    // sf_info(ASF, 0, 0): frame_len_base 512 < 1536, so one transf_length.
    // transf_length = 0 is the 128-sample transform (512 >> (full_frame_index
    // 2 - 0)): num_sfb_48(128) = 14, n_msfb_bits at 128 samples = 4, and
    // n_grp_bits (Table 110) = 3 - all three grouping bits are set to 1
    // below so num_window_groups still comes to 1 despite num_windows being
    // n_grp_bits + 1 = 4.
    o.put(0, 2);      // transf_length = 0
    o.put(5, 4);      // max_sfb[0] = 5
    o.put(0b111, 3);  // scale_factor_grouping: all 1 -> one window group
    // asf_section_data(): one section [0, max_sfb) with sect_cb 0, split by
    // asf_section_data() itself at num_sfb_48(transform length).
    o.put(0, 4);      // sect_cb = 0
    o.put(0b111, 3);  // sect_len_incr escape
    o.put(0b111, 3);  // sect_len_incr escape
    o.put(0b000, 3);  // sect_len_incr terminator: sect_len = 1+7+7+0 = 15
    // asf_scalefac_data(), asf_snf_data(): all bands sect_cb 0, so nothing to
    // read past reference_scale_factor; no noise fill.
    o.put(0, 8);   // reference_scale_factor
    o.put(0, 1);   // b_snf_data_exists
    // audio_data_chan() ends at bit 16 (the audio_size header) + 33 = 49;
    // audio_size (5 bytes) puts metadata() at bit 16 + 40 = 56, so fill_bits
    // is 7 bits here, not the 0 a same-length metadata() might suggest.
    o.put(0, 7);   // fill_bits
    // metadata(), sus_ver 1: basic_metadata() (b_more_basic_metadata = 0),
    // extended_metadata() (b_dialog = 0, b_channels_classifier = 0,
    // b_event_probability = 0), tools_metadata_size (the 1 bit
    // dialog_enhancement() below reads), dialog_enhancement()
    // (b_de_data_present = 0).
    o.put(0, 1);   // b_more_basic_metadata
    o.put(0, 1);   // b_dialog
    o.put(0, 1);   // b_channels_classifier
    o.put(0, 1);   // b_event_probability
    o.put(1, 7);   // tools_metadata_size_value = 1
    o.put(0, 1);   // b_more_bits
    o.put(0, 1);   // b_de_data_present
    // byte_align: bit 69 -> 72.
    auto owner = o.bytes();
    owner.resize(9, std::byte{0});

    // Substream 1 (extension): ac4_hsf_ext_substream().
    Bits e;
    e.put(10, 6);  // max_sfb_ext_hsf[0] = 10: get_max_sfb_hsf(0) = 5 + 10 = 15
    // b_different_framing is false at this frame_len_base (single
    // transf_length), so max_sfb_ext_hsf[1] is not read. sf_hsf_data() for
    // the one track reads nothing: its only section (the split's extension
    // half, sfb [14, 15)) has sect_cb 0.
    auto ext = e.bytes();
    ext.resize(1, std::byte{0});

    auto data = toc;
    if (ext_index_lower) {
        data.insert(data.end(), ext.begin(), ext.end());
        data.insert(data.end(), owner.begin(), owner.end());
    } else {
        data.insert(data.end(), owner.begin(), owner.end());
        data.insert(data.end(), ext.begin(), ext.end());
    }
    data.resize(data.size() + 2 + 32, std::byte{0});  // presentation substream + padding
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

TEST_CASE("ac4::Decoder reads a channel's HSF extension substream alongside it",
          "[ac4dec]") {
    bool ext_index_lower = false;
    SECTION("the extension's substream index is higher than its owner's") { ext_index_lower = false; }
    SECTION("the extension's substream index is lower than its owner's") { ext_index_lower = true; }
    const int owner_index = ext_index_lower ? 1 : 0;
    const int ext_index = ext_index_lower ? 0 : 1;

    ac4::Decoder decoder;
    const auto report = decoder.parse(hsf_ext_two_substream_frame(ext_index_lower));
    REQUIRE(report.has_value());
    const auto find = [&](int index) {
        return std::find_if(report->substreams.begin(), report->substreams.end(),
                            [index](const ac4::SubstreamReport& s) { return s.index == index; });
    };
    const auto owner = find(owner_index);
    REQUIRE(owner != report->substreams.end());
    INFO("owner refused_reason: " << owner->refused_reason);
    CHECK(owner->kind == ac4::SubstreamReport::Kind::kAudio);
    CHECK_FALSE(owner->refused.has_value());
    // 9 bytes: 16-bit audio_size header + 33-bit audio_data_chan + 7 fill +
    // 13-bit metadata + 3 align.
    CHECK(owner->bits_read == 72);

    const auto ext = find(ext_index);
    REQUIRE(ext != report->substreams.end());
    INFO("extension refused_reason: " << ext->refused_reason);
    CHECK(ext->kind == ac4::SubstreamReport::Kind::kHsfExt);
    CHECK_FALSE(ext->refused.has_value());
    CHECK(ext->bits_read == 8);  // the 6-bit header, byte_align'd
}

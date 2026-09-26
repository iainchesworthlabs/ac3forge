// ac4::Decoder's frame-to-frame behaviour, on the committed DEE streams: what
// a frame whose table of contents does not parse returns, when I-frame
// configuration carried between frames is kept or forgotten, what a decode
// begun at an I-frame or continued across a splice gives, and the concealment
// policies.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
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
// `linked` false leaves b_hsf_ext clear, so that nothing names the
// extension's substream, which the index table still holds.
//
// `ext_index_lower` swaps which of the two substream indices is which - the
// decoder's own assignment map is ordered by index (decoder.cpp's pre-pass
// looks the extension up by its hsf_ext_substream_index rather than relying
// on map iteration order to reach the owner first), and nothing in the
// syntax orders an owner before its extension, so a fuzzed (or, in
// principle, a real) stream naming its extension at a lower index than its
// own must resolve identically.
std::vector<std::byte> hsf_ext_two_substream_frame(bool ext_index_lower = false,
                                                   bool linked = true) {
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
    w.put(linked ? 1U : 0U, 1);  // b_hsf_ext
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
    if (linked) {
        w.put(static_cast<std::uint32_t>(ext_index),
              2);  // hsf_ext_substream_index (a separate substream)
    }
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

// decode() over `frames`, each of which must not fail: the channels of the
// frames that came out, appended, and each one's length and sequence_counter.
struct Decoded {
    std::vector<std::vector<float>> channels;
    std::vector<std::size_t> lengths;
    std::vector<int> counters;
    std::size_t nothing = 0;  // frames that returned no output
};

Decoded decode_frames(ac4::Decoder& decoder, std::span<const std::vector<std::byte>> frames) {
    Decoded out;
    for (const std::vector<std::byte>& frame : frames) {
        const auto decoded = decoder.decode(frame);
        INFO(decoder.refusal_reason());
        REQUIRE(decoded.has_value());
        if (!decoded->has_value()) {
            ++out.nothing;
            continue;
        }
        const ac4::DecodedFrame& pcm = **decoded;
        if (out.channels.empty()) {
            out.channels.resize(pcm.channels.size());
        }
        REQUIRE(pcm.channels.size() == out.channels.size());
        for (std::size_t c = 0; c < pcm.channels.size(); ++c) {
            out.channels[c].insert(out.channels[c].end(), pcm.channels[c].begin(),
                                   pcm.channels[c].end());
        }
        out.lengths.push_back(pcm.channels.front().size());
        out.counters.push_back(pcm.sequence_counter);
    }
    return out;
}

// The largest difference between two decodes over `count` samples of every
// channel, `a` from sample `a_first` and `b` from `b_first`.
float peak_difference(const std::vector<std::vector<float>>& a, std::size_t a_first,
                      const std::vector<std::vector<float>>& b, std::size_t b_first,
                      std::size_t count) {
    REQUIRE(a.size() == b.size());
    float peak = 0.0F;
    for (std::size_t c = 0; c < a.size(); ++c) {
        REQUIRE(a_first + count <= a[c].size());
        REQUIRE(b_first + count <= b[c].size());
        for (std::size_t n = 0; n < count; ++n) {
            peak = std::max(peak, std::abs(a[c][a_first + n] - b[c][b_first + n]));
        }
    }
    return peak;
}

// The largest magnitude over `count` samples of every channel from `first`.
float peak(const std::vector<std::vector<float>>& channels, std::size_t first, std::size_t count) {
    float out = 0.0F;
    for (const std::vector<float>& channel : channels) {
        REQUIRE(first + count <= channel.size());
        for (std::size_t n = first; n < first + count; ++n) {
            out = std::max(out, std::abs(channel[n]));
        }
    }
    return out;
}

// audio_size_value 32 767 and b_more_bits 0 in the frame's audio substream:
// past the end of the substream, which decode() refuses with the table of
// contents intact.
void damage_audio(std::vector<std::byte>& frame) {
    const auto parsed = ac4::parse_raw_frame(frame);
    REQUIRE(parsed.has_value());
    for (const ac4::Substream& substream : parsed->substreams) {
        if (substream.is_audio) {
            frame[substream.offset] = std::byte{0xFF};
            frame[substream.offset + 1] = std::byte{0xFE};
            return;
        }
    }
    FAIL("the frame has no audio substream");
}

// At frame_rate_index 13 a frame is 2 048 samples, and frame k's audio comes
// out from sample 2 048 k + 1 313, the decoder's delay (decoder.hpp). The QMF
// banks spread a change in the signal over a prototype's length either side,
// 640 samples, at under -100 dBFS.
constexpr std::size_t kFrame = 2048;
constexpr std::size_t kDelay = 1313;
constexpr std::size_t kQmfSpread = 640;

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

TEST_CASE("an HSF extension substream nothing names is reported as refused and unread",
          "[ac4dec]") {
    // The review of #700: the header said an HSF substream was refused, yet
    // a substream no ac4_hsf_ext_substream_info() named got no report at all.
    // Every substream of the index table now has one.
    // The extension's substream comes first in the table, before its owner.
    ac4::Decoder decoder;
    const auto report = decoder.parse(hsf_ext_two_substream_frame(true, false));
    REQUIRE(report.has_value());
    REQUIRE(report->substreams.size() == 3);
    const ac4::SubstreamReport& ext = report->substreams[0];
    CHECK(ext.index == 0);
    CHECK(ext.kind == ac4::SubstreamReport::Kind::kOther);
    REQUIRE(ext.refused.has_value());
    CHECK(*ext.refused == ac4::DecodeError::kUnsupported);
    CHECK_FALSE(ext.refused_reason.empty());
    CHECK(ext.bits_read == 0);
    CHECK(ext.size_bits == 8);
    // Its owner, at 96 kHz with no extension to read beside it, is refused
    // as it was.
    const ac4::SubstreamReport& owner = report->substreams[1];
    CHECK(owner.kind == ac4::SubstreamReport::Kind::kAudio);
    REQUIRE(owner.refused.has_value());
    CHECK(*owner.refused == ac4::DecodeError::kUnsupported);
    // And decode() names the owner's reason, not the unnamed substream's.
    ac4::Decoder decoding;
    const auto decoded = decoding.decode(hsf_ext_two_substream_frame(true, false));
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoding.refusal_reason() == owner.refused_reason);
}

TEST_CASE("a decode begun at an I-frame gives the whole stream's output from the frame after it",
          "[ac4dec][pcm]") {
    // The I-frame's own audio needs the frame before it to overlap with; from
    // the next frame's audio the two decodes agree, as closely as each codec
    // mode's state allows.
    struct Leg {
        const char* name;
        std::size_t settle;  // the frame after the I-frame from whose audio on the decodes agree
        float tolerance;     // their largest difference over that frame's audio and the next's
    };
    constexpr std::array<Leg, 6> kLegs{{
        // SIMPLE: the QMF banks' transient alone, under -110 dBFS.
        {"ac4-20-music-192", 1, 1e-5F},
        {"ac4-51-tones-384", 1, 1e-5F},
        // ASPX: A-SPX's noise and tone generators step through their tables
        // from the decoder's first frame (Part 1 Pseudocodes 103 and 105), a
        // phase no I-frame restores, so the noise and tones it adds come out
        // at their levels and another phase.
        {"ac4-20-speech-128", 1, 3e-3F},
        {"ac4-51-music-192", 1, 3e-3F},
        // A-CPL: the decorrelators' IIR filters and the transient ducker carry
        // state from frame to frame that no I-frame restores (clause 5.7.7.4);
        // on these streams it has settled by the fourth frame.
        {"ac4-51-film-96", 4, 2e-3F},
        {"ac4-51-music-128", 4, 2e-3F},
    }};
    for (const Leg& leg : kLegs) {
        CAPTURE(leg.name);
        const auto frames = raw_frames(read_stream(leg.name));
        ac4::Decoder whole_decoder;
        const Decoded whole = decode_frames(whole_decoder, frames);
        REQUIRE(whole.lengths.size() == frames.size());
        std::size_t checked = 0;
        for (std::size_t k = 1; k < frames.size(); ++k) {
            const std::size_t from = (k + leg.settle) * kFrame + kDelay;
            if (!is_iframe(frames[k]) || from + 2 * kFrame > whole.channels.front().size()) {
                continue;
            }
            CAPTURE(k);
            // Enough frames for the output to reach the two frames' audio.
            const std::size_t count = leg.settle + 3;
            ac4::Decoder decoder;
            const Decoded part = decode_frames(decoder, std::span(frames).subspan(k, count));
            REQUIRE(part.lengths.size() == count);
            CHECK(peak_difference(whole.channels, from, part.channels, from - k * kFrame,
                                  2 * kFrame) < leg.tolerance);
            ++checked;
        }
        // DEE's second frame, an I-frame after its priming frame, and the
        // I-frames every 23 or 24 frames after it that leave room to compare.
        CHECK(checked == 5);
    }
}

TEST_CASE("a splice at an I-frame joins the two streams' audio without a gap", "[ac4dec][pcm]") {
    // Part 1 clause 6.2.19: a switch at an I-frame decodes without a flaw,
    // and the first frame after a splice is read without what the frames
    // before it sent. The signal carries on: the first stream's audio comes out
    // to its end, the second's from its first frame, and the two overlap across
    // that frame's audio. The first stream is SIMPLE stereo music.
    const auto first_stream = raw_frames(read_stream("ac4-20-music-192"));
    constexpr std::size_t kSplice = 30;  // the first stream's frames before the splice
    constexpr std::size_t kFrom = 47;    // the second stream's I-frame after it
    bool marked = false;
    SECTION("marked 0, a controlled splice") {
        marked = true;
    }
    SECTION("the counter jumping") {}

    struct Second {
        const char* name;
        float tolerance;  // after the joint frame, against the stream decoded alone
        bool steady;      // tones, which never fall quiet
    };
    // SIMPLE tones, then ASPX speech, whose A-SPX starts at the splice as it
    // does in a decode of the stream alone.
    for (const Second& second :
         {Second{"ac4-20-tones-192", 1e-5F, true}, Second{"ac4-20-speech-128", 1e-5F, false}}) {
        CAPTURE(second.name);
        const auto second_stream = raw_frames(read_stream(second.name));
        REQUIRE(is_iframe(second_stream[kFrom]));
        std::vector<std::vector<std::byte>> tail(second_stream.begin() + kFrom,
                                                 second_stream.end());
        if (marked) {
            set_sequence_counter(tail.front(), 0);
        }

        // Each stream alone: the first one frame past the splice, for the
        // audio its delay line still holds there.
        ac4::Decoder first_decoder;
        const Decoded first_alone =
            decode_frames(first_decoder, std::span(first_stream).first(kSplice + 1));
        ac4::Decoder second_decoder;
        const Decoded second_alone = decode_frames(second_decoder, tail);

        std::vector<std::vector<std::byte>> frames(first_stream.begin(),
                                                   first_stream.begin() + kSplice);
        frames.insert(frames.end(), tail.begin(), tail.end());
        ac4::Decoder decoder;
        const Decoded spliced = decode_frames(decoder, frames);
        REQUIRE(spliced.lengths.size() == frames.size());
        // The second stream's first audio starts here.
        const std::size_t joint = kSplice * kFrame + kDelay;
        CHECK(peak_difference(spliced.channels, 0, first_alone.channels, 0, joint - kQmfSpread) ==
              0.0F);
        // From the audio of the second stream's next frame, the second stream
        // as decoded alone, past the QMF banks' transient.
        const std::size_t next = joint + kFrame;
        CHECK(peak_difference(spliced.channels, next, second_alone.channels,
                              next - kSplice * kFrame,
                              spliced.channels.front().size() - next) < second.tolerance);
        // Across the joint frame's audio the two streams overlap: the first
        // stream's last audio falls as the second's rises, where a decoder that
        // started again from silence would put a frame's delay of it.
        for (std::size_t n = joint; second.steady && n < next; n += kFrame / 8) {
            CAPTURE(n);
            CHECK(peak(spliced.channels, n, kFrame / 8) > 0.01F);
        }
    }
}

TEST_CASE(
    "after a change of source between I-frames the output resumes as the new stream decoded alone",
    "[ac4dec][pcm]") {
    // Without a concealment policy the frames that wait for an I-frame return
    // nothing, and the signal before them goes with them.
    const auto first_stream = raw_frames(read_stream("ac4-20-music-192"));
    const auto second_stream = raw_frames(read_stream("ac4-20-speech-128"));
    REQUIRE_FALSE(is_iframe(second_stream[50]));
    const std::vector<std::vector<std::byte>> tail(second_stream.begin() + 50, second_stream.end());
    ac4::Decoder alone_decoder;
    const Decoded alone = decode_frames(alone_decoder, tail);
    CHECK(alone.nothing == 21);  // until the I-frame at 71

    ac4::Decoder decoder;
    REQUIRE(decode_frames(decoder, std::span(first_stream).first(30)).lengths.size() == 30);
    const Decoded spliced = decode_frames(decoder, tail);
    CHECK(spliced.nothing == alone.nothing);
    REQUIRE(spliced.lengths == alone.lengths);
    CHECK(peak_difference(spliced.channels, 0, alone.channels, 0, alone.channels.front().size()) ==
          0.0F);
}

TEST_CASE("the converter's output counts stay locked to sequence_counter across a splice",
          "[ac4dec][pcm][src]") {
    // Part 2 Table 47 at 29.97 fps, by phi_t.
    constexpr std::array<std::size_t, 5> kCounts{1601, 1602, 1601, 1602, 1602};
    const auto frames = raw_frames(read_stream("ac4-ims-music-64-2997"));
    REQUIRE(frames.size() > 120);
    REQUIRE(is_iframe(frames[90]));
    std::vector<std::vector<std::byte>> tail(frames.begin() + 90, frames.end());
    ac4::Decoder decoder;
    const Decoded lead = decode_frames(decoder, std::span(frames).first(40));
    REQUIRE(lead.lengths.size() == 40);

    SECTION("a counter that jumps: the new stream's phase from its first frame") {
        const Decoded spliced = decode_frames(decoder, tail);
        REQUIRE(spliced.lengths.size() == tail.size());
        for (std::size_t n = 0; n < tail.size(); ++n) {
            CAPTURE(n);
            CHECK(spliced.lengths[n] == kCounts[static_cast<std::size_t>(spliced.counters[n] % 5)]);
        }
    }
    SECTION("the splice mark: phi_t goes on from the frame before (Part 2 clause 5.11)") {
        set_sequence_counter(tail.front(), 0);
        const Decoded spliced = decode_frames(decoder, tail);
        REQUIRE(spliced.lengths.size() == tail.size());
        CHECK(spliced.counters.front() == 0);
        CHECK(spliced.lengths.front() ==
              kCounts[static_cast<std::size_t>((lead.counters.back() % 5 + 1) % 5)]);
        for (std::size_t n = 1; n < tail.size(); ++n) {
            CAPTURE(n);
            CHECK(spliced.lengths[n] == kCounts[static_cast<std::size_t>(spliced.counters[n] % 5)]);
        }
    }
}

TEST_CASE("without a concealment policy a frame that does not decode fails and the next goes on",
          "[ac4dec][pcm]") {
    const auto frames = raw_frames(read_stream("ac4-20-tones-192"));
    constexpr std::size_t kLost = 10;
    REQUIRE_FALSE(is_iframe(frames[kLost + 1]));
    auto damaged = frames;
    ac4::DecodeError expected = ac4::DecodeError::kInvalidStream;
    SECTION("an audio substream that does not read") {
        damage_audio(damaged[kLost]);
    }
    SECTION("a table of contents that does not read, taken to be the frame the stream expected") {
        damaged[kLost] = {std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
        expected = ac4::DecodeError::kInvalidToc;
    }
    ac4::Decoder decoder;
    REQUIRE(decode_frames(decoder, std::span(damaged).first(kLost)).lengths.size() == kLost);
    const auto lost = decoder.decode(damaged[kLost]);
    REQUIRE_FALSE(lost.has_value());
    CHECK(lost.error() == expected);
    CHECK_FALSE(decoder.refusal_reason().empty());
    // The frame after it continues the stream: it decodes, where a change of
    // source would have it wait for an I-frame.
    const auto next = decoder.decode(damaged[kLost + 1]);
    REQUIRE(next.has_value());
    CHECK(next->has_value());
    CHECK(decoder.refusal_reason().empty());
}

TEST_CASE("a concealment policy puts a frame in place of each one that does not decode",
          "[ac4dec][pcm]") {
    // SIMPLE stereo tones, at -20 dBFS: three frames lost in a row.
    const auto frames = raw_frames(read_stream("ac4-20-tones-192"));
    constexpr std::size_t kLost = 10;
    constexpr std::size_t kLosses = 3;
    auto damaged = frames;
    for (std::size_t k = kLost; k < kLost + kLosses; ++k) {
        REQUIRE_FALSE(is_iframe(frames[k]));
        damage_audio(damaged[k]);
    }
    ac4::Decoder clean_decoder;
    const Decoded clean = decode_frames(clean_decoder, frames);

    ac4::ConcealmentPolicy policy = ac4::ConcealmentPolicy::kMute;
    ac4::ConcealmentAction action = ac4::ConcealmentAction::kMute;
    SECTION("mute") {}
    SECTION("repeat and fade") {
        policy = ac4::ConcealmentPolicy::kRepeatFade;
        action = ac4::ConcealmentAction::kRepeatFade;
    }
    ac4::Decoder decoder(ac4::DecoderConfig{.syntax = {}, .output = {}, .concealment = policy});
    std::vector<std::vector<float>> out(2);
    for (std::size_t k = 0; k < damaged.size(); ++k) {
        CAPTURE(k);
        const auto decoded = decoder.decode(damaged[k]);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const ac4::DecodedFrame& pcm = **decoded;
        const bool lost = k >= kLost && k < kLost + kLosses;
        REQUIRE(pcm.concealed.has_value() == lost);
        if (lost) {
            CHECK(pcm.concealed->error == ac4::DecodeError::kInvalidStream);
            CHECK(pcm.concealed->action == action);
            CHECK_FALSE(decoder.refusal_reason().empty());
        }
        CHECK(pcm.sequence_counter == clean.counters[k]);
        CHECK(pcm.sample_rate_hz == 48000);
        CHECK(pcm.speakers == std::vector<ac4::Speaker>{ac4::Speaker::kLeft, ac4::Speaker::kRight});
        REQUIRE(pcm.channels.size() == 2);
        for (std::size_t c = 0; c < 2; ++c) {
            REQUIRE(pcm.channels[c].size() == kFrame);
            out[c].insert(out[c].end(), pcm.channels[c].begin(), pcm.channels[c].end());
        }
    }
    // The clean stream's output up to the first lost frame's audio, and again
    // from the audio of the second good frame after the losses (the first
    // overlaps a concealed frame).
    const std::size_t damage_from = kLost * kFrame + kDelay;
    const std::size_t damage_to = (kLost + kLosses + 1) * kFrame + kDelay;
    CHECK(peak_difference(out, 0, clean.channels, 0, damage_from - kQmfSpread) == 0.0F);
    CHECK(peak_difference(out, damage_from - kQmfSpread, clean.channels, damage_from - kQmfSpread,
                          kQmfSpread) < 1e-5F);
    CHECK(peak_difference(out, damage_to, clean.channels, damage_to,
                          out.front().size() - damage_to) < 1e-5F);
    // The second and third lost frames' audio lies between concealed frames:
    // in the second half of each, where the frame before has faded, silence
    // under kMute and the tones faded under kRepeatFade, over 20 dB further
    // in the third than in the second.
    const std::size_t half = kFrame / 2 - kQmfSpread;
    const float second = peak(out, (kLost + 1) * kFrame + kDelay + kFrame / 2, half);
    const float third = peak(out, (kLost + 2) * kFrame + kDelay + kFrame / 2, half);
    CAPTURE(second, third);
    if (policy == ac4::ConcealmentPolicy::kMute) {
        CHECK(second == 0.0F);
        CHECK(third == 0.0F);
    } else {
        CHECK(second > 1e-4F);
        CHECK(second < 1e-2F);
        CHECK(third < second / 10.0F);
    }
}

TEST_CASE("a concealment policy has nothing to conceal from before a frame has decoded",
          "[ac4dec][pcm]") {
    auto frames = raw_frames(read_stream("ac4-20-tones-192"));
    damage_audio(frames[0]);
    ac4::Decoder decoder(ac4::DecoderConfig{
        .syntax = {}, .output = {}, .concealment = ac4::ConcealmentPolicy::kRepeatFade});
    const auto first = decoder.decode(frames[0]);
    REQUIRE_FALSE(first.has_value());
    CHECK(first.error() == ac4::DecodeError::kInvalidStream);
    // DEE's second frame is an I-frame, and decodes.
    const auto second = decoder.decode(frames[1]);
    REQUIRE(second.has_value());
    REQUIRE(second->has_value());
    CHECK_FALSE((*second)->concealed.has_value());
}

TEST_CASE("a concealed frame whose table of contents did not read keeps the converter's counts",
          "[ac4dec][pcm][src]") {
    // Part 2 Table 47 at 29.97 fps, by phi_t.
    constexpr std::array<std::size_t, 5> kCounts{1601, 1602, 1601, 1602, 1602};
    auto frames = raw_frames(read_stream("ac4-ims-music-64-2997"));
    constexpr std::size_t kLost = 40;
    REQUIRE_FALSE(is_iframe(frames[kLost]));
    const auto parsed = ac4::parse_raw_frame(frames[kLost]);
    REQUIRE(parsed.has_value());
    const int counter = parsed->toc.sequence_counter;
    frames[kLost] = {std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
    ac4::Decoder decoder(ac4::DecoderConfig{
        .syntax = {}, .output = {}, .concealment = ac4::ConcealmentPolicy::kMute});
    for (std::size_t k = 0; k < frames.size(); ++k) {
        CAPTURE(k);
        const auto decoded = decoder.decode(frames[k]);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const ac4::DecodedFrame& pcm = **decoded;
        REQUIRE(pcm.concealed.has_value() == (k == kLost));
        if (k == kLost) {
            CHECK(pcm.concealed->error == ac4::DecodeError::kInvalidToc);
            CHECK(pcm.sequence_counter == counter);
        }
        CHECK(pcm.channels.front().size() ==
              kCounts[static_cast<std::size_t>(pcm.sequence_counter % 5)]);
    }
}

TEST_CASE("a concealment policy fills the frames that wait for an I-frame after a change of source",
          "[ac4dec][pcm]") {
    // Frames 0 to 19 of one stream, then 50 on: the counter jumps, and 50 to
    // 70 wait for the I-frame at 71.
    const auto frames = raw_frames(read_stream("ac4-20-tones-192"));
    REQUIRE(is_iframe(frames[71]));
    const std::vector<std::vector<std::byte>> tail(frames.begin() + 50, frames.end());
    ac4::Decoder alone_decoder;
    const Decoded alone = decode_frames(alone_decoder, tail);
    REQUIRE(alone.nothing == 21);

    ac4::Decoder decoder(ac4::DecoderConfig{
        .syntax = {}, .output = {}, .concealment = ac4::ConcealmentPolicy::kMute});
    REQUIRE(decode_frames(decoder, std::span(frames).first(20)).lengths.size() == 20);
    std::vector<std::vector<float>> out(2);
    for (std::size_t k = 0; k < tail.size(); ++k) {
        CAPTURE(k);
        const auto decoded = decoder.decode(tail[k]);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const ac4::DecodedFrame& pcm = **decoded;
        REQUIRE(pcm.concealed.has_value() == (k < alone.nothing));
        if (pcm.concealed) {
            CHECK(pcm.concealed->error == ac4::DecodeError::kMissingIFrame);
            CHECK(pcm.channels.front().size() == kFrame);
            continue;
        }
        for (std::size_t c = 0; c < 2; ++c) {
            out[c].insert(out[c].end(), pcm.channels[c].begin(), pcm.channels[c].end());
        }
    }
    // From the I-frame on, the new source's output as decoded alone.
    REQUIRE(out.front().size() == alone.channels.front().size());
    CHECK(peak_difference(out, 0, alone.channels, 0, out.front().size()) == 0.0F);
}

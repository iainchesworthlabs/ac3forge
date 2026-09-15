// ac4_presentation_v1_info()'s substream group references for the
// presentation configurations that carry a dialogue enhancement substream.
//
// ETSI TS 103 190-2 V1.3.1 clause 6.2.1.3 reads two ac4_sgi_specifier()
// elements for presentation_config 1 ("Main + DE") and three for 4 ("Main +
// DE + Associated Audio"), while setting n_substream_groups to 1 and 2 for
// them. The inspector used to read as many specifiers as n_substream_groups,
// one too few for both, which left every later field of the table of contents
// a group index's width out of place. No encoder this project has writes
// either configuration, so the frames here are built bit by bit, with a
// writer that shares no code with src/ac4, and each is followed by an
// ordinary presentation whose flags are set to values a misaligned read would
// not reproduce.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"

namespace {

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

// emdf_info() with nothing in it: emdf_version 0, key_id 0, no payloads
// substream, and emdf_reserved()'s two zero length codes.
void empty_emdf_info(Bits& w) {
    w.put(0, 2);
    w.put(0, 3);
    w.put(0, 1);
    w.put(0, 2);
    w.put(0, 2);
}

// A substream_index: 2 bits, where 3 continues with variable_bits(2) (Part 1
// Table 3), whose single group of two bits and a clear b_read_more carries
// any remainder below 4.
void substream_index(Bits& w, int index) {
    if (index < 3) {
        w.put(static_cast<std::uint32_t>(index), 2);
    } else {
        w.put(3, 2);
        w.put(static_cast<std::uint32_t>(index - 3), 2);
        w.put(0, 1);
    }
}

// A frame of two presentations - the first with `config` and one group
// reference per entry of `groups`, the second a single-group presentation of
// group 0 with b_pre_virtualized and b_alternative set - then one
// single-substream channel-coded group per referenced index (stereo for group
// 0, mono for the rest), and a substream index table of one substream per
// group plus the presentation substream both presentations share. The
// substream sizes are 4, 5, 6 and so on, and the frame is padded past the
// last of them.
std::vector<std::byte> frame_with(int config, const std::vector<int>& groups) {
    Bits w;
    // Part 2 6.2.1.1 ac4_toc()
    w.put(2, 2);   // bitstream_version
    w.put(1, 10);  // sequence_counter
    w.put(0, 1);   // b_wait_frames
    w.put(1, 1);   // fs_index: 48 kHz
    w.put(13, 4);  // frame_rate_index 13: frame_rate_multiply_info() and
                   // frame_rate_fractions_info() read nothing
    w.put(1, 1);   // b_iframe_global
    w.put(0, 1);   // b_single_presentation
    w.put(1, 1);   // b_more_presentations
    w.put(0, 2);   // variable_bits(2): value 0 ...
    w.put(0, 1);   // ... b_read_more 0 -> n_presentations = 2
    w.put(0, 1);   // b_payload_base
    w.put(0, 1);   // b_program_id

    const int presentation_substream = static_cast<int>(groups.size());

    // Presentation 0, 6.2.1.3
    w.put(0, 1);                                    // b_single_substream_group
    w.put(static_cast<std::uint32_t>(config), 3);  // presentation_config
    w.put(1, 1);                                    // presentation_version: 1 ...
    w.put(0, 1);                                    // ... then the terminating 0
    w.put(0, 3);                                    // md_compat
    w.put(0, 1);                                    // b_presentation_id
    empty_emdf_info(w);
    w.put(0, 1);  // b_presentation_filter
    w.put(0, 1);  // b_multi_pid
    for (const int group : groups) {
        w.put(static_cast<std::uint32_t>(group), 3);  // ac4_sgi_specifier(): group_index
    }
    w.put(0, 1);  // b_pre_virtualized
    w.put(0, 1);  // b_add_emdf_substreams
    w.put(0, 1);  // b_alternative
    w.put(1, 1);  // b_pres_ndot
    substream_index(w, presentation_substream);

    // Presentation 1
    w.put(1, 1);  // b_single_substream_group
    w.put(1, 1);  // presentation_version: 1 ...
    w.put(0, 1);  // ... then 0
    w.put(0, 3);  // md_compat
    w.put(0, 1);  // b_presentation_id
    empty_emdf_info(w);
    w.put(0, 1);  // b_presentation_filter
    w.put(0, 3);  // ac4_sgi_specifier(): group 0
    w.put(1, 1);  // b_pre_virtualized
    w.put(0, 1);  // b_add_emdf_substreams
    w.put(1, 1);  // b_alternative
    w.put(1, 1);  // b_pres_ndot
    substream_index(w, presentation_substream);

    // total_n_substream_groups = 1 + the highest group_index referenced.
    for (std::size_t g = 0; g < groups.size(); ++g) {
        w.put(1, 1);  // b_substreams_present
        w.put(0, 1);  // b_hsf_ext
        w.put(1, 1);  // b_single_substream
        w.put(1, 1);  // b_channel_coded
        // 6.2.1.8 ac4_substream_info_chan()
        if (g == 0) {
            w.put(0b10, 2);  // channel_mode: stereo
        } else {
            w.put(0b0, 1);  // channel_mode: mono
        }
        w.put(0, 1);  // b_sf_multiplier
        w.put(0, 1);  // b_bitrate_info
        w.put(1, 1);  // b_audio_ndot
        substream_index(w, static_cast<int>(g));
        w.put(0, 1);  // b_content_type
    }

    // Part 1 4.2.3.11 substream_index_table(): one substream per group and
    // the presentation substream. A count of 4 or more is a 0 followed by
    // variable_bits(2) of the count less 4.
    const int n_substreams = static_cast<int>(groups.size()) + 1;
    if (n_substreams < 4) {
        w.put(static_cast<std::uint32_t>(n_substreams), 2);
    } else {
        w.put(0, 2);
        w.put(static_cast<std::uint32_t>(n_substreams - 4), 2);
        w.put(0, 1);
    }
    for (int s = 0; s < n_substreams; ++s) {
        w.put(0, 1);                                    // b_more_bits
        w.put(static_cast<std::uint32_t>(4 + s), 10);  // substream_size
    }
    auto data = w.bytes();
    data.resize(data.size() + 32, std::byte{0});
    return data;
}

void check_frame(int config, const std::vector<int>& groups) {
    const auto data = frame_with(config, groups);
    const auto frame = ac4::parse_raw_frame(data);
    REQUIRE(frame.has_value());
    const ac4::Toc& toc = frame->toc;
    REQUIRE(toc.presentations_v1.size() == 2);

    const auto& first = toc.presentations_v1[0];
    CHECK(first.presentation_config == config);
    CHECK(first.group_refs == groups);
    CHECK_FALSE(first.b_pre_virtualized);
    CHECK_FALSE(first.b_alternative);
    CHECK(first.b_pres_ndot);
    CHECK(first.presentation_substream_index == static_cast<int>(groups.size()));

    const auto& second = toc.presentations_v1[1];
    CHECK(second.group_refs == std::vector<int>{0});
    CHECK(second.b_pre_virtualized);
    CHECK(second.b_alternative);
    CHECK(second.presentation_substream_index == static_cast<int>(groups.size()));

    REQUIRE(toc.substream_groups.size() == groups.size());
    REQUIRE(toc.substream_groups[0].substreams.size() == 1);
    const auto& stereo = toc.substream_groups[0].substreams[0].chan;
    REQUIRE(stereo.has_value());
    CHECK(stereo->ch_mode == 1);
    CHECK(stereo->b_iframe == std::vector<bool>{true});
    CHECK(stereo->substream_index == 0);
    for (std::size_t g = 1; g < groups.size(); ++g) {
        const auto& mono = toc.substream_groups[g].substreams[0].chan;
        REQUIRE(mono.has_value());
        CHECK(mono->ch_mode == 0);
        CHECK(mono->substream_index == static_cast<int>(g));
    }

    std::vector<int> sizes;
    for (std::size_t s = 0; s <= groups.size(); ++s) {
        sizes.push_back(4 + static_cast<int>(s));
    }
    CHECK(toc.substream_sizes == sizes);
}

}  // namespace

TEST_CASE("ac4_presentation_v1_info: presentation_config 1 reads two substream group references",
          "[ac4]") {
    check_frame(1, {0, 1});
}

TEST_CASE("ac4_presentation_v1_info: presentation_config 4 reads three substream group references",
          "[ac4]") {
    check_frame(4, {0, 1, 2});
}

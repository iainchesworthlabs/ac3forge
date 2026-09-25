#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "bit_writer.hpp"

// The table of contents of a raw_ac4_frame() at bitstream_version 2 (ETSI TS
// 103 190-2 V1.3.1 clause 6.2.1), for any number of version 1 presentations
// over any number of substream groups of channel-coded substreams, with their
// EMDF payload substreams, and the frame it heads. The encoder writes its
// presentations with it (frame_writer.hpp), and the decoder's tests build
// presentations of other encoders' substreams with it
// (tests/ac4dec/ac4dec_mux.hpp).

namespace ac4::detail {

// One ac4_substream_info_chan() (clause 6.2.1.8) of a group whose substreams
// are in the stream, at 48 kHz or 44.1 kHz, with no bit rate indicator and a
// frame rate factor of 1.
struct TocSubstream {
    int ch_mode = 1;           // Part 1 Table 88: 0 mono to 10 (7.1 3/2/2)
    bool add_ch_base = false;  // for 7.X 5/2/0 and 3/2/2 (clause 6.3.2.7)
    bool iframe = true;        // b_audio_ndot
    int substream_index = 0;
};

// One ac4_substream_group_info() (clause 6.2.1.6) of channel-coded substreams,
// with its content_type() (Part 1 clause 4.2.3.7) where it has one.
struct TocGroup {
    std::vector<TocSubstream> substreams;
    std::optional<int> content_classifier;  // Part 1 Table 91; unset: b_content_type 0
    // language_tag_bytes (Part 1 clause 4.3.3.8.7), sent whole; empty for no
    // b_language_indicator.
    std::string language;
};

// One ac4_presentation_v1_info() (clause 6.2.1.3): with its presentation
// substream, or for presentation_config 6 its EMDF payload substreams alone.
struct TocPresentation {
    // Table 53's presentation_config, 0 to 6; unset for
    // b_single_substream_group.
    std::optional<int> presentation_config;
    std::vector<int> groups;  // each ac4_sgi_specifier()'s group_index; none for 6
    int presentation_version = 1;
    int md_compat = 0;
    std::optional<int> presentation_id;
    // b_presentation_filter, and b_enable_presentation where it is set.
    std::optional<bool> enable;
    bool pre_virtualized = false;
    bool alternative = false;  // ac4_presentation_substream_info()'s b_alternative
    bool pres_ndot = true;     // and its b_pres_ndot
    int presentation_substream = 0;
    // The EMDF payloads substream the presentation's emdf_info() names
    // (b_emdf_payloads_substream_info), and those of the additional
    // emdf_info()s (b_add_emdf_substreams), which presentation_config 6 has
    // alone. Part 1 Tables 8 and 13, with emdf_version and key_id 0.
    std::optional<int> emdf_substream{};
    std::vector<int> add_emdf{};
};

struct TocLayout {
    int sequence_counter = 0;  // 0 to 1020
    // Part 1 Table 81, with b_wait_frames always sent; above 0, Part 2 Table
    // 52's br_code follows.
    int wait_frames = 0;
    int br_code = 0;
    int fs_index = 1;  // Part 1 Table 82
    int frame_rate_index = 13;
    bool iframe_global = true;
    std::vector<TocPresentation> presentations;
    // total_n_substream_groups of them, which is one more than the largest
    // group_index the presentations name (clause 6.3.2.1.8).
    std::vector<TocGroup> groups;
};

// ac4_toc() with `payload_base` bytes between it and the first substream and
// `sizes` in substream_index_table(), byte-aligned. The table of contents is
// not traced, so `w` records nothing a trace needs.
void write_toc(BitWriter& w, const TocLayout& layout, std::size_t payload_base,
               std::span<const std::size_t> sizes);

// Its length in bytes.
[[nodiscard]] std::size_t toc_bytes(const TocLayout& layout, std::size_t payload_base,
                                    std::span<const std::size_t> sizes);

// The whole raw_ac4_frame(): the table of contents and `substreams`, in index
// order, each as long as its bytes, with `payload_base` zero bytes between
// them. Nothing when the layout names a group, a substream or a presentation
// substream that is not there, or one the syntax cannot send (a channel mode
// above 10, a presentation_config above 6, a language tag longer than 63
// bytes).
[[nodiscard]] std::optional<std::vector<std::byte>> assemble_frame(
    const TocLayout& layout, std::span<const std::vector<std::byte>> substreams,
    std::size_t payload_base = 0);

// Whether assemble_frame() can write the layout over `substreams` substreams.
[[nodiscard]] bool writable(const TocLayout& layout, std::size_t substreams);

}  // namespace ac4::detail

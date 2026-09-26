#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "bit_writer.hpp"

// The table of contents of a raw_ac4_frame() at bitstream_version 2 (ETSI TS
// 103 190-2 V1.3.1 clause 6.2.1), for any number of version 1 presentations
// over any number of substream groups of channel-coded substreams, and the
// frame it heads. The encoder's own frame (frame_writer.hpp) is the case of one
// presentation over one group of one substream; the decoder's tests build
// presentations of several substreams with it (tests/ac4dec/ac4dec_mux.hpp),
// and phase E6 of planning/ac4.md writes the encoder's own.

namespace ac4::detail {

// One ac4_substream_info_chan() (clause 6.2.1.8) of a group whose substreams
// are in the stream, at 48 kHz or 44.1 kHz, with no bit rate indicator and a
// frame rate factor of 1.
struct TocSubstream {
    // Part 1 Table 88: 0 mono to 10 (7.1 3/2/2); and Part 2 Table 56's 11 and
    // 12, 7.0.4 and 7.1.4.
    int ch_mode = 1;
    bool add_ch_base = false;  // for 7.X 5/2/0 and 3/2/2 (clause 6.3.2.7)
    bool iframe = true;        // b_audio_ndot
    int substream_index = 0;
    // For 7.0.4 and 7.1.4, the channels their source has (Tables 57 to 59).
    bool b_4_back_channels_present = true;
    bool b_centre_present = true;
    int top_channels_present = 3;
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

// One ac4_presentation_v1_info() (clause 6.2.1.3), with its presentation
// substream and no EMDF substreams.
struct TocPresentation {
    // Table 53's presentation_config, 0 to 5; unset for
    // b_single_substream_group.
    std::optional<int> presentation_config;
    std::vector<int> groups;  // each ac4_sgi_specifier()'s group_index
    int presentation_version = 1;
    int md_compat = 0;
    std::optional<int> presentation_id;
    // b_presentation_filter, and b_enable_presentation where it is set.
    std::optional<bool> enable;
    bool pre_virtualized = false;
    bool pres_ndot = true;  // ac4_presentation_substream_info()'s b_pres_ndot
    int presentation_substream = 0;
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
// order, each as long as its bytes, with no payload_base. Nothing when the
// layout names a group, a substream or a presentation substream that is not
// there, or one the syntax cannot send (a channel mode above 12, a
// presentation_config above 5, a language tag longer than 63 bytes).
[[nodiscard]] std::optional<std::vector<std::byte>> assemble_frame(
    const TocLayout& layout, std::span<const std::vector<std::byte>> substreams);

}  // namespace ac4::detail

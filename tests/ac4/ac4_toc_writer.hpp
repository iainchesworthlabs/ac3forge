#pragma once

// A writer for hand-built raw_ac4_frame()s: the table of contents of either
// part, field by field, and the assembly of a frame from its substreams. It
// shares no code with src/ac4, so a frame it builds checks the inspector
// rather than restating it. Clause and table numbers are Part 2's for
// bitstream_version 2 and Part 1's below it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ac4dec/ac4dec_bits.hpp"

namespace ac4_toc_test {

using ac4dec_test::BitWriter;

// Part 1 Table 3's substream_index: 2 bits, 3 escaping to variable_bits(2).
inline void substream_index(BitWriter& w, int index) {
    if (index < 3) {
        w.put(static_cast<std::uint64_t>(index), 2);
    } else {
        w.put(3, 2);
        w.variable_bits(static_cast<std::uint64_t>(index - 3), 2);
    }
}

// ac4_sgi_specifier()'s group_index: 3 bits, 7 escaping.
inline void group_index(BitWriter& w, int index) {
    if (index < 7) {
        w.put(static_cast<std::uint64_t>(index), 3);
    } else {
        w.put(7, 3);
        w.variable_bits(static_cast<std::uint64_t>(index - 7), 2);
    }
}

// emdf_info() with version 0, key_id 0, an optional payloads substream, and
// no reserved bytes.
inline void emdf_info(BitWriter& w, std::optional<int> payloads_substream = std::nullopt) {
    w.put(0, 2);
    w.put(0, 3);
    w.flag(payloads_substream.has_value());
    if (payloads_substream) {
        substream_index(w, *payloads_substream);
    }
    w.put(0, 2);
    w.put(0, 2);
}

inline void presentation_version(BitWriter& w, int version) {
    for (int i = 0; i < version; ++i) {
        w.flag(true);
    }
    w.flag(false);
}

struct TocStart {
    int bitstream_version = 2;
    int sequence_counter = 1;
    int fs_index = 1;          // 48 kHz
    int frame_rate_index = 13;  // no frame rate multiply or fraction bits
    bool b_iframe_global = true;
    int n_presentations = 1;
};

// ac4_toc() up to the presentations: no wait frames, no payload base, and at
// bitstream_version 2 no program id.
inline void toc_start(BitWriter& w, const TocStart& start) {
    w.put(static_cast<std::uint64_t>(start.bitstream_version), 2);
    w.put(static_cast<std::uint64_t>(start.sequence_counter), 10);
    w.flag(false);  // b_wait_frames
    w.put(static_cast<std::uint64_t>(start.fs_index), 1);
    w.put(static_cast<std::uint64_t>(start.frame_rate_index), 4);
    w.flag(start.b_iframe_global);
    if (start.n_presentations == 1) {
        w.flag(true);  // b_single_presentation
    } else {
        w.flag(false);
        w.flag(true);  // b_more_presentations
        w.variable_bits(static_cast<std::uint64_t>(start.n_presentations - 2), 2);
    }
    w.flag(false);  // b_payload_base
    if (start.bitstream_version >= 2) {
        w.flag(false);  // b_program_id
    }
}

// Table 56's channel_mode code for a ch_mode (Table 88's for 0 to 10).
inline void channel_mode(BitWriter& w, int ch_mode) {
    switch (ch_mode) {
        case 0:
            w.put(0b0, 1);
            break;
        case 1:
            w.put(0b10, 2);
            break;
        case 2:
        case 3:
        case 4:
            w.put(0b1100U + static_cast<unsigned>(ch_mode - 2), 4);
            break;
        case 11:
        case 12:
            w.put(0b11111100U + static_cast<unsigned>(ch_mode - 11), 8);
            break;
        case 13:
        case 14:
        case 15:
            w.put(0b111111100U + static_cast<unsigned>(ch_mode - 13), 9);
            break;
        default:  // 5 to 10
            w.put(0b1111000U + static_cast<unsigned>(ch_mode - 5), 7);
            break;
    }
}

// One ac4_substream_info_chan() and, when its group has b_hsf_ext, the
// ac4_hsf_ext_substream_info() after it.
struct ChanInfo {
    int ch_mode = 1;
    std::optional<int> sf_multiplier;
    std::vector<bool> b_audio_ndot{true};
    int substream_index = 0;
    std::optional<int> hsf_ext_substream_index;
};

inline void chan_info(BitWriter& w, const ChanInfo& info, int fs_index, bool substreams_present) {
    channel_mode(w, info.ch_mode);
    if (info.ch_mode >= 11 && info.ch_mode <= 14) {
        w.flag(true);   // b_4_back_channels_present
        w.flag(true);   // b_centre_present
        w.put(3, 2);    // top_channels_present
    }
    if (fs_index == 1) {
        w.flag(info.sf_multiplier.has_value());
        if (info.sf_multiplier) {
            w.put(static_cast<std::uint64_t>(*info.sf_multiplier), 1);
        }
    }
    w.flag(false);  // b_bitrate_info
    if (info.ch_mode >= 7 && info.ch_mode <= 10) {
        w.flag(false);  // add_ch_base
    }
    for (const bool ndot : info.b_audio_ndot) {
        w.flag(ndot);
    }
    if (substreams_present) {
        substream_index(w, info.substream_index);
    }
}

// ac4_substream_group_info() of channel-coded substreams, 1 or 2 to 4.
inline void chan_group(BitWriter& w, const std::vector<ChanInfo>& infos, int fs_index = 1,
                       bool substreams_present = true, std::optional<int> content_classifier = std::nullopt) {
    bool hsf = false;
    for (const ChanInfo& info : infos) {
        hsf = hsf || info.hsf_ext_substream_index.has_value();
    }
    w.flag(substreams_present);
    w.flag(hsf);
    w.flag(infos.size() == 1);  // b_single_substream
    if (infos.size() != 1) {
        w.put(infos.size() - 2, 2);  // n_lf_substreams_minus2
    }
    w.flag(true);  // b_channel_coded
    for (const ChanInfo& info : infos) {
        chan_info(w, info, fs_index, substreams_present);
        if (hsf && substreams_present) {
            substream_index(w, info.hsf_ext_substream_index.value_or(0));
        }
    }
    w.flag(content_classifier.has_value());  // b_content_type
    if (content_classifier) {
        w.put(static_cast<std::uint64_t>(*content_classifier), 3);
        w.flag(false);  // b_language_indicator
    }
}

// ac4_presentation_v1_info() at frame_rate_index 13 unless `frame_rate_bits`
// is given (the frame_rate_multiply_info() and frame_rate_fractions_info()
// bits the index calls for).
struct PresV1 {
    std::optional<int> presentation_config;  // unset: b_single_substream_group
    std::vector<int> groups{0};
    int presentation_version = 1;
    int presentation_substream = 1;
    bool b_alternative = false;
    bool b_pres_ndot = true;
    std::vector<bool> frame_rate_bits;
    std::optional<int> emdf_payloads_substream;
};

inline void presentation_v1(BitWriter& w, const PresV1& p) {
    w.flag(!p.presentation_config.has_value());
    if (p.presentation_config) {
        if (*p.presentation_config < 7) {
            w.put(static_cast<std::uint64_t>(*p.presentation_config), 3);
        } else {
            w.put(7, 3);
            w.variable_bits(static_cast<std::uint64_t>(*p.presentation_config - 7), 2);
        }
    }
    presentation_version(w, p.presentation_version);
    w.put(0, 3);    // md_compat
    w.flag(false);  // b_presentation_id
    for (const bool bit : p.frame_rate_bits) {
        w.flag(bit);
    }
    emdf_info(w, p.emdf_payloads_substream);
    w.flag(false);  // b_presentation_filter
    if (!p.presentation_config) {
        group_index(w, p.groups.at(0));
    } else {
        w.flag(false);  // b_multi_pid
        if (*p.presentation_config == 5) {
            w.put(p.groups.size() - 2, 2);
        }
        for (const int group : p.groups) {
            group_index(w, group);
        }
    }
    w.flag(false);  // b_pre_virtualized
    w.flag(false);  // b_add_emdf_substreams
    w.flag(p.b_alternative);
    w.flag(p.b_pres_ndot);
    substream_index(w, p.presentation_substream);
}

// substream_index_table() with every size transmitted.
inline void index_table(BitWriter& w, const std::vector<std::size_t>& sizes) {
    if (sizes.size() < 4) {
        w.put(sizes.size(), 2);
    } else {
        w.put(0, 2);
        w.variable_bits(sizes.size() - 4, 2);
    }
    if (sizes.size() == 1) {
        w.flag(true);  // b_size_present
    }
    for (const std::size_t size : sizes) {
        w.flag(size >= 1024);  // b_more_bits
        w.put(size & 0x3FFU, 10);
        if (size >= 1024) {
            w.variable_bits(size >> 10U, 2);
        }
    }
}

// The table of contents, byte-aligned, then the substreams in index order.
inline std::vector<std::byte> assemble(const BitWriter& toc, const std::vector<std::vector<std::byte>>& substreams) {
    std::vector<std::byte> frame = toc.bytes();
    for (const auto& substream : substreams) {
        frame.insert(frame.end(), substream.begin(), substream.end());
    }
    return frame;
}

inline std::vector<std::size_t> sizes_of(const std::vector<std::vector<std::byte>>& substreams) {
    std::vector<std::size_t> sizes;
    for (const auto& substream : substreams) {
        sizes.push_back(substream.size());
    }
    return sizes;
}

}  // namespace ac4_toc_test

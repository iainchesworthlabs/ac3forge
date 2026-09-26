#include "frame/toc_writer.hpp"

#include <algorithm>
#include <cstdint>

namespace ac4::detail {
namespace {

// A field of `bits` bits whose all-ones value escapes to variable_bits(n_bits)
// for what is above it (substream_index, group_index, presentation_config).
void write_escaped(BitWriter& w, unsigned bits, unsigned n_bits, std::uint64_t value,
                   std::string_view name) {
    const std::uint64_t escape = (std::uint64_t{1} << bits) - 1;
    if (value < escape) {
        w.write(bits, value, name);
        return;
    }
    w.write(bits, escape, name);
    w.write_variable_bits(n_bits, value - escape, name);
}

// Part 1 Table 8, emdf_info(), with Table 80's emdf_protection() (headed
// emdf_reserved()) carrying no reserved bytes.
void write_emdf_info(BitWriter& w) {
    w.write(2, 0, "emdf_version");
    w.write(3, 0, "key_id");
    w.write(1, 0, "b_emdf_payloads_substream_info");
    w.write(2, 0, "n_skip_bytes_length_primary");
    w.write(2, 0, "n_skip_bytes_length_secondary");
}

// Clause 6.2.1.7 at bitstream_version 2: the group's index.
void write_sgi_specifier(BitWriter& w, int group) {
    write_escaped(w, 3, 2, static_cast<std::uint64_t>(group), "group_index");
}

// Part 1 Table 6, presentation_version(): a one per count, then a zero.
void write_presentation_version(BitWriter& w, int version) {
    for (int i = 0; i < version; ++i) {
        w.write(1, 1, "b_tmp");
    }
    w.write(1, 0, "b_tmp");
}

// Table 53: the ac4_sgi_specifier()s presentation_configs 0 to 4 read.
[[nodiscard]] constexpr std::size_t specifiers_of(int config) noexcept {
    return config == 3 || config == 4 ? 3 : 2;
}

// Clause 6.2.1.3, ac4_presentation_v1_info().
void write_presentation_v1_info(BitWriter& w, const TocLayout& layout, const TocPresentation& p) {
    w.write(1, p.presentation_config ? 0U : 1U, "b_single_substream_group");
    if (p.presentation_config) {
        write_escaped(w, 3, 2, static_cast<std::uint64_t>(*p.presentation_config), "presentation_config");
    }
    write_presentation_version(w, p.presentation_version);
    w.write(3, static_cast<std::uint64_t>(p.md_compat), "md_compat");
    w.write(1, p.presentation_id ? 1U : 0U, "b_presentation_id");
    if (p.presentation_id) {
        w.write_variable_bits(2, static_cast<std::uint64_t>(*p.presentation_id), "presentation_id");
    }
    // frame_rate_multiply_info() (Part 1 Table 7): no multiplier, so
    // frame_rate_factor 1, where the index has one to send; and
    // frame_rate_fractions_info() (6.2.1.4): a fraction of 1 at 47.95 fps and
    // up. Neither sends anything at index 13.
    const int index = layout.frame_rate_index;
    if (index <= 4 || (index >= 7 && index <= 9)) {
        w.write(1, 0, "b_multiplier");
    }
    if (index >= 5 && index <= 12) {
        w.write(1, 0, "b_frame_rate_fraction");
    }
    write_emdf_info(w);
    w.write(1, p.enable ? 1U : 0U, "b_presentation_filter");
    if (p.enable) {
        w.write(1, *p.enable ? 1U : 0U, "b_enable_presentation");
    }
    if (!p.presentation_config) {
        write_sgi_specifier(w, p.groups.front());
    } else {
        w.write(1, 0, "b_multi_pid");
        if (*p.presentation_config == 5) {
            // n_substream_groups_minus2, 3 escaping to variable_bits(2) for
            // five groups and more.
            write_escaped(w, 2, 2, p.groups.size() - 2, "n_substream_groups_minus2");
        }
        for (const int group : p.groups) {
            write_sgi_specifier(w, group);
        }
    }
    w.write(1, p.pre_virtualized ? 1U : 0U, "b_pre_virtualized");
    w.write(1, 0, "b_add_emdf_substreams");
    // ac4_presentation_substream_info(), 6.2.1.12.
    w.write(1, 0, "b_alternative");
    w.write(1, p.pres_ndot ? 1U : 0U, "b_pres_ndot");
    write_escaped(w, 2, 2, static_cast<std::uint64_t>(p.presentation_substream), "substream_index");
}

// Table 56's channel_mode code: 0b0 mono, 0b10 stereo, 0b1100 to 0b1110 3.0,
// 5.0 and 5.1, 0b1111000 to 0b1111101 the 7.X modes, and 0b11111100 and
// 0b11111101 7.0.4 and 7.1.4, which name the channels their source has.
void write_channel_mode(BitWriter& w, const TocSubstream& s) {
    const int ch_mode = s.ch_mode;
    if (ch_mode == 0) {
        w.write(1, 0b0, "channel_mode");
    } else if (ch_mode == 1) {
        w.write(2, 0b10, "channel_mode");
    } else if (ch_mode <= 4) {
        w.write(4, 0b1100U + static_cast<unsigned>(ch_mode - 2), "channel_mode");
    } else if (ch_mode <= 10) {
        w.write(7, 0b1111000U + static_cast<unsigned>(ch_mode - 5), "channel_mode");
    } else {
        w.write(8, 0b11111100U + static_cast<unsigned>(ch_mode - 11), "channel_mode");
        w.write(1, s.b_4_back_channels_present ? 1U : 0U, "b_4_back_channels_present");
        w.write(1, s.b_centre_present ? 1U : 0U, "b_centre_present");
        w.write(2, static_cast<std::uint64_t>(s.top_channels_present), "top_channels_present");
    }
}

// A bed by std_bed_channel_assignment_flag[] or
// nonstd_bed_channel_assignment_flag[], as both bed_dyn_obj_assignment() and
// ac4_substream_info_obj() send one.
void write_bed_flags(BitWriter& w, const TocObjectAssignment& a) {
    const bool nonstd = a.kind == TocObjectAssignment::Kind::kBedNonstdFlags;
    w.write(1, nonstd ? 1U : 0U, "b_nonstd_bed_channel_assignment_flags_present");
    if (nonstd) {
        w.write(17, static_cast<std::uint64_t>(a.flags), "nonstd_bed_channel_assignment_flag");
    } else {
        w.write(10, static_cast<std::uint64_t>(a.flags), "std_bed_channel_assignment_flag");
    }
}

// Clause 6.2.1.10, bed_dyn_obj_assignment(n_signals).
void write_assignment(BitWriter& w, const TocObjectAssignment& a, int n_signals) {
    using Kind = TocObjectAssignment::Kind;
    w.write(1, a.kind == Kind::kDynamic ? 1U : 0U, "b_dyn_objects_only");
    if (a.kind == Kind::kDynamic) {
        return;
    }
    w.write(1, a.kind == Kind::kIsf ? 1U : 0U, "b_isf");
    if (a.kind == Kind::kIsf) {
        w.write(3, static_cast<std::uint64_t>(a.code), "isf_config");
        return;
    }
    w.write(1, a.kind == Kind::kBedCode ? 1U : 0U, "b_ch_assign_code");
    if (a.kind == Kind::kBedCode) {
        w.write(3, static_cast<std::uint64_t>(a.code), "bed_chan_assign_code");
        return;
    }
    w.write(1, a.kind == Kind::kBedList ? 0U : 1U, "b_channel_assignment_flags_present");
    if (a.kind != Kind::kBedList) {
        write_bed_flags(w, a);
        return;
    }
    // n_bed_signals_minus1 in ceil(log2(n_signals)) bits, for more than one.
    if (n_signals > 1) {
        unsigned bits = 0;
        while ((1 << bits) < n_signals) {
            ++bits;
        }
        w.write(bits, a.list.size() - 1, "n_bed_signals_minus1");
    }
    for (const int assignment : a.list) {
        w.write(4, static_cast<std::uint64_t>(assignment), "nonstd_bed_channel_assignment");
    }
}

// Clause 6.2.1.9, ac4_substream_info_ajoc(), or 6.2.1.11,
// ac4_substream_info_obj(), after the group's b_ajoc; from b_sf_multiplier on
// the two are alike.
void write_object_substream_info(BitWriter& w, const TocLayout& layout,
                                 const TocObjectSubstream& s) {
    w.write(1, s.ajoc ? 1U : 0U, "b_ajoc");
    if (s.ajoc) {
        w.write(1, s.lfe ? 1U : 0U, "b_lfe");
        w.write(1, s.static_dmx ? 1U : 0U, "b_static_dmx");
        if (!s.static_dmx) {
            w.write(4, static_cast<std::uint64_t>(s.dmx_signals - 1),
                    "n_fullband_dmx_signals_minus1");
            write_assignment(w, s.dmx, s.dmx_signals);
        }
        w.write(1, s.oamd_common ? 1U : 0U, "b_oamd_common_data_present");
        if (s.oamd_common) {
            write_oamd_common_data(w, *s.oamd_common);
        }
        const int umx = s.umx_signals;
        w.write(4, static_cast<std::uint64_t>(std::min(umx, 16) - 1),
                "n_fullband_upmix_signals_minus1");
        if (umx >= 16) {
            w.write_variable_bits(3, static_cast<std::uint64_t>(umx - 16),
                                  "n_fullband_upmix_signals");
        }
        write_assignment(w, s.umx, umx);
    } else {
        w.write(3, static_cast<std::uint64_t>(s.n_objects_code), "n_objects_code");
        w.write(1, s.dynamic ? 1U : 0U, "b_dynamic_objects");
        if (s.dynamic) {
            w.write(1, s.lfe ? 1U : 0U, "b_lfe");
        } else if (s.static_kind == TocObjectSubstream::Static::kBed) {
            w.write(1, 1, "b_bed_objects");
            w.write(1, s.start ? 1U : 0U, "b_bed_start");
            if (s.start) {
                const TocObjectAssignment& a = s.start_assignment;
                const bool code = a.kind == TocObjectAssignment::Kind::kBedCode;
                w.write(1, code ? 1U : 0U, "b_ch_assign_code");
                if (code) {
                    w.write(3, static_cast<std::uint64_t>(a.code), "bed_chan_assign_code");
                } else {
                    write_bed_flags(w, a);
                }
            }
        } else {
            w.write(1, 0, "b_bed_objects");
            const bool isf = s.static_kind == TocObjectSubstream::Static::kIsf;
            w.write(1, isf ? 1U : 0U, "b_isf");
            if (isf) {
                w.write(1, s.start ? 1U : 0U, "b_isf_start");
                if (s.start) {
                    w.write(3, static_cast<std::uint64_t>(s.start_assignment.code), "isf_config");
                }
            } else {
                w.write(4, static_cast<std::uint64_t>(s.res_bytes), "res_bytes");
                for (int i = 0; i < s.res_bytes; ++i) {
                    w.write(8, 0, "reserved_data");
                }
            }
        }
    }
    if (layout.fs_index == 1) {
        w.write(1, 0, "b_sf_multiplier");
    }
    w.write(1, 0, "b_bitrate_info");
    // frame_rate_factor is 1.
    w.write(1, s.iframe ? 1U : 0U, "b_audio_ndot");
    write_escaped(w, 2, 2, static_cast<std::uint64_t>(s.substream_index), "substream_index");
}

// Clause 6.2.1.6, ac4_substream_group_info(): each ac4_substream_info_chan()
// (6.2.1.8), or the OAMD substream's oamd_substream_info() (6.2.1.13) and each
// object audio substream's info, then the content_type() (Part 1 Table 10).
void write_substream_group_info(BitWriter& w, const TocLayout& layout, const TocGroup& g) {
    const bool channel_coded = g.objects.empty();
    const std::size_t count = channel_coded ? g.substreams.size() : g.objects.size();
    w.write(1, 1, "b_substreams_present");
    w.write(1, 0, "b_hsf_ext");
    w.write(1, count == 1 ? 1U : 0U, "b_single_substream");
    if (count != 1) {
        write_escaped(w, 2, 2, count - 2, "n_lf_substreams_minus2");
    }
    w.write(1, channel_coded ? 1U : 0U, "b_channel_coded");
    if (!channel_coded) {
        w.write(1, g.oamd_substream ? 1U : 0U, "b_oamd_substream");
        if (g.oamd_substream) {
            w.write(1, g.oamd_iframe ? 1U : 0U, "b_oamd_ndot");
            write_escaped(w, 2, 2, static_cast<std::uint64_t>(*g.oamd_substream),
                          "substream_index");
        }
        for (const TocObjectSubstream& s : g.objects) {
            write_object_substream_info(w, layout, s);
        }
    }
    for (const TocSubstream& s : channel_coded ? std::span<const TocSubstream>(g.substreams)
                                               : std::span<const TocSubstream>{}) {
        write_channel_mode(w, s);
        if (layout.fs_index == 1) {
            w.write(1, 0, "b_sf_multiplier");
        }
        w.write(1, 0, "b_bitrate_info");
        if (s.ch_mode >= 7 && s.ch_mode <= 10) {
            w.write(1, s.add_ch_base ? 1U : 0U, "add_ch_base");
        }
        // frame_rate_factor is 1.
        w.write(1, s.iframe ? 1U : 0U, "b_audio_ndot");
        write_escaped(w, 2, 2, static_cast<std::uint64_t>(s.substream_index), "substream_index");
    }
    w.write(1, g.content_classifier ? 1U : 0U, "b_content_type");
    if (g.content_classifier) {
        w.write(3, static_cast<std::uint64_t>(*g.content_classifier), "content_classifier");
        w.write(1, g.language.empty() ? 0U : 1U, "b_language_indicator");
        if (!g.language.empty()) {
            w.write(1, 0, "b_serialized_language_tag");
            w.write(6, g.language.size(), "n_language_tag_bytes");
            for (const char c : g.language) {
                w.write(8, static_cast<unsigned char>(c), "language_tag_bytes");
            }
        }
    }
}

// Part 1 Table 14, substream_index_table(): b_size_present where there is one
// substream, and every size's low 10 bits with variable_bits(2) for the rest.
void write_substream_index_table(BitWriter& w, std::span<const std::size_t> sizes) {
    if (sizes.size() < 4) {
        w.write(2, sizes.size(), "n_substreams");
    } else {
        w.write(2, 0, "n_substreams");
        w.write_variable_bits(2, sizes.size() - 4, "n_substreams");
    }
    if (sizes.size() == 1) {
        w.write(1, 1, "b_size_present");
    }
    for (const std::size_t size : sizes) {
        const bool more = size >= 1024;
        w.write(1, more ? 1U : 0U, "b_more_bits");
        w.write(10, size & 0x3FFU, "substream_size");
        if (more) {
            w.write_variable_bits(2, size >> 10U, "substream_size");
        }
    }
}

// Whether the syntax can send the layout, and every index in it names
// something: each group once at least, and the presentation substreams and
// audio substreams among `count`.
[[nodiscard]] bool valid(const TocLayout& layout, std::size_t count) {
    if (layout.presentations.empty() || layout.groups.empty()) {
        return false;
    }
    std::vector<bool> named(layout.groups.size(), false);
    for (const TocPresentation& p : layout.presentations) {
        const std::size_t specifiers =
            !p.presentation_config ? 1
            : *p.presentation_config <= 4 ? specifiers_of(*p.presentation_config)
                                          : std::max<std::size_t>(p.groups.size(), 2);
        if ((p.presentation_config && (*p.presentation_config < 0 || *p.presentation_config > 5)) ||
            p.groups.size() != specifiers || p.md_compat < 0 || p.md_compat > 7 ||
            p.presentation_version < 0 || p.presentation_substream < 0 ||
            static_cast<std::size_t>(p.presentation_substream) >= count) {
            return false;
        }
        for (const int group : p.groups) {
            if (group < 0 || static_cast<std::size_t>(group) >= layout.groups.size()) {
                return false;
            }
            named[static_cast<std::size_t>(group)] = true;
        }
    }
    if (std::ranges::find(named, false) != named.end()) {
        return false;  // total_n_substream_groups comes from the indices named
    }
    for (const TocGroup& g : layout.groups) {
        // A group is channel coded or object coded, never both or neither.
        if (g.substreams.empty() == g.objects.empty() || g.language.size() > 63 ||
            (g.content_classifier && (*g.content_classifier < 0 || *g.content_classifier > 7))) {
            return false;
        }
        if (g.oamd_substream &&
            (*g.oamd_substream < 0 || static_cast<std::size_t>(*g.oamd_substream) >= count)) {
            return false;
        }
        for (const TocObjectSubstream& s : g.objects) {
            if (s.substream_index < 0 || static_cast<std::size_t>(s.substream_index) >= count ||
                s.dmx_signals < 1 || s.dmx_signals > 16 || s.umx_signals < 1 ||
                s.n_objects_code < 0 || s.n_objects_code > 7 || s.res_bytes < 0 ||
                s.res_bytes > 15) {
                return false;
            }
        }
        for (const TocSubstream& s : g.substreams) {
            if (s.ch_mode < 0 || s.ch_mode > 12 || s.top_channels_present < 0 ||
                s.top_channels_present > 3 || s.substream_index < 0 ||
                static_cast<std::size_t>(s.substream_index) >= count) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

void write_toc(BitWriter& w, const TocLayout& layout, std::size_t payload_base,
               std::span<const std::size_t> sizes) {
    w.write(2, 2, "bitstream_version");
    w.write(10, static_cast<std::uint64_t>(layout.sequence_counter), "sequence_counter");
    w.write(1, 1, "b_wait_frames");
    w.write(3, static_cast<std::uint64_t>(layout.wait_frames), "wait_frames");
    if (layout.wait_frames > 0) {
        w.write(2, static_cast<std::uint64_t>(layout.br_code), "br_code");
    }
    w.write(1, static_cast<std::uint64_t>(layout.fs_index), "fs_index");
    w.write(4, static_cast<std::uint64_t>(layout.frame_rate_index), "frame_rate_index");
    w.write(1, layout.iframe_global ? 1U : 0U, "b_iframe_global");
    const std::size_t n = layout.presentations.size();
    w.write(1, n == 1 ? 1U : 0U, "b_single_presentation");
    if (n != 1) {
        w.write(1, n >= 2 ? 1U : 0U, "b_more_presentations");
        if (n >= 2) {
            w.write_variable_bits(2, n - 2, "n_presentations");
        }
    }
    w.write(1, payload_base > 0 ? 1U : 0U, "b_payload_base");
    if (payload_base > 0) {
        // payload_base_minus1 is 5 bits; 32 and up take variable_bits(3) on top.
        const std::size_t minus1 = payload_base >= 32 ? 31 : payload_base - 1;
        w.write(5, minus1, "payload_base_minus1");
        if (payload_base >= 32) {
            w.write_variable_bits(3, payload_base - 32, "payload_base");
        }
    }
    w.write(1, 0, "b_program_id");
    for (const TocPresentation& p : layout.presentations) {
        write_presentation_v1_info(w, layout, p);
    }
    for (const TocGroup& g : layout.groups) {
        write_substream_group_info(w, layout, g);
    }
    write_substream_index_table(w, sizes);
    w.align();
}

std::size_t toc_bytes(const TocLayout& layout, std::size_t payload_base, std::span<const std::size_t> sizes) {
    BitWriter w;
    write_toc(w, layout, payload_base, sizes);
    return w.byte_size();
}

std::optional<std::vector<std::byte>> assemble_frame(const TocLayout& layout,
                                                     std::span<const std::vector<std::byte>> substreams) {
    if (!valid(layout, substreams.size())) {
        return std::nullopt;
    }
    std::vector<std::size_t> sizes;
    std::size_t total = 0;
    for (const std::vector<std::byte>& substream : substreams) {
        sizes.push_back(substream.size());
        total += substream.size();
    }
    BitWriter toc;
    write_toc(toc, layout, 0, sizes);
    std::vector<std::byte> frame;
    frame.reserve(toc.byte_size() + total);
    frame.insert(frame.end(), toc.bytes().begin(), toc.bytes().end());
    for (const std::vector<std::byte>& substream : substreams) {
        frame.insert(frame.end(), substream.begin(), substream.end());
    }
    return frame;
}

}  // namespace ac4::detail

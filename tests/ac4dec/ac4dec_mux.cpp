#include "ac4dec_mux.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4enc/encoder.hpp"
#include "bit_writer.hpp"
#include "frame/toc_writer.hpp"

namespace ac4dec_test {
namespace {

using ac4::SyntaxRecord;
using ac4::detail::BitWriter;

// One frame of a source, taken apart: each substream's bytes and the records
// the decoder read from it, and what the table of contents says of them.
struct SourceFrame {
    std::span<const std::byte> presentation;
    std::span<const std::byte> audio;
    std::vector<SyntaxRecord> presentation_records;
    std::vector<SyntaxRecord> audio_records;
    int ch_mode = 1;
    bool audio_iframe = false;
    bool pres_ndot = false;
    int sequence_counter = 0;
    int fs_index = 1;
    int frame_rate_index = 13;
};

[[noreturn]] void refuse(const std::string& what) {
    throw std::runtime_error("multiplex: " + what);
}

// Copies bits [from, to) of `bytes` after what `w` holds, unrecorded.
void copy_bits(BitWriter& w, std::span<const std::byte> bytes, std::size_t from, std::size_t to) {
    for (std::size_t bit = from; bit < to; ++bit) {
        const auto byte = std::to_integer<unsigned>(bytes[bit / 8]);
        w.write_unrecorded(1, (byte >> (7 - (bit % 8))) & 1U);
    }
}

// Where the first record named `name` starts; refuses where there is none.
[[nodiscard]] std::size_t offset_of(std::span<const SyntaxRecord> records, std::string_view name) {
    const auto it = std::ranges::find(records, name, &SyntaxRecord::name);
    if (it == records.end()) {
        refuse("no " + std::string{name} + " in the source substream");
    }
    return it->bit_offset;
}

// Where the last record ends: the end of the substream's syntax, before its
// final byte_align.
[[nodiscard]] std::size_t end_of(std::span<const SyntaxRecord> records) {
    std::size_t end = 0;
    for (const SyntaxRecord& r : records) {
        end = std::max<std::size_t>(end, std::size_t{r.bit_offset} + r.bits);
    }
    return end;
}

// A size field of `bits` bits and variable_bits(3) above it, then the
// element (tools_metadata_size, Part 2 clause 6.2.7.1).
void write_sized(BitWriter& w, const BitWriter& element, unsigned bits) {
    const std::size_t size = element.bit_position();
    const std::uint64_t low = size & ((1U << bits) - 1U);
    const std::uint64_t high = size >> bits;
    w.write(bits, low, "tools_metadata_size_value");
    w.write(1, high > 0 ? 1U : 0U, "b_more_bits");
    if (high > 0) {
        w.write_variable_bits(3, high, "tools_metadata_size");
    }
    w.append(element);
}

// The group's audio substream: the source's audio_size header and
// audio_data() as they are, basic_metadata() as it is, extended_metadata()
// with the layout's dialogue fields, and the tools with the layout's dialogue
// enhancement where it has one (Part 2 clause 6.2.7.1, sus_ver 1).
[[nodiscard]] std::vector<std::byte> audio_substream(const SourceFrame& f, const MuxGroup& group,
                                                     const ac4::detail::DeFrameParameters* previous) {
    const std::span<const SyntaxRecord> records = f.audio_records;
    const std::size_t extended = offset_of(records, "b_dialog");
    const std::size_t tools = offset_of(records, "tools_metadata_size_value");
    const std::size_t end = end_of(records);
    BitWriter w;
    copy_bits(w, f.audio, 0, extended);
    ac4::detail::write_extended_metadata(w, f.ch_mode, group.dialogue ? &*group.dialogue : nullptr);
    if (group.de) {
        const std::size_t de = offset_of(records, "b_de_data_present");
        const std::size_t emdf = offset_of(records, "b_emdf_payloads_substream");
        if (de < tools) {
            refuse("dialog_enhancement() before tools_metadata_size");
        }
        BitWriter element = BitWriter::buffered();
        // At sus_ver 1 the tools are dialog_enhancement() alone.
        ac4::detail::write_dialog_enhancement(element, &group.de->config, &group.de->parameters, previous,
                                              f.audio_iframe);
        write_sized(w, element, 7);
        copy_bits(w, f.audio, emdf, end);
    } else {
        copy_bits(w, f.audio, tools, end);
    }
    w.align();
    return w.bytes();
}

// The presentation's presentation substream: the source's as it is up to
// b_associated, then the layout's group gains and associated audio's values,
// then the source's custom_dmx_data() and loud_corr() (Part 2 clause
// 6.2.2.3). The source's presentation has one group and no associated audio,
// so its b_associated is one bit, 0.
[[nodiscard]] std::vector<std::byte> presentation_substream(const SourceFrame& f,
                                                            const ac4::detail::PresentationMixCodes& mix) {
    const std::span<const SyntaxRecord> records = f.presentation_records;
    const auto associated = std::ranges::find(records, std::string_view{"b_associated"}, &SyntaxRecord::name);
    if (associated == records.end() || associated->value != 0) {
        refuse("the source's presentation substream carries associated audio");
    }
    BitWriter w;
    copy_bits(w, f.presentation, 0, associated->bit_offset);
    ac4::detail::write_presentation_mix(w, mix);
    copy_bits(w, f.presentation, std::size_t{associated->bit_offset} + 1, end_of(records));
    w.align();
    return w.bytes();
}

// Part 2 clause 6.2.1.3's n_substream_groups for a configuration.
[[nodiscard]] int substream_groups(const MuxPresentation& p) {
    if (!p.presentation_config) {
        return 1;
    }
    switch (*p.presentation_config) {
        case 1:
            return 1;
        case 3:
            return 3;
        case 5:
            return static_cast<int>(p.groups.size());
        default:
            return 2;
    }
}

}  // namespace

MuxSource mux_source(std::span<const std::byte> file) {
    MuxSource source;
    const ac4::ScanResult scan = ac4::scan(file);
    for (const ac4::SyncFrame& frame : scan.frames) {
        source.frames.emplace_back(frame.raw_ac4_frame.begin(), frame.raw_ac4_frame.end());
    }
    return source;
}

std::vector<std::vector<std::byte>> multiplex(std::span<const MuxSource> sources, const MuxLayout& layout,
                                              std::size_t frames) {
    // One decoder per source, carrying its I-frame configuration from frame to
    // frame, to read each frame's syntax and so find the fields to replace.
    std::vector<SyntaxRecord> records;
    const auto keep = [&records](const SyntaxRecord& r) { records.push_back(r); };
    std::vector<ac4::Decoder> decoders;
    for (std::size_t s = 0; s < sources.size(); ++s) {
        ac4::DecoderConfig config;
        config.syntax = keep;
        decoders.emplace_back(config);
    }
    // Each group's last parameters, which a hybrid method's frames code
    // against.
    std::vector<std::optional<ac4::detail::DeFrameParameters>> previous(layout.groups.size());
    std::vector<std::vector<std::byte>> out;
    std::vector<SourceFrame> taken(sources.size());
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t s = 0; s < sources.size(); ++s) {
            if (f >= sources[s].frames.size()) {
                refuse("source " + std::to_string(s) + " has fewer frames than asked for");
            }
            const std::span<const std::byte> raw = sources[s].frames[f];
            const auto parsed = ac4::parse_raw_frame(raw);
            if (!parsed || parsed->toc.presentations_v1.size() != 1 || parsed->toc.substream_groups.size() != 1 ||
                parsed->toc.substream_groups[0].substreams.size() != 1 ||
                !parsed->toc.substream_groups[0].substreams[0].chan) {
                refuse("source " + std::to_string(s) + " is not one presentation of one substream");
            }
            const ac4::Toc& toc = parsed->toc;
            const ac4::PresentationInfoV1& p = toc.presentations_v1[0];
            const ac4::ChannelSubstreamInfo& chan = *toc.substream_groups[0].substreams[0].chan;
            if (!p.presentation_substream_index || !chan.substream_index || !chan.ch_mode) {
                refuse("source " + std::to_string(s) + " has no presentation or audio substream");
            }
            records.clear();
            const auto report = decoders[s].parse(raw);
            if (!report) {
                refuse("source " + std::to_string(s) + "'s table of contents does not read");
            }
            SourceFrame& frame = taken[s];
            const int presentation = *p.presentation_substream_index;
            const int audio = *chan.substream_index;
            const ac4::Substream& located_presentation = parsed->substreams[static_cast<std::size_t>(presentation)];
            const ac4::Substream& located_audio = parsed->substreams[static_cast<std::size_t>(audio)];
            frame.presentation = raw.subspan(located_presentation.offset, located_presentation.size);
            frame.audio = raw.subspan(located_audio.offset, located_audio.size);
            frame.presentation_records.clear();
            frame.audio_records.clear();
            for (const SyntaxRecord& r : records) {
                if (r.substream == presentation) {
                    frame.presentation_records.push_back(r);
                } else if (r.substream == audio) {
                    frame.audio_records.push_back(r);
                }
            }
            frame.ch_mode = *chan.ch_mode;
            frame.audio_iframe = !chan.b_iframe.empty() && chan.b_iframe.front();
            frame.pres_ndot = p.b_pres_ndot;
            frame.sequence_counter = toc.sequence_counter;
            frame.fs_index = toc.sample_rate_hz == 44100 ? 0 : 1;
            frame.frame_rate_index = toc.frame_rate_index;
        }

        ac4::detail::TocLayout toc;
        toc.sequence_counter = taken.front().sequence_counter;
        // The frames are as long as their substreams make them: a variable
        // rate (Part 1 Table 81).
        toc.wait_frames = 7;
        toc.br_code = 3;
        toc.fs_index = taken.front().fs_index;
        toc.frame_rate_index = taken.front().frame_rate_index;
        std::vector<std::vector<std::byte>> substreams;
        bool iframe = true;
        for (std::size_t i = 0; i < layout.presentations.size(); ++i) {
            const MuxPresentation& p = layout.presentations[i];
            const SourceFrame& from = taken.at(p.source);
            ac4::detail::PresentationMixCodes mix = p.mix;
            mix.n_substream_groups = substream_groups(p);
            substreams.push_back(presentation_substream(from, mix));
            toc.presentations.push_back(ac4::detail::TocPresentation{.presentation_config = p.presentation_config,
                                                                     .groups = p.groups,
                                                                     .presentation_version = 1,
                                                                     .md_compat = p.md_compat,
                                                                     .presentation_id = p.presentation_id,
                                                                     .enable = p.enable,
                                                                     .pre_virtualized = p.pre_virtualized,
                                                                     .pres_ndot = from.pres_ndot,
                                                                     .presentation_substream = static_cast<int>(i)});
            iframe = iframe && from.pres_ndot;
        }
        for (std::size_t g = 0; g < layout.groups.size(); ++g) {
            const MuxGroup& group = layout.groups[g];
            const SourceFrame& from = taken.at(group.source);
            substreams.push_back(audio_substream(from, group, previous[g] ? &*previous[g] : nullptr));
            if (group.de) {
                previous[g] = group.de->parameters;
            }
            ac4::detail::TocGroup written;
            written.substreams.push_back(ac4::detail::TocSubstream{
                .ch_mode = from.ch_mode,
                .add_ch_base = false,
                .iframe = from.audio_iframe,
                .substream_index = static_cast<int>(layout.presentations.size() + g)});
            written.content_classifier = group.content_classifier;
            written.language = group.language;
            toc.groups.push_back(std::move(written));
            iframe = iframe && from.audio_iframe;
        }
        toc.iframe_global = iframe;
        std::optional<std::vector<std::byte>> frame = ac4::detail::assemble_frame(toc, substreams);
        if (!frame) {
            refuse("the layout's table of contents cannot be written");
        }
        out.push_back(std::move(*frame));
    }
    return out;
}

std::vector<std::byte> mux_sync_framed(std::span<const std::vector<std::byte>> frames) {
    std::vector<std::byte> out;
    for (const std::vector<std::byte>& frame : frames) {
        const std::vector<std::byte> framed = ac4::sync_frame(frame, true);
        out.insert(out.end(), framed.begin(), framed.end());
    }
    return out;
}

}  // namespace ac4dec_test

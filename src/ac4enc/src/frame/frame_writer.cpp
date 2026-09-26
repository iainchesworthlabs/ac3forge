#include "frame/frame_writer.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ac4::detail {
namespace {

// Substream indices in substream_index_table() of the frame of one
// presentation: the presentation substream first, then the audio substream.
constexpr int kPresentationSubstream = 0;
constexpr int kAudioSubstream = 1;

// Part 1 Table 88's channel modes with an LFE: 5.1 and the three 7.1s; and
// Part 2 Table 56's 7.1.4.
[[nodiscard]] bool has_lfe(int ch_mode) noexcept {
    return ch_mode == 4 || ch_mode == 6 || ch_mode == 8 || ch_mode == 10 || ch_mode == 12;
}

// The presentation's channels for custom_dmx_data() and loud_corr(): one
// substream's, with Part 2 Table 71's core for the 7.X.4 modes and Table 72's
// top pairs from top_channels_present.
[[nodiscard]] PresentationChannels presentation_channels(const FrameFields& f) noexcept {
    PresentationChannels p;
    p.ch_mode = f.ch_mode;
    p.lfe = has_lfe(f.ch_mode);
    if (f.ch_mode == 11 || f.ch_mode == 12) {
        p.ch_mode_core = f.ch_mode == 11 ? 5 : 6;
        p.back = f.b_4_back_channels_present;
        p.top_channel_pairs =
            f.top_channels_present == 3 ? 2 : (f.top_channels_present == 0 ? 0 : 1);
    }
    return p;
}

// A size field of `bits` bits, and variable_bits(3) for what is above them
// (drc_metadata_size, tools_metadata_size), then the element it sizes.
void write_sized(BitWriter& w, const BitWriter& element, unsigned bits, std::string_view value_name,
                 std::string_view extension_name) {
    const std::size_t size = element.bit_position();
    const std::uint64_t low = size & ((1U << bits) - 1U);
    const std::uint64_t high = size >> bits;
    w.write(bits, low, value_name);
    w.write(1, high > 0 ? 1U : 0U, "b_more_bits");
    if (high > 0) {
        w.write_variable_bits(3, high, extension_name);
    }
    w.append(element);
}

// Part 2 clause 6.2.7.1, metadata(), for a channel-coded substream at sus_ver
// 1 without b_alternative's object data: basic_metadata() (6.2.7.2) with
// nothing optional, extended_metadata() (6.2.7.4) with a dialogue
// substream's fields, dialog_enhancement() (6.2.7.5) with data where dialogue
// enhancement is configured, and the EMDF payloads.
void write_metadata(BitWriter& w, const AudioSubstreamFields& f) {
    w.write(1, 0, "b_more_basic_metadata");
    write_extended_metadata(w, f.ch_mode, f.dialogue);
    BitWriter tools = BitWriter::buffered();
    write_dialog_enhancement(tools, f.de_config, f.de, f.de_previous, f.iframe);
    write_sized(w, tools, 7, "tools_metadata_size_value", "tools_metadata_size");
    w.write(1, f.emdf.empty() ? 0U : 1U, "b_emdf_payloads_substream");
    if (!f.emdf.empty()) {
        write_emdf_payloads(w, f.emdf);
    }
    w.align();
}

[[nodiscard]] std::size_t metadata_bytes(const AudioSubstreamFields& f) {
    BitWriter w;
    write_metadata(w, f);
    return w.byte_size();
}

// Part 2 clause 6.2.2.2: audio_size_value's 15 bits and b_more_bits, and
// variable_bits(7) for the size above 15 bits.
[[nodiscard]] std::size_t audio_header_bytes(std::size_t audio_size) {
    const std::size_t bits = 16 + (audio_size >= 0x8000 ? variable_bits_width(7, audio_size >> 15U) : 0U);
    return (bits + 7) / 8;
}

// The audio_size that makes an audio substream `substream` bytes long with
// `metadata` bytes of metadata(); nothing where none does.
[[nodiscard]] std::optional<std::size_t> audio_size_for(std::size_t substream,
                                                        std::size_t metadata) {
    if (substream < metadata + 2) {
        return std::nullopt;
    }
    std::size_t audio_size = substream - metadata - 2;
    if (audio_header_bytes(audio_size) != 2) {
        audio_size = substream - metadata - audio_header_bytes(audio_size);
        if (audio_header_bytes(audio_size) + audio_size + metadata != substream) {
            return std::nullopt;
        }
    }
    return audio_size;
}

}  // namespace

std::size_t audio_substream_overhead_bits(const AudioSubstreamFields& fields,
                                          std::size_t substream_bytes) {
    // One byte of alignment ahead of metadata(), at most, on top.
    return 8 * (audio_header_bytes(substream_bytes) + metadata_bytes(fields) + 1);
}

std::size_t audio_substream_bytes(const AudioSubstreamFields& fields, std::size_t audio_bytes) {
    return audio_header_bytes(audio_bytes) + audio_bytes + metadata_bytes(fields);
}

bool audio_substream_size_possible(const AudioSubstreamFields& fields,
                                   std::size_t substream_bytes) {
    return audio_size_for(substream_bytes, metadata_bytes(fields)).has_value();
}

std::optional<BitWriter> write_audio_substream(const AudioSubstreamFields& fields,
                                               const BitWriter& audio,
                                               std::size_t substream_bytes) {
    const std::size_t needed = (audio.bit_position() + 7) / 8;
    std::size_t audio_size = needed;
    if (substream_bytes > 0) {
        const std::optional<std::size_t> fitted =
            audio_size_for(substream_bytes, metadata_bytes(fields));
        if (!fitted || *fitted < needed) {
            return std::nullopt;
        }
        audio_size = *fitted;
    }
    BitWriter w = BitWriter::buffered();
    w.write(15, audio_size & 0x7FFFU, "audio_size_value");
    const bool more = audio_size >= 0x8000;
    w.write(1, more ? 1U : 0U, "b_more_bits");
    if (more) {
        w.write_variable_bits(7, audio_size >> 15U, "audio_size_value");
    }
    const std::size_t start = w.bit_position();
    w.append(audio);
    // fill_bits and byte_align, to audio_size bytes after audio_data() began.
    while (w.bit_position() < start + 8 * audio_size) {
        w.write_unrecorded(1, 0);
    }
    write_metadata(w, fields);
    if (substream_bytes > 0 && w.byte_size() != substream_bytes) {
        return std::nullopt;
    }
    return w;
}

BitWriter write_presentation_substream(const PresentationSubstreamFields& f) {
    // Part 2 clause 6.2.2.3: b_alternative's name and target, no additional
    // data, dialogue normalisation, further_loudness_info() where it is
    // configured, a drc_frame(), the mixing values, and custom_dmx_data() and
    // loud_corr() (6.2.9.2, 6.2.9.1), which read nothing for a mono or stereo
    // presentation, and for the others the stereo coefficients and their
    // corrections in I-frames where they are configured, with no custom
    // downmix data or corrections for the immersive outputs.
    BitWriter w = BitWriter::buffered();
    if (f.alternative != nullptr) {
        write_alternative(w, *f.alternative);
    }
    w.write(1, f.immersive_audio_indicator ? 1U : 0U, "b_additional_data");
    if (f.immersive_audio_indicator) {
        // One byte (add_data_bytes_minus1 0) after the byte_align: the
        // indicator, b_advanced_de_data_present, and the rest add_data. A
        // channel-based presentation has a pres_ch_mode, so no
        // b_oamd_common_timing.
        w.write(4, 0, "add_data_bytes_minus1");
        w.align();
        w.write(1, 1, "immersive_audio_indicator");
        w.write(1, 0, "b_advanced_de_data_present");
        w.write_zero_run(6, "add_data");
    }
    w.write(7, static_cast<std::uint64_t>(f.dialnorm_bits), "dialnorm_bits");
    w.write(1, f.loudness != nullptr ? 1U : 0U, "b_further_loudness_info");
    if (f.loudness != nullptr) {
        write_further_loudness_info(w, *f.loudness, f.iframe);
    }
    BitWriter drc = BitWriter::buffered();
    write_drc_frame(drc, f.drc, f.iframe, f.drc_gains);
    write_sized(w, drc, 5, "drc_metadata_size_value", "drc_metadata_size");
    write_presentation_mix(w, f.mix);
    write_downmix(w, f.channels, f.downmix, f.iframe);
    w.align();
    return w;
}

BitWriter write_emdf_payloads_substream(std::span<const EmdfPayloadCodes> payloads) {
    BitWriter w = BitWriter::buffered();
    write_emdf_payloads(w, payloads);
    return w;
}

std::optional<std::vector<std::byte>> assemble(const TocLayout& layout,
                                               std::span<const BitWriter> substreams,
                                               std::size_t payload_base, SyntaxSink sink) {
    if (!writable(layout, substreams.size())) {
        return std::nullopt;
    }
    std::vector<std::size_t> sizes;
    std::size_t total = 0;
    for (const BitWriter& substream : substreams) {
        sizes.push_back(substream.byte_size());
        total += substream.byte_size();
    }
    BitWriter toc;
    write_toc(toc, layout, payload_base, sizes);
    std::vector<std::byte> frame;
    frame.reserve(toc.byte_size() + payload_base + total);
    frame.insert(frame.end(), toc.bytes().begin(), toc.bytes().end());
    frame.insert(frame.end(), payload_base, std::byte{0});
    for (const BitWriter& substream : substreams) {
        frame.insert(frame.end(), substream.bytes().begin(), substream.bytes().end());
    }
    if (sink) {
        for (std::size_t index = 0; index < substreams.size(); ++index) {
            for (SyntaxRecord record : substreams[index].kept()) {
                record.substream = static_cast<int>(index);
                sink(record);
            }
        }
    }
    return frame;
}

// --- One presentation of one substream -----------------------------------------

TocLayout single_layout(const FrameFields& f) {
    // One version 1 presentation of one substream group, whose one
    // channel-coded substream is complete main (Part 1 Table 91) with no
    // language, and the presentation substream.
    TocLayout layout;
    layout.sequence_counter = f.sequence_counter;
    layout.wait_frames = f.wait_frames;
    layout.br_code = f.br_code;
    layout.fs_index = f.fs_index;
    layout.frame_rate_index = f.frame_rate_index;
    layout.iframe_global = f.iframe;
    TocPresentation presentation;
    presentation.groups = {0};
    presentation.md_compat = f.md_compat;
    presentation.presentation_id = f.presentation_id;
    presentation.pres_ndot = f.iframe;
    presentation.presentation_substream = kPresentationSubstream;
    layout.presentations.push_back(presentation);
    TocGroup group;
    group.substreams.push_back(TocSubstream{.ch_mode = f.ch_mode,
                                            .add_ch_base = f.add_ch_base,
                                            .iframe = f.iframe,
                                            .substream_index = kAudioSubstream,
                                            .b_4_back_channels_present = f.b_4_back_channels_present,
                                            .b_centre_present = f.b_centre_present,
                                            .top_channels_present = f.top_channels_present});
    group.content_classifier = 0;
    layout.groups.push_back(group);
    return layout;
}

PresentationSubstreamFields presentation_fields(const FrameFields& f) {
    const StreamMetadata* m = f.metadata;
    PresentationSubstreamFields out;
    out.iframe = f.iframe;
    out.dialnorm_bits = f.dialnorm_bits;
    out.loudness = m != nullptr && m->loudness ? &*m->loudness : nullptr;
    out.drc = m != nullptr && m->drc ? &*m->drc : nullptr;
    out.drc_gains = f.drc_gains;
    out.channels = presentation_channels(f);
    out.downmix = m != nullptr && m->downmix ? &*m->downmix : nullptr;
    return out;
}

AudioSubstreamFields audio_fields(const FrameFields& f) {
    AudioSubstreamFields out;
    out.ch_mode = f.ch_mode;
    out.iframe = f.iframe;
    out.de_config = f.metadata != nullptr && f.metadata->de ? &*f.metadata->de : nullptr;
    out.de = f.de;
    out.de_previous = f.de_previous;
    return out;
}

std::size_t frame_overhead_bits(const FrameFields& fields, std::size_t audio_substream_bytes) {
    const std::size_t presentation =
        write_presentation_substream(presentation_fields(fields)).byte_size();
    const std::array<std::size_t, 2> sizes{presentation, audio_substream_bytes};
    return 8 * toc_bytes(single_layout(fields), 0, sizes) + 8 * presentation +
           audio_substream_overhead_bits(audio_fields(fields), audio_substream_bytes);
}

std::optional<std::vector<std::byte>> write_frame(const FrameFields& fields, const BitWriter& audio,
                                                  std::size_t frame_bytes, SyntaxSink sink) {
    const TocLayout layout = single_layout(fields);
    const AudioSubstreamFields audio_f = audio_fields(fields);
    std::array<BitWriter, 2> substreams{write_presentation_substream(presentation_fields(fields)),
                                        BitWriter{}};
    std::size_t payload_base = 0;
    std::size_t audio_bytes = 0;
    if (frame_bytes > 0) {
        const std::array<std::size_t, 2> sizes{substreams[kPresentationSubstream].byte_size(), 0};
        const std::optional<FrameFit> fit =
            fit_frame(layout, sizes, kAudioSubstream, frame_bytes, [&audio_f](std::size_t bytes) {
                return audio_substream_size_possible(audio_f, bytes);
            });
        if (!fit) {
            return std::nullopt;
        }
        payload_base = fit->payload_base;
        audio_bytes = fit->slack_bytes;
    }
    std::optional<BitWriter> written = write_audio_substream(audio_f, audio, audio_bytes);
    if (!written) {
        return std::nullopt;
    }
    substreams[kAudioSubstream] = std::move(*written);
    // Assembled without the records first, so that a frame that turns out
    // not to fit sends none.
    std::optional<std::vector<std::byte>> frame = assemble(layout, substreams, payload_base, {});
    if (!frame || (frame_bytes > 0 && frame->size() != frame_bytes)) {
        return std::nullopt;
    }
    if (sink) {
        for (std::size_t index = 0; index < substreams.size(); ++index) {
            for (SyntaxRecord record : substreams[index].kept()) {
                record.substream = static_cast<int>(index);
                sink(record);
            }
        }
    }
    return frame;
}

}  // namespace ac4::detail

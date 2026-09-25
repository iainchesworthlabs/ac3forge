#include "frame/frame_writer.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "frame/toc_writer.hpp"

namespace ac4::detail {
namespace {

// Substream indices in substream_index_table(): the presentation substream
// first, then the audio substream.
constexpr int kPresentationSubstream = 0;
constexpr int kAudioSubstream = 1;

// The encoder's table of contents (toc_writer.hpp): one version 1 presentation
// of one substream group, whose one channel-coded substream is complete main
// (Part 1 Table 91) with no language, and the presentation substream.
[[nodiscard]] TocLayout toc_layout(const FrameFields& f) {
    TocLayout layout;
    layout.sequence_counter = f.sequence_counter;
    layout.wait_frames = f.wait_frames;
    layout.br_code = f.br_code;
    layout.fs_index = f.fs_index;
    layout.frame_rate_index = f.frame_rate_index;
    layout.iframe_global = f.iframe;
    TocPresentation presentation;
    presentation.groups = {0};
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

// Part 2 clause 6.2.1.1, ac4_toc(), at bitstream_version 2. `payload_base`
// bytes of padding separate it from the first substream.
void write_toc(BitWriter& w, const FrameFields& f, std::size_t payload_base, std::span<const std::size_t> sizes) {
    write_toc(w, toc_layout(f), payload_base, sizes);
}

[[nodiscard]] std::size_t toc_bytes(const FrameFields& f, std::size_t payload_base,
                                    std::span<const std::size_t> sizes) {
    return toc_bytes(toc_layout(f), payload_base, sizes);
}

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

// Part 2 clause 6.2.2.3, ac4_presentation_substream(), without b_alternative:
// dialogue normalisation, further_loudness_info() where it is configured, a
// drc_frame() (with DRC's configuration in I-frames where it is configured),
// no associated audio, and custom_dmx_data() and loud_corr() (6.2.9.2,
// 6.2.9.1), which read nothing for a mono or stereo presentation, and for the
// others the stereo coefficients and their corrections in I-frames where they
// are configured.
void write_presentation_substream(BitWriter& w, const FrameFields& f) {
    const StreamMetadata* m = f.metadata;
    w.write(1, 0, "b_additional_data");
    w.write(7, static_cast<std::uint64_t>(f.dialnorm_bits), "dialnorm_bits");
    const LoudnessCodes* loudness = m != nullptr && m->loudness ? &*m->loudness : nullptr;
    w.write(1, loudness != nullptr ? 1U : 0U, "b_further_loudness_info");
    if (loudness != nullptr) {
        write_further_loudness_info(w, *loudness, f.iframe);
    }
    BitWriter drc = BitWriter::buffered();
    write_drc_frame(drc, m != nullptr && m->drc ? &*m->drc : nullptr, f.iframe, f.drc_gains);
    write_sized(w, drc, 5, "drc_metadata_size_value", "drc_metadata_size");
    write_presentation_mix(w, PresentationMixCodes{});  // one group, no associated audio
    write_downmix(w, presentation_channels(f), m != nullptr && m->downmix ? &*m->downmix : nullptr,
                  f.iframe);
    w.align();
}

// Part 2 clause 6.2.7.1, metadata(), for a channel-coded substream at sus_ver
// 1 without b_alternative: basic_metadata() (6.2.7.2) and extended_metadata()
// (6.2.7.4) with nothing optional, and dialog_enhancement() (6.2.7.5), with
// data where dialogue enhancement is configured.
void write_metadata(BitWriter& w, const FrameFields& f) {
    w.write(1, 0, "b_more_basic_metadata");
    write_extended_metadata(w, f.ch_mode, nullptr);
    BitWriter tools = BitWriter::buffered();
    const DeConfigCodes* de = f.metadata != nullptr && f.metadata->de ? &*f.metadata->de : nullptr;
    write_dialog_enhancement(tools, de, f.de, f.de_previous, f.iframe);
    write_sized(w, tools, 7, "tools_metadata_size_value", "tools_metadata_size");
    w.write(1, 0, "b_emdf_payloads_substream");
    w.align();
}

[[nodiscard]] std::size_t metadata_bytes(const FrameFields& f) {
    BitWriter w;
    write_metadata(w, f);
    return w.byte_size();
}

[[nodiscard]] std::size_t presentation_bytes(const FrameFields& f) {
    BitWriter w;
    write_presentation_substream(w, f);
    return w.byte_size();
}

// Part 2 clause 6.2.2.2: audio_size_value's 15 bits and b_more_bits, and
// variable_bits(7) for the size above 15 bits.
[[nodiscard]] std::size_t audio_header_bytes(std::size_t audio_size) {
    const std::size_t bits = 16 + (audio_size >= 0x8000 ? variable_bits_width(7, audio_size >> 15U) : 0U);
    return (bits + 7) / 8;
}

// The audio substream's size in bytes, and the audio_size in it, that make
// the frame `frame_bytes` long with `payload_base` bytes of padding before the
// substreams; nothing when no audio substream size does.
struct AudioLayout {
    std::size_t substream_bytes = 0;
    std::size_t audio_size = 0;
};

[[nodiscard]] std::optional<AudioLayout> fit_audio(const FrameFields& f, std::size_t frame_bytes,
                                                   std::size_t payload_base, std::size_t presentation,
                                                   std::size_t metadata) {
    // The table of contents grows with the sizes it lists, so settle it by
    // trying the sizes it allows: a few steps of the fixed point settle it,
    // and a size the table of contents' growth jumps over is left for a
    // larger payload_base to take.
    std::size_t guess = frame_bytes > presentation + payload_base + 8
                            ? frame_bytes - presentation - payload_base - 8
                            : 0;
    for (int step = 0; step < 4; ++step) {
        const std::array<std::size_t, 2> sizes{presentation, guess};
        const std::size_t toc = toc_bytes(f, payload_base, sizes);
        if (toc + payload_base + presentation >= frame_bytes) {
            return std::nullopt;
        }
        const std::size_t substream = frame_bytes - toc - payload_base - presentation;
        if (substream == guess) {
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
            return AudioLayout{substream, audio_size};
        }
        guess = substream;
    }
    return std::nullopt;
}

void write_audio_substream(BitWriter& w, const FrameFields& f, const BitWriter& audio,
                           std::size_t audio_size) {
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
    write_metadata(w, f);
}

}  // namespace

std::size_t frame_overhead_bits(const FrameFields& fields, std::size_t audio_substream_bytes) {
    const std::size_t presentation = presentation_bytes(fields);
    const std::array<std::size_t, 2> sizes{presentation, audio_substream_bytes};
    // One byte of alignment ahead of metadata(), at most, on top.
    return 8 * (toc_bytes(fields, 0, sizes) + presentation +
                audio_header_bytes(audio_substream_bytes) + metadata_bytes(fields) + 1);
}

std::optional<std::vector<std::byte>> write_frame(const FrameFields& fields, const BitWriter& audio,
                                                  std::size_t frame_bytes, SyntaxSink sink) {
    const std::size_t presentation = presentation_bytes(fields);
    const std::size_t metadata = metadata_bytes(fields);
    const std::size_t audio_bytes_needed = (audio.bit_position() + 7) / 8;

    AudioLayout layout;
    std::size_t payload_base = 0;
    if (frame_bytes == 0) {
        layout.audio_size = audio_bytes_needed;
        layout.substream_bytes = audio_header_bytes(layout.audio_size) + layout.audio_size + metadata;
    } else {
        std::optional<AudioLayout> fitted;
        for (payload_base = 0; payload_base < 8 && !fitted; ++payload_base) {
            fitted = fit_audio(fields, frame_bytes, payload_base, presentation, metadata);
            if (fitted) {
                break;
            }
        }
        if (!fitted || fitted->audio_size < audio_bytes_needed) {
            return std::nullopt;
        }
        layout = *fitted;
    }

    // Buffered, so that a frame that turns out not to fit sends no records.
    BitWriter presentation_writer = BitWriter::buffered();
    write_presentation_substream(presentation_writer, fields);
    BitWriter audio_writer = BitWriter::buffered();
    write_audio_substream(audio_writer, fields, audio, layout.audio_size);

    const std::array<std::size_t, 2> sizes{presentation_writer.byte_size(), audio_writer.byte_size()};
    BitWriter toc;
    write_toc(toc, fields, payload_base, sizes);

    std::vector<std::byte> frame;
    frame.reserve(toc.byte_size() + payload_base + sizes[0] + sizes[1]);
    frame.insert(frame.end(), toc.bytes().begin(), toc.bytes().end());
    frame.insert(frame.end(), payload_base, std::byte{0});
    frame.insert(frame.end(), presentation_writer.bytes().begin(), presentation_writer.bytes().end());
    frame.insert(frame.end(), audio_writer.bytes().begin(), audio_writer.bytes().end());
    if (frame_bytes > 0 && frame.size() != frame_bytes) {
        return std::nullopt;
    }
    if (sink) {
        for (const auto& [substream, writer] :
             {std::pair{kPresentationSubstream, &presentation_writer}, std::pair{kAudioSubstream, &audio_writer}}) {
            for (SyntaxRecord record : writer->kept()) {
                record.substream = substream;
                sink(record);
            }
        }
    }
    return frame;
}

}  // namespace ac4::detail

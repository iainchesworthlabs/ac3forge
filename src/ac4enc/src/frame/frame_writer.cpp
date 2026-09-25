#include "frame/frame_writer.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ac4::detail {
namespace {

// Substream indices in substream_index_table(): the presentation substream
// first, then the audio substream.
constexpr int kPresentationSubstream = 0;
constexpr int kAudioSubstream = 1;

// Part 1 Table 8, emdf_info(), with Table 80's emdf_protection() (headed
// emdf_reserved()) carrying no reserved bytes.
void write_emdf_info(BitWriter& w) {
    w.write(2, 0, "emdf_version");
    w.write(3, 0, "key_id");
    w.write(1, 0, "b_emdf_payloads_substream_info");
    w.write(2, 0, "n_skip_bytes_length_primary");
    w.write(2, 0, "n_skip_bytes_length_secondary");
}

// Part 2 clause 6.2.1.3, ac4_presentation_v1_info(), for one presentation of
// one substream group with its presentation substream.
void write_presentation_v1_info(BitWriter& w, const FrameFields& f) {
    w.write(1, 1, "b_single_substream_group");
    // Part 1 Table 6, presentation_version(): a one per count, then a zero.
    w.write(1, 1, "b_tmp");
    w.write(1, 0, "b_tmp");
    w.write(3, 0, "md_compat");
    w.write(1, 0, "b_presentation_id");
    // frame_rate_multiply_info() (Part 1 Table 7): no multiplier, so
    // frame_rate_factor 1, where the index has one to send; and
    // frame_rate_fractions_info() (6.2.1.4): a fraction of 1 at 47.95 fps and
    // up. Neither sends anything at index 13.
    const int index = f.frame_rate_index;
    if (index <= 4 || (index >= 7 && index <= 9)) {
        w.write(1, 0, "b_multiplier");
    }
    if (index >= 5 && index <= 12) {
        w.write(1, 0, "b_frame_rate_fraction");
    }
    write_emdf_info(w);
    w.write(1, 0, "b_presentation_filter");
    // ac4_sgi_specifier(), 6.2.1.7: the one substream group.
    w.write(3, 0, "group_index");
    w.write(1, 0, "b_pre_virtualized");
    w.write(1, 0, "b_add_emdf_substreams");
    // ac4_presentation_substream_info(), 6.2.1.12.
    w.write(1, 0, "b_alternative");
    w.write(1, f.iframe ? 1U : 0U, "b_pres_ndot");
    w.write(2, kPresentationSubstream, "substream_index");
}

// Part 2 clause 6.2.1.6, ac4_substream_group_info(), with its one
// ac4_substream_info_chan() (6.2.1.8) and content_type() (Part 1 Table 10).
void write_substream_group_info(BitWriter& w, const FrameFields& f) {
    w.write(1, 1, "b_substreams_present");
    w.write(1, 0, "b_hsf_ext");
    w.write(1, 1, "b_single_substream");
    w.write(1, 1, "b_channel_coded");
    // Table 56: 0b0 mono, 0b10 stereo, 0b1100 to 0b1110 3.0, 5.0 and 5.1,
    // 0b1111000 to 0b1111101 the 7.X modes, 0b11111100 and 0b11111101 7.0.4
    // and 7.1.4, which name the channels their source has.
    if (f.ch_mode == 0) {
        w.write(1, 0b0, "channel_mode");
    } else if (f.ch_mode == 1) {
        w.write(2, 0b10, "channel_mode");
    } else if (f.ch_mode <= 4) {
        w.write(4, 0b1100U + static_cast<unsigned>(f.ch_mode - 2), "channel_mode");
    } else if (f.ch_mode <= 10) {
        w.write(7, 0b1111000U + static_cast<unsigned>(f.ch_mode - 5), "channel_mode");
    } else {
        w.write(8, 0b11111100U + static_cast<unsigned>(f.ch_mode - 11), "channel_mode");
        w.write(1, f.b_4_back_channels_present ? 1U : 0U, "b_4_back_channels_present");
        w.write(1, f.b_centre_present ? 1U : 0U, "b_centre_present");
        w.write(2, static_cast<std::uint64_t>(f.top_channels_present), "top_channels_present");
    }
    if (f.fs_index == 1) {
        w.write(1, 0, "b_sf_multiplier");
    }
    w.write(1, 0, "b_bitrate_info");
    if (f.ch_mode >= 7 && f.ch_mode <= 10) {
        w.write(1, f.add_ch_base ? 1U : 0U, "add_ch_base");
    }
    // frame_rate_factor is 1.
    w.write(1, f.iframe ? 1U : 0U, "b_audio_ndot");
    w.write(2, kAudioSubstream, "substream_index");
    w.write(1, 1, "b_content_type");
    w.write(3, 0, "content_classifier");  // Table 91: complete main
    w.write(1, 0, "b_language_indicator");
}

// Part 1 Table 14, substream_index_table(): two substreams, so b_size_present
// is implied; each size's low 10 bits, and variable_bits(2) for the rest.
void write_substream_index_table(BitWriter& w, std::span<const std::size_t> sizes) {
    w.write(2, sizes.size(), "n_substreams");
    for (const std::size_t size : sizes) {
        const bool more = size >= 1024;
        w.write(1, more ? 1U : 0U, "b_more_bits");
        w.write(10, size & 0x3FFU, "substream_size");
        if (more) {
            w.write_variable_bits(2, size >> 10U, "substream_size");
        }
    }
}

// Part 2 clause 6.2.1.1, ac4_toc(), at bitstream_version 2. `payload_base`
// bytes of padding separate it from the first substream.
void write_toc(BitWriter& w, const FrameFields& f, std::size_t payload_base, std::span<const std::size_t> sizes) {
    w.write(2, 2, "bitstream_version");
    w.write(10, static_cast<std::uint64_t>(f.sequence_counter), "sequence_counter");
    w.write(1, 1, "b_wait_frames");
    w.write(3, static_cast<std::uint64_t>(f.wait_frames), "wait_frames");
    if (f.wait_frames > 0) {
        w.write(2, static_cast<std::uint64_t>(f.br_code), "br_code");
    }
    w.write(1, static_cast<std::uint64_t>(f.fs_index), "fs_index");
    w.write(4, static_cast<std::uint64_t>(f.frame_rate_index), "frame_rate_index");
    w.write(1, f.iframe ? 1U : 0U, "b_iframe_global");
    w.write(1, 1, "b_single_presentation");
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
    write_presentation_v1_info(w, f);
    write_substream_group_info(w, f);  // total_n_substream_groups is 1
    write_substream_index_table(w, sizes);
    w.align();
}

[[nodiscard]] std::size_t toc_bytes(const FrameFields& f, std::size_t payload_base,
                                    std::span<const std::size_t> sizes) {
    BitWriter w;
    write_toc(w, f, payload_base, sizes);
    return w.byte_size();
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
    w.write(1, 0, "b_associated");
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
    w.write(1, 0, "b_dialog");
    w.write(1, 0, "b_channels_classifier");
    w.write(1, 0, "b_event_probability");
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

#include "frame/frame_writer.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>
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
    // frame_rate_multiply_info() (Part 1 Table 7) and frame_rate_fractions_info()
    // (6.2.1.4) transmit nothing at frame_rate_index 13.
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
    // Table 56: 0b0 mono, 0b10 stereo.
    if (f.stereo) {
        w.write(2, 0b10, "channel_mode");
    } else {
        w.write(1, 0b0, "channel_mode");
    }
    if (f.fs_index == 1) {
        w.write(1, 0, "b_sf_multiplier");
    }
    w.write(1, 0, "b_bitrate_info");
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
    w.write(3, 0, "wait_frames");  // Part 1 Table 81: a constant bit rate; no br_code
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

// Part 2 clause 6.2.2.3, ac4_presentation_substream(), without b_alternative:
// dialogue normalisation, no further loudness information, a drc_frame()
// without DRC, no associated audio. custom_dmx_data() and loud_corr() read
// nothing for a mono or stereo presentation (6.2.9.1, 6.2.9.2).
void write_presentation_substream(BitWriter& w, const FrameFields& f) {
    w.write(1, 0, "b_additional_data");
    w.write(7, static_cast<std::uint64_t>(f.dialnorm_bits), "dialnorm_bits");
    w.write(1, 0, "b_further_loudness_info");
    // drc_metadata_size counts the bits of the drc_frame() after it: here the
    // one bit of b_drc_present.
    w.write(5, 1, "drc_metadata_size_value");
    w.write(1, 0, "b_more_bits");
    w.write(1, 0, "b_drc_present");  // Part 1 Table 70, drc_frame()
    w.write(1, 0, "b_associated");
    w.align();
}

// Part 2 clause 6.2.7.1, metadata(), for a channel-coded substream at sus_ver
// 1 without b_alternative: basic_metadata() (6.2.7.2) and extended_metadata()
// (6.2.7.4) with nothing optional, and dialog_enhancement() (6.2.7.5) without
// data.
void write_metadata(BitWriter& w) {
    w.write(1, 0, "b_more_basic_metadata");
    w.write(1, 0, "b_dialog");
    w.write(1, 0, "b_channels_classifier");
    w.write(1, 0, "b_event_probability");
    // tools_metadata_size counts the bits of dialog_enhancement(): one.
    w.write(7, 1, "tools_metadata_size_value");
    w.write(1, 0, "b_more_bits");
    w.write(1, 0, "b_de_data_present");
    w.write(1, 0, "b_emdf_payloads_substream");
    w.align();
}

[[nodiscard]] std::size_t metadata_bytes() {
    BitWriter w;
    write_metadata(w);
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

void write_audio_substream(BitWriter& w, const BitWriter& audio, std::size_t audio_size) {
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
    write_metadata(w);
}

}  // namespace

std::size_t frame_overhead_bits(const FrameFields& fields, std::size_t audio_substream_bytes) {
    const std::size_t presentation = presentation_bytes(fields);
    const std::array<std::size_t, 2> sizes{presentation, audio_substream_bytes};
    // One byte of alignment ahead of metadata(), at most, on top.
    return 8 * (toc_bytes(fields, 0, sizes) + presentation + audio_header_bytes(audio_substream_bytes) +
                metadata_bytes() + 1);
}

std::optional<std::vector<std::byte>> write_frame(const FrameFields& fields, const BitWriter& audio,
                                                  std::size_t frame_bytes, SyntaxSink sink) {
    const std::size_t presentation = presentation_bytes(fields);
    const std::size_t metadata = metadata_bytes();
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
    write_audio_substream(audio_writer, audio, layout.audio_size);

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

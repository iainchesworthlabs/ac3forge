#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "ac4/syntax.hpp"
#include "bit_writer.hpp"
#include "frame/metadata.hpp"
#include "frame/toc_writer.hpp"

// The substreams of a raw_ac4_frame() (ETSI TS 103 190-2 V1.3.1 clause 6.2.2,
// Part 1 clause 4.2.4) and the frame they make with their table of contents
// (toc_writer.hpp): audio substreams, presentation substreams and EMDF payload
// substreams, each written to the size the frame gives it, and the fitting
// that makes a frame of several substreams exactly as long as a constant rate
// asks. The encoder writes its frames with these; write_frame() is the
// frame of one presentation of one substream, which the decoder's tests
// build frames of the channel modes of Part 1 Table 88, and of Part 2's
// 7.X.4 modes, with.

namespace ac4::detail {

// --- Audio substreams ----------------------------------------------------------

// What an audio substream's metadata() carries (Part 2 clause 6.2.7.1, sus_ver
// 1, without b_alternative's object data): its dialogue enhancement, with this
// frame's parameters and the last frame's, which a frame that is not an
// I-frame codes against; a dialogue substream's mixing values; and EMDF
// payloads.
struct AudioSubstreamFields {
    int ch_mode = 1;     // Part 1 Table 88
    bool iframe = true;  // b_audio_ndot
    const DeConfigCodes* de_config = nullptr;
    const DeFrameParameters* de = nullptr;
    const DeFrameParameters* de_previous = nullptr;
    const DialogueMixCodes* dialogue = nullptr;  // b_dialog
    std::span<const EmdfPayloadCodes> emdf;      // b_emdf_payloads_substream
};

// The bits an audio substream of `substream_bytes` spends on everything but
// its channel element and fill: audio_size's field, metadata() and the
// alignment before it, an upper bound over the alignment.
[[nodiscard]] std::size_t audio_substream_overhead_bits(const AudioSubstreamFields& fields,
                                                        std::size_t substream_bytes);

// The bytes of an audio substream whose audio_data() is `audio_bytes` long,
// with nothing to fill.
[[nodiscard]] std::size_t audio_substream_bytes(const AudioSubstreamFields& fields,
                                                std::size_t audio_bytes);

// Whether an audio substream of exactly `substream_bytes` can be written:
// audio_size's field takes more bytes above 32 767, and a size its growth
// jumps over has no audio_size that makes it.
[[nodiscard]] bool audio_substream_size_possible(const AudioSubstreamFields& fields,
                                                 std::size_t substream_bytes);

// ac4_substream() (clause 6.2.2.2) around `audio`, a buffered writer holding
// audio_data_chan(): with `substream_bytes` above zero exactly that long, the
// audio taking fill_bits, and nothing when the content alone is longer or the
// size is not possible; with zero, as long as its content. A buffered writer,
// its records kept.
[[nodiscard]] std::optional<BitWriter> write_audio_substream(const AudioSubstreamFields& fields,
                                                             const BitWriter& audio,
                                                             std::size_t substream_bytes);

// --- Presentation and EMDF payload substreams ------------------------------------

// What a presentation substream carries (clause 6.2.2.3), each where it is
// set: an alternative presentation's name and target, the additional data's
// immersive_audio_indicator, dialogue normalisation, further loudness values,
// DRC (its configuration in I-frames, and the frame's gains where a mode sends
// them), the substream groups' gains and the associated audio's values, and
// custom_dmx_data() and loud_corr() for the presentation's channels.
struct PresentationSubstreamFields {
    bool iframe = true;  // b_pres_ndot
    const AlternativeCodes* alternative = nullptr;
    // b_additional_data with one byte of it, as DEE sends it for a 5.1.4
    // presentation: immersive_audio_indicator set, no advanced dialogue
    // enhancement data, and six bits of add_data.
    bool immersive_audio_indicator = false;
    int dialnorm_bits = 124;  // Part 1 clause 4.3.12.2.1: -dialnorm_bits / 4 dBFS
    const LoudnessCodes* loudness = nullptr;
    const DrcCodes* drc = nullptr;
    std::span<const DrcModeGains> drc_gains;
    PresentationMixCodes mix{};
    PresentationChannels channels{};  // pres_ch_mode and the rest (clause 6.3.3.1.27)
    const DownmixCodes* downmix = nullptr;
};

// ac4_presentation_substream(): a buffered writer, its records kept.
[[nodiscard]] BitWriter write_presentation_substream(const PresentationSubstreamFields& fields);

// emdf_payloads_substream() (Part 1 clause 4.2.4.4): a buffered writer.
[[nodiscard]] BitWriter write_emdf_payloads_substream(std::span<const EmdfPayloadCodes> payloads);

// --- The frame ----------------------------------------------------------------

// The size of substream `slack`, and payload_base, that make a frame of
// `layout` over substreams of `sizes` (in index order; the slack's own entry
// ignored) exactly `frame_bytes` long: the table of contents grows with the
// sizes it lists, so its size is settled by trying the sizes it allows, and
// a size its growth jumps over, or one `possible` refuses, is left for a
// larger payload_base to take, up to 7 bytes of it. Nothing when none does.
struct FrameFit {
    std::size_t slack_bytes = 0;
    std::size_t payload_base = 0;
};

template <typename Possible>
[[nodiscard]] std::optional<FrameFit> fit_frame(const TocLayout& layout,
                                                std::span<const std::size_t> sizes,
                                                std::size_t slack, std::size_t frame_bytes,
                                                const Possible& possible);

// The frame's substreams in index order, and their records sent to `sink`
// in that order, each carrying its substream's index: the order in which a
// reader reads them.
[[nodiscard]] std::optional<std::vector<std::byte>> assemble(const TocLayout& layout,
                                                             std::span<const BitWriter> substreams,
                                                             std::size_t payload_base,
                                                             SyntaxSink sink);

// --- One presentation of one substream -----------------------------------------

// A frame of one version 1 presentation over one substream group of one
// channel-coded substream: the presentation substream (index 0) and the audio
// substream (index 1).
struct FrameFields {
    int sequence_counter = 0;   // 0 to 1020
    // Part 1 Table 81: 0 a constant bit rate, 1 to 6 an average one with the
    // decoder waiting 0 to 5 frames (twice that at indices 10 to 12), 7 a
    // variable one; above 0, Part 2 Table 52's br_code follows.
    int wait_frames = 0;
    int br_code = 0;
    bool iframe = true;         // b_iframe_global, b_pres_ndot and b_audio_ndot
    int fs_index = 1;           // Part 1 Table 82: 1 = 48 kHz, 0 = 44.1 kHz
    int frame_rate_index = 13;
    // Part 1 Table 88: 0 mono, 1 stereo, 2 3.0, 3 and 4 5.X, 5 to 10 7.X; and
    // Part 2 Table 56's 11 and 12, 7.0.4 and 7.1.4.
    int ch_mode = 1;
    bool add_ch_base = false;   // for 7.X 5/2/0 and 3/2/2 (Part 2 clause 6.3.2.7)
    // The 7.X.4 modes' channels the source has (Part 2 clauses 6.3.2.7.3 to
    // 6.3.2.7.5, Tables 57 to 59).
    bool b_4_back_channels_present = true;
    bool b_centre_present = true;
    int top_channels_present = 3;
    int dialnorm_bits = 124;    // Part 1 clause 4.3.12.2.1: -dialnorm_bits / 4 dBFS
    // The presentation's md_compat and presentation_id (Part 2 Table 55,
    // clause 6.3.2.2.4a); unset, none.
    int md_compat = 0;
    std::optional<int> presentation_id;
    // The stream's metadata beside dialnorm (frame/metadata.hpp), and with
    // dialogue enhancement this frame's parameters and the last frame's, which
    // a frame that is not an I-frame codes against.
    const StreamMetadata* metadata = nullptr;
    const DeFrameParameters* de = nullptr;
    const DeFrameParameters* de_previous = nullptr;
    // DRC's gains for this frame, per mode in drc_config()'s order where the
    // mode sends them.
    std::span<const DrcModeGains> drc_gains;
};

// The table of contents of such a frame.
[[nodiscard]] TocLayout single_layout(const FrameFields& fields);

// Its presentation substream's and audio substream's fields.
[[nodiscard]] PresentationSubstreamFields presentation_fields(const FrameFields& fields);
[[nodiscard]] AudioSubstreamFields audio_fields(const FrameFields& fields);

// The bits the frame spends on everything but the channel element and its
// fill, with the audio substream at `audio_substream_bytes`: the table of
// contents, the presentation substream, the audio substream's header,
// metadata() and the alignment after both. An upper bound over the alignment.
[[nodiscard]] std::size_t frame_overhead_bits(const FrameFields& fields, std::size_t audio_substream_bytes);

// Writes the frame. `audio` is a buffered writer holding audio_data_chan(): the
// channel element and nothing after it. With `frame_bytes` above zero, the
// audio substream takes fill_bits so that the frame is exactly that long, and
// nothing is returned when the content alone is longer; with zero, the frame
// is as long as its content. The substreams' records go to `sink`, the audio
// data's offset by where it lands.
[[nodiscard]] std::optional<std::vector<std::byte>> write_frame(const FrameFields& fields, const BitWriter& audio,
                                                                std::size_t frame_bytes, SyntaxSink sink);

// --- fit_frame -----------------------------------------------------------------

template <typename Possible>
std::optional<FrameFit> fit_frame(const TocLayout& layout, std::span<const std::size_t> sizes,
                                  std::size_t slack, std::size_t frame_bytes,
                                  const Possible& possible) {
    std::vector<std::size_t> trial(sizes.begin(), sizes.end());
    std::size_t others = 0;
    for (std::size_t i = 0; i < trial.size(); ++i) {
        if (i != slack) {
            others += trial[i];
        }
    }
    for (std::size_t payload_base = 0; payload_base < 8; ++payload_base) {
        // A few steps of the fixed point settle the table of contents.
        std::size_t guess =
            frame_bytes > others + payload_base + 8 ? frame_bytes - others - payload_base - 8 : 0;
        for (int step = 0; step < 4; ++step) {
            trial[slack] = guess;
            const std::size_t toc = toc_bytes(layout, payload_base, trial);
            if (toc + payload_base + others >= frame_bytes) {
                break;
            }
            const std::size_t left = frame_bytes - toc - payload_base - others;
            if (left == guess) {
                if (possible(left)) {
                    return FrameFit{left, payload_base};
                }
                break;
            }
            guess = left;
        }
    }
    return std::nullopt;
}

}  // namespace ac4::detail

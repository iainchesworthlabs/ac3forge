#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "iamf/iamf.hpp"

// OBU-level plumbing for iamf::mux(): leb128, the generic OBU header, and byte builders for the
// four Descriptor OBUs (IA Sequence Header, Codec Config, Audio Element, Mix Presentation) this
// writer emits, plus the Audio Frame OBU that carries each frame's PCM. Every syntax element
// below is transcribed directly from the published IAMF v1.1.0 specification
// (https://aomediacodec.github.io/iamf/v1.1.0.html, final), section/table cited at each builder,
// per CONTRIBUTING.md's clean-room rule - see iamf/iamf.hpp's own header comment for the
// module-level summary of what this writer does and does not cover.
//
// Internal to src/iamf/src/ - this header is included by more than one .cpp in this target, the
// same reason src/mp4/src/isobmff_detail.hpp gives for staying out of the public include/ tree.

namespace iamf::detail {

using Bytes = std::vector<std::byte>;

inline void put_u8(Bytes& out, std::uint8_t v) { out.push_back(static_cast<std::byte>(v)); }

inline void put_u16(Bytes& out, std::uint16_t v) {
    put_u8(out, static_cast<std::uint8_t>(v >> 8));
    put_u8(out, static_cast<std::uint8_t>(v & 0xFF));
}

inline void put_u32(Bytes& out, std::uint32_t v) {
    put_u8(out, static_cast<std::uint8_t>(v >> 24));
    put_u8(out, static_cast<std::uint8_t>((v >> 16) & 0xFF));
    put_u8(out, static_cast<std::uint8_t>((v >> 8) & 0xFF));
    put_u8(out, static_cast<std::uint8_t>(v & 0xFF));
}

inline void put_s16(Bytes& out, std::int16_t v) { put_u16(out, static_cast<std::uint16_t>(v)); }

inline void put_fourcc(Bytes& out, std::string_view fourcc) {
    assert(fourcc.size() == 4);
    for (const char c : fourcc) {
        put_u8(out, static_cast<std::uint8_t>(c));
    }
}

inline void put_bytes(Bytes& out, std::span<const std::byte> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

// AV1 Bitstream & Decoding Process Specification Section 5.3.3 "leb128()": IAMF's own leb128()
// descriptor, used throughout IAMF §3 for every *_size/*_id/*_count field, is never re-defined in
// the IAMF spec text itself - IAMF v1.1.0's Normative References list cites [AV1-Spec] for it,
// and IAMF's OBU framing (§3.2) is explicitly modeled on AV1's own OBU syntax. Up to 8 bytes, 7
// payload bits per byte, MSB of each byte a continuation flag, least-significant group first.
inline void put_leb128(Bytes& out, std::uint64_t value) {
    do {
        auto byte = static_cast<std::uint8_t>(value & 0x7FU);
        value >>= 7;
        if (value != 0) {
            byte |= 0x80U;
        }
        put_u8(out, byte);
    } while (value != 0);
}

// IAMF §3.2 Table (obu_type): the OBU types this writer ever emits. Audio_Frame_ID0..ID17
// (6..23) are handled separately by audio_frame_id_obu_type() below, since they are not a fixed
// enumerator but "6 + substream_id".
enum class ObuType : std::uint8_t {
    kCodecConfig = 0,
    kAudioElement = 1,
    kMixPresentation = 2,
    kSequenceHeader = 31,
};

// IAMF §3.2/§3.9: OBU_IA_Audio_Frame_ID0..ID17 give the first 18 Audio Substreams a compact
// per-ID obu_type (6 + substream_id) that needs no separate explicit_audio_substream_id field.
// This writer's Audio Element never has more than 7 substreams (§3.6.2's channel-based 7.1.4
// layer: 5 coupled + 2 non-coupled), so every substream id it ever uses fits this range.
[[nodiscard]] inline std::uint8_t audio_frame_id_obu_type(int substream_id) {
    assert(substream_id >= 0 && substream_id <= 17);
    return static_cast<std::uint8_t>(6 + substream_id);
}

// IAMF §3.2 OBUHeader(): obu_type(5)/obu_redundant_copy(1)/obu_trimming_status_flag(1)/
// obu_extension_flag(1) packed MSB-first into one byte (confirmed against §3.4's own worked
// example: an IA Sequence Header OBU, obu_type 31, encodes as header byte 0xF8 non-redundant or
// 0xFC redundant - 31<<3 = 0xF8, plus bit2 for obu_redundant_copy), then leb128() obu_size. This
// writer never trims mid-file and defines no extension header, so both flag bits are always 0 and
// neither optional field is ever written - `obu_size` is exactly `payload.size()`.
inline void put_obu(Bytes& out, std::uint8_t obu_type, std::span<const std::byte> payload) {
    put_u8(out, static_cast<std::uint8_t>(obu_type << 3));
    put_leb128(out, payload.size());
    put_bytes(out, payload);
}

inline void put_obu(Bytes& out, ObuType obu_type, std::span<const std::byte> payload) {
    put_obu(out, static_cast<std::uint8_t>(obu_type), payload);
}

// --- IA Sequence Header OBU (IAMF §3.4) -------------------------------------------------------

// primary_profile/additional_profile = 0 (Simple Profile, §3.4's profile table). A single
// CHANNEL_BASED audio element with one 7.1.4 layer (12 rendered channels) fits Simple Profile's
// "exactly one Audio Element OBU, up to 16 channels" ceiling (v1.0.0-errata §4.1, incorporated by
// reference rather than restated in v1.1.0's own §4.1/§4.2).
[[nodiscard]] inline Bytes build_ia_sequence_header_obu_payload() {
    Bytes out;
    put_fourcc(out, "iamf");  // ia_code (§3.4)
    put_u8(out, 0);           // primary_profile: Simple
    put_u8(out, 0);           // additional_profile: Simple
    return out;
}

// --- Codec Config OBU (IAMF §3.5/§3.11.4) -----------------------------------------------------

[[nodiscard]] inline Bytes build_codec_config_obu_payload(std::uint64_t codec_config_id,
                                                          const AudioTrack& track) {
    Bytes out;
    put_leb128(out, codec_config_id);
    put_fourcc(out, "ipcm");                                     // codec_id (§3.5)
    put_leb128(out, track.samples_per_frame);                    // num_samples_per_frame
    put_s16(out, 0);                                             // audio_roll_distance: 0 for ipcm (§3.5)
    put_u8(out, 0x01);                                           // sample_format_flags: little-endian
    put_u8(out, static_cast<std::uint8_t>(track.bit_depth));     // sample_size
    put_u32(out, track.sample_rate);                             // sample_rate
    return out;
}

// --- Audio Element OBU, channel-based 7.1.4 (IAMF §3.6/§3.6.2/§3.6.3.3) ----------------------

// The 7 Audio Substreams a single-layer 7.1.4 Channel Group carries, in the order §3.6.3.3
// requires: coupled substreams first (surround pairs before top pairs, front before side/rear
// within each), then non-coupled (Centre before LFE). channel_a/channel_b index into
// Frame::channels (iamf.hpp's own L,C,R,Lss,Rss,Lrs,Rrs,Ltf,Rtf,Ltb,Rtb,LFE order); channel_b is
// -1 for a non-coupled (mono) substream.
struct SubstreamChannels {
    int channel_a;
    int channel_b;  // -1 if this substream is mono
};

inline constexpr std::array<SubstreamChannels, 7> kSubstreamLayout{{
    {0, 2},    // id 0: L/R      - surround, front
    {3, 4},    // id 1: Lss/Rss  - surround, side
    {5, 6},    // id 2: Lrs/Rrs  - surround, rear
    {7, 8},    // id 3: Ltf/Rtf  - top, front
    {9, 10},   // id 4: Ltb/Rtb  - top, rear/back
    {1, -1},   // id 5: C        - non-coupled
    {11, -1},  // id 6: LFE      - non-coupled
}};

inline constexpr int kCoupledSubstreamCount = 5;  // ids 0-4 above
inline constexpr int kSubstreamCount = 7;          // kSubstreamLayout.size()
inline constexpr std::uint8_t kLoudspeakerLayout714 = 7;  // IAMF §3.6.2 table

[[nodiscard]] inline Bytes build_audio_element_obu_payload(std::uint64_t audio_element_id,
                                                            std::uint64_t codec_config_id) {
    Bytes out;
    put_leb128(out, audio_element_id);
    put_u8(out, 0);  // audio_element_type(3)=CHANNEL_BASED(0) | reserved(5)=0
    put_leb128(out, codec_config_id);

    put_leb128(out, kSubstreamCount);  // num_substreams
    for (int id = 0; id < kSubstreamCount; ++id) {
        put_leb128(out, static_cast<std::uint64_t>(id));  // audio_substream_id
    }

    // num_parameters = 0: PARAMETER_DEFINITION_DEMIXING is only "MAY be present" at num_layers ==
    // 1 (§3.6, the num_parameters semantics list), and PARAMETER_DEFINITION_RECON_GAIN "SHALL NOT
    // be present" when codec_id = ipcm - neither is needed for one static layer.
    put_leb128(out, 0);

    // ScalableChannelLayoutConfig (§3.6.2): num_layers(3)=1 | reserved(5)=0.
    put_u8(out, 1U << 5);
    // ChannelAudioLayerConfig(1): loudspeaker_layout(4)=7 | output_gain_is_present_flag(1)=0 |
    // recon_gain_is_present_flag(1)=0 | reserved(2)=0.
    put_u8(out, static_cast<std::uint8_t>(kLoudspeakerLayout714 << 4));
    put_u8(out, static_cast<std::uint8_t>(kSubstreamCount));          // substream_count
    put_u8(out, static_cast<std::uint8_t>(kCoupledSubstreamCount));  // coupled_substream_count
    // output_gain_is_present_flag == 0: no output_gain fields. loudspeaker_layout != 15: no
    // expanded_loudspeaker_layout.
    return out;
}

// --- Mix Presentation OBU (IAMF §3.7-3.7.4) ---------------------------------------------------

// IAMF §3.7.2 MixGainParamDefinition() extends the abstract ParamDefinition() (§3.6.1) with one
// constant default_mix_gain and param_definition_mode = 0 - no Parameter Block OBU ever varies
// it. duration/constant_subblock_duration follow DemixingParamDefinition's own stated convention
// (§3.6, "duration SHALL be the same as num_samples_per_frame ... constant_subblock_duration
// SHALL be the same as duration") since §3.7.2 gives no narrower rule of its own; parameter_rate =
// the track's sample rate makes "ticks per frame" (§3.6.1's own constraint) exactly
// num_samples_per_frame, a non-zero integer by construction.
inline void put_constant_mix_gain(Bytes& out, std::uint64_t parameter_id,
                                  const AudioTrack& track) {
    put_leb128(out, parameter_id);
    put_leb128(out, track.sample_rate);  // parameter_rate
    put_u8(out, 0);                      // param_definition_mode(1)=0 | reserved(7)=0
    put_leb128(out, track.samples_per_frame);  // duration
    put_leb128(out, track.samples_per_frame);  // constant_subblock_duration (== duration, so no
                                                // num_subblocks/subblock_duration loop, §3.6.1)
    put_s16(out, 0);                     // default_mix_gain: 0 dB, Q7.8 - this writer is transparent
}

// IAMF §3.7.4: Q7.8 fixed-point (16-bit signed, 8 fractional bits).
[[nodiscard]] inline std::int16_t to_q7_8(float value) {
    const float scaled = std::round(value * 256.0F);
    const float clamped = std::clamp(scaled, -32768.0F, 32767.0F);
    return static_cast<std::int16_t>(clamped);
}

// IAMF §3.7.4 LoudnessInfo(): info_type = 0 (no true_peak, no anchored_loudness, no
// info_type_bytes trailer - none of info_type's bits 0-7 are set).
inline void put_loudness_info(Bytes& out, const LoudnessInfo& loudness) {
    put_u8(out, 0);  // info_type
    put_s16(out, to_q7_8(loudness.integrated_loudness_lkfs));
    put_s16(out, to_q7_8(loudness.digital_peak_dbfs));
}

// IAMF §3.7.3 Layout(): layout_type(2)=LOUDSPEAKERS_SS_CONVENTION(2) | sound_system(4) |
// reserved(2)=0, packed into one byte.
inline void put_loudspeakers_layout(Bytes& out, std::uint8_t sound_system) {
    put_u8(out, static_cast<std::uint8_t>((2U << 6) | (sound_system << 2)));
}

[[nodiscard]] inline Bytes build_mix_presentation_obu_payload(std::uint64_t mix_presentation_id,
                                                               std::uint64_t audio_element_id,
                                                               const AudioTrack& track) {
    Bytes out;
    put_leb128(out, mix_presentation_id);
    put_leb128(out, 0);  // count_label = 0: no localized annotations (§3.7 states no SHALL/SHALL
                          // NOT constraint on this value, unlike num_sub_mixes/num_audio_elements
                          // just below, which both explicitly forbid 0); every
                          // annotations_language[]/localized_*_annotations[] array this count
                          // sizes is therefore empty and no bytes are written for any of them.

    put_leb128(out, 1);  // num_sub_mixes
    put_leb128(out, 1);  // num_audio_elements
    put_leb128(out, audio_element_id);
    // localized_element_annotations[count_label] - empty, count_label == 0.

    // RenderingConfig (§3.7.1): headphones_rendering_mode(2)=0 (render to Stereo) |
    // reserved(6)=0, then rendering_config_extension_size(leb128) = 0 (no extension bytes).
    put_u8(out, 0);
    put_leb128(out, 0);

    put_constant_mix_gain(out, 0, track);  // element_mix_gain, parameter_id 0
    put_constant_mix_gain(out, 1, track);  // output_mix_gain, parameter_id 1

    put_leb128(out, 2);  // num_layouts: Stereo (mandatory, §3.7) + this element's own 7.1.4 layout
    put_loudspeakers_layout(out, 0);  // sound_system 0: Sound System A (Stereo), §3.7.3
    put_loudness_info(out, track.stereo_loudness);
    put_loudspeakers_layout(out, 9);  // sound_system 9: Sound System J (7.1.4ch), §3.7.3
    put_loudness_info(out, track.layout_714_loudness);

    // MixPresentationTags omitted: obu_size (measured from this payload's own length) ends
    // exactly at the close of the num_sub_mixes loop, which §3.7's own semantics for
    // MixPresentationTags treats as "not present".
    return out;
}

// --- Audio Frame OBU (IAMF §3.9/§3.11.4) ------------------------------------------------------

// Appends one little-endian signed PCM sample at `bit_depth` bits (16/24/32), scaled from a
// [-1, 1] float the same way every other PCM path in this codebase rounds and clips (see
// ac3::audio's own conversion helpers) - IAMF §3.11.4 defines ipcm's sample values by reference
// to [MP4-PCM], ordinary signed integer LPCM.
inline void put_pcm_sample(Bytes& out, float sample, int bit_depth) {
    const auto full_scale = static_cast<double>((std::uint32_t{1} << (bit_depth - 1)) - 1);
    const double scaled = std::round(static_cast<double>(sample) * full_scale);
    const double clamped = std::clamp(scaled, -full_scale - 1.0, full_scale);
    const auto value = static_cast<std::int32_t>(clamped);
    const auto unsigned_value = static_cast<std::uint32_t>(value);
    switch (bit_depth) {
        case 16:
            put_u8(out, static_cast<std::uint8_t>(unsigned_value & 0xFF));
            put_u8(out, static_cast<std::uint8_t>((unsigned_value >> 8) & 0xFF));
            break;
        case 24:
            put_u8(out, static_cast<std::uint8_t>(unsigned_value & 0xFF));
            put_u8(out, static_cast<std::uint8_t>((unsigned_value >> 8) & 0xFF));
            put_u8(out, static_cast<std::uint8_t>((unsigned_value >> 16) & 0xFF));
            break;
        default:  // 32
            put_u8(out, static_cast<std::uint8_t>(unsigned_value & 0xFF));
            put_u8(out, static_cast<std::uint8_t>((unsigned_value >> 8) & 0xFF));
            put_u8(out, static_cast<std::uint8_t>((unsigned_value >> 16) & 0xFF));
            put_u8(out, static_cast<std::uint8_t>((unsigned_value >> 24) & 0xFF));
            break;
    }
}

// One substream's Audio Frame OBU payload (§3.11.4): mono is that channel's samples in order;
// stereo is L/R sample-interleaved ("the i-th audio sample of the Left channel is followed by the
// i-th audio sample of the Right channel"). `channels` is a Frame::channels-shaped span.
[[nodiscard]] inline Bytes build_audio_frame_payload(const SubstreamChannels& substream,
                                                     std::span<const std::vector<float>, 12> channels,
                                                     int bit_depth) {
    Bytes out;
    const auto& a = channels[static_cast<std::size_t>(substream.channel_a)];
    if (substream.channel_b < 0) {
        for (const float sample : a) {
            put_pcm_sample(out, sample, bit_depth);
        }
    } else {
        const auto& b = channels[static_cast<std::size_t>(substream.channel_b)];
        for (std::size_t i = 0; i < a.size(); ++i) {
            put_pcm_sample(out, a[i], bit_depth);
            put_pcm_sample(out, b[i], bit_depth);
        }
    }
    return out;
}

}  // namespace iamf::detail

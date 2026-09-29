#pragma once

// Private to the C API's own implementation: the opaque handle definitions,
// the enum-ordinal contract this whole translation layer leans on, and the
// exception-to-status_t boundary every entry point in ac3forge.h crosses
// through. Never installed - a consumer only ever sees the opaque forward
// declarations in ac3forge_c/ac3forge.h.

#include <cstdint>
#include <memory>
#include <new>
#include <vector>

#include "ac3/analysis/levels.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/latency.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/meta/qc.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3forge_c/ac3forge.h"

#ifdef AC3FORGE_HAS_AC4
#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4enc/encoder.hpp"
#endif

// --- enum-ordinal contract ---------------------------------------------
// ac3forge_c's enums are declared with the same ordinals as their C++
// counterparts on purpose, so translation is a bare static_cast rather than
// a switch - see e.g. encoder.cpp's to_cpp()/from_cpp() pairs. These
// static_asserts are what makes that safe: a future change to either side's
// enumerator order fails the build here rather than silently mistranslating.
static_assert(static_cast<int>(ac3::SampleRate::k48000) == AC3FORGE_SAMPLE_RATE_48000);
static_assert(static_cast<int>(ac3::SampleRate::k44100) == AC3FORGE_SAMPLE_RATE_44100);
static_assert(static_cast<int>(ac3::SampleRate::k32000) == AC3FORGE_SAMPLE_RATE_32000);
static_assert(static_cast<int>(ac3::SampleRate::k24000) == AC3FORGE_SAMPLE_RATE_24000);
static_assert(static_cast<int>(ac3::SampleRate::k22050) == AC3FORGE_SAMPLE_RATE_22050);
static_assert(static_cast<int>(ac3::SampleRate::k16000) == AC3FORGE_SAMPLE_RATE_16000);

static_assert(static_cast<int>(ac3::Acmod::kDualMono) == AC3FORGE_ACMOD_DUAL_MONO);
static_assert(static_cast<int>(ac3::Acmod::k1_0) == AC3FORGE_ACMOD_1_0);
static_assert(static_cast<int>(ac3::Acmod::k2_0) == AC3FORGE_ACMOD_2_0);
static_assert(static_cast<int>(ac3::Acmod::k3_0) == AC3FORGE_ACMOD_3_0);
static_assert(static_cast<int>(ac3::Acmod::k2_1) == AC3FORGE_ACMOD_2_1);
static_assert(static_cast<int>(ac3::Acmod::k3_1) == AC3FORGE_ACMOD_3_1);
static_assert(static_cast<int>(ac3::Acmod::k2_2) == AC3FORGE_ACMOD_2_2);
static_assert(static_cast<int>(ac3::Acmod::k3_2) == AC3FORGE_ACMOD_3_2);

static_assert(static_cast<int>(ac3::meta::CentreMixLevel::kMinus3dB) == AC3FORGE_CMIXLEV_MINUS_3DB);
static_assert(static_cast<int>(ac3::meta::CentreMixLevel::kMinus4_5dB) ==
              AC3FORGE_CMIXLEV_MINUS_4_5DB);
static_assert(static_cast<int>(ac3::meta::CentreMixLevel::kMinus6dB) == AC3FORGE_CMIXLEV_MINUS_6DB);

static_assert(static_cast<int>(ac3::meta::SurroundMixLevel::kMinus3dB) ==
              AC3FORGE_SURMIXLEV_MINUS_3DB);
static_assert(static_cast<int>(ac3::meta::SurroundMixLevel::kMinus6dB) ==
              AC3FORGE_SURMIXLEV_MINUS_6DB);
static_assert(static_cast<int>(ac3::meta::SurroundMixLevel::kSilent) == AC3FORGE_SURMIXLEV_SILENT);

static_assert(static_cast<int>(ac3::meta::ProfileId::kFilmStandard) == AC3FORGE_DRC_FILM_STANDARD);
static_assert(static_cast<int>(ac3::meta::ProfileId::kFilmLight) == AC3FORGE_DRC_FILM_LIGHT);
static_assert(static_cast<int>(ac3::meta::ProfileId::kMusicStandard) ==
              AC3FORGE_DRC_MUSIC_STANDARD);
static_assert(static_cast<int>(ac3::meta::ProfileId::kMusicLight) == AC3FORGE_DRC_MUSIC_LIGHT);
static_assert(static_cast<int>(ac3::meta::ProfileId::kSpeech) == AC3FORGE_DRC_SPEECH);

static_assert(static_cast<int>(ac3::eac3::StreamType::kIndependent) ==
              AC3FORGE_STREAM_TYPE_INDEPENDENT);
static_assert(static_cast<int>(ac3::eac3::StreamType::kDependent) == AC3FORGE_STREAM_TYPE_DEPENDENT);
static_assert(static_cast<int>(ac3::eac3::StreamType::kConvertible) ==
              AC3FORGE_STREAM_TYPE_CONVERTIBLE);
static_assert(static_cast<int>(ac3::eac3::StreamType::kReserved) == AC3FORGE_STREAM_TYPE_RESERVED);

static_assert(AC3FORGE_SAMPLES_PER_FRAME == ac3::kSamplesPerFrame);
static_assert(AC3FORGE_BLOCKS_PER_FRAME == ac3::kBlocksPerFrame);
static_assert(AC3FORGE_SAMPLES_PER_BLOCK == ac3::kSamplesPerBlock);

static_assert(static_cast<int>(ac3::io::StreamKind::kAc3) == AC3FORGE_STREAM_KIND_AC3);
static_assert(static_cast<int>(ac3::io::StreamKind::kEac3) == AC3FORGE_STREAM_KIND_EAC3);
static_assert(static_cast<int>(ac3::io::StreamKind::kAc3CoreEac3Extension) ==
              AC3FORGE_STREAM_KIND_AC3_CORE_EAC3_EXTENSION);

static_assert(static_cast<int>(ac3::meta::QcLoudnessLimit::kBand) == AC3FORGE_QC_LOUDNESS_BAND);
static_assert(static_cast<int>(ac3::meta::QcLoudnessLimit::kCeiling) ==
              AC3FORGE_QC_LOUDNESS_CEILING);
static_assert(static_cast<int>(ac3::meta::QcPresetId::kEbuR128S2) == AC3FORGE_QC_PRESET_EBU_R128_S2);
static_assert(static_cast<int>(ac3::meta::QcPresetId::kAtscA85) == AC3FORGE_QC_PRESET_ATSC_A85);
static_assert(static_cast<int>(ac3::meta::QcPresetId::kAtscA85Streaming) ==
              AC3FORGE_QC_PRESET_ATSC_A85_STREAMING);
static_assert(static_cast<int>(ac3::meta::QcPresetId::kNetflix) == AC3FORGE_QC_PRESET_NETFLIX);
static_assert(static_cast<int>(ac3::meta::QcPresetId::kAppleMusicAtmos) ==
              AC3FORGE_QC_PRESET_APPLE_MUSIC_ATMOS);
static_assert(ac3::meta::kQcPresetIds.size() == 5);

#ifdef AC3FORGE_HAS_AC4
static_assert(static_cast<int>(ac4::Speaker::kLeft) == AC3FORGE_AC4_SPEAKER_LEFT);
static_assert(static_cast<int>(ac4::Speaker::kRight) == AC3FORGE_AC4_SPEAKER_RIGHT);
static_assert(static_cast<int>(ac4::Speaker::kCentre) == AC3FORGE_AC4_SPEAKER_CENTRE);
static_assert(static_cast<int>(ac4::Speaker::kLfe) == AC3FORGE_AC4_SPEAKER_LFE);
static_assert(static_cast<int>(ac4::Speaker::kLeftSurround) == AC3FORGE_AC4_SPEAKER_LEFT_SURROUND);
static_assert(static_cast<int>(ac4::Speaker::kRightSurround) == AC3FORGE_AC4_SPEAKER_RIGHT_SURROUND);
static_assert(static_cast<int>(ac4::Speaker::kLeftBack) == AC3FORGE_AC4_SPEAKER_LEFT_BACK);
static_assert(static_cast<int>(ac4::Speaker::kRightBack) == AC3FORGE_AC4_SPEAKER_RIGHT_BACK);
static_assert(static_cast<int>(ac4::Speaker::kLeftWide) == AC3FORGE_AC4_SPEAKER_LEFT_WIDE);
static_assert(static_cast<int>(ac4::Speaker::kRightWide) == AC3FORGE_AC4_SPEAKER_RIGHT_WIDE);
static_assert(static_cast<int>(ac4::Speaker::kTopFrontLeft) == AC3FORGE_AC4_SPEAKER_TOP_FRONT_LEFT);
static_assert(static_cast<int>(ac4::Speaker::kTopFrontRight) ==
              AC3FORGE_AC4_SPEAKER_TOP_FRONT_RIGHT);
static_assert(static_cast<int>(ac4::Speaker::kTopBackLeft) == AC3FORGE_AC4_SPEAKER_TOP_BACK_LEFT);
static_assert(static_cast<int>(ac4::Speaker::kTopBackRight) == AC3FORGE_AC4_SPEAKER_TOP_BACK_RIGHT);
static_assert(static_cast<int>(ac4::Speaker::kTopSideLeft) == AC3FORGE_AC4_SPEAKER_TOP_SIDE_LEFT);
static_assert(static_cast<int>(ac4::Speaker::kTopSideRight) == AC3FORGE_AC4_SPEAKER_TOP_SIDE_RIGHT);
static_assert(static_cast<int>(ac4::Speaker::kLfe2) == AC3FORGE_AC4_SPEAKER_LFE2);

static_assert(static_cast<int>(ac4::ObjectKind::kBed) == AC3FORGE_AC4_OBJECT_BED);
static_assert(static_cast<int>(ac4::ObjectKind::kDyn) == AC3FORGE_AC4_OBJECT_DYN);
static_assert(static_cast<int>(ac4::ObjectKind::kIsf) == AC3FORGE_AC4_OBJECT_ISF);

static_assert(static_cast<int>(ac4::DownmixTarget::kAsCoded) == AC3FORGE_AC4_DOWNMIX_AS_CODED);
static_assert(static_cast<int>(ac4::DownmixTarget::k5X) == AC3FORGE_AC4_DOWNMIX_5X);
static_assert(static_cast<int>(ac4::DownmixTarget::kStereo) == AC3FORGE_AC4_DOWNMIX_STEREO);
static_assert(static_cast<int>(ac4::DownmixTarget::kLoRo) == AC3FORGE_AC4_DOWNMIX_LORO);
static_assert(static_cast<int>(ac4::DownmixTarget::kLtRt) == AC3FORGE_AC4_DOWNMIX_LTRT);
static_assert(static_cast<int>(ac4::DownmixTarget::kMono) == AC3FORGE_AC4_DOWNMIX_MONO);
static_assert(static_cast<int>(ac4::DownmixTarget::k7X4) == AC3FORGE_AC4_DOWNMIX_7X4);
static_assert(static_cast<int>(ac4::DownmixTarget::k7X2) == AC3FORGE_AC4_DOWNMIX_7X2);
static_assert(static_cast<int>(ac4::DownmixTarget::k7X0) == AC3FORGE_AC4_DOWNMIX_7X0);
static_assert(static_cast<int>(ac4::DownmixTarget::k5X4) == AC3FORGE_AC4_DOWNMIX_5X4);
static_assert(static_cast<int>(ac4::DownmixTarget::k5X2) == AC3FORGE_AC4_DOWNMIX_5X2);

static_assert(static_cast<int>(ac4::DrcMode::kOff) == AC3FORGE_AC4_DRC_OFF);
static_assert(static_cast<int>(ac4::DrcMode::kDefault) == AC3FORGE_AC4_DRC_DEFAULT);
static_assert(static_cast<int>(ac4::DrcMode::kHomeTheatre) == AC3FORGE_AC4_DRC_HOME_THEATRE);
static_assert(static_cast<int>(ac4::DrcMode::kFlatPanelTv) == AC3FORGE_AC4_DRC_FLAT_PANEL_TV);
static_assert(static_cast<int>(ac4::DrcMode::kPortableSpeakers) ==
              AC3FORGE_AC4_DRC_PORTABLE_SPEAKERS);
static_assert(static_cast<int>(ac4::DrcMode::kPortableHeadphones) ==
              AC3FORGE_AC4_DRC_PORTABLE_HEADPHONES);

static_assert(static_cast<int>(ac4::DecodingMode::kFull) == AC3FORGE_AC4_DECODING_FULL);
static_assert(static_cast<int>(ac4::DecodingMode::kCore) == AC3FORGE_AC4_DECODING_CORE);

static_assert(static_cast<int>(ac4::ConcealmentPolicy::kNone) == AC3FORGE_AC4_CONCEALMENT_NONE);
static_assert(static_cast<int>(ac4::ConcealmentPolicy::kRepeatFade) ==
              AC3FORGE_AC4_CONCEALMENT_REPEAT_FADE);
static_assert(static_cast<int>(ac4::ConcealmentPolicy::kMute) == AC3FORGE_AC4_CONCEALMENT_MUTE);

static_assert(static_cast<int>(ac4::ConcealmentAction::kRepeatFade) ==
              AC3FORGE_AC4_CONCEALMENT_ACTION_REPEAT_FADE);
static_assert(static_cast<int>(ac4::ConcealmentAction::kMute) ==
              AC3FORGE_AC4_CONCEALMENT_ACTION_MUTE);

static_assert(static_cast<int>(ac4::AssociatedType::kAny) == AC3FORGE_AC4_ASSOCIATED_ANY);
static_assert(static_cast<int>(ac4::AssociatedType::kAudioDescription) ==
              AC3FORGE_AC4_ASSOCIATED_AUDIO_DESCRIPTION);
static_assert(static_cast<int>(ac4::AssociatedType::kAudioDescriptionSubtitles) ==
              AC3FORGE_AC4_ASSOCIATED_AUDIO_DESCRIPTION_SUBTITLES);
static_assert(static_cast<int>(ac4::AssociatedType::kSpokenSubtitles) ==
              AC3FORGE_AC4_ASSOCIATED_SPOKEN_SUBTITLES);
static_assert(static_cast<int>(ac4::AssociatedType::kEmergencyInformation) ==
              AC3FORGE_AC4_ASSOCIATED_EMERGENCY_INFORMATION);

static_assert(static_cast<int>(ac4::CodecMode::kAuto) == AC3FORGE_AC4_CODEC_AUTO);
static_assert(static_cast<int>(ac4::CodecMode::kSimple) == AC3FORGE_AC4_CODEC_SIMPLE);
static_assert(static_cast<int>(ac4::CodecMode::kAspx) == AC3FORGE_AC4_CODEC_ASPX);
static_assert(static_cast<int>(ac4::CodecMode::kAspxAcpl1) == AC3FORGE_AC4_CODEC_ASPX_ACPL1);
static_assert(static_cast<int>(ac4::CodecMode::kAspxAcpl2) == AC3FORGE_AC4_CODEC_ASPX_ACPL2);
static_assert(static_cast<int>(ac4::CodecMode::kAspxAcpl3) == AC3FORGE_AC4_CODEC_ASPX_ACPL3);
static_assert(static_cast<int>(ac4::CodecMode::kScpl) == AC3FORGE_AC4_CODEC_SCPL);
static_assert(static_cast<int>(ac4::CodecMode::kAspxScpl) == AC3FORGE_AC4_CODEC_ASPX_SCPL);
static_assert(static_cast<int>(ac4::CodecMode::kAspxAjcc) == AC3FORGE_AC4_CODEC_ASPX_AJCC);

static_assert(static_cast<int>(ac4::RateMode::kConstant) == AC3FORGE_AC4_RATE_CONSTANT);
static_assert(static_cast<int>(ac4::RateMode::kAverage) == AC3FORGE_AC4_RATE_AVERAGE);
static_assert(static_cast<int>(ac4::RateMode::kVariable) == AC3FORGE_AC4_RATE_VARIABLE);
#endif  // AC3FORGE_HAS_AC4

namespace ac3forge_c {

[[nodiscard]] inline ac3forge_sample_rate_t from_cpp(ac3::SampleRate rate) {
    return static_cast<ac3forge_sample_rate_t>(rate);
}
[[nodiscard]] inline ac3::SampleRate to_cpp(ac3forge_sample_rate_t rate) {
    return static_cast<ac3::SampleRate>(rate);
}
[[nodiscard]] inline ac3forge_acmod_t from_cpp(ac3::Acmod acmod) {
    return static_cast<ac3forge_acmod_t>(acmod);
}
[[nodiscard]] inline ac3::Acmod to_cpp(ac3forge_acmod_t acmod) {
    return static_cast<ac3::Acmod>(acmod);
}
[[nodiscard]] inline ac3::meta::CentreMixLevel to_cpp(ac3forge_centre_mix_level_t level) {
    return static_cast<ac3::meta::CentreMixLevel>(level);
}
[[nodiscard]] inline ac3forge_centre_mix_level_t from_cpp(ac3::meta::CentreMixLevel level) {
    return static_cast<ac3forge_centre_mix_level_t>(level);
}
[[nodiscard]] inline ac3::meta::SurroundMixLevel to_cpp(ac3forge_surround_mix_level_t level) {
    return static_cast<ac3::meta::SurroundMixLevel>(level);
}
[[nodiscard]] inline ac3forge_surround_mix_level_t from_cpp(ac3::meta::SurroundMixLevel level) {
    return static_cast<ac3forge_surround_mix_level_t>(level);
}
[[nodiscard]] inline ac3::meta::ProfileId to_cpp(ac3forge_drc_profile_t profile) {
    return static_cast<ac3::meta::ProfileId>(profile);
}
[[nodiscard]] inline ac3forge_stream_type_t from_cpp(ac3::eac3::StreamType type) {
    return static_cast<ac3forge_stream_type_t>(type);
}
[[nodiscard]] inline ac3::eac3::StreamType to_cpp(ac3forge_stream_type_t type) {
    return static_cast<ac3::eac3::StreamType>(type);
}
[[nodiscard]] inline ac3::meta::HeavyConfig to_cpp(const ac3forge_heavy_config_t& config) {
    return ac3::meta::HeavyConfig{.dialogue_target_dbfs = config.dialogue_target_dbfs,
                                   .peak_ceiling_dbfs = config.peak_ceiling_dbfs,
                                   .release_db_per_second = config.release_db_per_second};
}

[[nodiscard]] inline ac3forge_latency_t from_cpp(const ac3::LatencyBudget& budget) {
    return ac3forge_latency_t{.frame_samples = budget.frame_samples,
                              .transform_samples = budget.transform_samples,
                              .lookahead_samples = budget.lookahead_samples,
                              .holdback_samples = budget.holdback_samples};
}

[[nodiscard]] inline ac3forge_status_t from_cpp(ac3::FrameError error) {
    switch (error) {
        case ac3::FrameError::kInvalidBitrate: return AC3FORGE_ERROR_ENCODE_INVALID_BITRATE;
        case ac3::FrameError::kInvalidDialnorm: return AC3FORGE_ERROR_ENCODE_INVALID_DIALNORM;
        case ac3::FrameError::kInvalidSubstream: return AC3FORGE_ERROR_ENCODE_INVALID_SUBSTREAM;
        case ac3::FrameError::kInvalidChannelMap: return AC3FORGE_ERROR_ENCODE_INVALID_CHANNEL_MAP;
        case ac3::FrameError::kTooManyChannels: return AC3FORGE_ERROR_ENCODE_TOO_MANY_CHANNELS;
        case ac3::FrameError::kInvalidMixLevel: return AC3FORGE_ERROR_ENCODE_INVALID_MIX_LEVEL;
        case ac3::FrameError::kInvalidBsi: return AC3FORGE_ERROR_ENCODE_INVALID_BSI;
        case ac3::FrameError::kInvalidObjectAudio:
            return AC3FORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO;
    }
    return AC3FORGE_ERROR_INTERNAL;
}

[[nodiscard]] inline ac3forge_status_t from_cpp(ac3::DecodeError error) {
    switch (error) {
        case ac3::DecodeError::kTruncated: return AC3FORGE_ERROR_DECODE_TRUNCATED;
        case ac3::DecodeError::kBadSyncWord: return AC3FORGE_ERROR_DECODE_BAD_SYNC_WORD;
        case ac3::DecodeError::kBadCrc: return AC3FORGE_ERROR_DECODE_BAD_CRC;
        case ac3::DecodeError::kReservedValue: return AC3FORGE_ERROR_DECODE_RESERVED_VALUE;
        case ac3::DecodeError::kUnsupported: return AC3FORGE_ERROR_DECODE_UNSUPPORTED;
        case ac3::DecodeError::kInvalidStream: return AC3FORGE_ERROR_DECODE_INVALID_STREAM;
        // Never reaches this API: it has no fast_imdct switch, so its decoders
        // always run the fast transform every build carries. Mapped to the
        // nearest code rather than left to the INTERNAL fallback all the same.
        case ac3::DecodeError::kNoReferenceTransform: return AC3FORGE_ERROR_DECODE_UNSUPPORTED;
    }
    return AC3FORGE_ERROR_INTERNAL;
}

[[nodiscard]] inline ac3forge_status_t from_cpp(ac3::io::ScanError error) {
    switch (error) {
        case ac3::io::ScanError::kEmpty: return AC3FORGE_ERROR_SCAN_EMPTY;
        case ac3::io::ScanError::kLostSync: return AC3FORGE_ERROR_SCAN_LOST_SYNC;
        case ac3::io::ScanError::kUnsupportedBsid: return AC3FORGE_ERROR_SCAN_UNSUPPORTED_BSID;
        case ac3::io::ScanError::kReservedValue: return AC3FORGE_ERROR_SCAN_RESERVED_VALUE;
        case ac3::io::ScanError::kTruncated: return AC3FORGE_ERROR_SCAN_TRUNCATED;
        case ac3::io::ScanError::kUnsupportedStructure:
            return AC3FORGE_ERROR_SCAN_UNSUPPORTED_STRUCTURE;
    }
    return AC3FORGE_ERROR_INTERNAL;
}

#ifdef AC3FORGE_HAS_AC4

[[nodiscard]] inline ac3forge_ac4_speaker_t from_cpp(ac4::Speaker speaker) {
    return static_cast<ac3forge_ac4_speaker_t>(speaker);
}
[[nodiscard]] inline ac3forge_ac4_object_kind_t from_cpp(ac4::ObjectKind kind) {
    return static_cast<ac3forge_ac4_object_kind_t>(kind);
}
[[nodiscard]] inline ac4::DownmixTarget to_cpp(ac3forge_ac4_downmix_target_t target) {
    return static_cast<ac4::DownmixTarget>(target);
}
[[nodiscard]] inline ac3forge_ac4_downmix_target_t from_cpp(ac4::DownmixTarget target) {
    return static_cast<ac3forge_ac4_downmix_target_t>(target);
}
[[nodiscard]] inline ac4::DrcMode to_cpp(ac3forge_ac4_drc_mode_t mode) {
    return static_cast<ac4::DrcMode>(mode);
}
[[nodiscard]] inline ac3forge_ac4_drc_mode_t from_cpp(ac4::DrcMode mode) {
    return static_cast<ac3forge_ac4_drc_mode_t>(mode);
}
[[nodiscard]] inline ac4::DecodingMode to_cpp(ac3forge_ac4_decoding_mode_t mode) {
    return static_cast<ac4::DecodingMode>(mode);
}
[[nodiscard]] inline ac4::ConcealmentPolicy to_cpp(ac3forge_ac4_concealment_policy_t policy) {
    return static_cast<ac4::ConcealmentPolicy>(policy);
}
[[nodiscard]] inline ac3forge_ac4_concealment_policy_t from_cpp(ac4::ConcealmentPolicy policy) {
    return static_cast<ac3forge_ac4_concealment_policy_t>(policy);
}
[[nodiscard]] inline ac3forge_ac4_concealment_action_t from_cpp(ac4::ConcealmentAction action) {
    return static_cast<ac3forge_ac4_concealment_action_t>(action);
}
[[nodiscard]] inline ac4::AssociatedType to_cpp(ac3forge_ac4_associated_type_t type) {
    return static_cast<ac4::AssociatedType>(type);
}
[[nodiscard]] inline ac4::CodecMode to_cpp(ac3forge_ac4_codec_mode_t mode) {
    return static_cast<ac4::CodecMode>(mode);
}
[[nodiscard]] inline ac3forge_ac4_codec_mode_t from_cpp(ac4::CodecMode mode) {
    return static_cast<ac3forge_ac4_codec_mode_t>(mode);
}
[[nodiscard]] inline ac4::RateMode to_cpp(ac3forge_ac4_rate_mode_t mode) {
    return static_cast<ac4::RateMode>(mode);
}
[[nodiscard]] inline ac3forge_ac4_rate_mode_t from_cpp(ac4::RateMode mode) {
    return static_cast<ac3forge_ac4_rate_mode_t>(mode);
}

[[nodiscard]] inline ac3forge_status_t from_cpp(ac4::DecodeError error) {
    switch (error) {
        case ac4::DecodeError::kTruncated: return AC3FORGE_ERROR_AC4_DECODE_TRUNCATED;
        case ac4::DecodeError::kInvalidToc: return AC3FORGE_ERROR_AC4_DECODE_INVALID_TOC;
        case ac4::DecodeError::kInvalidStream: return AC3FORGE_ERROR_AC4_DECODE_INVALID_STREAM;
        case ac4::DecodeError::kUnsupported: return AC3FORGE_ERROR_AC4_DECODE_UNSUPPORTED;
        case ac4::DecodeError::kMissingIFrame: return AC3FORGE_ERROR_AC4_DECODE_MISSING_IFRAME;
    }
    return AC3FORGE_ERROR_INTERNAL;
}

[[nodiscard]] inline ac3forge_status_t from_cpp(ac4::EncodeError error) {
    switch (error) {
        case ac4::EncodeError::kInvalidConfig: return AC3FORGE_ERROR_AC4_ENCODE_INVALID_CONFIG;
        case ac4::EncodeError::kInvalidInput: return AC3FORGE_ERROR_AC4_ENCODE_INVALID_INPUT;
    }
    return AC3FORGE_ERROR_INTERNAL;
}

#endif  // AC3FORGE_HAS_AC4

// Every entry point in ac3forge.h that can fail funnels through this: `body`
// returns ac3forge_status_t on its own successful path (AC3FORGE_OK or an
// error this layer chose deliberately), and any C++ exception that escapes
// it - std::bad_alloc from an allocation this layer or the codec core makes,
// or anything else - is caught here instead of crossing into the caller's
// (possibly non-C++) frame, which is undefined behaviour. The codec core
// itself never throws (see ac3::FrameError/DecodeError's std::expected
// convention), and every entry point validates its pointers and counts
// BEFORE calling guard() - in particular any count a body sizes a container
// from (reserve(n) throws std::length_error past max_size()) - so in practice
// only allocation failure should reach the catch clauses; the catch-all is
// the backstop that turns a missed check into AC3FORGE_ERROR_INTERNAL rather
// than into an exception crossing the C boundary.
template <class F>
[[nodiscard]] ac3forge_status_t guard(F&& body) noexcept {
    try {
        return body();
    } catch (const std::bad_alloc&) {
        return AC3FORGE_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return AC3FORGE_ERROR_INTERNAL;
    }
}

}  // namespace ac3forge_c

// --- opaque handle definitions ------------------------------------------
// Each wraps exactly one C++ value; construction only ever happens inside
// this library (via std::make_unique/new, released as a raw pointer through
// an out-parameter), so a handle's lifetime is entirely caller-driven from
// the matching _create/_destroy pair - the same convention every handle in
// ac3forge.h documents.

struct ac3forge_bytes {
    std::vector<std::byte> data;
};

struct ac3forge_encoder {
    explicit ac3forge_encoder(const ac3::EncoderConfig& config) : impl(config) {}
    ac3::FrameEncoder impl;
};

struct ac3forge_decoder {
    explicit ac3forge_decoder(const ac3::DecoderConfig& config) : impl(config) {}
    ac3::FrameDecoder impl;
};

struct ac3forge_decoded_frame {
    ac3::DecodedFrame data;
};

struct ac3forge_eac3_decoder {
    explicit ac3forge_eac3_decoder(const ac3::DecoderConfig& config) : impl(config) {}
    ac3::Eac3Decoder impl;
};

struct ac3forge_decoded_substream {
    ac3::DecodedSubstream data;
};

struct ac3forge_decoded_access_unit {
    ac3::DecodedAccessUnit data;
};

struct ac3forge_atmos_encoder {
    ac3forge_atmos_encoder(const ac3::oba::AtmosConfig& config, int objects) : impl(config, objects) {}
    ac3::oba::AtmosEncoder impl;
};

struct ac3forge_eac3_encoder {
    explicit ac3forge_eac3_encoder(const ac3::eac3::FrameConfig& config) : impl(config) {}
    ac3::eac3::FrameEncoder impl;
};

struct ac3forge_eac3_access_unit_encoder {
    explicit ac3forge_eac3_access_unit_encoder(const ac3::eac3::AccessUnitConfig& config)
        : impl(config) {}
    ac3::eac3::AccessUnitEncoder impl;
};

struct ac3forge_eac3_access_unit {
    ac3::eac3::AccessUnit data;
};

struct ac3forge_spans {
    std::vector<ac3forge_span_t> items;
};

struct ac3forge_scanned_stream {
    ac3::io::ScannedStream data;
    // ScannedStream::access_units/ScannedProgramme::access_units point into
    // the caller's own buffer (std::span<const std::byte>), exactly as
    // ac3::split_frames()'s result does - see ac3forge_spans above. Rather
    // than expose that pointer directly (which would tie this handle to a
    // std::byte* the header never otherwise names), ac3forge_scan() converts
    // every one of them to an offset/length ac3forge_span_t once, at scan
    // time, the same way split_into_spans() (eac3.cpp) already does for
    // ac3forge_split_frames()/ac3forge_split_access_units(). Parallel to
    // data.access_units and to each of data.programmes[i].access_units.
    std::vector<ac3forge_span_t> access_units;
    std::vector<std::vector<ac3forge_span_t>> programme_access_units;
};

struct ac3forge_loudness_meter {
    // Neither ac3::meta::LoudnessMeter constructor is default-constructible
    // (both need rate/acmod/lfe or rate/layout up front), so this holds one
    // built at create() time rather than embedding it by value the way
    // ac3forge_encoder/ac3forge_decoder do - matches ac3::io::WavStreamReader's
    // own reason for the same shape (elementary.hpp).
    std::unique_ptr<ac3::meta::LoudnessMeter> impl;
};

struct ac3forge_level_meter {
    // Same reasoning as ac3forge_loudness_meter above - LevelMeter is not
    // default-constructible either.
    std::unique_ptr<ac3::analysis::LevelMeter> impl;
};

#ifdef AC3FORGE_HAS_AC4

struct ac3forge_ac4_decoder {
    explicit ac3forge_ac4_decoder(const ac4::DecoderConfig& config) : impl(config) {}
    ac4::Decoder impl;
};

struct ac3forge_ac4_decoded_frame {
    ac4::DecodedFrame data;
};

struct ac3forge_ac4_encoder {
    // ac4::Encoder has no public constructor (only the static create() this
    // library's ac3forge_ac4_encoder_create() calls) but is move-constructible,
    // so this takes ownership by move rather than constructing in place the
    // way ac3forge_encoder/ac3forge_eac3_encoder above do.
    explicit ac3forge_ac4_encoder(ac4::Encoder&& encoder) : impl(std::move(encoder)) {}
    ac4::Encoder impl;
};

struct ac3forge_ac4_encoded_frame {
    ac4::EncodedFrame data;
};

// An owned copy of ac4::Encoder::toc()'s result - see
// ac3forge_ac4_encoder_toc()'s own comment in ac3forge.h.
struct ac3forge_ac4_toc {
    ac4::Toc data;
};

#endif  // AC3FORGE_HAS_AC4

#pragma once

// AC-4's own slice of internal.hpp: the iclforge::ac4:: includes, the enum-ordinal
// contract with ac3forge_c/ac3forge.h's AC-4 section, the opaque handle
// definitions behind ac3forge_ac4_decoder_t and its neighbours (global scope,
// like every other opaque handle definition in internal.hpp - it completes
// the incomplete type the public header forward-declares, also at global
// scope), and the to_cpp()/from_cpp() pairs ac4.cpp and ac4_encoder.cpp share
// (in namespace ac3forge_c, alongside their non-AC-4 neighbours). Split out
// because ac3forge.h's AC-4 declarations are always present (see that
// header's own comment) but the iclforge::ac4:: C++ types behind them are only when
// AC3FORGE_BUILD_AC4 is on: this header is included only from the "present"
// translation units (ac4.cpp, ac4_encoder.cpp), which CMake compiles only
// then (src/capi/CMakeLists.txt). ac4_absent.cpp, compiled the other way,
// never includes it and never names an iclforge::ac4:: type - every opaque handle it
// touches stays an incomplete pointer, which is all a NULL comparison or a
// pass-through needs.

#include <cstring>

#include "internal.hpp"

#include "iclforge/ac4/ac4.hpp"
#include "iclforge/ac4dec/decoder.hpp"
#include "iclforge/ac4enc/encoder.hpp"

// --- enum-ordinal contract (global scope, matching internal.hpp's own
// non-AC-4 static_asserts above the point its namespace ac3forge_c opens) ---

static_assert(static_cast<int>(iclforge::ac4::Speaker::kLeft) == AC3FORGE_AC4_SPEAKER_LEFT);
static_assert(static_cast<int>(iclforge::ac4::Speaker::kRight) == AC3FORGE_AC4_SPEAKER_RIGHT);
static_assert(static_cast<int>(iclforge::ac4::Speaker::kCentre) == AC3FORGE_AC4_SPEAKER_CENTRE);
static_assert(static_cast<int>(iclforge::ac4::Speaker::kLfe) == AC3FORGE_AC4_SPEAKER_LFE);
static_assert(static_cast<int>(iclforge::ac4::Speaker::kLeftSurround) ==
              AC3FORGE_AC4_SPEAKER_LEFT_SURROUND);
static_assert(static_cast<int>(iclforge::ac4::Speaker::kRightSurround) == AC3FORGE_AC4_SPEAKER_RIGHT_SURROUND);
static_assert(static_cast<int>(iclforge::ac4::Speaker::kLeftBack) ==
              AC3FORGE_AC4_SPEAKER_LEFT_BACK);
static_assert(static_cast<int>(iclforge::ac4::Speaker::kRightBack) ==
              AC3FORGE_AC4_SPEAKER_RIGHT_BACK);
static_assert(static_cast<int>(iclforge::ac4::Speaker::kLeftWide) ==
              AC3FORGE_AC4_SPEAKER_LEFT_WIDE);
static_assert(static_cast<int>(iclforge::ac4::Speaker::kRightWide) ==
              AC3FORGE_AC4_SPEAKER_RIGHT_WIDE);
static_assert(static_cast<int>(iclforge::ac4::Speaker::kTopFrontLeft) ==
              AC3FORGE_AC4_SPEAKER_TOP_FRONT_LEFT);
static_assert(static_cast<int>(iclforge::ac4::Speaker::kTopFrontRight) ==
              AC3FORGE_AC4_SPEAKER_TOP_FRONT_RIGHT);
static_assert(static_cast<int>(iclforge::ac4::Speaker::kTopBackLeft) ==
              AC3FORGE_AC4_SPEAKER_TOP_BACK_LEFT);
static_assert(static_cast<int>(iclforge::ac4::Speaker::kTopBackRight) ==
              AC3FORGE_AC4_SPEAKER_TOP_BACK_RIGHT);
static_assert(static_cast<int>(iclforge::ac4::Speaker::kTopSideLeft) ==
              AC3FORGE_AC4_SPEAKER_TOP_SIDE_LEFT);
static_assert(static_cast<int>(iclforge::ac4::Speaker::kTopSideRight) ==
              AC3FORGE_AC4_SPEAKER_TOP_SIDE_RIGHT);
static_assert(static_cast<int>(iclforge::ac4::Speaker::kLfe2) == AC3FORGE_AC4_SPEAKER_LFE2);

static_assert(static_cast<int>(iclforge::ac4::ObjectKind::kBed) == AC3FORGE_AC4_OBJECT_BED);
static_assert(static_cast<int>(iclforge::ac4::ObjectKind::kDyn) == AC3FORGE_AC4_OBJECT_DYN);
static_assert(static_cast<int>(iclforge::ac4::ObjectKind::kIsf) == AC3FORGE_AC4_OBJECT_ISF);

static_assert(static_cast<int>(iclforge::ac4::DownmixTarget::kAsCoded) ==
              AC3FORGE_AC4_DOWNMIX_AS_CODED);
static_assert(static_cast<int>(iclforge::ac4::DownmixTarget::k5X) == AC3FORGE_AC4_DOWNMIX_5X);
static_assert(static_cast<int>(iclforge::ac4::DownmixTarget::kStereo) ==
              AC3FORGE_AC4_DOWNMIX_STEREO);
static_assert(static_cast<int>(iclforge::ac4::DownmixTarget::kLoRo) == AC3FORGE_AC4_DOWNMIX_LORO);
static_assert(static_cast<int>(iclforge::ac4::DownmixTarget::kLtRt) == AC3FORGE_AC4_DOWNMIX_LTRT);
static_assert(static_cast<int>(iclforge::ac4::DownmixTarget::kMono) == AC3FORGE_AC4_DOWNMIX_MONO);
static_assert(static_cast<int>(iclforge::ac4::DownmixTarget::k7X4) == AC3FORGE_AC4_DOWNMIX_7X4);
static_assert(static_cast<int>(iclforge::ac4::DownmixTarget::k7X2) == AC3FORGE_AC4_DOWNMIX_7X2);
static_assert(static_cast<int>(iclforge::ac4::DownmixTarget::k7X0) == AC3FORGE_AC4_DOWNMIX_7X0);
static_assert(static_cast<int>(iclforge::ac4::DownmixTarget::k5X4) == AC3FORGE_AC4_DOWNMIX_5X4);
static_assert(static_cast<int>(iclforge::ac4::DownmixTarget::k5X2) == AC3FORGE_AC4_DOWNMIX_5X2);

static_assert(static_cast<int>(iclforge::ac4::DrcMode::kOff) == AC3FORGE_AC4_DRC_OFF);
static_assert(static_cast<int>(iclforge::ac4::DrcMode::kDefault) == AC3FORGE_AC4_DRC_DEFAULT);
static_assert(static_cast<int>(iclforge::ac4::DrcMode::kHomeTheatre) ==
              AC3FORGE_AC4_DRC_HOME_THEATRE);
static_assert(static_cast<int>(iclforge::ac4::DrcMode::kFlatPanelTv) ==
              AC3FORGE_AC4_DRC_FLAT_PANEL_TV);
static_assert(static_cast<int>(iclforge::ac4::DrcMode::kPortableSpeakers) ==
              AC3FORGE_AC4_DRC_PORTABLE_SPEAKERS);
static_assert(static_cast<int>(iclforge::ac4::DrcMode::kPortableHeadphones) ==
              AC3FORGE_AC4_DRC_PORTABLE_HEADPHONES);

static_assert(static_cast<int>(iclforge::ac4::DecodingMode::kFull) == AC3FORGE_AC4_DECODING_FULL);
static_assert(static_cast<int>(iclforge::ac4::DecodingMode::kCore) == AC3FORGE_AC4_DECODING_CORE);

static_assert(static_cast<int>(iclforge::ac4::ConcealmentPolicy::kNone) ==
              AC3FORGE_AC4_CONCEALMENT_NONE);
static_assert(static_cast<int>(iclforge::ac4::ConcealmentPolicy::kRepeatFade) ==
              AC3FORGE_AC4_CONCEALMENT_REPEAT_FADE);
static_assert(static_cast<int>(iclforge::ac4::ConcealmentPolicy::kMute) ==
              AC3FORGE_AC4_CONCEALMENT_MUTE);

static_assert(static_cast<int>(iclforge::ac4::ConcealmentAction::kRepeatFade) ==
              AC3FORGE_AC4_CONCEALMENT_ACTION_REPEAT_FADE);
static_assert(static_cast<int>(iclforge::ac4::ConcealmentAction::kMute) ==
              AC3FORGE_AC4_CONCEALMENT_ACTION_MUTE);

static_assert(static_cast<int>(iclforge::ac4::AssociatedType::kAny) == AC3FORGE_AC4_ASSOCIATED_ANY);
static_assert(static_cast<int>(iclforge::ac4::AssociatedType::kAudioDescription) ==
              AC3FORGE_AC4_ASSOCIATED_AUDIO_DESCRIPTION);
static_assert(static_cast<int>(iclforge::ac4::AssociatedType::kAudioDescriptionSubtitles) ==
              AC3FORGE_AC4_ASSOCIATED_AUDIO_DESCRIPTION_SUBTITLES);
static_assert(static_cast<int>(iclforge::ac4::AssociatedType::kSpokenSubtitles) ==
              AC3FORGE_AC4_ASSOCIATED_SPOKEN_SUBTITLES);
static_assert(static_cast<int>(iclforge::ac4::AssociatedType::kEmergencyInformation) ==
              AC3FORGE_AC4_ASSOCIATED_EMERGENCY_INFORMATION);

static_assert(static_cast<int>(iclforge::ac4::CodecMode::kAuto) == AC3FORGE_AC4_CODEC_AUTO);
static_assert(static_cast<int>(iclforge::ac4::CodecMode::kSimple) == AC3FORGE_AC4_CODEC_SIMPLE);
static_assert(static_cast<int>(iclforge::ac4::CodecMode::kAspx) == AC3FORGE_AC4_CODEC_ASPX);
static_assert(static_cast<int>(iclforge::ac4::CodecMode::kAspxAcpl1) ==
              AC3FORGE_AC4_CODEC_ASPX_ACPL1);
static_assert(static_cast<int>(iclforge::ac4::CodecMode::kAspxAcpl2) ==
              AC3FORGE_AC4_CODEC_ASPX_ACPL2);
static_assert(static_cast<int>(iclforge::ac4::CodecMode::kAspxAcpl3) ==
              AC3FORGE_AC4_CODEC_ASPX_ACPL3);
static_assert(static_cast<int>(iclforge::ac4::CodecMode::kScpl) == AC3FORGE_AC4_CODEC_SCPL);
static_assert(static_cast<int>(iclforge::ac4::CodecMode::kAspxScpl) ==
              AC3FORGE_AC4_CODEC_ASPX_SCPL);
static_assert(static_cast<int>(iclforge::ac4::CodecMode::kAspxAjcc) ==
              AC3FORGE_AC4_CODEC_ASPX_AJCC);

static_assert(static_cast<int>(iclforge::ac4::RateMode::kConstant) == AC3FORGE_AC4_RATE_CONSTANT);
static_assert(static_cast<int>(iclforge::ac4::RateMode::kAverage) == AC3FORGE_AC4_RATE_AVERAGE);
static_assert(static_cast<int>(iclforge::ac4::RateMode::kVariable) == AC3FORGE_AC4_RATE_VARIABLE);

static_assert(static_cast<int>(iclforge::ac4::BedChannel::kLeft) == AC3FORGE_AC4_BED_LEFT);
static_assert(static_cast<int>(iclforge::ac4::BedChannel::kRight) == AC3FORGE_AC4_BED_RIGHT);
static_assert(static_cast<int>(iclforge::ac4::BedChannel::kCentre) == AC3FORGE_AC4_BED_CENTRE);
static_assert(static_cast<int>(iclforge::ac4::BedChannel::kLeftSurround) ==
              AC3FORGE_AC4_BED_LEFT_SURROUND);
static_assert(static_cast<int>(iclforge::ac4::BedChannel::kRightSurround) ==
              AC3FORGE_AC4_BED_RIGHT_SURROUND);
static_assert(static_cast<int>(iclforge::ac4::BedChannel::kLeftBack) == AC3FORGE_AC4_BED_LEFT_BACK);
static_assert(static_cast<int>(iclforge::ac4::BedChannel::kRightBack) ==
              AC3FORGE_AC4_BED_RIGHT_BACK);
static_assert(static_cast<int>(iclforge::ac4::BedChannel::kTopFrontLeft) ==
              AC3FORGE_AC4_BED_TOP_FRONT_LEFT);
static_assert(static_cast<int>(iclforge::ac4::BedChannel::kTopFrontRight) ==
              AC3FORGE_AC4_BED_TOP_FRONT_RIGHT);
static_assert(static_cast<int>(iclforge::ac4::BedChannel::kTopSideLeft) ==
              AC3FORGE_AC4_BED_TOP_SIDE_LEFT);
static_assert(static_cast<int>(iclforge::ac4::BedChannel::kTopSideRight) ==
              AC3FORGE_AC4_BED_TOP_SIDE_RIGHT);
static_assert(static_cast<int>(iclforge::ac4::BedChannel::kTopBackLeft) ==
              AC3FORGE_AC4_BED_TOP_BACK_LEFT);
static_assert(static_cast<int>(iclforge::ac4::BedChannel::kTopBackRight) ==
              AC3FORGE_AC4_BED_TOP_BACK_RIGHT);
static_assert(static_cast<int>(iclforge::ac4::BedChannel::kLeftWide) == AC3FORGE_AC4_BED_LEFT_WIDE);
static_assert(static_cast<int>(iclforge::ac4::BedChannel::kRightWide) ==
              AC3FORGE_AC4_BED_RIGHT_WIDE);

static_assert(static_cast<int>(iclforge::ac4::ObjectCoding::kAjoc) ==
              AC3FORGE_AC4_OBJECT_CODING_AJOC);
static_assert(static_cast<int>(iclforge::ac4::ObjectCoding::kDirect) ==
              AC3FORGE_AC4_OBJECT_CODING_DIRECT);

static_assert(static_cast<int>(iclforge::ac4::AjocDownmix::kComputed) ==
              AC3FORGE_AC4_AJOC_DOWNMIX_COMPUTED);
static_assert(static_cast<int>(iclforge::ac4::AjocDownmix::kStatic50) ==
              AC3FORGE_AC4_AJOC_DOWNMIX_STATIC_50);
static_assert(static_cast<int>(iclforge::ac4::AjocDownmix::kStatic51) ==
              AC3FORGE_AC4_AJOC_DOWNMIX_STATIC_51);

static_assert(static_cast<int>(iclforge::ac4::AdditionalPair::kNone) == AC3FORGE_AC4_PAIR_NONE);
static_assert(static_cast<int>(iclforge::ac4::AdditionalPair::kBack) == AC3FORGE_AC4_PAIR_BACK);
static_assert(static_cast<int>(iclforge::ac4::AdditionalPair::kWide) == AC3FORGE_AC4_PAIR_WIDE);
static_assert(static_cast<int>(iclforge::ac4::AdditionalPair::kTopFront) ==
              AC3FORGE_AC4_PAIR_TOP_FRONT);

// --- opaque handle definitions (global scope, matching internal.hpp's own
// non-AC-4 ones - ac3forge_ac4_decoder_t and its neighbours are forward-
// declared at global scope in ac3forge.h, so what completes them lives there
// too) ---

struct ac3forge_ac4_decoder {
    explicit ac3forge_ac4_decoder(const iclforge::ac4::DecoderConfig& config) : impl(config) {}
    iclforge::ac4::Decoder impl;
};

struct ac3forge_ac4_decoded_frame {
    iclforge::ac4::DecodedFrame data;
};

struct ac3forge_ac4_encoder {
    // iclforge::ac4::Encoder has no public constructor (only the static create() this
    // library's ac3forge_ac4_encoder_create() calls) but is move-constructible,
    // so this takes ownership by move rather than constructing in place the
    // way ac3forge_encoder/ac3forge_eac3_encoder above do.
    explicit ac3forge_ac4_encoder(iclforge::ac4::Encoder&& encoder) : impl(std::move(encoder)) {}
    iclforge::ac4::Encoder impl;
};

struct ac3forge_ac4_encoded_frame {
    iclforge::ac4::EncodedFrame data;
};

// An owned copy of iclforge::ac4::Encoder::toc()'s result - see
// ac3forge_ac4_encoder_toc()'s own comment in ac3forge.h.
struct ac3forge_ac4_toc {
    iclforge::ac4::Toc data;
};

namespace ac3forge_c {

[[nodiscard]] inline ac3forge_ac4_speaker_t from_cpp(iclforge::ac4::Speaker speaker) {
    return static_cast<ac3forge_ac4_speaker_t>(speaker);
}
[[nodiscard]] inline ac3forge_ac4_object_kind_t from_cpp(iclforge::ac4::ObjectKind kind) {
    return static_cast<ac3forge_ac4_object_kind_t>(kind);
}
[[nodiscard]] inline iclforge::ac4::DownmixTarget to_cpp(ac3forge_ac4_downmix_target_t target) {
    return static_cast<iclforge::ac4::DownmixTarget>(target);
}
[[nodiscard]] inline ac3forge_ac4_downmix_target_t from_cpp(iclforge::ac4::DownmixTarget target) {
    return static_cast<ac3forge_ac4_downmix_target_t>(target);
}
[[nodiscard]] inline iclforge::ac4::DrcMode to_cpp(ac3forge_ac4_drc_mode_t mode) {
    return static_cast<iclforge::ac4::DrcMode>(mode);
}
[[nodiscard]] inline ac3forge_ac4_drc_mode_t from_cpp(iclforge::ac4::DrcMode mode) {
    return static_cast<ac3forge_ac4_drc_mode_t>(mode);
}
[[nodiscard]] inline iclforge::ac4::DecodingMode to_cpp(ac3forge_ac4_decoding_mode_t mode) {
    return static_cast<iclforge::ac4::DecodingMode>(mode);
}
[[nodiscard]] inline iclforge::ac4::ConcealmentPolicy to_cpp(
    ac3forge_ac4_concealment_policy_t policy) {
    return static_cast<iclforge::ac4::ConcealmentPolicy>(policy);
}
[[nodiscard]] inline ac3forge_ac4_concealment_policy_t from_cpp(
    iclforge::ac4::ConcealmentPolicy policy) {
    return static_cast<ac3forge_ac4_concealment_policy_t>(policy);
}
[[nodiscard]] inline ac3forge_ac4_concealment_action_t from_cpp(
    iclforge::ac4::ConcealmentAction action) {
    return static_cast<ac3forge_ac4_concealment_action_t>(action);
}
[[nodiscard]] inline iclforge::ac4::AssociatedType to_cpp(ac3forge_ac4_associated_type_t type) {
    return static_cast<iclforge::ac4::AssociatedType>(type);
}
[[nodiscard]] inline iclforge::ac4::CodecMode to_cpp(ac3forge_ac4_codec_mode_t mode) {
    return static_cast<iclforge::ac4::CodecMode>(mode);
}
[[nodiscard]] inline ac3forge_ac4_codec_mode_t from_cpp(iclforge::ac4::CodecMode mode) {
    return static_cast<ac3forge_ac4_codec_mode_t>(mode);
}
[[nodiscard]] inline iclforge::ac4::RateMode to_cpp(ac3forge_ac4_rate_mode_t mode) {
    return static_cast<iclforge::ac4::RateMode>(mode);
}
[[nodiscard]] inline ac3forge_ac4_rate_mode_t from_cpp(iclforge::ac4::RateMode mode) {
    return static_cast<ac3forge_ac4_rate_mode_t>(mode);
}

[[nodiscard]] inline ac3forge_ac4_bed_channel_t from_cpp(iclforge::ac4::BedChannel channel) {
    return static_cast<ac3forge_ac4_bed_channel_t>(channel);
}
[[nodiscard]] inline iclforge::ac4::ObjectCoding to_cpp(ac3forge_ac4_object_coding_t coding) {
    return static_cast<iclforge::ac4::ObjectCoding>(coding);
}
[[nodiscard]] inline iclforge::ac4::AjocDownmix to_cpp(ac3forge_ac4_ajoc_downmix_t downmix) {
    return static_cast<iclforge::ac4::AjocDownmix>(downmix);
}
[[nodiscard]] inline iclforge::ac4::AdditionalPair to_cpp(ac3forge_ac4_additional_pair_t pair) {
    return static_cast<iclforge::ac4::AdditionalPair>(pair);
}
[[nodiscard]] inline ac3forge_ac4_additional_pair_t from_cpp(iclforge::ac4::AdditionalPair pair) {
    return static_cast<ac3forge_ac4_additional_pair_t>(pair);
}

// The int an enumeration-typed C field stores. A C caller can store any int
// there, and loading a value the enumeration does not name is undefined in C++
// (Clang's -fsanitize=enum reports it), so the checks below read the bytes and
// never the enumeration. The underlying type of these enumerations is unsigned
// on some compilers, where `>= 0` on it is a -Wtype-limits finding: the int
// is compared instead.
template <typename E>
[[nodiscard]] inline int stored_value(const E& value) {
    static_assert(sizeof(E) == sizeof(int));
    int out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

// The new enumerations' values as ac3forge.h defines them, for the boundary
// checks ac4_encoder.cpp makes on what a caller hands in: a C enumeration can
// hold any int, and the encoder's own refusal_reason() speaks only of
// configurations it can be asked about.
[[nodiscard]] inline bool valid(const ac3forge_ac4_bed_channel_t& channel) {
    // Table 66's codes: 3 is not a loudspeaker a bed object can name.
    const int code = stored_value(channel);
    return code >= 0 && code <= static_cast<int>(AC3FORGE_AC4_BED_RIGHT_WIDE) && code != 3;
}
[[nodiscard]] inline bool valid(const ac3forge_ac4_object_coding_t& coding) {
    const int code = stored_value(coding);
    return code == static_cast<int>(AC3FORGE_AC4_OBJECT_CODING_AJOC) ||
           code == static_cast<int>(AC3FORGE_AC4_OBJECT_CODING_DIRECT);
}
[[nodiscard]] inline bool valid(const ac3forge_ac4_ajoc_downmix_t& downmix) {
    const int code = stored_value(downmix);
    return code >= 0 && code <= static_cast<int>(AC3FORGE_AC4_AJOC_DOWNMIX_STATIC_51);
}
[[nodiscard]] inline bool valid(const ac3forge_ac4_additional_pair_t& pair) {
    const int code = stored_value(pair);
    return code >= 0 && code <= static_cast<int>(AC3FORGE_AC4_PAIR_TOP_FRONT);
}

// iclforge::ac4::ObjectProperties and its C mirror, both ways: the decoder's accessors
// report them and the encoder's configuration and updates take them.
[[nodiscard]] inline ac3forge_ac4_object_properties_t from_cpp(
    const iclforge::ac4::ObjectProperties& p) {
    ac3forge_ac4_object_properties_t out{};
    out.active = p.active ? 1 : 0;
    out.gain_db = p.gain_db;
    out.priority = p.priority;
    out.x = p.position[0];
    out.y = p.position[1];
    out.z = p.position[2];
    out.zone_mask = p.zone_mask;
    out.enable_elevation = p.enable_elevation ? 1 : 0;
    out.snap = p.snap ? 1 : 0;
    out.width_x = p.width[0];
    out.width_y = p.width[1];
    out.width_z = p.width[2];
    out.screen_factor = p.screen_factor;
    out.depth_exponent = p.depth_exponent;
    out.has_distance = p.distance.has_value() ? 1 : 0;
    out.distance = p.distance.value_or(0.0);
    out.divergence = p.divergence;
    out.trim_disabled = p.trim_disabled ? 1 : 0;
    out.has_headphone_render_mode = p.headphone_render_mode.has_value() ? 1 : 0;
    out.headphone_render_mode = p.headphone_render_mode.value_or(0);
    out.head_track_disabled = p.head_track_disabled ? 1 : 0;
    return out;
}

[[nodiscard]] inline iclforge::ac4::ObjectProperties to_cpp(
    const ac3forge_ac4_object_properties_t& p) {
    iclforge::ac4::ObjectProperties out;
    out.active = p.active != 0;
    out.gain_db = p.gain_db;
    out.priority = p.priority;
    out.position = {p.x, p.y, p.z};
    out.zone_mask = p.zone_mask;
    out.enable_elevation = p.enable_elevation != 0;
    out.snap = p.snap != 0;
    out.width = {p.width_x, p.width_y, p.width_z};
    out.screen_factor = p.screen_factor;
    out.depth_exponent = p.depth_exponent;
    out.distance = p.has_distance != 0 ? std::optional<double>(p.distance) : std::nullopt;
    out.divergence = p.divergence;
    out.trim_disabled = p.trim_disabled != 0;
    out.headphone_render_mode = p.has_headphone_render_mode != 0
                                    ? std::optional<int>(p.headphone_render_mode)
                                    : std::nullopt;
    out.head_track_disabled = p.head_track_disabled != 0;
    return out;
}

[[nodiscard]] inline ac3forge_status_t from_cpp(iclforge::ac4::DecodeError error) {
    switch (error) {
        case iclforge::ac4::DecodeError::kTruncated: return AC3FORGE_ERROR_AC4_DECODE_TRUNCATED;
        case iclforge::ac4::DecodeError::kInvalidToc: return AC3FORGE_ERROR_AC4_DECODE_INVALID_TOC;
        case iclforge::ac4::DecodeError::kInvalidStream:
            return AC3FORGE_ERROR_AC4_DECODE_INVALID_STREAM;
        case iclforge::ac4::DecodeError::kUnsupported: return AC3FORGE_ERROR_AC4_DECODE_UNSUPPORTED;
        case iclforge::ac4::DecodeError::kMissingIFrame:
            return AC3FORGE_ERROR_AC4_DECODE_MISSING_IFRAME;
    }
    return AC3FORGE_ERROR_INTERNAL;
}

[[nodiscard]] inline ac3forge_status_t from_cpp(iclforge::ac4::EncodeError error) {
    switch (error) {
        case iclforge::ac4::EncodeError::kInvalidConfig:
            return AC3FORGE_ERROR_AC4_ENCODE_INVALID_CONFIG;
        case iclforge::ac4::EncodeError::kInvalidInput:
            return AC3FORGE_ERROR_AC4_ENCODE_INVALID_INPUT;
    }
    return AC3FORGE_ERROR_INTERNAL;
}

}  // namespace ac3forge_c

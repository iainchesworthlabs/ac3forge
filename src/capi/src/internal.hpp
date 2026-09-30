#pragma once

// Private to the C API's own implementation: the opaque handle definitions,
// the enum-ordinal contract this whole translation layer leans on, and the
// exception-to-status_t boundary every entry point in iclforge.h crosses
// through. Never installed - a consumer only ever sees the opaque forward
// declarations in iclforge_c/iclforge.h.

#include <cstdint>
#include <memory>
#include <new>
#include <vector>

#include "iclforge/ac3/analysis/levels.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/ac3/encoder/encoder.hpp"
#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/ac3/latency.hpp"
#include "iclforge/ac3/meta/loudness.hpp"
#include "iclforge/ac3/meta/qc.hpp"
#include "iclforge/ac3/oba/atmos.hpp"
#include "iclforge_c/iclforge.h"


// --- enum-ordinal contract ---------------------------------------------
// iclforge_c's enums are declared with the same ordinals as their C++
// counterparts on purpose, so translation is a bare static_cast rather than
// a switch - see e.g. encoder.cpp's to_cpp()/from_cpp() pairs. These
// static_asserts are what makes that safe: a future change to either side's
// enumerator order fails the build here rather than silently mistranslating.
static_assert(static_cast<int>(iclforge::SampleRate::k48000) == ICLFORGE_SAMPLE_RATE_48000);
static_assert(static_cast<int>(iclforge::SampleRate::k44100) == ICLFORGE_SAMPLE_RATE_44100);
static_assert(static_cast<int>(iclforge::SampleRate::k32000) == ICLFORGE_SAMPLE_RATE_32000);
static_assert(static_cast<int>(iclforge::SampleRate::k24000) == ICLFORGE_SAMPLE_RATE_24000);
static_assert(static_cast<int>(iclforge::SampleRate::k22050) == ICLFORGE_SAMPLE_RATE_22050);
static_assert(static_cast<int>(iclforge::SampleRate::k16000) == ICLFORGE_SAMPLE_RATE_16000);

static_assert(static_cast<int>(iclforge::Acmod::kDualMono) == ICLFORGE_ACMOD_DUAL_MONO);
static_assert(static_cast<int>(iclforge::Acmod::k1_0) == ICLFORGE_ACMOD_1_0);
static_assert(static_cast<int>(iclforge::Acmod::k2_0) == ICLFORGE_ACMOD_2_0);
static_assert(static_cast<int>(iclforge::Acmod::k3_0) == ICLFORGE_ACMOD_3_0);
static_assert(static_cast<int>(iclforge::Acmod::k2_1) == ICLFORGE_ACMOD_2_1);
static_assert(static_cast<int>(iclforge::Acmod::k3_1) == ICLFORGE_ACMOD_3_1);
static_assert(static_cast<int>(iclforge::Acmod::k2_2) == ICLFORGE_ACMOD_2_2);
static_assert(static_cast<int>(iclforge::Acmod::k3_2) == ICLFORGE_ACMOD_3_2);

static_assert(static_cast<int>(iclforge::meta::CentreMixLevel::kMinus3dB) ==
              ICLFORGE_CMIXLEV_MINUS_3DB);
static_assert(static_cast<int>(iclforge::meta::CentreMixLevel::kMinus4_5dB) ==
              ICLFORGE_CMIXLEV_MINUS_4_5DB);
static_assert(static_cast<int>(iclforge::meta::CentreMixLevel::kMinus6dB) ==
              ICLFORGE_CMIXLEV_MINUS_6DB);

static_assert(static_cast<int>(iclforge::meta::SurroundMixLevel::kMinus3dB) ==
              ICLFORGE_SURMIXLEV_MINUS_3DB);
static_assert(static_cast<int>(iclforge::meta::SurroundMixLevel::kMinus6dB) ==
              ICLFORGE_SURMIXLEV_MINUS_6DB);
static_assert(static_cast<int>(iclforge::meta::SurroundMixLevel::kSilent) ==
              ICLFORGE_SURMIXLEV_SILENT);

static_assert(static_cast<int>(iclforge::meta::ProfileId::kFilmStandard) ==
              ICLFORGE_DRC_FILM_STANDARD);
static_assert(static_cast<int>(iclforge::meta::ProfileId::kFilmLight) == ICLFORGE_DRC_FILM_LIGHT);
static_assert(static_cast<int>(iclforge::meta::ProfileId::kMusicStandard) ==
              ICLFORGE_DRC_MUSIC_STANDARD);
static_assert(static_cast<int>(iclforge::meta::ProfileId::kMusicLight) == ICLFORGE_DRC_MUSIC_LIGHT);
static_assert(static_cast<int>(iclforge::meta::ProfileId::kSpeech) == ICLFORGE_DRC_SPEECH);

static_assert(static_cast<int>(iclforge::eac3::StreamType::kIndependent) ==
              ICLFORGE_STREAM_TYPE_INDEPENDENT);
static_assert(static_cast<int>(iclforge::eac3::StreamType::kDependent) == ICLFORGE_STREAM_TYPE_DEPENDENT);
static_assert(static_cast<int>(iclforge::eac3::StreamType::kConvertible) ==
              ICLFORGE_STREAM_TYPE_CONVERTIBLE);
static_assert(static_cast<int>(iclforge::eac3::StreamType::kReserved) ==
              ICLFORGE_STREAM_TYPE_RESERVED);

static_assert(ICLFORGE_SAMPLES_PER_FRAME == iclforge::kSamplesPerFrame);
static_assert(ICLFORGE_BLOCKS_PER_FRAME == iclforge::kBlocksPerFrame);
static_assert(ICLFORGE_SAMPLES_PER_BLOCK == iclforge::kSamplesPerBlock);

static_assert(static_cast<int>(iclforge::io::StreamKind::kAc3) == ICLFORGE_STREAM_KIND_AC3);
static_assert(static_cast<int>(iclforge::io::StreamKind::kEac3) == ICLFORGE_STREAM_KIND_EAC3);
static_assert(static_cast<int>(iclforge::io::StreamKind::kAc3CoreEac3Extension) ==
              ICLFORGE_STREAM_KIND_AC3_CORE_EAC3_EXTENSION);

static_assert(static_cast<int>(iclforge::meta::QcLoudnessLimit::kBand) ==
              ICLFORGE_QC_LOUDNESS_BAND);
static_assert(static_cast<int>(iclforge::meta::QcLoudnessLimit::kCeiling) ==
              ICLFORGE_QC_LOUDNESS_CEILING);
static_assert(static_cast<int>(iclforge::meta::QcPresetId::kEbuR128S2) == ICLFORGE_QC_PRESET_EBU_R128_S2);
static_assert(static_cast<int>(iclforge::meta::QcPresetId::kAtscA85) ==
              ICLFORGE_QC_PRESET_ATSC_A85);
static_assert(static_cast<int>(iclforge::meta::QcPresetId::kAtscA85Streaming) ==
              ICLFORGE_QC_PRESET_ATSC_A85_STREAMING);
static_assert(static_cast<int>(iclforge::meta::QcPresetId::kNetflix) == ICLFORGE_QC_PRESET_NETFLIX);
static_assert(static_cast<int>(iclforge::meta::QcPresetId::kAppleMusicAtmos) ==
              ICLFORGE_QC_PRESET_APPLE_MUSIC_ATMOS);
static_assert(iclforge::meta::kQcPresetIds.size() == 5);


namespace iclforge_c {

[[nodiscard]] inline iclforge_sample_rate_t from_cpp(iclforge::SampleRate rate) {
    return static_cast<iclforge_sample_rate_t>(rate);
}
[[nodiscard]] inline iclforge::SampleRate to_cpp(iclforge_sample_rate_t rate) {
    return static_cast<iclforge::SampleRate>(rate);
}
[[nodiscard]] inline iclforge_acmod_t from_cpp(iclforge::Acmod acmod) {
    return static_cast<iclforge_acmod_t>(acmod);
}
[[nodiscard]] inline iclforge::Acmod to_cpp(iclforge_acmod_t acmod) {
    return static_cast<iclforge::Acmod>(acmod);
}
[[nodiscard]] inline iclforge::meta::CentreMixLevel to_cpp(iclforge_centre_mix_level_t level) {
    return static_cast<iclforge::meta::CentreMixLevel>(level);
}
[[nodiscard]] inline iclforge_centre_mix_level_t from_cpp(iclforge::meta::CentreMixLevel level) {
    return static_cast<iclforge_centre_mix_level_t>(level);
}
[[nodiscard]] inline iclforge::meta::SurroundMixLevel to_cpp(iclforge_surround_mix_level_t level) {
    return static_cast<iclforge::meta::SurroundMixLevel>(level);
}
[[nodiscard]] inline iclforge_surround_mix_level_t from_cpp(
    iclforge::meta::SurroundMixLevel level) {
    return static_cast<iclforge_surround_mix_level_t>(level);
}
[[nodiscard]] inline iclforge::meta::ProfileId to_cpp(iclforge_drc_profile_t profile) {
    return static_cast<iclforge::meta::ProfileId>(profile);
}
[[nodiscard]] inline iclforge_stream_type_t from_cpp(iclforge::eac3::StreamType type) {
    return static_cast<iclforge_stream_type_t>(type);
}
[[nodiscard]] inline iclforge::eac3::StreamType to_cpp(iclforge_stream_type_t type) {
    return static_cast<iclforge::eac3::StreamType>(type);
}
[[nodiscard]] inline iclforge::meta::HeavyConfig to_cpp(const iclforge_heavy_config_t& config) {
    return iclforge::meta::HeavyConfig{.dialogue_target_dbfs = config.dialogue_target_dbfs,
                                   .peak_ceiling_dbfs = config.peak_ceiling_dbfs,
                                   .release_db_per_second = config.release_db_per_second};
}

[[nodiscard]] inline iclforge_latency_t from_cpp(const iclforge::LatencyBudget& budget) {
    return iclforge_latency_t{.frame_samples = budget.frame_samples,
                              .transform_samples = budget.transform_samples,
                              .lookahead_samples = budget.lookahead_samples,
                              .holdback_samples = budget.holdback_samples};
}

[[nodiscard]] inline iclforge_status_t from_cpp(iclforge::FrameError error) {
    switch (error) {
        case iclforge::FrameError::kInvalidBitrate: return ICLFORGE_ERROR_ENCODE_INVALID_BITRATE;
        case iclforge::FrameError::kInvalidDialnorm: return ICLFORGE_ERROR_ENCODE_INVALID_DIALNORM;
        case iclforge::FrameError::kInvalidSubstream:
            return ICLFORGE_ERROR_ENCODE_INVALID_SUBSTREAM;
        case iclforge::FrameError::kInvalidChannelMap:
            return ICLFORGE_ERROR_ENCODE_INVALID_CHANNEL_MAP;
        case iclforge::FrameError::kTooManyChannels: return ICLFORGE_ERROR_ENCODE_TOO_MANY_CHANNELS;
        case iclforge::FrameError::kInvalidMixLevel: return ICLFORGE_ERROR_ENCODE_INVALID_MIX_LEVEL;
        case iclforge::FrameError::kInvalidBsi: return ICLFORGE_ERROR_ENCODE_INVALID_BSI;
        case iclforge::FrameError::kInvalidObjectAudio:
            return ICLFORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO;
    }
    return ICLFORGE_ERROR_INTERNAL;
}

[[nodiscard]] inline iclforge_status_t from_cpp(iclforge::DecodeError error) {
    switch (error) {
        case iclforge::DecodeError::kTruncated: return ICLFORGE_ERROR_DECODE_TRUNCATED;
        case iclforge::DecodeError::kBadSyncWord: return ICLFORGE_ERROR_DECODE_BAD_SYNC_WORD;
        case iclforge::DecodeError::kBadCrc: return ICLFORGE_ERROR_DECODE_BAD_CRC;
        case iclforge::DecodeError::kReservedValue: return ICLFORGE_ERROR_DECODE_RESERVED_VALUE;
        case iclforge::DecodeError::kUnsupported: return ICLFORGE_ERROR_DECODE_UNSUPPORTED;
        case iclforge::DecodeError::kInvalidStream: return ICLFORGE_ERROR_DECODE_INVALID_STREAM;
        // Never reaches this API: it has no fast_imdct switch, so its decoders
        // always run the fast transform every build carries. Mapped to the
        // nearest code rather than left to the INTERNAL fallback all the same.
        case iclforge::DecodeError::kNoReferenceTransform: return ICLFORGE_ERROR_DECODE_UNSUPPORTED;
    }
    return ICLFORGE_ERROR_INTERNAL;
}

[[nodiscard]] inline iclforge_status_t from_cpp(iclforge::io::ScanError error) {
    switch (error) {
        case iclforge::io::ScanError::kEmpty: return ICLFORGE_ERROR_SCAN_EMPTY;
        case iclforge::io::ScanError::kLostSync: return ICLFORGE_ERROR_SCAN_LOST_SYNC;
        case iclforge::io::ScanError::kUnsupportedBsid: return ICLFORGE_ERROR_SCAN_UNSUPPORTED_BSID;
        case iclforge::io::ScanError::kReservedValue: return ICLFORGE_ERROR_SCAN_RESERVED_VALUE;
        case iclforge::io::ScanError::kTruncated: return ICLFORGE_ERROR_SCAN_TRUNCATED;
        case iclforge::io::ScanError::kUnsupportedStructure:
            return ICLFORGE_ERROR_SCAN_UNSUPPORTED_STRUCTURE;
    }
    return ICLFORGE_ERROR_INTERNAL;
}


// Every entry point in iclforge.h that can fail funnels through this: `body`
// returns iclforge_status_t on its own successful path (ICLFORGE_OK or an
// error this layer chose deliberately), and any C++ exception that escapes
// it - std::bad_alloc from an allocation this layer or the codec core makes,
// or anything else - is caught here instead of crossing into the caller's
// (possibly non-C++) frame, which is undefined behaviour. The codec core
// itself never throws (see iclforge::FrameError/DecodeError's std::expected
// convention), and every entry point validates its pointers and counts
// BEFORE calling guard() - in particular any count a body sizes a container
// from (reserve(n) throws std::length_error past max_size()) - so in practice
// only allocation failure should reach the catch clauses; the catch-all is
// the backstop that turns a missed check into ICLFORGE_ERROR_INTERNAL rather
// than into an exception crossing the C boundary.
template <class F>
[[nodiscard]] iclforge_status_t guard(F&& body) noexcept {
    try {
        return body();
    } catch (const std::bad_alloc&) {
        return ICLFORGE_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return ICLFORGE_ERROR_INTERNAL;
    }
}

}  // namespace iclforge_c

// --- opaque handle definitions ------------------------------------------
// Each wraps exactly one C++ value; construction only ever happens inside
// this library (via std::make_unique/new, released as a raw pointer through
// an out-parameter), so a handle's lifetime is entirely caller-driven from
// the matching _create/_destroy pair - the same convention every handle in
// iclforge.h documents.

struct iclforge_bytes {
    std::vector<std::byte> data;
};

struct iclforge_encoder {
    explicit iclforge_encoder(const iclforge::EncoderConfig& config) : impl(config) {}
    iclforge::FrameEncoder impl;
};

struct iclforge_decoder {
    explicit iclforge_decoder(const iclforge::DecoderConfig& config) : impl(config) {}
    iclforge::FrameDecoder impl;
};

struct iclforge_decoded_frame {
    iclforge::DecodedFrame data;
};

struct iclforge_eac3_decoder {
    explicit iclforge_eac3_decoder(const iclforge::DecoderConfig& config) : impl(config) {}
    iclforge::Eac3Decoder impl;
};

struct iclforge_decoded_substream {
    iclforge::DecodedSubstream data;
};

struct iclforge_decoded_access_unit {
    iclforge::DecodedAccessUnit data;
};

struct iclforge_atmos_encoder {
    iclforge_atmos_encoder(const iclforge::oba::AtmosConfig& config, int objects) : impl(config, objects) {}
    iclforge::oba::AtmosEncoder impl;
};

struct iclforge_eac3_encoder {
    explicit iclforge_eac3_encoder(const iclforge::eac3::FrameConfig& config) : impl(config) {}
    iclforge::eac3::FrameEncoder impl;
};

struct iclforge_eac3_access_unit_encoder {
    explicit iclforge_eac3_access_unit_encoder(const iclforge::eac3::AccessUnitConfig& config)
        : impl(config) {}
    iclforge::eac3::AccessUnitEncoder impl;
};

struct iclforge_eac3_access_unit {
    iclforge::eac3::AccessUnit data;
};

struct iclforge_spans {
    std::vector<iclforge_span_t> items;
};

struct iclforge_scanned_stream {
    iclforge::io::ScannedStream data;
    // ScannedStream::access_units/ScannedProgramme::access_units point into
    // the caller's own buffer (std::span<const std::byte>), exactly as
    // iclforge::split_frames()'s result does - see iclforge_spans above. Rather
    // than expose that pointer directly (which would tie this handle to a
    // std::byte* the header never otherwise names), iclforge_scan() converts
    // every one of them to an offset/length iclforge_span_t once, at scan
    // time, the same way split_into_spans() (eac3.cpp) already does for
    // iclforge_split_frames()/iclforge_split_access_units(). Parallel to
    // data.access_units and to each of data.programmes[i].access_units.
    std::vector<iclforge_span_t> access_units;
    std::vector<std::vector<iclforge_span_t>> programme_access_units;
};

struct iclforge_loudness_meter {
    // Neither iclforge::meta::LoudnessMeter constructor is default-constructible
    // (both need rate/acmod/lfe or rate/layout up front), so this holds one
    // built at create() time rather than embedding it by value the way
    // iclforge_encoder/iclforge_decoder do - matches iclforge::io::WavStreamReader's
    // own reason for the same shape (elementary.hpp).
    std::unique_ptr<iclforge::meta::LoudnessMeter> impl;
};

struct iclforge_level_meter {
    // Same reasoning as iclforge_loudness_meter above - LevelMeter is not
    // default-constructible either.
    std::unique_ptr<iclforge::analysis::LevelMeter> impl;
};


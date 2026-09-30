#pragma once

#include <stddef.h>
#include <stdint.h>

#include "iclforge_c/export.h"
#include "iclforge_c/version.h"

/* iclforge's C API — C API: a stable, minimal C-callable surface
 * over the encode/decode core, for bindings and embedding by callers that
 * cannot or do not want to link C++23.
 *
 * Conventions used throughout:
 *   - Every handle type (iclforge_encoder_t, iclforge_decoded_frame_t, ...)
 *     is opaque; only pointers to it ever cross this header. Each has a
 *     matching _destroy function. Passing NULL to a _destroy function is a
 *     no-op, matching free()'s own convention.
 *   - Every fallible function returns iclforge_status_t. ICLFORGE_OK is
 *     always zero, so `if (iclforge_xxx(...) != ICLFORGE_OK)` and
 *     `if (status)` are equally correct.
 *   - A function that produces a variable-length or structured result writes
 *     an owned handle through an out-parameter (the pointee is left
 *     untouched on failure); the caller reads it through accessor functions
 *     and then destroys it. Nothing is returned through raw caller-supplied
 *     buffers, so no accessor here requires the caller to predict a size in
 *     advance.
 *   - This library has no ABI-compatibility promise before v1.0 (see
 *     docs/library/api-stability.md): a rebuild against a newer iclforge may require a
 *     recompile, not merely a relink. iclforge_version() reports what was
 *     actually linked at runtime.
 *
 * See docs/library/c-api.md for a worked example and the full ownership
 * discussion; examples/capi_encode_decode.c is the same walkthrough as a
 * buildable program.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- *
 * Status / error codes
 * --------------------------------------------------------------------- */

/* Mirrors iclforge::FrameError (encode side) and iclforge::DecodeError (decode side)
 * one-for-one, plus a handful of codes this layer itself can raise. Grouped
 * with gaps between groups so a future addition to either C++ enum gets its
 * own number without renumbering anything already shipped. */
typedef enum iclforge_status {
    ICLFORGE_OK = 0,

    ICLFORGE_ERROR_INVALID_ARGUMENT = 1,
    ICLFORGE_ERROR_OUT_OF_MEMORY = 2,
    ICLFORGE_ERROR_INTERNAL = 3, /* an exception crossed the C boundary; see docs/library/c-api.md */
    /* A call this library was not built to answer - a codec this build left
     * out (ICLFORGE_BUILD_AC4 off), not a bad argument or a bad stream. Every
     * fallible entry point of that codec's section returns it, including
     * *_create() (the created-object out-parameter is left NULL); a function
     * that returns something else directly returns a NULL pointer, 0, or a
     * zero-initialized struct as its type allows. */
    ICLFORGE_ERROR_UNSUPPORTED = 4,

    /* iclforge::FrameError — FrameEncoder::encode_frame(), AtmosEncoder::encode_frame() */
    ICLFORGE_ERROR_ENCODE_INVALID_BITRATE = 10,
    ICLFORGE_ERROR_ENCODE_INVALID_DIALNORM = 11,
    ICLFORGE_ERROR_ENCODE_INVALID_SUBSTREAM = 12,
    ICLFORGE_ERROR_ENCODE_INVALID_CHANNEL_MAP = 13,
    ICLFORGE_ERROR_ENCODE_TOO_MANY_CHANNELS = 14,
    ICLFORGE_ERROR_ENCODE_INVALID_MIX_LEVEL = 15,
    ICLFORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO = 16,
    ICLFORGE_ERROR_ENCODE_INVALID_BSI = 17,

    /* iclforge::DecodeError — FrameDecoder::decode_frame(), Eac3Decoder::decode_substream()/decode_access_unit() */
    ICLFORGE_ERROR_DECODE_TRUNCATED = 30,
    ICLFORGE_ERROR_DECODE_BAD_SYNC_WORD = 31,
    ICLFORGE_ERROR_DECODE_BAD_CRC = 32,
    ICLFORGE_ERROR_DECODE_RESERVED_VALUE = 33,
    ICLFORGE_ERROR_DECODE_UNSUPPORTED = 34,
    ICLFORGE_ERROR_DECODE_INVALID_STREAM = 35,

    /* iclforge::io::ScanError — iclforge_scan() only; split_frames()/split_access_units()/
     * stream_bsid() stay on iclforge::DecodeError above, since iclforge::io::scan() is the
     * only entry point in this header built on iclforge::io's own error type. */
    ICLFORGE_ERROR_SCAN_EMPTY = 50,
    ICLFORGE_ERROR_SCAN_LOST_SYNC = 51,
    ICLFORGE_ERROR_SCAN_UNSUPPORTED_BSID = 52,
    ICLFORGE_ERROR_SCAN_RESERVED_VALUE = 53,
    ICLFORGE_ERROR_SCAN_TRUNCATED = 54,
    ICLFORGE_ERROR_SCAN_UNSUPPORTED_STRUCTURE = 55,

    /* iclforge::ac4::DecodeError - iclforge::ac4::Decoder::parse()/decode() */
    ICLFORGE_ERROR_AC4_DECODE_TRUNCATED = 60,
    ICLFORGE_ERROR_AC4_DECODE_INVALID_TOC = 61,
    ICLFORGE_ERROR_AC4_DECODE_INVALID_STREAM = 62,
    ICLFORGE_ERROR_AC4_DECODE_UNSUPPORTED = 63,
    ICLFORGE_ERROR_AC4_DECODE_MISSING_IFRAME = 64,

    /* iclforge::ac4::EncodeError - iclforge::ac4::Encoder::create()/encode()/flush() */
    ICLFORGE_ERROR_AC4_ENCODE_INVALID_CONFIG = 80,
    ICLFORGE_ERROR_AC4_ENCODE_INVALID_INPUT = 81
} iclforge_status_t;

/* A short, static, human-readable description of `status` — e.g. for a log
 * line. The returned pointer is to library-owned storage valid for the
 * process lifetime; never free() it. */
ICLFORGE_C_EXPORT const char* iclforge_status_message(iclforge_status_t status);

/* --------------------------------------------------------------------- *
 * Version
 * --------------------------------------------------------------------- */

/* ICLFORGE_C_VERSION_MAJOR/MINOR/PATCH and ICLFORGE_C_VERSION (iclforge_c/version.h,
 * included above) are the SDK version this translation unit compiled against, available
 * to #if. iclforge_version() below is the complementary runtime check: what actually got
 * linked, which before v1.0 can differ from what a caller built against. */

/* Tagged iclforge_version_info rather than iclforge_version: GCC's -Wshadow
 * (built C++) treats a struct tag and a same-named free function as the
 * function "hiding" the tag's implicit constructor-like name. The typedef
 * name below is what callers actually use. */
typedef struct iclforge_version_info {
    int major;
    int minor;
    int patch;
    /* Semver plus prerelease suffix (e.g. "0.8.0-beta.1"), library-owned
     * storage valid for the process lifetime — mirrors iclforge::version_full. */
    const char* full;
} iclforge_version_t;

ICLFORGE_C_EXPORT iclforge_version_t iclforge_version(void);

/* --------------------------------------------------------------------- *
 * Shared enums (iclforge::SampleRate, iclforge::Acmod)
 * --------------------------------------------------------------------- */

/* Ordinals match iclforge::SampleRate exactly (A/52 Table 5.6 fscod, plus the
 * three Annex E fscod2 reduced rates). */
typedef enum iclforge_sample_rate {
    ICLFORGE_SAMPLE_RATE_48000 = 0,
    ICLFORGE_SAMPLE_RATE_44100 = 1,
    ICLFORGE_SAMPLE_RATE_32000 = 2,
    ICLFORGE_SAMPLE_RATE_24000 = 3, /* E-AC-3 only */
    ICLFORGE_SAMPLE_RATE_22050 = 4, /* E-AC-3 only */
    ICLFORGE_SAMPLE_RATE_16000 = 5  /* E-AC-3 only */
} iclforge_sample_rate_t;

/* Ordinals match iclforge::Acmod exactly (A/52 Table 5.8). kDualMono (0) is 1+1:
 * two independent programmes sharing one syncframe, not a channel count. */
typedef enum iclforge_acmod {
    ICLFORGE_ACMOD_DUAL_MONO = 0, /* 1+1: Ch1, Ch2 */
    ICLFORGE_ACMOD_1_0 = 1,       /* C */
    ICLFORGE_ACMOD_2_0 = 2,       /* L, R */
    ICLFORGE_ACMOD_3_0 = 3,       /* L, C, R */
    ICLFORGE_ACMOD_2_1 = 4,       /* L, R, S */
    ICLFORGE_ACMOD_3_1 = 5,       /* L, C, R, S */
    ICLFORGE_ACMOD_2_2 = 6,       /* L, R, SL, SR */
    ICLFORGE_ACMOD_3_2 = 7        /* L, C, R, SL, SR */
} iclforge_acmod_t;

/* One audio block is always 256 samples (A/52 §4.1); one syncframe is always
 * six blocks. Exposed as constants because encode_frame()'s channel spans
 * and decoded-frame accessors are both sized against them. */
#define ICLFORGE_SAMPLES_PER_BLOCK 256
#define ICLFORGE_BLOCKS_PER_FRAME 6
#define ICLFORGE_SAMPLES_PER_FRAME 1536

/* Every AC-3 layout codes at most 5 full-bandwidth channels plus LFE - the
 * span count iclforge_decoder_decode_frame_into() always requires,
 * regardless of what a given frame actually codes, since that is not known
 * until the frame's own header is parsed. */
#define ICLFORGE_DECODER_MAX_CHANNELS 6

/* --------------------------------------------------------------------- *
 * Latency (bare-metal probe harness)
 * --------------------------------------------------------------------- */

/* Mirrors iclforge::LatencyBudget: the ALGORITHMIC delay of an encode -> decode
 * chain, in samples at the coded sample rate. Compute time is a separate
 * question (docs/performance-trend.md); transport, device buffers and
 * resampling are the integrator's own to add.
 *
 *   frame_samples      Input granularity. Nothing leaves the encoder until a
 *                      whole frame has gone in.
 *   transform_samples  The MDCT/IMDCT overlap. This is the one term that is a
 *                      sample-domain SHIFT: decoded output sample k is input
 *                      sample k - transform_samples. ICLFORGE_SAMPLES_PER_BLOCK
 *                      for AC-3 and E-AC-3; twice that for an Atmos OBJECT
 *                      waveform, whose reconstruction re-transforms the
 *                      already-decoded bed.
 *   lookahead_samples  Input the encoder needs beyond the frame it is coding.
 *                      Zero throughout this library.
 *   holdback_samples   The E-AC-3 §3.7 transient-pre-noise hold-back: a
 *                      decoder returns frame N-1's PCM from the call that
 *                      supplies frame N. One frame period, or zero.
 *
 * See docs/library/encoding-ac3.md's Latency section for the measured
 * numbers and tests/ac3/decoder/test_latency.cpp for how they were measured. */
typedef struct iclforge_latency {
    int frame_samples;
    int transform_samples;
    int lookahead_samples;
    int holdback_samples;
} iclforge_latency_t;

/* The figure to budget with: the sum of the four terms above. No sample
 * entering the encoder is delayed by more than this many samples before the
 * matching decoded sample leaves the decoder. NULL returns 0. */
ICLFORGE_C_EXPORT int iclforge_latency_total_samples(const iclforge_latency_t* latency);

/* Milliseconds for a sample count at a coded rate — e.g. 1792 samples at
 * ICLFORGE_SAMPLE_RATE_48000 is 37.33 ms. */
ICLFORGE_C_EXPORT double iclforge_latency_ms(int samples, iclforge_sample_rate_t sample_rate);

/* Table 5.9 (§5.4.2.4) / Table 5.10 (§5.4.2.5). */
typedef enum iclforge_centre_mix_level {
    ICLFORGE_CMIXLEV_MINUS_3DB = 0,
    ICLFORGE_CMIXLEV_MINUS_4_5DB = 1,
    ICLFORGE_CMIXLEV_MINUS_6DB = 2
} iclforge_centre_mix_level_t;

typedef enum iclforge_surround_mix_level {
    ICLFORGE_SURMIXLEV_MINUS_3DB = 0,
    ICLFORGE_SURMIXLEV_MINUS_6DB = 1,
    ICLFORGE_SURMIXLEV_SILENT = 2
} iclforge_surround_mix_level_t;

/* The five conventional Dolby DRC curves (iclforge::meta::ProfileId) — the same
 * named presets ac3cli's own --drc flag accepts. The full custom
 * iclforge::meta::Profile curve (attack/release timing, boost ratios, ...) is an
 * internal tuning knob, not part of this minimal stable surface. */
typedef enum iclforge_drc_profile {
    ICLFORGE_DRC_FILM_STANDARD = 0,
    ICLFORGE_DRC_FILM_LIGHT = 1,
    ICLFORGE_DRC_MUSIC_STANDARD = 2,
    ICLFORGE_DRC_MUSIC_LIGHT = 3,
    ICLFORGE_DRC_SPEECH = 4
} iclforge_drc_profile_t;

/* iclforge::meta::HeavyConfig verbatim (§7.7.2) — small enough, and specific
 * enough per-field, to expose directly rather than behind a preset. */
typedef struct iclforge_heavy_config {
    double dialogue_target_dbfs; /* default -20.0 */
    double peak_ceiling_dbfs;    /* default -0.5 */
    double release_db_per_second; /* default 20.0 */
} iclforge_heavy_config_t;

ICLFORGE_C_EXPORT void iclforge_heavy_config_init(iclforge_heavy_config_t* config);

/* --------------------------------------------------------------------- *
 * AC-3 encoder (iclforge::FrameEncoder)
 * --------------------------------------------------------------------- */

typedef struct iclforge_encoder iclforge_encoder_t;

/* Mirrors iclforge::EncoderConfig. `has_*` flags stand in for std::optional<T>,
 * since C has no direct equivalent — the paired field is read only when its
 * flag is non-zero. Call iclforge_encoder_config_init() first so every field
 * this struct doesn't set explicitly carries the same default EncoderConfig{}
 * does; a zero-initialized struct is NOT equivalent (e.g. dialnorm 0 is
 * invalid — §5.4.2.8 reserves it — where EncoderConfig's real default is 31). */
typedef struct iclforge_encoder_config {
    iclforge_sample_rate_t sample_rate;
    uint32_t bitrate_kbps;
    int dialnorm; /* 1..31, §5.4.2.8 */

    int has_dialnorm2; /* dual mono (acmod == ICLFORGE_ACMOD_DUAL_MONO) only */
    int dialnorm2;

    int chbwcod; /* 0..60, or -1 for auto-from-bitrate; above 60 fails _create */
    iclforge_acmod_t acmod;
    int lfe;
    int coupling;
    int cplbegf; /* -1 = auto */
    int cplendf; /* -1 = auto */
    int fast_mdct;

    int has_drc;
    iclforge_drc_profile_t drc_profile;
    int has_heavy;
    iclforge_heavy_config_t heavy;

    /* Ch2's own DRC/heavy — dual mono only, no fallback to the above. */
    int has_drc2;
    iclforge_drc_profile_t drc2_profile;
    int has_heavy2;
    iclforge_heavy_config_t heavy2;

    iclforge_centre_mix_level_t cmixlev;
    iclforge_surround_mix_level_t surmixlev;
} iclforge_encoder_config_t;

/* Fills `config` with the same defaults as iclforge::EncoderConfig{}. */
ICLFORGE_C_EXPORT void iclforge_encoder_config_init(iclforge_encoder_config_t* config);

ICLFORGE_C_EXPORT iclforge_status_t iclforge_encoder_create(const iclforge_encoder_config_t* config,
                                                         iclforge_encoder_t** out_encoder);
ICLFORGE_C_EXPORT void iclforge_encoder_destroy(iclforge_encoder_t* encoder);

/* Full-bandwidth channels (per config.acmod) plus, when config.lfe is set,
 * the LFE channel last — the same count encode_frame() below expects. */
ICLFORGE_C_EXPORT size_t iclforge_encoder_channel_count(const iclforge_encoder_t* encoder);

/* This encoder's latency budget. Constant for the encoder's whole life: no
 * field of iclforge_encoder_config_t moves any term. `out_latency` is left
 * untouched when either pointer is NULL. */
ICLFORGE_C_EXPORT void iclforge_encoder_latency(const iclforge_encoder_t* encoder,
                                              iclforge_latency_t* out_latency);

/* The same budget's total — the single number an engine or conferencing
 * integrator asks for. 1792 samples (37.33 ms at 48 kHz) for every AC-3
 * configuration. NULL returns 0. */
ICLFORGE_C_EXPORT int iclforge_encoder_latency_samples(const iclforge_encoder_t* encoder);

/* An owned, immutable byte buffer — the result type for every function here
 * that produces one encoded frame's worth of bytes. */
typedef struct iclforge_bytes iclforge_bytes_t;

ICLFORGE_C_EXPORT const uint8_t* iclforge_bytes_data(const iclforge_bytes_t* bytes);
ICLFORGE_C_EXPORT size_t iclforge_bytes_size(const iclforge_bytes_t* bytes);
ICLFORGE_C_EXPORT void iclforge_bytes_destroy(iclforge_bytes_t* bytes);

/* channels: `channel_count` pointers (must equal
 * iclforge_encoder_channel_count(encoder)), each to exactly
 * ICLFORGE_SAMPLES_PER_FRAME samples nominally in [-1, 1), in AC-3 channel
 * order (Table 5.8) with LFE last. On success, *out_frame receives one
 * complete syncframe; the caller must destroy it. *out_frame is left
 * untouched on failure. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_encoder_encode_frame(iclforge_encoder_t* encoder,
                                                               const float* const* channels,
                                                               size_t channel_count,
                                                               size_t samples_per_channel,
                                                               iclforge_bytes_t** out_frame);

/* --------------------------------------------------------------------- *
 * AC-3 decoder (iclforge::FrameDecoder)
 * --------------------------------------------------------------------- */

typedef struct iclforge_decoder iclforge_decoder_t;

/* Mirrors iclforge::DecoderConfig. */
typedef struct iclforge_decoder_config {
    double drc_scale;      /* 0.0..1.0, default 0.0 (§7.7.1's "Partial Compression") */
    int heavy_compression; /* default 0 */
} iclforge_decoder_config_t;

ICLFORGE_C_EXPORT void iclforge_decoder_config_init(iclforge_decoder_config_t* config);

ICLFORGE_C_EXPORT iclforge_status_t iclforge_decoder_create(const iclforge_decoder_config_t* config,
                                                         iclforge_decoder_t** out_decoder);
ICLFORGE_C_EXPORT void iclforge_decoder_destroy(iclforge_decoder_t* decoder);

/* The delay THIS decoder adds on top of the encoder's own budget. Always 0
 * for AC-3: decode_frame returns a frame's full PCM from the call that
 * supplies its bytes, and the IMDCT overlap those samples came from is
 * already the chain's transform term. Present so "encoder plus decoder" is a
 * sum a caller can write. */
ICLFORGE_C_EXPORT int iclforge_decoder_latency_samples(const iclforge_decoder_t* decoder);

/* One decoded syncframe (iclforge::DecodedFrame), read through the accessors
 * below. */
typedef struct iclforge_decoded_frame iclforge_decoded_frame_t;

/* `frame` must be exactly one syncframe, same precondition as
 * FrameDecoder::decode_frame(). On success, *out_frame receives the decode
 * result; the caller must destroy it. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_decoder_decode_frame(iclforge_decoder_t* decoder,
                                                               const uint8_t* frame,
                                                               size_t frame_size,
                                                               iclforge_decoded_frame_t** out_frame);

/* As iclforge_decoder_decode_frame, but the PCM lands in caller-owned planar
 * storage instead of an allocation this call would otherwise own - the C
 * mirror of FrameDecoder::decode_frame_into(), for a realtime embedder that
 * cannot allocate on the decode path. channels: exactly
 * ICLFORGE_DECODER_MAX_CHANNELS pointers, each exactly
 * ICLFORGE_SAMPLES_PER_FRAME samples - always six spans, regardless of what
 * this particular frame codes, since that is only known once its header is
 * parsed; a trailing span this frame's acmod/lfe do not need is left
 * untouched. On success, *out_frame carries every field decode_frame()
 * would EXCEPT the PCM itself (its channel_count() reports 0 - the samples
 * went to the caller's spans instead, in AC-3 channel order with LFE last).
 * On an error return the spans' contents are unspecified, exactly as
 * discarded as the value form's partial frame is. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_decoder_decode_frame_into(
    iclforge_decoder_t* decoder, const uint8_t* frame, size_t frame_size, float* const* channels,
    size_t channel_count, size_t samples_per_channel, iclforge_decoded_frame_t** out_frame);

ICLFORGE_C_EXPORT iclforge_sample_rate_t iclforge_decoded_frame_sample_rate(
    const iclforge_decoded_frame_t* frame);
ICLFORGE_C_EXPORT uint32_t iclforge_decoded_frame_bitrate_kbps(const iclforge_decoded_frame_t* frame);
ICLFORGE_C_EXPORT iclforge_acmod_t iclforge_decoded_frame_acmod(const iclforge_decoded_frame_t* frame);
ICLFORGE_C_EXPORT int iclforge_decoded_frame_lfe(const iclforge_decoded_frame_t* frame);
ICLFORGE_C_EXPORT int iclforge_decoded_frame_dialnorm(const iclforge_decoded_frame_t* frame);

ICLFORGE_C_EXPORT int iclforge_decoded_frame_has_compr(const iclforge_decoded_frame_t* frame);
ICLFORGE_C_EXPORT uint8_t iclforge_decoded_frame_compr(const iclforge_decoded_frame_t* frame);
/* block_index in [0, ICLFORGE_BLOCKS_PER_FRAME). */
ICLFORGE_C_EXPORT uint8_t iclforge_decoded_frame_dynrng(const iclforge_decoded_frame_t* frame,
                                                     int block_index);

/* Ch2's words — meaningful only when acmod() == ICLFORGE_ACMOD_DUAL_MONO. */
ICLFORGE_C_EXPORT int iclforge_decoded_frame_has_dialnorm2(const iclforge_decoded_frame_t* frame);
ICLFORGE_C_EXPORT int iclforge_decoded_frame_dialnorm2(const iclforge_decoded_frame_t* frame);
ICLFORGE_C_EXPORT int iclforge_decoded_frame_has_compr2(const iclforge_decoded_frame_t* frame);
ICLFORGE_C_EXPORT uint8_t iclforge_decoded_frame_compr2(const iclforge_decoded_frame_t* frame);
ICLFORGE_C_EXPORT uint8_t iclforge_decoded_frame_dynrng2(const iclforge_decoded_frame_t* frame,
                                                      int block_index);

ICLFORGE_C_EXPORT size_t iclforge_decoded_frame_channel_count(const iclforge_decoded_frame_t* frame);
/* Always ICLFORGE_SAMPLES_PER_FRAME; exposed for a caller that would rather
 * not depend on the macro. */
ICLFORGE_C_EXPORT size_t iclforge_decoded_frame_samples_per_channel(
    const iclforge_decoded_frame_t* frame);
/* channel_index in [0, channel_count()), AC-3 coded order (Table 5.8), LFE
 * last when lfe() is set. The returned pointer is valid until `frame` is
 * destroyed. */
ICLFORGE_C_EXPORT const float* iclforge_decoded_frame_channel_samples(
    const iclforge_decoded_frame_t* frame, size_t channel_index);
/* True where that block used the short (block-switched) transform.
 * channel_index only ranges over the full-bandwidth channels (no LFE/coupling
 * entry — see DecodedFrame::blksw); out of range returns 0. */
ICLFORGE_C_EXPORT int iclforge_decoded_frame_block_switched(const iclforge_decoded_frame_t* frame,
                                                         size_t channel_index, int block_index);

ICLFORGE_C_EXPORT void iclforge_decoded_frame_destroy(iclforge_decoded_frame_t* frame);

/* --------------------------------------------------------------------- *
 * E-AC-3 encoder (iclforge::eac3::FrameEncoder / AccessUnitEncoder)
 * --------------------------------------------------------------------- */

/* Mirrors iclforge::eac3::StreamType (Table E1.2, §E2.3.1.2). This encoder only
 * ever emits kIndependent/kDependent; kConvertible/kReserved are accepted
 * here for a faithful mirror but never produced by anything below, the same
 * way iclforge::eac3::FrameEncoder's own validate() refuses them. */
typedef enum iclforge_stream_type {
    ICLFORGE_STREAM_TYPE_INDEPENDENT = 0,
    ICLFORGE_STREAM_TYPE_DEPENDENT = 1,
    ICLFORGE_STREAM_TYPE_CONVERTIBLE = 2,
    ICLFORGE_STREAM_TYPE_RESERVED = 3
} iclforge_stream_type_t;

typedef struct iclforge_eac3_encoder iclforge_eac3_encoder_t;
typedef struct iclforge_eac3_access_unit_encoder iclforge_eac3_access_unit_encoder_t;

/* Mirrors iclforge::eac3::FrameConfig's core surface - the fields needed to
 * produce a real E-AC-3 substream. `has_*` flags stand in for
 * std::optional<T>, same convention as iclforge_encoder_config_t. Not
 * mirrored here: the mixmdate/infomdat metadata groups, dialnorm2/drc/heavy
 * (dual mono and DRC are exactly as useful on this side as on AC-3's, but
 * the broader Table E1.2 metadata surface is deliberately deferred - see
 * docs/library/c-api.md's "What is deliberately out of scope"), vbr and
 * numblkscod (CBR, six-block syncframes only), and the internal self-check
 * `trace` hook. Call iclforge_eac3_frame_config_init() first so every field
 * this struct doesn't set explicitly carries the same default FrameConfig{}
 * does. */
typedef struct iclforge_eac3_frame_config {
    iclforge_sample_rate_t sample_rate; /* includes the 3 fscod2 reduced rates */
    uint32_t bitrate_kbps;
    int dialnorm; /* 1..31, §5.4.2.8 */
    iclforge_acmod_t acmod;
    int lfe;

    /* --- Annex E coding tools ------------------------------------------- */
    /* Hands the whole tool set (coupling/spx/aht below) to the encoder,
     * chosen from the per-channel rate and the frame's own content instead of
     * the flags below - it overrides them rather than combining with them
     * (see docs/library/encoding-eac3.md's "How auto chooses"). cplbegf/
     * spxbegf/gaqmod still steer the geometry of whatever it turns on. */
    int auto_tools;
    int coupling; /* §E3.3 */
    int cplbegf;  /* -1 = auto */
    int enhanced; /* §E3.5 enhanced coupling; only meaningful with coupling */
    int spx;      /* §E3.6 spectral extension */
    int spxbegf;  /* -1 = auto */
    int spx_atten;
    int spxattencod;        /* -1 = auto */
    int aht;                /* §E3.4 adaptive hybrid transform */
    int gaqmod;              /* -1 = auto, 0..3 otherwise */
    int transient_prenoise; /* §3.7; the only tool that adds decoder hold-back */
    int fast_mdct;

    /* --- substream identity (Table E1.2) -------------------------------- *
     * Meaningful when building a multi-substream access unit by hand out of
     * several iclforge_eac3_encoder_t instances.
     * iclforge_eac3_access_unit_encoder_t assigns these itself the way
     * iclforge::eac3::AccessUnitEncoder does, and does not read them from the
     * configs passed to it (see iclforge::eac3::AccessUnitConfig's own comment) -
     * only chanmap/has_chanmap on a dependent matters there. */
    iclforge_stream_type_t strmtyp;
    int substreamid;
    int has_chanmap; /* dependent substreams only */
    uint16_t chanmap; /* Table E2.5 bitmask; ICLFORGE_CHANMAP_* below name a few */
} iclforge_eac3_frame_config_t;

ICLFORGE_C_EXPORT void iclforge_eac3_frame_config_init(iclforge_eac3_frame_config_t* config);

/* A few of Table E2.5's chanmap combinations, matching
 * iclforge::eac3::chanmap::k71Rear/k512Height/kTopQuad - what a dependent
 * substream needs to widen a 5.1 bed. See docs/library/encoding-eac3.md's
 * "Wide layouts" table. */
#define ICLFORGE_CHANMAP_71_REAR 0x1A00u    /* Ls, Rs, Lrs, Rrs -> 7.1 */
#define ICLFORGE_CHANMAP_512_HEIGHT 0x0010u /* Vhl, Vhr -> 5.1.2 */
/* Vhl, Vhr, Lts, Rts -> 5.1.4 (or 7.1.4 with 71_REAR in a second dependent) */
#define ICLFORGE_CHANMAP_TOP_QUAD 0x0014u

/* Mirrors iclforge::eac3::FrameMetadata - the §7.7 words for one frame, shared
 * across every substream of one programme by
 * iclforge_eac3_access_unit_encoder_encode() so they never disagree (see
 * iclforge::eac3::FrameEncoder's own comment on why "measured per substream" and
 * "shared across substreams" give different answers). */
typedef struct iclforge_eac3_frame_metadata {
    uint8_t dynrng[ICLFORGE_BLOCKS_PER_FRAME];
    int has_compr;
    uint8_t compr;
    /* Ch2's own words, meaningful only when acmod is ICLFORGE_ACMOD_DUAL_MONO. */
    uint8_t dynrng2[ICLFORGE_BLOCKS_PER_FRAME];
    int has_compr2;
    uint8_t compr2;
} iclforge_eac3_frame_metadata_t;

/* All-zero dynrng (§7.7.1's "no change"), no compr - same defaults an
 * all-zero-initialized struct would already have, provided for symmetry with
 * every other _init() function here. */
ICLFORGE_C_EXPORT void iclforge_eac3_frame_metadata_init(iclforge_eac3_frame_metadata_t* metadata);

ICLFORGE_C_EXPORT iclforge_status_t iclforge_eac3_encoder_create(
    const iclforge_eac3_frame_config_t* config, iclforge_eac3_encoder_t** out_encoder);
ICLFORGE_C_EXPORT void iclforge_eac3_encoder_destroy(iclforge_eac3_encoder_t* encoder);

/* Full-bandwidth channels (per config.acmod) plus, when config.lfe is set,
 * the LFE channel last - the same count encode_frame() below expects. */
ICLFORGE_C_EXPORT size_t iclforge_eac3_encoder_channel_count(const iclforge_eac3_encoder_t* encoder);
/* Always ICLFORGE_SAMPLES_PER_FRAME (numblkscod is not exposed above,
 * so every substream this API builds carries six blocks); exposed as its own
 * accessor rather than assumed so a caller never has to special-case this
 * encoder against the AC-3 one - iclforge::eac3::FrameEncoder::samples_per_frame()
 * varies once a caller reaches numblkscod, which nothing here can ask
 * for. */
ICLFORGE_C_EXPORT size_t iclforge_eac3_encoder_samples_per_frame(
    const iclforge_eac3_encoder_t* encoder);

ICLFORGE_C_EXPORT void iclforge_eac3_encoder_latency(const iclforge_eac3_encoder_t* encoder,
                                                   iclforge_latency_t* out_latency);
ICLFORGE_C_EXPORT int iclforge_eac3_encoder_latency_samples(const iclforge_eac3_encoder_t* encoder);

/* channels: `channel_count` pointers (must equal
 * iclforge_eac3_encoder_channel_count(encoder)), each to exactly
 * iclforge_eac3_encoder_samples_per_frame(encoder) samples nominally in
 * [-1, 1), in AC-3 channel order (Table 5.8) with LFE last. `metadata`, when
 * non-NULL, supplies the §7.7 words explicitly (iclforge::eac3::FrameEncoder's
 * second encode_frame() overload) instead of measuring them from `channels`
 * - the access-unit path needs this so every substream of one programme
 * agrees; NULL measures internally, matching a standalone stream. `aux`/
 * `aux_size` carry a caller-built EMDF container (iclforge::emdf::build_container)
 * in the frame's aux data, or NULL/0 for none - at most 511 bytes (it rides
 * block 0's skip field, whose skipl is 9 bits); a larger payload fails with
 * ICLFORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO. On success, *out_frame
 * receives one complete syncframe; the caller must destroy it. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_eac3_encoder_encode_frame(
    iclforge_eac3_encoder_t* encoder, const float* const* channels, size_t channel_count,
    size_t samples_per_channel, const iclforge_eac3_frame_metadata_t* metadata,
    const uint8_t* aux, size_t aux_size, iclforge_bytes_t** out_frame);

/* --- wide layouts: iclforge::eac3::AccessUnitEncoder ------------------------- */

/* An access unit's bytes plus per-substream boundaries - mirrors
 * iclforge::eac3::AccessUnit. Unlike iclforge_atmos_encoder_encode_frame() (which
 * always produces exactly one substream and so returns a plain
 * iclforge_bytes_t), a general access-unit encoder can produce several, and a
 * caller re-deriving crc2 or demuxing substreams individually needs to know
 * where each one starts. */
typedef struct iclforge_eac3_access_unit iclforge_eac3_access_unit_t;

ICLFORGE_C_EXPORT const uint8_t* iclforge_eac3_access_unit_data(
    const iclforge_eac3_access_unit_t* unit);
ICLFORGE_C_EXPORT size_t iclforge_eac3_access_unit_size(const iclforge_eac3_access_unit_t* unit);
ICLFORGE_C_EXPORT size_t iclforge_eac3_access_unit_substream_count(
    const iclforge_eac3_access_unit_t* unit);
/* Byte length of substream `index` (independent first); sums to
 * iclforge_eac3_access_unit_size(). */
ICLFORGE_C_EXPORT uint32_t iclforge_eac3_access_unit_substream_bytes(
    const iclforge_eac3_access_unit_t* unit, size_t index);
ICLFORGE_C_EXPORT void iclforge_eac3_access_unit_destroy(iclforge_eac3_access_unit_t* unit);

/* `independent` is the bed's config; `dependents`/`dependent_count` are the
 * substreams that widen it (at most 8 - a larger count fails with
 * ICLFORGE_ERROR_INVALID_ARGUMENT), in transmission order - see
 * iclforge::eac3::AccessUnitConfig. Every substream must agree on sample_rate;
 * strmtyp/substreamid on `independent` and each of `dependents` are assigned
 * by this call the way iclforge::eac3::AccessUnitEncoder's constructor does, so
 * whatever the caller set there is not read - only chanmap/has_chanmap on a
 * dependent matters (Table E2.5; ICLFORGE_CHANMAP_* above name a few
 * combinations). */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_eac3_access_unit_encoder_create(
    const iclforge_eac3_frame_config_t* independent, const iclforge_eac3_frame_config_t* dependents,
    size_t dependent_count, iclforge_eac3_access_unit_encoder_t** out_encoder);
ICLFORGE_C_EXPORT void iclforge_eac3_access_unit_encoder_destroy(
    iclforge_eac3_access_unit_encoder_t* encoder);

/* Summed across every substream - the span count encode() below expects. */
ICLFORGE_C_EXPORT size_t iclforge_eac3_access_unit_encoder_channel_count(
    const iclforge_eac3_access_unit_encoder_t* encoder);

ICLFORGE_C_EXPORT void iclforge_eac3_access_unit_encoder_latency(
    const iclforge_eac3_access_unit_encoder_t* encoder, iclforge_latency_t* out_latency);
ICLFORGE_C_EXPORT int iclforge_eac3_access_unit_encoder_latency_samples(
    const iclforge_eac3_access_unit_encoder_t* encoder);

/* channels: every channel of the access unit grouped by substream in
 * transmission order - the independent's first (AC-3 order, LFE last), then
 * each dependent's in the order its chanmap names them - channel_count()
 * spans total, each ICLFORGE_SAMPLES_PER_FRAME samples. NULL is accepted when
 * channel_count is 0, which happens when `independent`/`dependents` described
 * a config iclforge::eac3::AccessUnitEncoder's constructor could not build any
 * substreams from (see iclforge_eac3_access_unit_encoder_create()'s own
 * comment) - calling this then reports the real reason as a status code
 * rather than silently producing nothing. `aux`/`aux_size`: see
 * iclforge_eac3_encoder_encode_frame(). On success, *out_unit receives the
 * whole access unit; the caller must destroy it. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_eac3_access_unit_encoder_encode(
    iclforge_eac3_access_unit_encoder_t* encoder, const float* const* channels,
    size_t channel_count, size_t samples_per_channel, const uint8_t* aux, size_t aux_size,
    iclforge_eac3_access_unit_t** out_unit);

/* --------------------------------------------------------------------- *
 * E-AC-3 / Atmos decode (iclforge::Eac3Decoder)
 * --------------------------------------------------------------------- */

typedef struct iclforge_eac3_decoder iclforge_eac3_decoder_t;

/* Table E2.5 caps one rendered programme (bed plus every dependent) at 16
 * channels (§E3.8.2) - the span count
 * iclforge_eac3_decoder_decode_access_unit_into() always requires, for the
 * same "not known until parsed" reason as ICLFORGE_DECODER_MAX_CHANNELS. */
#define ICLFORGE_EAC3_DECODER_MAX_CHANNELS 16

ICLFORGE_C_EXPORT iclforge_status_t iclforge_eac3_decoder_create(
    const iclforge_decoder_config_t* config, iclforge_eac3_decoder_t** out_decoder);
ICLFORGE_C_EXPORT void iclforge_eac3_decoder_destroy(iclforge_eac3_decoder_t* decoder);

/* The delay THIS decoder adds, same contract as
 * iclforge_decoder_latency_samples(). 0 until some substream's frame sets
 * transproce, ICLFORGE_SAMPLES_PER_FRAME from then on — §3.7's hold-back is a
 * property of the stream, not of the decoder, and once engaged it stays
 * engaged. A caller sizing buffers BEFORE the stream starts should ask the
 * encoder instead; this reports what has actually happened so far. */
ICLFORGE_C_EXPORT int iclforge_eac3_decoder_latency_samples(
    const iclforge_eac3_decoder_t* decoder);

typedef struct iclforge_decoded_substream iclforge_decoded_substream_t;
typedef struct iclforge_decoded_access_unit iclforge_decoded_access_unit_t;

/* Same std::nullopt-via-out-parameter convention as the C++ API: a return of
 * ICLFORGE_OK with *out_substream left NULL means this frame's PCM is being
 * held back pending transient pre-noise processing (§3.7) — not an error.
 * Call iclforge_eac3_decoder_flush() at end of stream to collect it. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_eac3_decoder_decode_substream(
    iclforge_eac3_decoder_t* decoder, const uint8_t* frame, size_t frame_size,
    iclforge_decoded_substream_t** out_substream);

/* Same held-back convention as decode_substream, for the same reason (see
 * iclforge::Eac3Decoder::decode_access_unit's own comment). `unit` must be
 * delimited exactly as iclforge_split_access_units() would delimit it. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_eac3_decoder_decode_access_unit(
    iclforge_eac3_decoder_t* decoder, const uint8_t* unit, size_t unit_size,
    iclforge_decoded_access_unit_t** out_unit);

/* As iclforge_eac3_decoder_decode_access_unit, but the rendered programme's
 * PCM lands in caller-owned planar storage - the C mirror of
 * Eac3Decoder::decode_access_unit_into(). channels: exactly
 * ICLFORGE_EAC3_DECODER_MAX_CHANNELS pointers, each exactly
 * ICLFORGE_SAMPLES_PER_FRAME samples, written in the rendered layout's own
 * slot order (coded order for dual mono); a trailing span this programme
 * does not render is left untouched. Same held-back convention as the value
 * form: ICLFORGE_OK with *out_unit left NULL means the §3.7 hold-back - and
 * the spans are left untouched for that call too, not partially written,
 * because a held-back frame's PCM is buffered internally either way and
 * only copied out (to the caller's spans this time) at the call that
 * releases it. iclforge_eac3_decoder_flush() is still the release path at
 * end of stream and still returns library-owned data even for a decoder
 * driven entirely through this form - there is no flush_into, since flush's
 * own per-substream results were never assembled into one programme to
 * begin with (see its own comment). On an error return the spans' contents
 * are unspecified. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_eac3_decoder_decode_access_unit_into(
    iclforge_eac3_decoder_t* decoder, const uint8_t* unit, size_t unit_size,
    float* const* channels, size_t channel_count, size_t samples_per_channel,
    iclforge_decoded_access_unit_t** out_unit);

/* Releases whichever frames transient pre-noise processing is still holding
 * back. *out_substreams receives a library-owned array of *out_count owned
 * handles; *out_count is 0 (and *out_substreams NULL) for a stream that never
 * used the tool. Release it with
 * iclforge_decoded_substream_array_destroy(*out_substreams, *out_count), which
 * destroys every element AND the array - do not also destroy the elements
 * individually. array_destroy destroys only the non-NULL elements in
 * [0, count) and always frees the array itself, so a caller that keeps a
 * handle beyond the array's lifetime (and later destroys it itself with
 * iclforge_decoded_substream_destroy()) either sets its slot to NULL first or,
 * having taken every handle, passes a count of 0 to free just the array.
 * A NULL array is a no-op. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_eac3_decoder_flush(
    iclforge_eac3_decoder_t* decoder, iclforge_decoded_substream_t*** out_substreams,
    size_t* out_count);
ICLFORGE_C_EXPORT void iclforge_decoded_substream_array_destroy(
    iclforge_decoded_substream_t** substreams, size_t count);

/* --- iclforge::DecodedSubstream accessors ------------------------------------ */

ICLFORGE_C_EXPORT int iclforge_decoded_substream_is_independent(
    const iclforge_decoded_substream_t* substream);
ICLFORGE_C_EXPORT int iclforge_decoded_substream_id(const iclforge_decoded_substream_t* substream);
ICLFORGE_C_EXPORT iclforge_sample_rate_t iclforge_decoded_substream_sample_rate(
    const iclforge_decoded_substream_t* substream);
ICLFORGE_C_EXPORT iclforge_acmod_t iclforge_decoded_substream_acmod(
    const iclforge_decoded_substream_t* substream);
ICLFORGE_C_EXPORT int iclforge_decoded_substream_lfe(const iclforge_decoded_substream_t* substream);
ICLFORGE_C_EXPORT int iclforge_decoded_substream_dialnorm(
    const iclforge_decoded_substream_t* substream);
ICLFORGE_C_EXPORT int iclforge_decoded_substream_has_compr(
    const iclforge_decoded_substream_t* substream);
ICLFORGE_C_EXPORT uint8_t iclforge_decoded_substream_compr(
    const iclforge_decoded_substream_t* substream);
ICLFORGE_C_EXPORT uint8_t iclforge_decoded_substream_dynrng(
    const iclforge_decoded_substream_t* substream, int block_index);
ICLFORGE_C_EXPORT int iclforge_decoded_substream_has_dialnorm2(
    const iclforge_decoded_substream_t* substream);
ICLFORGE_C_EXPORT int iclforge_decoded_substream_dialnorm2(
    const iclforge_decoded_substream_t* substream);
ICLFORGE_C_EXPORT int iclforge_decoded_substream_has_compr2(
    const iclforge_decoded_substream_t* substream);
ICLFORGE_C_EXPORT uint8_t iclforge_decoded_substream_compr2(
    const iclforge_decoded_substream_t* substream);
ICLFORGE_C_EXPORT uint8_t iclforge_decoded_substream_dynrng2(
    const iclforge_decoded_substream_t* substream, int block_index);
ICLFORGE_C_EXPORT int iclforge_decoded_substream_numblkscod(
    const iclforge_decoded_substream_t* substream);
ICLFORGE_C_EXPORT int iclforge_decoded_substream_has_chanmap(
    const iclforge_decoded_substream_t* substream);
ICLFORGE_C_EXPORT uint16_t iclforge_decoded_substream_chanmap(
    const iclforge_decoded_substream_t* substream);
ICLFORGE_C_EXPORT int iclforge_decoded_substream_last_dependent(
    const iclforge_decoded_substream_t* substream);
/* Table E2.5 location map this substream's channels occupy — chanmap() when
 * present, the acmod/lfe-derived map otherwise. */
ICLFORGE_C_EXPORT uint16_t iclforge_decoded_substream_location_map(
    const iclforge_decoded_substream_t* substream);

ICLFORGE_C_EXPORT size_t iclforge_decoded_substream_channel_count(
    const iclforge_decoded_substream_t* substream);
ICLFORGE_C_EXPORT size_t iclforge_decoded_substream_samples_per_channel(
    const iclforge_decoded_substream_t* substream);
/* channel_index in [0, channel_count()). The returned pointer is valid until `substream` is
 * destroyed, same convention as iclforge_decoded_frame_channel_samples() above. */
ICLFORGE_C_EXPORT const float* iclforge_decoded_substream_channel_samples(
    const iclforge_decoded_substream_t* substream, size_t channel_index);
ICLFORGE_C_EXPORT int iclforge_decoded_substream_block_switched(
    const iclforge_decoded_substream_t* substream, size_t channel_index, int block_index);

/* --- object audio: OAMD (iclforge::oba::DecodedProgram) + JOC reconstruction -- */

ICLFORGE_C_EXPORT int iclforge_decoded_substream_has_object_metadata(
    const iclforge_decoded_substream_t* substream);
/* Below are only meaningful when has_object_metadata() is non-zero. */
ICLFORGE_C_EXPORT int iclforge_decoded_substream_program_dynamic_only(
    const iclforge_decoded_substream_t* substream);
/* b_lfe_present — dynamic_only programmes only; see program_bed() for the
 * bed-instance branch's own LFE flag (bit ICLFORGE_BED_LFE). */
ICLFORGE_C_EXPORT int iclforge_decoded_substream_program_lfe(
    const iclforge_decoded_substream_t* substream);
/* Table 12 bed-instance channel assignment bitmask (0 when dynamic_only). */
ICLFORGE_C_EXPORT uint16_t iclforge_decoded_substream_program_bed(
    const iclforge_decoded_substream_t* substream);
ICLFORGE_C_EXPORT int iclforge_decoded_substream_program_dynamic_object_count(
    const iclforge_decoded_substream_t* substream);
/* object_index in [0, program_dynamic_object_count()). Position is §4.2.1's
 * room-anchored, left-handed, normalized-to-the-room-cuboid system: x in
 * [0, 1] left to right, y in [0, 1] front to back, z in [-1, 1] floor to
 * ceiling. gain_db is §5.6.1.4's object gain. */
ICLFORGE_C_EXPORT void iclforge_decoded_substream_dynamic_object(
    const iclforge_decoded_substream_t* substream, int object_index, double* out_x, double* out_y,
    double* out_z, double* out_gain_db);

/* JOC's reconstructed per-object audio, parallel to the dynamic objects
 * above (same index = same object) — 0 when no JOC payload rode alongside
 * the OAMD one. Each waveform is samples_per_channel() samples long, and the returned pointer is
 * valid until `substream` is destroyed, same convention as
 * iclforge_decoded_frame_channel_samples() above. */
ICLFORGE_C_EXPORT size_t iclforge_decoded_substream_object_audio_count(
    const iclforge_decoded_substream_t* substream);
ICLFORGE_C_EXPORT const float* iclforge_decoded_substream_object_audio(
    const iclforge_decoded_substream_t* substream, size_t object_index);

ICLFORGE_C_EXPORT void iclforge_decoded_substream_destroy(iclforge_decoded_substream_t* substream);

/* --- Table 12 bed-instance channel assignment bits (program_bed()) ------ */
#define ICLFORGE_BED_LR (1u << 9)
#define ICLFORGE_BED_C (1u << 8)
#define ICLFORGE_BED_LFE (1u << 7)
#define ICLFORGE_BED_LS_RS (1u << 6)
#define ICLFORGE_BED_LB_RB (1u << 5)
#define ICLFORGE_BED_TFL_TFR (1u << 4)
#define ICLFORGE_BED_TSL_TSR (1u << 3)
#define ICLFORGE_BED_TBL_TBR (1u << 2)
#define ICLFORGE_BED_LW_RW (1u << 1)
#define ICLFORGE_BED_LFE2 (1u << 0)

/* --- iclforge::eac3::Location (Table E2.5), used by location_map()/layout ---- */
typedef enum iclforge_location {
    ICLFORGE_LOCATION_L = 0,
    ICLFORGE_LOCATION_C = 1,
    ICLFORGE_LOCATION_R = 2,
    ICLFORGE_LOCATION_LS = 3,
    ICLFORGE_LOCATION_RS = 4,
    ICLFORGE_LOCATION_LC = 5,
    ICLFORGE_LOCATION_RC = 6,
    ICLFORGE_LOCATION_LRS = 7,
    ICLFORGE_LOCATION_RRS = 8,
    ICLFORGE_LOCATION_CS = 9,
    ICLFORGE_LOCATION_TS = 10,
    ICLFORGE_LOCATION_LSD = 11,
    ICLFORGE_LOCATION_RSD = 12,
    ICLFORGE_LOCATION_LW = 13,
    ICLFORGE_LOCATION_RW = 14,
    ICLFORGE_LOCATION_VHL = 15,
    ICLFORGE_LOCATION_VHR = 16,
    ICLFORGE_LOCATION_VHC = 17,
    ICLFORGE_LOCATION_LTS = 18,
    ICLFORGE_LOCATION_RTS = 19,
    ICLFORGE_LOCATION_LFE2 = 20,
    ICLFORGE_LOCATION_LFE = 21
} iclforge_location_t;

/* --- iclforge::DecodedAccessUnit accessors ----------------------------------- */

/* Same held-back-frame convention as decode_substream/decode_access_unit
 * above; see iclforge::Eac3Decoder::decode_access_unit's own comment. */
ICLFORGE_C_EXPORT iclforge_sample_rate_t iclforge_decoded_access_unit_sample_rate(
    const iclforge_decoded_access_unit_t* unit);
ICLFORGE_C_EXPORT iclforge_acmod_t iclforge_decoded_access_unit_acmod(
    const iclforge_decoded_access_unit_t* unit);
ICLFORGE_C_EXPORT int iclforge_decoded_access_unit_dialnorm(
    const iclforge_decoded_access_unit_t* unit);
ICLFORGE_C_EXPORT int iclforge_decoded_access_unit_has_compr(
    const iclforge_decoded_access_unit_t* unit);
ICLFORGE_C_EXPORT uint8_t iclforge_decoded_access_unit_compr(
    const iclforge_decoded_access_unit_t* unit);
ICLFORGE_C_EXPORT uint8_t iclforge_decoded_access_unit_dynrng(
    const iclforge_decoded_access_unit_t* unit, int block_index);
/* Ch2's own word — meaningful only when acmod() == ICLFORGE_ACMOD_DUAL_MONO;
 * see iclforge_decoded_frame_has_dialnorm2()'s own comment. */
ICLFORGE_C_EXPORT int iclforge_decoded_access_unit_has_dialnorm2(
    const iclforge_decoded_access_unit_t* unit);
ICLFORGE_C_EXPORT int iclforge_decoded_access_unit_dialnorm2(
    const iclforge_decoded_access_unit_t* unit);
ICLFORGE_C_EXPORT int iclforge_decoded_access_unit_numblkscod(
    const iclforge_decoded_access_unit_t* unit);
ICLFORGE_C_EXPORT int iclforge_decoded_access_unit_substream_count(
    const iclforge_decoded_access_unit_t* unit);

/* The rendered programme, laid out in Table E2.5 location order — parallel
 * to layout() below, except for dual mono (see layout_count()'s own
 * comment). */
ICLFORGE_C_EXPORT size_t iclforge_decoded_access_unit_channel_count(
    const iclforge_decoded_access_unit_t* unit);
ICLFORGE_C_EXPORT size_t iclforge_decoded_access_unit_samples_per_channel(
    const iclforge_decoded_access_unit_t* unit);
/* channel_index in [0, channel_count()). The returned pointer is valid until `unit` is
 * destroyed, same convention as iclforge_decoded_frame_channel_samples() above. */
ICLFORGE_C_EXPORT const float* iclforge_decoded_access_unit_channel_samples(
    const iclforge_decoded_access_unit_t* unit, size_t channel_index);

/* 0 for dual mono (acmod == ICLFORGE_ACMOD_DUAL_MONO): 1+1 has no Table
 * E2.5 layout, its two channels are unrelated programmes — see
 * iclforge::DecodedAccessUnit::layout's own comment. Otherwise equal to
 * channel_count() above. */
ICLFORGE_C_EXPORT size_t iclforge_decoded_access_unit_layout_count(
    const iclforge_decoded_access_unit_t* unit);
ICLFORGE_C_EXPORT iclforge_location_t iclforge_decoded_access_unit_layout_location(
    const iclforge_decoded_access_unit_t* unit, size_t index);

ICLFORGE_C_EXPORT int iclforge_decoded_access_unit_has_object_metadata(
    const iclforge_decoded_access_unit_t* unit);
ICLFORGE_C_EXPORT int iclforge_decoded_access_unit_program_dynamic_only(
    const iclforge_decoded_access_unit_t* unit);
ICLFORGE_C_EXPORT int iclforge_decoded_access_unit_program_lfe(
    const iclforge_decoded_access_unit_t* unit);
ICLFORGE_C_EXPORT uint16_t iclforge_decoded_access_unit_program_bed(
    const iclforge_decoded_access_unit_t* unit);
ICLFORGE_C_EXPORT int iclforge_decoded_access_unit_program_dynamic_object_count(
    const iclforge_decoded_access_unit_t* unit);
ICLFORGE_C_EXPORT void iclforge_decoded_access_unit_dynamic_object(
    const iclforge_decoded_access_unit_t* unit, int object_index, double* out_x, double* out_y,
    double* out_z, double* out_gain_db);
/* The returned pointer is valid until `unit` is destroyed, same convention as
 * iclforge_decoded_frame_channel_samples() above. */
ICLFORGE_C_EXPORT size_t iclforge_decoded_access_unit_object_audio_count(
    const iclforge_decoded_access_unit_t* unit);
ICLFORGE_C_EXPORT const float* iclforge_decoded_access_unit_object_audio(
    const iclforge_decoded_access_unit_t* unit, size_t object_index);

ICLFORGE_C_EXPORT void iclforge_decoded_access_unit_destroy(iclforge_decoded_access_unit_t* unit);

/* --------------------------------------------------------------------- *
 * Stream framing helpers (iclforge::split_frames / split_access_units / stream_bsid)
 * --------------------------------------------------------------------- */

/* A library-owned array of (offset, length) spans into the SAME buffer the
 * caller passed to iclforge_split_frames()/iclforge_split_access_units() —
 * the caller must keep that buffer alive and unmodified for as long as this
 * result is in use. */
typedef struct iclforge_span {
    size_t offset;
    size_t length;
} iclforge_span_t;

typedef struct iclforge_spans iclforge_spans_t;

ICLFORGE_C_EXPORT size_t iclforge_spans_count(const iclforge_spans_t* spans);
ICLFORGE_C_EXPORT iclforge_span_t iclforge_spans_get(const iclforge_spans_t* spans, size_t index);
ICLFORGE_C_EXPORT void iclforge_spans_destroy(iclforge_spans_t* spans);

/* Splits a raw elementary stream into syncframes by sync word and declared
 * size. Handles both AC-3 and E-AC-3. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_split_frames(const uint8_t* stream, size_t stream_size,
                                                       iclforge_spans_t** out_spans);
/* Groups those syncframes into access units — a new one begins at each
 * independent substream. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_split_access_units(const uint8_t* stream,
                                                             size_t stream_size,
                                                             iclforge_spans_t** out_spans);
/* bsid at bit 40, without committing to either generation. Fails only if
 * `frame` is too short to hold a header. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_stream_bsid(const uint8_t* frame, size_t frame_size,
                                                      int* out_bsid);

/* --------------------------------------------------------------------- *
 * Stream scan (iclforge::io::scan / iclforge::io::ScannedStream)
 * --------------------------------------------------------------------- */

/* split_frames()/split_access_units()/stream_bsid() above only delimit a
 * stream; iclforge_scan() actually reads what it contains - sample rate,
 * layout, every programme it carries, the Annex G/DVB service fields a
 * muxer's descriptors want - without decoding any audio. Mirrors
 * iclforge::io::scan()/ScannedStream. */

/* Mirrors iclforge::io::StreamKind. */
typedef enum iclforge_stream_kind {
    ICLFORGE_STREAM_KIND_AC3 = 0,   /* bsid <= 10 */
    ICLFORGE_STREAM_KIND_EAC3 = 1,  /* bsid 16 (Annex E) */
    /* §E2.3.1.2 legacy-core delivery: an AC-3 syncframe carrying the 5.1 bed,
     * immediately followed by one or more Annex E dependent substreams that
     * extend it - see iclforge::io::StreamKind's own comment on why this is its
     * own kind rather than folded into either of the two above. */
    ICLFORGE_STREAM_KIND_AC3_CORE_EAC3_EXTENSION = 2
} iclforge_stream_kind_t;

typedef struct iclforge_scanned_stream iclforge_scanned_stream_t;

/* On success, *out_stream receives the scan result; the caller must destroy
 * it. `stream`'s bytes must stay valid and unmodified for as long as any
 * access-unit span this result reports is still in use - same convention as
 * iclforge_split_frames()/iclforge_split_access_units(). */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_scan(const uint8_t* stream, size_t stream_size,
                                               iclforge_scanned_stream_t** out_stream);

ICLFORGE_C_EXPORT iclforge_stream_kind_t iclforge_scanned_stream_kind(
    const iclforge_scanned_stream_t* stream);
ICLFORGE_C_EXPORT iclforge_sample_rate_t iclforge_scanned_stream_sample_rate(
    const iclforge_scanned_stream_t* stream);
/* Of the first (or only) substream - see iclforge_scanned_stream_channels()
 * for what the stream as a whole RENDERS. */
ICLFORGE_C_EXPORT iclforge_acmod_t iclforge_scanned_stream_acmod(
    const iclforge_scanned_stream_t* stream);
ICLFORGE_C_EXPORT int iclforge_scanned_stream_lfe(const iclforge_scanned_stream_t* stream);
/* Channels the stream RENDERS - for E-AC-3 this folds in every dependent
 * substream's chanmap, so it is not the bed's own channel count. */
ICLFORGE_C_EXPORT int iclforge_scanned_stream_channels(const iclforge_scanned_stream_t* stream);

/* The FIRST programme's access units only (offset/length into the `stream`
 * buffer passed to iclforge_scan()) - see ScannedStream::access_units's own
 * comment on why a second programme's units are not appended here. Use the
 * programme accessors below to reach any others. */
ICLFORGE_C_EXPORT size_t iclforge_scanned_stream_access_unit_count(
    const iclforge_scanned_stream_t* stream);
ICLFORGE_C_EXPORT iclforge_span_t iclforge_scanned_stream_access_unit(
    const iclforge_scanned_stream_t* stream, size_t index);
/* Samples access unit `index` codes - parallel to the count above. Always
 * ICLFORGE_SAMPLES_PER_FRAME for AC-3; an E-AC-3 independent substream's own
 * numblkscod (§E2.3.1.4) lets this be 256, 512, 768 or 1536, and a stream may
 * mix lengths. */
ICLFORGE_C_EXPORT uint32_t iclforge_scanned_stream_access_unit_samples(
    const iclforge_scanned_stream_t* stream, size_t index);
/* Substreams in the first access unit; always 1 for AC-3. */
ICLFORGE_C_EXPORT size_t iclforge_scanned_stream_substreams_per_unit(
    const iclforge_scanned_stream_t* stream);

/* --- raw syntax fields (iclforge::io::dec3.hpp's codec-config boxes / MPEG-TS
 * descriptors want these straight off the bitstream) --------------------- */

ICLFORGE_C_EXPORT int iclforge_scanned_stream_bsid(const iclforge_scanned_stream_t* stream);
/* 0 when the stream never carried bsmod - see bsmod_present() below, which
 * is what distinguishes "the stream said complete main" from "never said". */
ICLFORGE_C_EXPORT int iclforge_scanned_stream_bsmod(const iclforge_scanned_stream_t* stream);
/* AC-3 only (a kAc3CoreEac3Extension stream's core included): Table 5.18's
 * index into kBitratesKbps. Meaningless for plain E-AC-3, which has no
 * equivalent fixed-table field. */
ICLFORGE_C_EXPORT int iclforge_scanned_stream_bit_rate_code(const iclforge_scanned_stream_t* stream);
/* TS 103 420 §8.3.2.2's complexity_index_type_a - the Atmos/JOC marker
 * readable without decoding the EMDF container itself. */
ICLFORGE_C_EXPORT int iclforge_scanned_stream_has_oba_complexity_index(
    const iclforge_scanned_stream_t* stream);
ICLFORGE_C_EXPORT int iclforge_scanned_stream_oba_complexity_index(
    const iclforge_scanned_stream_t* stream);
/* Whether bsmod was actually transmitted - always true for AC-3, only when
 * infomdate was set for E-AC-3. */
ICLFORGE_C_EXPORT int iclforge_scanned_stream_bsmod_present(const iclforge_scanned_stream_t* stream);
/* §5.4.2.8/§E2.3.2.3 dsurmod: 0 = not indicated, 1 = NOT Dolby Surround
 * encoded, 2 = Dolby Surround encoded. Only transmitted when acmod is 2/0. */
ICLFORGE_C_EXPORT int iclforge_scanned_stream_dsurmod(const iclforge_scanned_stream_t* stream);
/* The Annex G §3.5 mixinfoexists conditions for independent substream 0. */
ICLFORGE_C_EXPORT int iclforge_scanned_stream_mix_metadata(const iclforge_scanned_stream_t* stream);
/* Bit n set when an independent substream with substreamid n appears
 * ANYWHERE in the stream - an observation over the whole stream, computed
 * independently of how the programme list below groups them. */
ICLFORGE_C_EXPORT uint8_t iclforge_scanned_stream_independent_substreams(
    const iclforge_scanned_stream_t* stream);
/* The FIRST programme's rendered channel LOCATIONS as one Table E2.5
 * custom-channel-map word (bit 0 = Left in the MSB through bit 15 = LFE in
 * the LSB) - see iclforge::io::ScannedStream::channel_map's own comment. Written
 * for iclforge::io::dash_channel_configuration()'s DASH @value. */
ICLFORGE_C_EXPORT uint16_t iclforge_scanned_stream_channel_map(
    const iclforge_scanned_stream_t* stream);

/* --- independent substreams 1-3 (index 0-2), for the DVB/ATSC descriptors
 * that name them individually - substream 0 is the stream's main service,
 * already described by acmod/lfe/bsmod/mix_metadata above. `present` false
 * means that substream id was never seen. -------------------------------- */

ICLFORGE_C_EXPORT int iclforge_scanned_stream_associated_substream_present(
    const iclforge_scanned_stream_t* stream, int index);
ICLFORGE_C_EXPORT int iclforge_scanned_stream_associated_substream_bsmod(
    const iclforge_scanned_stream_t* stream, int index);
ICLFORGE_C_EXPORT int iclforge_scanned_stream_associated_substream_bsmod_present(
    const iclforge_scanned_stream_t* stream, int index);
ICLFORGE_C_EXPORT iclforge_acmod_t iclforge_scanned_stream_associated_substream_acmod(
    const iclforge_scanned_stream_t* stream, int index);
ICLFORGE_C_EXPORT int iclforge_scanned_stream_associated_substream_lfe(
    const iclforge_scanned_stream_t* stream, int index);
ICLFORGE_C_EXPORT int iclforge_scanned_stream_associated_substream_mix_metadata(
    const iclforge_scanned_stream_t* stream, int index);

/* --- iclforge::io::ScannedProgramme, by index - ascending substreamid order,
 * never empty on a successful scan. Entry 0 is the same programme every
 * scalar accessor above describes. §E2.3.1.2 allows up to 8 for E-AC-3;
 * always exactly 1 for AC-3 and ICLFORGE_STREAM_KIND_AC3_CORE_EAC3_EXTENSION,
 * neither of which has a second independent substream to number away from. */

ICLFORGE_C_EXPORT size_t iclforge_scanned_stream_programme_count(
    const iclforge_scanned_stream_t* stream);
ICLFORGE_C_EXPORT int iclforge_scanned_stream_programme_substream_id(
    const iclforge_scanned_stream_t* stream, size_t programme_index);
ICLFORGE_C_EXPORT iclforge_acmod_t iclforge_scanned_stream_programme_acmod(
    const iclforge_scanned_stream_t* stream, size_t programme_index);
ICLFORGE_C_EXPORT int iclforge_scanned_stream_programme_lfe(const iclforge_scanned_stream_t* stream,
                                                         size_t programme_index);
/* Channels this PROGRAMME renders, folding in every dependent's chanmap. */
ICLFORGE_C_EXPORT int iclforge_scanned_stream_programme_channels(
    const iclforge_scanned_stream_t* stream, size_t programme_index);
ICLFORGE_C_EXPORT int iclforge_scanned_stream_programme_bsid(const iclforge_scanned_stream_t* stream,
                                                          size_t programme_index);
/* §5.4.2.2's service type - what tells a receiver this programme is a
 * complete main service (0-1) rather than one to be mixed against another
 * (2-7). */
ICLFORGE_C_EXPORT int iclforge_scanned_stream_programme_bsmod(const iclforge_scanned_stream_t* stream,
                                                           size_t programme_index);
ICLFORGE_C_EXPORT size_t iclforge_scanned_stream_programme_substreams_per_unit(
    const iclforge_scanned_stream_t* stream, size_t programme_index);
ICLFORGE_C_EXPORT int iclforge_scanned_stream_programme_has_oba_complexity_index(
    const iclforge_scanned_stream_t* stream, size_t programme_index);
ICLFORGE_C_EXPORT int iclforge_scanned_stream_programme_oba_complexity_index(
    const iclforge_scanned_stream_t* stream, size_t programme_index);
/* One entry per frame period, offset/length into the `stream` buffer passed
 * to iclforge_scan() - same span convention as
 * iclforge_scanned_stream_access_unit() above. */
ICLFORGE_C_EXPORT size_t iclforge_scanned_stream_programme_access_unit_count(
    const iclforge_scanned_stream_t* stream, size_t programme_index);
ICLFORGE_C_EXPORT iclforge_span_t iclforge_scanned_stream_programme_access_unit(
    const iclforge_scanned_stream_t* stream, size_t programme_index, size_t au_index);

ICLFORGE_C_EXPORT void iclforge_scanned_stream_destroy(iclforge_scanned_stream_t* stream);

/* --- timing (iclforge::io::access_unit_timing() and neighbours) - over the FIRST
 * programme's access units, same convention as the scalar fields above. --- */

/* Access unit `index`'s absolute position - returns 0 (out-parameters left
 * untouched) when there is no such unit, 1 otherwise. A caller wanting
 * seconds divides start_sample/duration_samples by sample_rate itself (or an
 * arbitrary-timescale tick by multiplying before dividing) - deliberately not
 * wrapped here, unlike AccessUnitTiming's own start_seconds()/
 * start_in_timescale(), since it is one line either caller side and this
 * keeps the entry point count down. */
ICLFORGE_C_EXPORT int iclforge_scanned_stream_access_unit_timing(
    const iclforge_scanned_stream_t* stream, size_t index, uint64_t* out_start_sample,
    uint32_t* out_duration_samples, uint32_t* out_sample_rate);
ICLFORGE_C_EXPORT uint64_t iclforge_scanned_stream_duration_samples(
    const iclforge_scanned_stream_t* stream);
ICLFORGE_C_EXPORT double iclforge_scanned_stream_duration_seconds(
    const iclforge_scanned_stream_t* stream);
/* The access unit covering `sample`/`seconds` - i.e. the one to cut at for a
 * given position. Returns 0 (out_index untouched) past the end of the
 * stream, 1 otherwise. */
ICLFORGE_C_EXPORT int iclforge_scanned_stream_access_unit_at_sample(
    const iclforge_scanned_stream_t* stream, uint64_t sample, size_t* out_index);
ICLFORGE_C_EXPORT int iclforge_scanned_stream_access_unit_at_seconds(
    const iclforge_scanned_stream_t* stream, double seconds, size_t* out_index);
/* The one length every access unit shares - returns 0 (out_samples
 * untouched) when they differ, 1 otherwise. iclforge::mp4::AudioTrack/iclforge::mpegts::
 * AudioTrack/iclforge::matroska::AudioTrack each need exactly this before a stream can
 * be muxed into a fixed-duration track. */
ICLFORGE_C_EXPORT int iclforge_scanned_stream_uniform_access_unit_samples(
    const iclforge_scanned_stream_t* stream, uint32_t* out_samples);

/* --------------------------------------------------------------------- *
 * Atmos encode (iclforge::oba::AtmosEncoder)
 * --------------------------------------------------------------------- */

typedef struct iclforge_atmos_encoder iclforge_atmos_encoder_t;

/* Mirrors iclforge::oba::AtmosConfig. */
typedef struct iclforge_atmos_config {
    iclforge_sample_rate_t sample_rate;
    uint32_t bitrate_kbps; /* default 448 */
    int dialnorm;
    int num_bands_idx; /* index into iclforge::oba::joc::kNumBands (Table 50), 0..7; default 4 */
    int fine_quant;
    int emit_object_metadata; /* default 1 — see AtmosConfig's own comment on turning this off */
    int fast_mdct;
} iclforge_atmos_config_t;

ICLFORGE_C_EXPORT void iclforge_atmos_config_init(iclforge_atmos_config_t* config);

/* One object's placement for one frame — mirrors iclforge::oba::ObjectPlacement.
 * Position is §4.2.1's room-anchored system, same ranges as
 * iclforge_decoded_substream_dynamic_object()'s out_x/out_y/out_z above. */
typedef struct iclforge_object_placement {
    double x, y, z;
    double gain;     /* linear, default 1.0 */
    double lfe_send; /* linear, default 0.0 — the only route an object reaches the LFE */
} iclforge_object_placement_t;

/* Fills `placement` with the same defaults iclforge::oba::ObjectPlacement's own default member
 * initializers give — room-centre position (x 0.5, y 0.5, z 0.0), unity gain, no LFE send —
 * call this before setting only the fields you need, the same convention every
 * iclforge_*_config_init() above follows. Without it, a zero-initialized
 * iclforge_object_placement_t silently encodes gain 0.0 (a muted object) rather than the
 * documented default of unity gain, and a position at the room's front-left-floor corner rather
 * than its centre. */
ICLFORGE_C_EXPORT void iclforge_object_placement_init(iclforge_object_placement_t* placement);

/* Fails with ICLFORGE_ERROR_INVALID_ARGUMENT for a negative object_count or a
 * config.num_bands_idx outside 0..7. An object_count the object container
 * cannot carry is reported by encode_frame() below instead. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_atmos_encoder_create(
    const iclforge_atmos_config_t* config, int object_count,
    iclforge_atmos_encoder_t** out_encoder);
ICLFORGE_C_EXPORT void iclforge_atmos_encoder_destroy(iclforge_atmos_encoder_t* encoder);
ICLFORGE_C_EXPORT int iclforge_atmos_encoder_dynamic_object_count(
    const iclforge_atmos_encoder_t* encoder);

/* The OBJECT path's latency budget — what this encoder is for. Its
 * transform_samples is ICLFORGE_SAMPLES_PER_BLOCK plus the §7.1 QMF
 * filterbank's own delay (576 samples, iclforge::dsp::kQmfDelay): JOC codes a
 * matrix that pulls objects back out of the decoded bed in a 64-band complex
 * QMF domain rather than the MDCT's, and analysis plus synthesis costs that
 * much on top of the bed's own overlap. With config.emit_object_metadata
 * clear there is no container, no JOC and no filterbank, and this collapses
 * to the bed's own budget below. */
ICLFORGE_C_EXPORT void iclforge_atmos_encoder_latency(const iclforge_atmos_encoder_t* encoder,
                                                    iclforge_latency_t* out_latency);
ICLFORGE_C_EXPORT int iclforge_atmos_encoder_latency_samples(
    const iclforge_atmos_encoder_t* encoder);

/* The 5.1 BED's budget: what a legacy decoder that ignores the container
 * hears. One transform overlap, like any other E-AC-3 stream. */
ICLFORGE_C_EXPORT void iclforge_atmos_encoder_bed_latency(const iclforge_atmos_encoder_t* encoder,
                                                        iclforge_latency_t* out_latency);

/* objects: `object_count` (as given to _create) mono spans, each exactly
 * ICLFORGE_SAMPLES_PER_FRAME samples; placements: the same count, one per
 * object in the same order. On success, *out_unit receives one complete
 * E-AC-3 access unit (a single independent substream carrying the 5.1 bed,
 * with the EMDF object container in its aux data when
 * config.emit_object_metadata was set); the caller must destroy it. With
 * emit_object_metadata set, an encoder created with 0 objects or with more
 * than 15 (the bed's LFE makes the 16th, TS 103 420 §8.3.2.2's cap) fails
 * here with ICLFORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_atmos_encoder_encode_frame(
    iclforge_atmos_encoder_t* encoder, const float* const* objects, size_t object_count,
    size_t samples_per_object, const iclforge_object_placement_t* placements,
    size_t placement_count, iclforge_bytes_t** out_unit);

/* --------------------------------------------------------------------- *
 * Loudness metering (iclforge::meta::LoudnessMeter)
 * --------------------------------------------------------------------- */

typedef struct iclforge_loudness_meter iclforge_loudness_meter_t;

/* BS.1770-4 Annex 1's basic algorithm, keyed on acmod/lfe exactly like
 * iclforge::meta::LoudnessMeter's own first constructor - the lone surround of
 * 2/1 and 3/1 is weighted as the surround FIELD collapsed to one channel
 * (see the C++ class's own comment on how this differs from the chanmap
 * form below for that one case). push() below expects spans in the coded
 * order this acmod implies (Table 5.8), LFE last. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_loudness_meter_create(
    iclforge_sample_rate_t sample_rate, iclforge_acmod_t acmod, int lfe,
    iclforge_loudness_meter_t** out_meter);

/* BS.1770-5 Annex 3's extended algorithm over a rendered Table E2.5 layout -
 * the wide layouts (7.1, 5.1.2, 5.1.4, 7.1.4) an acmod cannot name, because a
 * dependent substream's height/wide/rear channels are not members of Table
 * 5.8 at all. `chanmap` is the same Table E2.5 bitmask
 * iclforge_decoded_substream_location_map()/ICLFORGE_CHANMAP_* use; push()
 * then expects spans in that map's own bit order (iclforge::eac3::chanmap::expand()'s
 * order). Fails with ICLFORGE_ERROR_INVALID_ARGUMENT for a chanmap with no
 * channels set. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_loudness_meter_create_for_chanmap(
    iclforge_sample_rate_t sample_rate, uint16_t chanmap, iclforge_loudness_meter_t** out_meter);

ICLFORGE_C_EXPORT void iclforge_loudness_meter_destroy(iclforge_loudness_meter_t* meter);
ICLFORGE_C_EXPORT int iclforge_loudness_meter_channel_count(const iclforge_loudness_meter_t* meter);

/* channels: channel_count() planar spans, coded order with LFE last, each
 * samples_per_channel samples - any length works, unlike encode_frame()'s
 * fixed frame size, since a meter is fed incrementally over a whole
 * programme rather than one frame at a time. A channel_count above
 * channel_count() fails with ICLFORGE_ERROR_INVALID_ARGUMENT. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_loudness_meter_push(
    iclforge_loudness_meter_t* meter, const float* const* channels, size_t channel_count,
    size_t samples_per_channel);

/* std::nullopt-via-has_* convention, same as every optional field elsewhere
 * in this header (e.g. iclforge_decoded_frame_has_compr()) - false before
 * enough audio has been pushed for that measurement to mean anything (see
 * iclforge::meta::LoudnessMeter's own per-accessor comments on how much). */
ICLFORGE_C_EXPORT int iclforge_loudness_meter_has_integrated_lkfs(
    const iclforge_loudness_meter_t* meter);
ICLFORGE_C_EXPORT double iclforge_loudness_meter_integrated_lkfs(
    const iclforge_loudness_meter_t* meter);
ICLFORGE_C_EXPORT int iclforge_loudness_meter_has_momentary_lkfs(
    const iclforge_loudness_meter_t* meter);
ICLFORGE_C_EXPORT double iclforge_loudness_meter_momentary_lkfs(
    const iclforge_loudness_meter_t* meter);
ICLFORGE_C_EXPORT int iclforge_loudness_meter_has_short_term_lkfs(
    const iclforge_loudness_meter_t* meter);
ICLFORGE_C_EXPORT double iclforge_loudness_meter_short_term_lkfs(
    const iclforge_loudness_meter_t* meter);
ICLFORGE_C_EXPORT int iclforge_loudness_meter_has_loudness_range(
    const iclforge_loudness_meter_t* meter);
ICLFORGE_C_EXPORT double iclforge_loudness_meter_loudness_range(
    const iclforge_loudness_meter_t* meter);
ICLFORGE_C_EXPORT int iclforge_loudness_meter_has_true_peak_dbtp(
    const iclforge_loudness_meter_t* meter);
ICLFORGE_C_EXPORT double iclforge_loudness_meter_true_peak_dbtp(
    const iclforge_loudness_meter_t* meter);

/* §5.4.2.8: dialnorm is how many dB dialogue sits below digital 100 percent,
 * valid 1..31 - a programme louder than -1 LKFS or quieter than -31 clamps,
 * which is why a stream that never measured anything reports 31. */
ICLFORGE_C_EXPORT int iclforge_dialnorm_from_lkfs(double lkfs);

/* --------------------------------------------------------------------- *
 * Level metering (iclforge::analysis::LevelMeter)
 * --------------------------------------------------------------------- */

/* Everything at or below this reports as this on
 * iclforge_channel_level_t/iclforge_channel_summary_t's *_db fields, so a
 * caller never meets log10(0) - mirrors iclforge::analysis::kFloorDb. */
#define ICLFORGE_LEVEL_METER_FLOOR_DB (-120.0)

typedef struct iclforge_level_meter_ballistics {
    double rms_integration_ms;  /* default 300.0 */
    double peak_decay_db_per_s; /* default 20.0 */
    double peak_hold_ms;        /* default 1200.0 */
} iclforge_level_meter_ballistics_t;

/* Fills `ballistics` with the same defaults as iclforge::analysis::MeterBallistics{}. */
ICLFORGE_C_EXPORT void iclforge_level_meter_ballistics_init(
    iclforge_level_meter_ballistics_t* ballistics);

typedef struct iclforge_level_meter iclforge_level_meter_t;

/* `channels` 0 meters exactly acmod's own channel count (mirrors LevelMeter's
 * first constructor); a larger value meters a wider layout no acmod can name
 * - E-AC-3's dependent substreams add speakers Table 5.8 has no word for -
 * with the acmod still naming and placing the first channels of them as the
 * bed (see LevelMeter's second constructor's own comment); a value below
 * acmod's own count is raised to it rather than truncating a layout the
 * caller has already committed to. `ballistics` NULL uses the same defaults
 * iclforge_level_meter_ballistics_init() fills in. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_level_meter_create(
    iclforge_acmod_t acmod, int lfe, uint32_t sample_rate, int channels,
    const iclforge_level_meter_ballistics_t* ballistics, iclforge_level_meter_t** out_meter);

ICLFORGE_C_EXPORT void iclforge_level_meter_destroy(iclforge_level_meter_t* meter);
ICLFORGE_C_EXPORT iclforge_acmod_t iclforge_level_meter_acmod(const iclforge_level_meter_t* meter);
ICLFORGE_C_EXPORT int iclforge_level_meter_lfe(const iclforge_level_meter_t* meter);
ICLFORGE_C_EXPORT int iclforge_level_meter_channel_count(const iclforge_level_meter_t* meter);
ICLFORGE_C_EXPORT uint32_t iclforge_level_meter_sample_rate(const iclforge_level_meter_t* meter);

/* Planar, one span per channel in A/52 order. The shortest span sets the
 * length; channels beyond the ones supplied are metered as silence, so a
 * caller that hands over fewer spans sees the rest fall away rather than
 * freeze. More spans than channel_count() fails with
 * ICLFORGE_ERROR_INVALID_ARGUMENT. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_level_meter_process(
    iclforge_level_meter_t* meter, const float* const* channels, size_t channel_count,
    size_t samples_per_channel);

/* Drops both the ballistic state and the accumulated summary. */
ICLFORGE_C_EXPORT void iclforge_level_meter_reset(iclforge_level_meter_t* meter);

typedef struct iclforge_channel_level {
    double peak_db; /* ballistic peak */
    double hold_db; /* held maximum */
    double rms_db;  /* integrated RMS */
    int clipped;    /* reached full scale since the last reset */
} iclforge_channel_level_t;

/* The live ballistic view - channel_index in [0, channel_count()); every
 * field is ICLFORGE_LEVEL_METER_FLOOR_DB/clipped=0 out of range. */
ICLFORGE_C_EXPORT iclforge_channel_level_t iclforge_level_meter_level(
    const iclforge_level_meter_t* meter, size_t channel_index);

typedef struct iclforge_channel_summary {
    double peak; /* linear */
    double rms;  /* linear - iclforge::analysis::ChannelSummary::rms() */
    double peak_db;
    double rms_db;
    uint64_t samples;
    uint64_t clipped_samples;
} iclforge_channel_summary_t;

/* Unweighted, exact statistics over everything processed so far - what a
 * file report wants; levels() above exists to make a moving display readable
 * and would only smear a question this has an exact answer to. */
ICLFORGE_C_EXPORT iclforge_channel_summary_t iclforge_level_meter_summary(
    const iclforge_level_meter_t* meter, size_t channel_index);

/* --------------------------------------------------------------------- *
 * QC gate (iclforge::meta::qc)
 * --------------------------------------------------------------------- */

typedef enum iclforge_qc_loudness_limit {
    ICLFORGE_QC_LOUDNESS_BAND = 0,   /* |measured - target| <= tolerance_lu */
    ICLFORGE_QC_LOUDNESS_CEILING = 1 /* measured <= target; tolerance_lu unused */
} iclforge_qc_loudness_limit_t;

/* Mirrors iclforge::meta::QcPresetId, ordinals matching kQcPresetIds's own
 * declaration order. */
typedef enum iclforge_qc_preset_id {
    ICLFORGE_QC_PRESET_EBU_R128_S2 = 0,
    ICLFORGE_QC_PRESET_ATSC_A85 = 1,
    ICLFORGE_QC_PRESET_ATSC_A85_STREAMING = 2,
    ICLFORGE_QC_PRESET_NETFLIX = 3,
    ICLFORGE_QC_PRESET_APPLE_MUSIC_ATMOS = 4
} iclforge_qc_preset_id_t;

/* Every preset id is valid in [0, iclforge_qc_preset_count()) - for a caller
 * that wants to check a measurement against all of them (mirrors
 * iclforge::meta::kQcPresetIds's own size). */
ICLFORGE_C_EXPORT size_t iclforge_qc_preset_count(void);

/* Tagged iclforge_qc_preset_info rather than iclforge_qc_preset: GCC's
 * -Wshadow (built C++) treats a struct tag and a same-named free function as
 * the function "hiding" the tag's implicit constructor-like name - the same
 * reason iclforge_version_info/iclforge_version() above are split. The
 * typedef name below is what callers actually use. */
typedef struct iclforge_qc_preset_info {
    double target_lkfs;
    double tolerance_lu;       /* +/- around target_lkfs, BAND only */
    double max_true_peak_dbtp; /* a ceiling, not a tolerance band */
    iclforge_qc_loudness_limit_t loudness_limit;
    /* The primary document this row was read out of, with its version and
     * date - library-owned storage valid for the process lifetime; never
     * free() it. */
    const char* source;
} iclforge_qc_preset_t;

ICLFORGE_C_EXPORT iclforge_qc_preset_t iclforge_qc_preset(iclforge_qc_preset_id_t id);
/* Library-owned storage valid for the process lifetime. */
ICLFORGE_C_EXPORT const char* iclforge_qc_preset_name(iclforge_qc_preset_id_t id);
/* 1 and *out_id set on a recognized name (e.g. "ebu-r128-s2"), 0 otherwise -
 * *out_id is left untouched when this returns 0. */
ICLFORGE_C_EXPORT int iclforge_parse_qc_preset(const char* name, iclforge_qc_preset_id_t* out_id);

/* One preset's verdict against one measurement - mirrors iclforge::meta::QcVerdict.
 * Either half is left at its not-passing default when the corresponding
 * measurement was itself unavailable (has_integrated_lkfs/has_true_peak_dbtp
 * false below) - material this gate cannot actually judge, not a false
 * pass. */
typedef struct iclforge_qc_verdict {
    int has_loudness_delta_lu;
    double loudness_delta_lu; /* measured - target */
    int loudness_pass;
    int has_true_peak_margin_dbtp;
    double true_peak_margin_dbtp; /* ceiling - measured; >= 0 passes */
    int true_peak_pass;
} iclforge_qc_verdict_t;

ICLFORGE_C_EXPORT int iclforge_qc_verdict_pass(const iclforge_qc_verdict_t* verdict);

/* has_integrated_lkfs/has_true_peak_dbtp: pass 0 exactly when
 * iclforge_loudness_meter_has_integrated_lkfs()/..._has_true_peak_dbtp()
 * would - see iclforge_qc_verdict_t's own comment on what that leaves in the
 * verdict. */
ICLFORGE_C_EXPORT iclforge_qc_verdict_t iclforge_evaluate_qc_gate(
    const iclforge_qc_preset_t* preset, int has_integrated_lkfs, double integrated_lkfs,
    int has_true_peak_dbtp, double true_peak_dbtp);

/* --------------------------------------------------------------------- *
 * AC-4 (iclforge::ac4::Decoder / iclforge::ac4::Encoder) - ETSI TS 103 190-1 V1.4.1 and
 * TS 103 190-2 V1.3.1
 * --------------------------------------------------------------------- *
 *
 * Always declared, whether or not this library was configured with
 * ICLFORGE_BUILD_AC4 (the default is on): the codebase selects a variant by
 * CMake, never by preprocessor conditional, so a caller does not need an
 * #ifdef of its own either. Built without it, every function below still
 * links; every fallible one, *_create() included, returns
 * ICLFORGE_ERROR_UNSUPPORTED (a *_create() leaves its out-parameter NULL),
 * and one that returns something else gives NULL, 0 or a zero-initialized
 * struct as its type allows.
 * iclforge_c/version.h's ICLFORGE_HAS_AC4 (a plain #define, #cmakedefine'd
 * from that option) still tells a caller which behaviour to expect. Mirrors
 * iclforge::ac4::Decoder (src/ac4dec/include/iclforge/ac4dec/decoder.hpp) and
 * iclforge::ac4::Encoder (src/ac4enc/include/iclforge/ac4enc/encoder.hpp), plus the
 * table-of-contents helpers of src/ac4/include/iclforge/ac4/ac4.hpp a container muxer needs beside
 * the encoder. AC-4's frame length varies by frame rate (Part 1 Tables 83/84), so unlike the
 * AC-3/E-AC-3 sections above there is no ICLFORGE_SAMPLES_PER_FRAME equivalent - every accessor
 * that needs a length reports it.
 *
 * The encoder writes channel-based and channel-based-immersive content
 * (mono, stereo, 5.0, 5.1, 5.0.4, 5.1.4) and, given an objects configuration
 * (iclforge_ac4_objects_config_t, in the encoder section below), the one
 * object substream iclforge::ac4::Encoder writes with experimental.objects: A-JOC, or
 * direct-coded objects (planning/ac4.md phases E9 and I4b). The decoder's
 * object accessors below read whatever object audio a stream carries. */

/* --- shared enums -------------------------------------------------------- */

/* Mirrors iclforge::ac4::Speaker (Part 1 clause D.1, Part 2 clause A.3). */
typedef enum iclforge_ac4_speaker {
    ICLFORGE_AC4_SPEAKER_LEFT = 0,
    ICLFORGE_AC4_SPEAKER_RIGHT = 1,
    ICLFORGE_AC4_SPEAKER_CENTRE = 2,
    ICLFORGE_AC4_SPEAKER_LFE = 3,
    ICLFORGE_AC4_SPEAKER_LEFT_SURROUND = 4,
    ICLFORGE_AC4_SPEAKER_RIGHT_SURROUND = 5,
    ICLFORGE_AC4_SPEAKER_LEFT_BACK = 6,
    ICLFORGE_AC4_SPEAKER_RIGHT_BACK = 7,
    ICLFORGE_AC4_SPEAKER_LEFT_WIDE = 8,
    ICLFORGE_AC4_SPEAKER_RIGHT_WIDE = 9,
    ICLFORGE_AC4_SPEAKER_TOP_FRONT_LEFT = 10,
    ICLFORGE_AC4_SPEAKER_TOP_FRONT_RIGHT = 11,
    ICLFORGE_AC4_SPEAKER_TOP_BACK_LEFT = 12,
    ICLFORGE_AC4_SPEAKER_TOP_BACK_RIGHT = 13,
    ICLFORGE_AC4_SPEAKER_TOP_SIDE_LEFT = 14,
    ICLFORGE_AC4_SPEAKER_TOP_SIDE_RIGHT = 15,
    ICLFORGE_AC4_SPEAKER_LFE2 = 16
} iclforge_ac4_speaker_t;

/* Mirrors iclforge::ac4::ObjectKind (ac4/ac4.hpp): a bed object, a dynamic object, or an
 * intermediate spatial format object (the decoder renders an ISF's own
 * objects into channels rather than listing them - see
 * iclforge_ac4_decoded_frame_object_kind()'s own comment). */
typedef enum iclforge_ac4_object_kind {
    ICLFORGE_AC4_OBJECT_BED = 0,
    ICLFORGE_AC4_OBJECT_DYN = 1,
    ICLFORGE_AC4_OBJECT_ISF = 2
} iclforge_ac4_object_kind_t;

/* --------------------------------------------------------------------- *
 * AC-4 decoder (iclforge::ac4::Decoder)
 * --------------------------------------------------------------------- */

/* Mirrors iclforge::ac4::DownmixTarget (Part 1 clause 6.2.17, Part 2 clause 5.10.2): the
 * layout iclforge_ac4_decoder_decode() renders to. */
typedef enum iclforge_ac4_downmix_target {
    ICLFORGE_AC4_DOWNMIX_AS_CODED = 0,
    ICLFORGE_AC4_DOWNMIX_5X = 1,
    ICLFORGE_AC4_DOWNMIX_STEREO = 2,
    ICLFORGE_AC4_DOWNMIX_LORO = 3,
    ICLFORGE_AC4_DOWNMIX_LTRT = 4,
    ICLFORGE_AC4_DOWNMIX_MONO = 5,
    ICLFORGE_AC4_DOWNMIX_7X4 = 6,
    ICLFORGE_AC4_DOWNMIX_7X2 = 7,
    ICLFORGE_AC4_DOWNMIX_7X0 = 8,
    ICLFORGE_AC4_DOWNMIX_5X4 = 9,
    ICLFORGE_AC4_DOWNMIX_5X2 = 10
} iclforge_ac4_downmix_target_t;

/* Mirrors iclforge::ac4::DrcMode (Part 1 Table 161). */
typedef enum iclforge_ac4_drc_mode {
    ICLFORGE_AC4_DRC_OFF = 0,
    ICLFORGE_AC4_DRC_DEFAULT = 1,
    ICLFORGE_AC4_DRC_HOME_THEATRE = 2,
    ICLFORGE_AC4_DRC_FLAT_PANEL_TV = 3,
    ICLFORGE_AC4_DRC_PORTABLE_SPEAKERS = 4,
    ICLFORGE_AC4_DRC_PORTABLE_HEADPHONES = 5
} iclforge_ac4_drc_mode_t;

/* Mirrors iclforge::ac4::DecodingMode (Part 2 clause 4.7): full reconstruction, or the
 * immersive element's core (5.X.2/5.X.0) for a low-complexity platform. */
typedef enum iclforge_ac4_decoding_mode {
    ICLFORGE_AC4_DECODING_FULL = 0,
    ICLFORGE_AC4_DECODING_CORE = 1
} iclforge_ac4_decoding_mode_t;

/* Mirrors iclforge::ac4::ConcealmentPolicy: what iclforge_ac4_decoder_decode() does with
 * a frame that will not decode, once at least one frame has. */
typedef enum iclforge_ac4_concealment_policy {
    ICLFORGE_AC4_CONCEALMENT_NONE = 0,
    ICLFORGE_AC4_CONCEALMENT_REPEAT_FADE = 1,
    ICLFORGE_AC4_CONCEALMENT_MUTE = 2
} iclforge_ac4_concealment_policy_t;

/* Mirrors iclforge::ac4::ConcealmentAction: what a concealed frame actually got. */
typedef enum iclforge_ac4_concealment_action {
    ICLFORGE_AC4_CONCEALMENT_ACTION_REPEAT_FADE = 0,
    ICLFORGE_AC4_CONCEALMENT_ACTION_MUTE = 1
} iclforge_ac4_concealment_action_t;

/* Mirrors iclforge::ac4::AssociatedType (Part 1 Table 92), a refinement of
 * iclforge_ac4_presentation_choice_t::associated. */
typedef enum iclforge_ac4_associated_type {
    ICLFORGE_AC4_ASSOCIATED_ANY = 0,
    ICLFORGE_AC4_ASSOCIATED_AUDIO_DESCRIPTION = 1,
    ICLFORGE_AC4_ASSOCIATED_AUDIO_DESCRIPTION_SUBTITLES = 2,
    ICLFORGE_AC4_ASSOCIATED_SPOKEN_SUBTITLES = 3,
    ICLFORGE_AC4_ASSOCIATED_EMERGENCY_INFORMATION = 4
} iclforge_ac4_associated_type_t;

/* Mirrors iclforge::ac4::OutputConfig. `has_output_level_dbfs` stands in for
 * std::optional<double>, same convention as elsewhere in this header. Call
 * iclforge_ac4_output_config_init() first so every field this struct doesn't
 * set explicitly carries the same default OutputConfig{} does. */
typedef struct iclforge_ac4_output_config {
    int has_output_level_dbfs;
    double output_level_dbfs;
    iclforge_ac4_drc_mode_t drc;
    int headphones;
    double dialogue_enhancement_db;
    iclforge_ac4_downmix_target_t downmix;
    int mix_lfe;
    double dialogue_gain_db;
    double associated_gain_db;
} iclforge_ac4_output_config_t;

ICLFORGE_C_EXPORT void iclforge_ac4_output_config_init(iclforge_ac4_output_config_t* config);

/* Mirrors iclforge::ac4::PresentationChoice. `language`, when non-NULL and non-empty, is
 * an IETF BCP 47 tag read during the call this struct is passed to and not
 * retained - it need not outlive that call. has_associated/has_index/
 * has_presentation_id stand in for std::optional<T>. Call
 * iclforge_ac4_presentation_choice_init() first for the same reason as every
 * other _init() function in this header. */
typedef struct iclforge_ac4_presentation_choice {
    int has_presentation_id;
    int presentation_id;
    int has_index;
    size_t index;
    const char* language; /* NULL or empty: no language preference */
    int has_associated;
    int associated; /* Part 1 Table 91 content_classifier */
    iclforge_ac4_associated_type_t associated_type;
    int headphones;
} iclforge_ac4_presentation_choice_t;

ICLFORGE_C_EXPORT void iclforge_ac4_presentation_choice_init(
    iclforge_ac4_presentation_choice_t* choice);

/* Mirrors iclforge::ac4::DecoderConfig, less its syntax trace (an internal diagnostic
 * hook with no C surface). */
typedef struct iclforge_ac4_decoder_config {
    iclforge_ac4_output_config_t output;
    iclforge_ac4_concealment_policy_t concealment;
    iclforge_ac4_presentation_choice_t presentation;
    int level; /* md_compat ceiling, Part 2 Table 55; default 3 */
    iclforge_ac4_decoding_mode_t decoding;
} iclforge_ac4_decoder_config_t;

ICLFORGE_C_EXPORT void iclforge_ac4_decoder_config_init(iclforge_ac4_decoder_config_t* config);

typedef struct iclforge_ac4_decoder iclforge_ac4_decoder_t;

ICLFORGE_C_EXPORT iclforge_status_t iclforge_ac4_decoder_create(
    const iclforge_ac4_decoder_config_t* config, iclforge_ac4_decoder_t** out_decoder);
ICLFORGE_C_EXPORT void iclforge_ac4_decoder_destroy(iclforge_ac4_decoder_t* decoder);

/* The output processing, from the next frame; and the presentation choice,
 * also from the next frame (a newly chosen presentation needs no I-frame -
 * see iclforge::ac4::Decoder::set_presentation()'s own comment). */
ICLFORGE_C_EXPORT void iclforge_ac4_decoder_set_output(iclforge_ac4_decoder_t* decoder,
                                                     const iclforge_ac4_output_config_t* output);
ICLFORGE_C_EXPORT void iclforge_ac4_decoder_set_presentation(
    iclforge_ac4_decoder_t* decoder, const iclforge_ac4_presentation_choice_t* choice);

/* Forgets everything carried between frames - iclforge::ac4::Decoder::reset(). */
ICLFORGE_C_EXPORT void iclforge_ac4_decoder_reset(iclforge_ac4_decoder_t* decoder);

/* The decoder's own added delay at the output rate, for the stream as last
 * decoded; 0 before a frame has decoded - iclforge::ac4::Decoder::latency_samples(). */
ICLFORGE_C_EXPORT int iclforge_ac4_decoder_latency_samples(const iclforge_ac4_decoder_t* decoder);

/* Why the last decode() call failed, returned nothing, or returned a
 * concealed frame - library-owned storage valid for the process lifetime;
 * empty ("") after a decode() that decoded its frame normally. */
ICLFORGE_C_EXPORT const char* iclforge_ac4_decoder_refusal_reason(
    const iclforge_ac4_decoder_t* decoder);

typedef struct iclforge_ac4_decoded_frame iclforge_ac4_decoded_frame_t;

/* `frame` must be exactly one raw_ac4_frame (an iclforge::ac4::SyncFrame's
 * raw_ac4_frame, or an MP4 sample). On success, *out_frame receives the
 * decode result, which the caller must destroy; std::nullopt-via-out-
 * parameter convention as elsewhere in this header: ICLFORGE_OK with
 * *out_frame left NULL means this frame has no output yet (its substreams
 * need configuration no I-frame has sent), not an error - see
 * iclforge::ac4::Decoder::decode()'s own comment. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_ac4_decoder_decode(
    iclforge_ac4_decoder_t* decoder, const uint8_t* frame, size_t frame_size,
    iclforge_ac4_decoded_frame_t** out_frame);

ICLFORGE_C_EXPORT int iclforge_ac4_decoded_frame_sample_rate_hz(
    const iclforge_ac4_decoded_frame_t* frame);
ICLFORGE_C_EXPORT int iclforge_ac4_decoded_frame_sequence_counter(
    const iclforge_ac4_decoded_frame_t* frame);
ICLFORGE_C_EXPORT size_t iclforge_ac4_decoded_frame_presentation_index(
    const iclforge_ac4_decoded_frame_t* frame);
ICLFORGE_C_EXPORT int iclforge_ac4_decoded_frame_has_presentation_id(
    const iclforge_ac4_decoded_frame_t* frame);
ICLFORGE_C_EXPORT int iclforge_ac4_decoded_frame_presentation_id(
    const iclforge_ac4_decoded_frame_t* frame);

/* Planar PCM at full scale 1.0, samples_per_channel() long - AC-4's frame
 * length varies by frame rate, so unlike the AC-3/E-AC-3 sections above this
 * is not a fixed constant. Speakers are parallel to the channels, in the
 * decoder's own order (not a fixed table): channel_index in
 * [0, channel_count()). Empty (channel_count() 0) for a presentation of
 * objects alone with no intermediate spatial format. */
ICLFORGE_C_EXPORT size_t iclforge_ac4_decoded_frame_channel_count(
    const iclforge_ac4_decoded_frame_t* frame);
ICLFORGE_C_EXPORT size_t iclforge_ac4_decoded_frame_samples_per_channel(
    const iclforge_ac4_decoded_frame_t* frame);
ICLFORGE_C_EXPORT const float* iclforge_ac4_decoded_frame_channel_samples(
    const iclforge_ac4_decoded_frame_t* frame, size_t channel_index);
ICLFORGE_C_EXPORT iclforge_ac4_speaker_t iclforge_ac4_decoded_frame_speaker(
    const iclforge_ac4_decoded_frame_t* frame, size_t channel_index);

/* Set only on a frame the decoder's ConcealmentPolicy made in place of one
 * that did not decode; has_concealed() 0 means the frame decoded normally. */
ICLFORGE_C_EXPORT int iclforge_ac4_decoded_frame_has_concealed(
    const iclforge_ac4_decoded_frame_t* frame);
ICLFORGE_C_EXPORT iclforge_ac4_concealment_action_t iclforge_ac4_decoded_frame_concealment_action(
    const iclforge_ac4_decoded_frame_t* frame);
ICLFORGE_C_EXPORT iclforge_status_t iclforge_ac4_decoded_frame_concealment_error(
    const iclforge_ac4_decoded_frame_t* frame);

/* --- objects (Part 2 clause 4.8.3.4; iclforge::ac4::DecodedFrame::objects) --------- *
 *
 * Each object's kind, its bed loudspeaker where it has one, its audio, the
 * ObjectProperties in force at the frame's first sample, and the updates
 * within the frame (iclforge::ac4::DecodedObject::updates): the block updates of the
 * object's metadata a renderer moves through, each at a sample of the frame
 * and with the number of samples it takes to reach its properties. An
 * intermediate spatial format's own objects are rendered into the channels
 * above, not listed here (iclforge::ac4::Decoder's header, "Objects").
 *
 * The objects come in the decoder's order, not that of the encoder's input:
 * the LFE object first, then the bed objects, then the dynamic objects, each
 * group in the order the encoder's configuration lists it (A-JOC and
 * direct-coded alike). Core decoding of an A-JOC substream lists the
 * downmix signals instead, as dynamic objects at their groups' centres. */

ICLFORGE_C_EXPORT size_t iclforge_ac4_decoded_frame_object_count(
    const iclforge_ac4_decoded_frame_t* frame);
ICLFORGE_C_EXPORT iclforge_ac4_object_kind_t iclforge_ac4_decoded_frame_object_kind(
    const iclforge_ac4_decoded_frame_t* frame, size_t object_index);
ICLFORGE_C_EXPORT int iclforge_ac4_decoded_frame_object_lfe(const iclforge_ac4_decoded_frame_t* frame,
                                                          size_t object_index);
ICLFORGE_C_EXPORT int iclforge_ac4_decoded_frame_object_has_speaker(
    const iclforge_ac4_decoded_frame_t* frame, size_t object_index);
ICLFORGE_C_EXPORT iclforge_ac4_speaker_t iclforge_ac4_decoded_frame_object_speaker(
    const iclforge_ac4_decoded_frame_t* frame, size_t object_index);
/* `samples_per_channel()` long, at full scale 1.0; the returned pointer is
 * valid until `frame` is destroyed. */
ICLFORGE_C_EXPORT const float* iclforge_ac4_decoded_frame_object_samples(
    const iclforge_ac4_decoded_frame_t* frame, size_t object_index);

/* Mirrors iclforge::ac4::ObjectProperties (Part 2 Annex F.2 to F.10 and
 * add_per_object_md()'s data): what one block update of an object's metadata
 * sets. The decoder reports it, and the encoder takes it, in these terms:
 *   - gain_db: F.5, +15 to -49 dB in steps of 1, or -infinity for silence;
 *   - priority: F.7, 0 to 1 in steps of 1/31;
 *   - x, y, z: F.2, X from the left wall (0) to the right (1) and Y from the
 *     front wall (0) to the back (1) in steps of 1/62, Z from the floor (-1)
 *     through the height of the screen (0) to the ceiling (1) in steps of
 *     1/15; a dynamic object's, and ignored for a bed object and the LFE;
 *   - zone_mask (F.8, Table 104: 0 to 7), enable_elevation, snap (F.10);
 *   - width_x, width_y, width_z: F.6, each 0 to 1 in steps of 1/31, the three
 *     sent as one object_width where they are equal;
 *   - screen_factor: F.4, 0 or 1/8 to 1 in steps of 1/8;
 *   - depth_exponent: the exponent object_depth_factor gives Y (Table 107),
 *     exactly 0.25, 0.5, 1 or 2, and where it is not 1 the object needs a
 *     screen_factor of 1/8 or more (the two are one group of fields, whose
 *     factor has no code for 0);
 *   - distance, where has_distance: F.4's object_distance_factor (Table 108),
 *     1 or more, or +infinity for b_obj_at_infinity;
 *   - divergence: F.9, 0 to 1;
 *   - trim_disabled, headphone_render_mode (0 to 3, where
 *     has_headphone_render_mode) and head_track_disabled (Table 121).
 * Each value the encoder is given is written to the nearest its code has and
 * refused outside its range (iclforge_ac4_encoder_refusal_reason() says
 * which); a dynamic object sends all of them, a bed object and the LFE the
 * activity, gain, priority and add_per_object_md()'s data alone. Call
 * iclforge_ac4_object_properties_init() before setting fields for the
 * encoder: a zero-initialised struct is priority 0, depth exponent 0 (which
 * no code holds) and position (0, 0, 0), not the defaults. */
typedef struct iclforge_ac4_object_properties {
    int active;
    double gain_db;
    double priority;
    double x, y, z;
    int zone_mask;
    int enable_elevation;
    int snap;
    double width_x, width_y, width_z;
    double screen_factor;
    double depth_exponent;
    int has_distance;
    double distance;
    double divergence;
    int trim_disabled;
    int has_headphone_render_mode;
    int headphone_render_mode;
    int head_track_disabled;
} iclforge_ac4_object_properties_t;

/* Fills `properties` with iclforge::ac4::ObjectProperties{}'s defaults: active, 0 dB,
 * priority 1, room centre (0.5, 0.5, 0), no zone constraint, elevation
 * enabled, no snap, zero width, screen factor 0, depth exponent 1, no
 * distance, no divergence, trim and head tracking enabled, no headphone
 * render mode. */
ICLFORGE_C_EXPORT void iclforge_ac4_object_properties_init(
    iclforge_ac4_object_properties_t* properties);

/* What is in force at the frame's first sample. */
ICLFORGE_C_EXPORT iclforge_ac4_object_properties_t iclforge_ac4_decoded_frame_object_properties(
    const iclforge_ac4_decoded_frame_t* frame, size_t object_index);

/* Mirrors iclforge::ac4::ObjectUpdate (Part 2 Annex F.11): one block update within the
 * frame - the output sample of the frame it takes effect at (counted with the
 * decoder's delay, as the frame's channels are), the samples a renderer takes
 * to move to `properties` from what was in force, and those properties. */
typedef struct iclforge_ac4_object_update {
    size_t sample;
    int ramp_samples;
    iclforge_ac4_object_properties_t properties;
} iclforge_ac4_object_update_t;

/* The number of updates within the frame for object `object_index`, in the
 * order they take effect; 0 for an index out of range. */
ICLFORGE_C_EXPORT size_t iclforge_ac4_decoded_frame_object_update_count(
    const iclforge_ac4_decoded_frame_t* frame, size_t object_index);
/* Update `update_index` of object `object_index`; sample 0, ramp 0 and the
 * properties iclforge_ac4_object_properties_init() gives for an index out of
 * range. */
ICLFORGE_C_EXPORT iclforge_ac4_object_update_t iclforge_ac4_decoded_frame_object_update(
    const iclforge_ac4_decoded_frame_t* frame, size_t object_index, size_t update_index);

ICLFORGE_C_EXPORT void iclforge_ac4_decoded_frame_destroy(iclforge_ac4_decoded_frame_t* frame);

/* --- presentations (iclforge::ac4::PresentationInfo, iclforge::ac4::Decoder::presentations()) - *
 * the last frame read's table of contents, in its own order; empty before
 * one. `name`/`language` accessors return library-owned storage valid until
 * the next iclforge_ac4_decoder_decode() call or the decoder's destruction -
 * the same lifetime as every other "valid until X" pointer in this header,
 * specialised to what actually invalidates it here. */

ICLFORGE_C_EXPORT size_t iclforge_ac4_decoder_presentation_count(
    const iclforge_ac4_decoder_t* decoder);
ICLFORGE_C_EXPORT size_t iclforge_ac4_decoder_presentation_toc_index(
    const iclforge_ac4_decoder_t* decoder, size_t presentation_index);
ICLFORGE_C_EXPORT int iclforge_ac4_decoder_presentation_has_id(const iclforge_ac4_decoder_t* decoder,
                                                             size_t presentation_index);
ICLFORGE_C_EXPORT int iclforge_ac4_decoder_presentation_id(const iclforge_ac4_decoder_t* decoder,
                                                          size_t presentation_index);
ICLFORGE_C_EXPORT int iclforge_ac4_decoder_presentation_has_md_compat(
    const iclforge_ac4_decoder_t* decoder, size_t presentation_index);
ICLFORGE_C_EXPORT int iclforge_ac4_decoder_presentation_md_compat(
    const iclforge_ac4_decoder_t* decoder, size_t presentation_index);
ICLFORGE_C_EXPORT int iclforge_ac4_decoder_presentation_enabled(const iclforge_ac4_decoder_t* decoder,
                                                              size_t presentation_index);
ICLFORGE_C_EXPORT int iclforge_ac4_decoder_presentation_alternative(
    const iclforge_ac4_decoder_t* decoder, size_t presentation_index);
ICLFORGE_C_EXPORT int iclforge_ac4_decoder_presentation_pre_virtualized(
    const iclforge_ac4_decoder_t* decoder, size_t presentation_index);
/* Empty ("") until an alternative presentation's name has arrived whole
 * (Part 2 clause 6.3.3.1.4) or for a presentation that is not one. */
ICLFORGE_C_EXPORT const char* iclforge_ac4_decoder_presentation_name(
    const iclforge_ac4_decoder_t* decoder, size_t presentation_index);
/* Its dialogue substream's language, else its main or music-and-effects
 * substream's; empty ("") for none. */
ICLFORGE_C_EXPORT const char* iclforge_ac4_decoder_presentation_language(
    const iclforge_ac4_decoder_t* decoder, size_t presentation_index);
ICLFORGE_C_EXPORT int iclforge_ac4_decoder_presentation_decodable(
    const iclforge_ac4_decoder_t* decoder, size_t presentation_index);
ICLFORGE_C_EXPORT int iclforge_ac4_decoder_presentation_selectable(
    const iclforge_ac4_decoder_t* decoder, size_t presentation_index);
/* The channels decode() puts out as coded for this presentation - its main or
 * music-and-effects substream's; parallel to a decoded frame's own
 * channel/speaker accessors above, but this list does not change frame to
 * frame the way a concealed or object-only frame's does. */
ICLFORGE_C_EXPORT size_t iclforge_ac4_decoder_presentation_speaker_count(
    const iclforge_ac4_decoder_t* decoder, size_t presentation_index);
ICLFORGE_C_EXPORT iclforge_ac4_speaker_t iclforge_ac4_decoder_presentation_speaker(
    const iclforge_ac4_decoder_t* decoder, size_t presentation_index, size_t speaker_index);

/* --- loudness metadata (iclforge::ac4::LoudnessInfo, of the presentation decode()
 * selected) - the fields iclforge_evaluate_qc_gate() above already takes, so
 * an AC-4 stream's own sent loudness can feed the same QC gate a
 * iclforge_loudness_meter_t measurement does. has_* 0 leaves the paired field
 * at 0.0, same std::optional convention as elsewhere in this header. */
typedef struct iclforge_ac4_loudness_info {
    int has_dialnorm_dbfs;
    double dialnorm_dbfs;
    int has_integrated_lkfs;
    double integrated_lkfs;
    int has_true_peak_dbtp;
    double true_peak_dbtp;
    int has_loudness_range_lu;
    double loudness_range_lu;
} iclforge_ac4_loudness_info_t;

/* Of the presentation the last decode() call selected; every has_* is 0
 * before any frame has sent loudness metadata. */
ICLFORGE_C_EXPORT iclforge_ac4_loudness_info_t iclforge_ac4_decoder_metadata_loudness(
    const iclforge_ac4_decoder_t* decoder);

/* --------------------------------------------------------------------- *
 * AC-4 encoder (iclforge::ac4::Encoder)
 * --------------------------------------------------------------------- */

/* Mirrors iclforge::ac4::CodecMode (Part 1 clause 4.3.6.1, Part 2 clause 6.3.5.1). */
typedef enum iclforge_ac4_codec_mode {
    ICLFORGE_AC4_CODEC_AUTO = 0,
    ICLFORGE_AC4_CODEC_SIMPLE = 1,
    ICLFORGE_AC4_CODEC_ASPX = 2,
    ICLFORGE_AC4_CODEC_ASPX_ACPL1 = 3,
    ICLFORGE_AC4_CODEC_ASPX_ACPL2 = 4,
    ICLFORGE_AC4_CODEC_ASPX_ACPL3 = 5,
    ICLFORGE_AC4_CODEC_SCPL = 6,
    ICLFORGE_AC4_CODEC_ASPX_SCPL = 7,
    ICLFORGE_AC4_CODEC_ASPX_AJCC = 8
} iclforge_ac4_codec_mode_t;

/* Mirrors iclforge::ac4::RateMode (Part 1 Table 81's wait_frames). */
typedef enum iclforge_ac4_rate_mode {
    ICLFORGE_AC4_RATE_CONSTANT = 0,
    ICLFORGE_AC4_RATE_AVERAGE = 1,
    ICLFORGE_AC4_RATE_VARIABLE = 2
} iclforge_ac4_rate_mode_t;

/* --- objects: the encoder's object substream (iclforge::ac4::ObjectsConfig) --------- *
 *
 * Object audio (Part 2 clause 4.8.3.4): each object is one input channel of
 * PCM, and its metadata (iclforge_ac4_object_properties_t) is what the
 * decoder reports back. It is written behind experimental.objects, which no
 * reader outside this project has read from this encoder. The stream is one
 * object substream in one presentation of it, at frame_rate_index 13 (the
 * 2 048-sample frame) and no other, with no dialogue enhancement; the limits
 * below are the encoder's, and iclforge_ac4_encoder_refusal_reason() names
 * the rule a configuration breaks:
 *   - 1 to ICLFORGE_AC4_MAX_OBJECTS objects, at most one of them the LFE, and
 *     at least one that is not;
 *   - as A-JOC (the default), a computed downmix of downmix_signals signals,
 *     1 to ICLFORGE_AC4_MAX_DOWNMIX_SIGNALS and no more than the full-band
 *     objects, or a static 5.0 bed (no LFE object) or 5.1 bed (with one), and
 *     parameter_bands one of 23, 15, 12, 9, 7, 5, 3 or 1;
 *   - direct-coded, dynamic objects and the LFE only: no bed objects;
 *   - a codec mode of AUTO, SIMPLE or ASPX. */

/* The most objects one object substream takes. */
#define ICLFORGE_AC4_MAX_OBJECTS 64
/* The most downmix signals a computed A-JOC downmix takes. */
#define ICLFORGE_AC4_MAX_DOWNMIX_SIGNALS 11

/* Mirrors iclforge::ac4::BedChannel: the loudspeaker a bed object plays from, Part 2
 * Table 66's nonstd_bed_channel_assignment, whose code each value is. */
typedef enum iclforge_ac4_bed_channel {
    ICLFORGE_AC4_BED_LEFT = 0,
    ICLFORGE_AC4_BED_RIGHT = 1,
    ICLFORGE_AC4_BED_CENTRE = 2,
    ICLFORGE_AC4_BED_LEFT_SURROUND = 4,
    ICLFORGE_AC4_BED_RIGHT_SURROUND = 5,
    ICLFORGE_AC4_BED_LEFT_BACK = 6,
    ICLFORGE_AC4_BED_RIGHT_BACK = 7,
    ICLFORGE_AC4_BED_TOP_FRONT_LEFT = 8,
    ICLFORGE_AC4_BED_TOP_FRONT_RIGHT = 9,
    ICLFORGE_AC4_BED_TOP_SIDE_LEFT = 10,
    ICLFORGE_AC4_BED_TOP_SIDE_RIGHT = 11,
    ICLFORGE_AC4_BED_TOP_BACK_LEFT = 12,
    ICLFORGE_AC4_BED_TOP_BACK_RIGHT = 13,
    ICLFORGE_AC4_BED_LEFT_WIDE = 14,
    ICLFORGE_AC4_BED_RIGHT_WIDE = 15
} iclforge_ac4_bed_channel_t;

/* Mirrors iclforge::ac4::ObjectCoding: how the objects are coded. */
typedef enum iclforge_ac4_object_coding {
    /* An A-JOC substream (Part 2 clause 5.7): a downmix coded in a
     * var_channel_element() or a static 5.X bed, and the matrices that
     * rebuild the objects from it. */
    ICLFORGE_AC4_OBJECT_CODING_AJOC = 0,
    /* Direct-coded object substreams (clause 6.2.1.11): the objects coded as
     * channels of Part 1's elements, with the group's OAMD substream. */
    ICLFORGE_AC4_OBJECT_CODING_DIRECT = 1
} iclforge_ac4_object_coding_t;

/* Mirrors iclforge::ac4::AjocDownmix: A-JOC's downmix, which Part 2 leaves to the
 * encoder. */
typedef enum iclforge_ac4_ajoc_downmix {
    /* Downmix signals the encoder computes: the objects in groups by where
     * they start, in the order of their azimuth, each signal the sum of its
     * group's objects, sent as a dynamic object at the group's centre for core
     * decoding. */
    ICLFORGE_AC4_AJOC_DOWNMIX_COMPUTED = 0,
    /* A static bed (b_static_dmx): the objects panned onto L, R, C, Ls and Rs
     * by X and Y, and with STATIC_51 the LFE object onto the LFE. */
    ICLFORGE_AC4_AJOC_DOWNMIX_STATIC_50 = 1,
    ICLFORGE_AC4_AJOC_DOWNMIX_STATIC_51 = 2
} iclforge_ac4_ajoc_downmix_t;

/* Mirrors iclforge::ac4::AdditionalPair (Part 1 Table 88): the 7.X element's pair
 * beyond L, R, C, Ls and Rs. */
typedef enum iclforge_ac4_additional_pair {
    ICLFORGE_AC4_PAIR_NONE = 0,
    ICLFORGE_AC4_PAIR_BACK = 1,     /* 3/4/0: Lb and Rb */
    ICLFORGE_AC4_PAIR_WIDE = 2,     /* 5/2/0: Lw and Rw */
    ICLFORGE_AC4_PAIR_TOP_FRONT = 3 /* 3/2/2: Tfl and Tfr */
} iclforge_ac4_additional_pair_t;

/* Mirrors iclforge::ac4::ObjectConfig: one object, the input channel at its index. */
typedef struct iclforge_ac4_object_config {
    /* A bed object from the loudspeaker `bed` where has_bed is non-zero; a
     * dynamic object where it is 0. */
    int has_bed;
    iclforge_ac4_bed_channel_t bed;
    /* The LFE, at most one object's: its bed channel and position are
     * ignored. */
    int lfe;
    /* What is in force from the first sample. */
    iclforge_ac4_object_properties_t properties;
} iclforge_ac4_object_config_t;

/* A dynamic object at iclforge::ac4::ObjectConfig{}'s defaults: the room's centre,
 * unity gain, properties as iclforge_ac4_object_properties_init() gives
 * them. */
ICLFORGE_C_EXPORT void iclforge_ac4_object_config_init(iclforge_ac4_object_config_t* config);

/* Mirrors iclforge::ac4::ObjectsConfig: the objects and how they are coded. Call
 * iclforge_ac4_objects_config_init() first. The struct and the array it points
 * to are read only while iclforge_ac4_encoder_create() and
 * iclforge_ac4_encoder_refusal_reason() run. */
typedef struct iclforge_ac4_objects_config {
    /* `object_count` entries, one per input channel of encode. */
    const iclforge_ac4_object_config_t* objects;
    size_t object_count;
    iclforge_ac4_object_coding_t coding;
    iclforge_ac4_ajoc_downmix_t downmix;
    /* A computed downmix's signals, where has_downmix_signals is non-zero;
     * otherwise one a 32 kbps of the substream's rate, up to 10. */
    int has_downmix_signals;
    int downmix_signals;
    /* A-JOC's decorrelators (Part 2 clause 5.7.3.5): each object's share of
     * what the downmix does not rebuild, sent as a decorrelated signal. */
    int decorrelation;
    /* The parameter bands A-JOC's matrices take (Table 78: 23, 15, 12, 9, 7,
     * 5, 3 or 1) and whether they are quantised coarsely, where the has_ flag
     * is non-zero; otherwise 23 fine from 64 kbps a downmix signal, 15 fine
     * from 32 and 12 coarse below. */
    int has_parameter_bands;
    int parameter_bands;
    int has_coarse;
    int coarse;
    /* oamd_common_data(): master_screen_size_ratio_code (0 to 31, where the
     * has_ flag is non-zero; otherwise b_default_screen_size_ratio) and
     * b_bed_object_chan_distribute. Sent where either is set. */
    int has_screen_size_ratio_code;
    int screen_size_ratio_code;
    int bed_object_chan_distribute;
} iclforge_ac4_objects_config_t;

ICLFORGE_C_EXPORT void iclforge_ac4_objects_config_init(iclforge_ac4_objects_config_t* config);

/* Mirrors iclforge::ac4::EncoderConfig::Experimental: syntax only this project's readers
 * have read from this encoder, off unless asked for (planning/ac4.md, "What
 * the encoder writes by default"). Not mirrored: drc_gains and three_zero,
 * which need the DRC modes and the substream list this struct does not
 * carry. */
typedef struct iclforge_ac4_experimental {
    int aspx_balance;    /* the ASPX mode's pairs as sum and balance where that is fewer bits */
    int aspx_varvar;     /* the ASPX mode's VARVAR framing */
    int aspx_interleave; /* frequency interleaved waveform coding above the crossover */
    int coding_configs;  /* the 5.X and 7.X elements' coding_config 1 to 3 and 2ch_mode 1 */
    iclforge_ac4_additional_pair_t
        seven_x;   /* 7 or 8 input channels, with this pair beyond L R C Ls Rs */
    int acpl;      /* the A-CPL modes DEE's streams do not use (ASPX_ACPL_1, stereo ACPL) */
    int back_pair; /* 7.0.4 and 7.1.4 with the back pair: 11 or 12 input channels */
    int ajcc;      /* the immersive element's ASPX_AJCC */
    int objects;   /* object audio: required by a non-NULL objects configuration */
} iclforge_ac4_experimental_t;

/* Mirrors iclforge::ac4::EncoderConfig: one substream in one presentation, channel-based
 * or channel-based-immersive input, or - with `objects` - one object
 * substream. Not mirrored here, as iclforge_eac3_frame_config_t's own comment
 * leaves its broader metadata surface for the same reason: the loudness, DRC,
 * downmix and dialogue-enhancement metadata groups (EncoderConfig::loudness,
 * drc, downmix, dialogue), several substreams and presentations
 * (EncoderConfig::substreams, presentations), EMDF payloads and the syntax
 * trace. A config left at these defaults writes DEE's own shape for the
 * channel count given (planning/ac4.md, "What the encoder writes by
 * default"). Call iclforge_ac4_encoder_config_init() first so every field this
 * struct doesn't set explicitly carries the same default EncoderConfig{}
 * does. The struct and the arrays it points to are read only while
 * iclforge_ac4_encoder_create() and iclforge_ac4_encoder_refusal_reason()
 * run. */
typedef struct iclforge_ac4_encoder_config {
    int channels; /* 1, 2, 5, 6, 9 or 10 - see EncoderConfig::channels; ignored with `objects` */
    int sample_rate_hz; /* 48000, or 44100 (frame_rate_index 13 only) */
    int frame_rate_index; /* Part 1 Table 83/84; default 13, the 2048-sample frame */
    int bitrate_kbps;
    iclforge_ac4_rate_mode_t rate_mode;
    iclforge_ac4_codec_mode_t codec_mode; /* with `objects`, the object substream's */
    int iframe_interval;
    double dialnorm_db;
    /* `iframe_count` frames, counted from 0, that must be I-frames besides
     * those iframe_interval makes: EncoderConfig::iframes. NULL for none. */
    const int64_t* iframes;
    size_t iframe_count;
    /* Where the caller's fragments start, in samples of the decoded output
     * from its first (the media time an MP4 track counts): the frame whose
     * output starts there, or the first to start after it, is an I-frame -
     * EncoderConfig::fragment_starts. NULL for none. */
    const int64_t* fragment_starts;
    size_t fragment_start_count;
    iclforge_ac4_experimental_t experimental;
    /* NULL for channel-based content; otherwise the stream is one object
     * substream of these objects, and requires experimental.objects. */
    const iclforge_ac4_objects_config_t* objects;
} iclforge_ac4_encoder_config_t;

ICLFORGE_C_EXPORT void iclforge_ac4_encoder_config_init(iclforge_ac4_encoder_config_t* config);

/* Why iclforge_ac4_encoder_create() refuses `config`: a string literal naming
 * the first rule it breaks, such as "objects at a frame_rate_index other than
 * 13"; empty ("") where it makes an encoder of it. For a NULL config, or one
 * that is not a valid argument (create() then returns
 * ICLFORGE_ERROR_INVALID_ARGUMENT), it says so instead. It does create()'s
 * work to find out - iclforge::ac4::Encoder::refusal_reason(). A library built without
 * AC-4 says that. The pointer is to library-owned storage valid for the
 * process lifetime. */
ICLFORGE_C_EXPORT const char* iclforge_ac4_encoder_refusal_reason(
    const iclforge_ac4_encoder_config_t* config);

typedef struct iclforge_ac4_encoder iclforge_ac4_encoder_t;

/* Fails with ICLFORGE_ERROR_AC4_ENCODE_INVALID_CONFIG for a configuration
 * outside what the encoder writes, or whose rate cannot hold its least
 * frame - iclforge::ac4::Encoder::create() - and with ICLFORGE_ERROR_INVALID_ARGUMENT for
 * a NULL pointer where an array has entries, or an enumerator outside its
 * enumeration. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_ac4_encoder_create(
    const iclforge_ac4_encoder_config_t* config, iclforge_ac4_encoder_t** out_encoder);
ICLFORGE_C_EXPORT void iclforge_ac4_encoder_destroy(iclforge_ac4_encoder_t* encoder);

/* The codec mode the stream is actually coded in - what ICLFORGE_AC4_CODEC_AUTO
 * resolved to; never AUTO. */
ICLFORGE_C_EXPORT iclforge_ac4_codec_mode_t iclforge_ac4_encoder_codec_mode(
    const iclforge_ac4_encoder_t* encoder);
/* Samples of silence the encoder puts before the input, at the input's rate -
 * iclforge::ac4::Encoder::delay_samples(). */
ICLFORGE_C_EXPORT int iclforge_ac4_encoder_delay_samples(const iclforge_ac4_encoder_t* encoder);
/* The delay iclforge_ac4_decoder_t adds on top, at the input's rate -
 * iclforge::ac4::Encoder::decoder_delay_samples(). */
ICLFORGE_C_EXPORT int iclforge_ac4_encoder_decoder_delay_samples(
    const iclforge_ac4_encoder_t* encoder);

/* One coded frame (iclforge::ac4::EncodedFrame): what an MP4 sample holds as it is, and
 * what iclforge_ac4_sync_frame() wraps for a raw .ac4 file or MPEG-2 TS. */
typedef struct iclforge_ac4_encoded_frame iclforge_ac4_encoded_frame_t;

ICLFORGE_C_EXPORT const uint8_t* iclforge_ac4_encoded_frame_data(
    const iclforge_ac4_encoded_frame_t* frame);
ICLFORGE_C_EXPORT size_t iclforge_ac4_encoded_frame_size(const iclforge_ac4_encoded_frame_t* frame);
/* PCM samples per channel this frame decodes to, at the input's rate. */
ICLFORGE_C_EXPORT int iclforge_ac4_encoded_frame_samples(const iclforge_ac4_encoded_frame_t* frame);
ICLFORGE_C_EXPORT int iclforge_ac4_encoded_frame_iframe(const iclforge_ac4_encoded_frame_t* frame);
ICLFORGE_C_EXPORT void iclforge_ac4_encoded_frame_destroy(iclforge_ac4_encoded_frame_t* frame);
/* Same array-plus-count convention as iclforge_decoded_substream_array_destroy()
 * above: destroys every non-NULL element in [0, count) and always frees the
 * array itself. */
ICLFORGE_C_EXPORT void iclforge_ac4_encoded_frame_array_destroy(
    iclforge_ac4_encoded_frame_t** frames, size_t count);

/* channels: `channel_count` pointers (must equal config.channels, or the
 * object count of an objects configuration), each to exactly
 * `samples_per_channel` planar samples nominally in [-1, 1), in
 * iclforge::ac4::Decoder's own channel order for that count. Any samples_per_channel
 * works, unlike encode_frame() elsewhere in this header, since the encoder
 * buffers input to its own frame length internally - see iclforge::ac4::Encoder::
 * encode()'s own comment. On success, *out_frames and *out_count receive the
 * frames this input completed (zero when the encoder's delay is still
 * filling); the caller must destroy the array with
 * iclforge_ac4_encoded_frame_array_destroy(). */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_ac4_encoder_encode(
    iclforge_ac4_encoder_t* encoder, const float* const* channels, size_t channel_count,
    size_t samples_per_channel, iclforge_ac4_encoded_frame_t*** out_frames, size_t* out_count);

/* Mirrors iclforge::ac4::ObjectMetadataUpdate: a change to an object's metadata, given
 * with the input it belongs to. From input sample `sample` of that call's
 * channels (0 its first, and any later one, past the call's own length too)
 * object `object` - an index into iclforge_ac4_objects_config_t::objects -
 * moves to `properties` over `ramp_samples` (0 to 2047, or 2048). The decoder
 * reports the update at the output sample its input sample comes out at
 * (iclforge_ac4_encoder_delay_samples() plus
 * iclforge_ac4_encoder_decoder_delay_samples() later), to within 32 samples.
 * The C++ struct's substream index is left out: the stream has one, index 0. */
typedef struct iclforge_ac4_object_metadata_update {
    size_t object;
    int64_t sample;
    int ramp_samples;
    iclforge_ac4_object_properties_t properties;
} iclforge_ac4_object_metadata_update_t;

/* Sample 0, ramp 0, object 0 and the properties
 * iclforge_ac4_object_properties_init() gives. */
ICLFORGE_C_EXPORT void iclforge_ac4_object_metadata_update_init(
    iclforge_ac4_object_metadata_update_t* update);

/* iclforge_ac4_encoder_encode() for an encoder with an objects configuration,
 * and the changes to the objects' metadata within this input or after it, in
 * any order: `objects` is `object_count` pointers (the configuration's object
 * count), each to `samples_per_object` samples of one object's PCM;
 * `updates` is `update_count` entries, and may be NULL when that is 0. An
 * update for an object the configuration lacks, before this input's first
 * sample or with a property off its range fails with
 * ICLFORGE_ERROR_AC4_ENCODE_INVALID_INPUT, and so does an encoder without an
 * object substream when given any update - iclforge::ac4::Encoder::encode()'s overload
 * with updates. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_ac4_encoder_encode_objects(
    iclforge_ac4_encoder_t* encoder, const float* const* objects, size_t object_count,
    size_t samples_per_object, const iclforge_ac4_object_metadata_update_t* updates,
    size_t update_count, iclforge_ac4_encoded_frame_t*** out_frames, size_t* out_count);

/* Ends the stream: pads to the end of the last frame and returns the frames
 * the delay still held, so a decoder's output covers every input sample. The
 * encoder takes no input after this - iclforge::ac4::Encoder::flush(). */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_ac4_encoder_flush(
    iclforge_ac4_encoder_t* encoder, iclforge_ac4_encoded_frame_t*** out_frames, size_t* out_count);

/* --- the table of contents, for the dac4 box (iclforge::ac4::Toc) ------------------ *
 *
 * An owned copy of the stream's table of contents as it stands after the
 * frames encoded so far - iclforge::ac4::Encoder::toc(). Everything below reads it
 * rather than raw bytes, matching iclforge::ac4::build_dac4() and neighbours' own
 * "read the already-parsed Toc" design (ac4/ac4.hpp's carriage comment). */
typedef struct iclforge_ac4_toc iclforge_ac4_toc_t;

ICLFORGE_C_EXPORT iclforge_status_t iclforge_ac4_encoder_toc(const iclforge_ac4_encoder_t* encoder,
                                                           iclforge_ac4_toc_t** out_toc);
ICLFORGE_C_EXPORT void iclforge_ac4_toc_destroy(iclforge_ac4_toc_t* toc);

/* The 'dac4' box payload (ac4_dsi_v1, Annex E.6, box header excluded) an ISO-
 * BMFF 'ac-4' sample entry carries - iclforge::ac4::build_dac4(). Empty
 * (iclforge_bytes_size() 0) where iclforge_ac4_dac4_refusal() names what the
 * table of contents holds that this cannot describe whole. */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_ac4_build_dac4(const iclforge_ac4_toc_t* toc,
                                                           iclforge_bytes_t** out_box);
/* Why iclforge_ac4_build_dac4() wrote nothing - library-owned storage valid
 * for the process lifetime; empty ("") where it describes every presentation
 * whole - iclforge::ac4::dac4_refusal(). */
ICLFORGE_C_EXPORT const char* iclforge_ac4_dac4_refusal(const iclforge_ac4_toc_t* toc);

/* TS 103 190-2 Table E.1: the media time scale an ISOBMFF track of the stream
 * counts in, and each sample's duration in it - iclforge::ac4::media_timing(). Returns 0
 * (out-parameters untouched) for a frame rate Table 83/84 does not define, 1
 * otherwise, same has-value convention as
 * iclforge_scanned_stream_uniform_access_unit_samples() above. */
ICLFORGE_C_EXPORT int iclforge_ac4_media_timing(const iclforge_ac4_toc_t* toc,
                                              uint32_t* out_timescale, uint32_t* out_sample_delta);
/* Samples per AC-4 frame at the stream's own sample rate - Table 84;
 * nullopt/0 for the 1000/1001-family frame rates, whose length alternates
 * from frame to frame (iclforge_ac4_media_timing() above gives the track a
 * time scale in which they have one) - iclforge::ac4::samples_per_frame(). Same
 * has-value convention as iclforge_ac4_media_timing(). */
ICLFORGE_C_EXPORT int iclforge_ac4_samples_per_frame(const iclforge_ac4_toc_t* toc,
                                                    uint32_t* out_samples);

/* --- sync-frame wrapping (iclforge::ac4::sync_frame) -------------------------------- *
 *
 * Part 2 Annex G.3.1's ac4_syncframe(): the sync word 0xAC40, or 0xAC41 and a
 * trailing crc_word (Annex G.4.2) when `crc` is set, then frame_size and
 * `raw_frame` - what a raw .ac4 file or MPEG-2 TS carries, as opposed to an
 * MP4 sample (iclforge_ac4_encoded_frame_data(), which is the raw frame
 * alone). */
ICLFORGE_C_EXPORT iclforge_status_t iclforge_ac4_sync_frame(const uint8_t* raw_frame,
                                                           size_t raw_frame_size, int crc,
                                                           iclforge_bytes_t** out_bytes);


#ifdef __cplusplus
} /* extern "C" */
#endif

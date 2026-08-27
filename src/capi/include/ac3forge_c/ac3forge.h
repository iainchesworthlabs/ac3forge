#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ac3forge_c/export.h"

/* ac3forge's C API — roadmap item F1: a stable, minimal C-callable surface
 * over the encode/decode core, for bindings and embedding by callers that
 * cannot or do not want to link C++23.
 *
 * Conventions used throughout:
 *   - Every handle type (ac3forge_encoder_t, ac3forge_decoded_frame_t, ...)
 *     is opaque; only pointers to it ever cross this header. Each has a
 *     matching _destroy function. Passing NULL to a _destroy function is a
 *     no-op, matching free()'s own convention.
 *   - Every fallible function returns ac3forge_status_t. AC3FORGE_OK is
 *     always zero, so `if (ac3forge_xxx(...) != AC3FORGE_OK)` and
 *     `if (status)` are equally correct.
 *   - A function that produces a variable-length or structured result writes
 *     an owned handle through an out-parameter (the pointee is left
 *     untouched on failure); the caller reads it through accessor functions
 *     and then destroys it. Nothing is returned through raw caller-supplied
 *     buffers, so no accessor here requires the caller to predict a size in
 *     advance.
 *   - This library has no ABI-compatibility promise before v1.0 (see
 *     roadmap item F5): a rebuild against a newer ac3forge may require a
 *     recompile, not merely a relink. ac3forge_version() reports what was
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

/* Mirrors ac3::FrameError (encode side) and ac3::DecodeError (decode side)
 * one-for-one, plus a handful of codes this layer itself can raise. Grouped
 * with gaps between groups so a future addition to either C++ enum gets its
 * own number without renumbering anything already shipped. */
typedef enum ac3forge_status {
    AC3FORGE_OK = 0,

    AC3FORGE_ERROR_INVALID_ARGUMENT = 1,
    AC3FORGE_ERROR_OUT_OF_MEMORY = 2,
    AC3FORGE_ERROR_INTERNAL = 3, /* an exception crossed the C boundary; see docs/library/c-api.md */

    /* ac3::FrameError — FrameEncoder::encode_frame(), AtmosEncoder::encode_frame() */
    AC3FORGE_ERROR_ENCODE_INVALID_BITRATE = 10,
    AC3FORGE_ERROR_ENCODE_INVALID_DIALNORM = 11,
    AC3FORGE_ERROR_ENCODE_INVALID_SUBSTREAM = 12,
    AC3FORGE_ERROR_ENCODE_INVALID_CHANNEL_MAP = 13,
    AC3FORGE_ERROR_ENCODE_TOO_MANY_CHANNELS = 14,
    AC3FORGE_ERROR_ENCODE_INVALID_MIX_LEVEL = 15,
    AC3FORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO = 16,
    AC3FORGE_ERROR_ENCODE_INVALID_BSI = 17,

    /* ac3::DecodeError — FrameDecoder::decode_frame(), Eac3Decoder::decode_substream()/decode_access_unit() */
    AC3FORGE_ERROR_DECODE_TRUNCATED = 30,
    AC3FORGE_ERROR_DECODE_BAD_SYNC_WORD = 31,
    AC3FORGE_ERROR_DECODE_BAD_CRC = 32,
    AC3FORGE_ERROR_DECODE_RESERVED_VALUE = 33,
    AC3FORGE_ERROR_DECODE_UNSUPPORTED = 34,
    AC3FORGE_ERROR_DECODE_INVALID_STREAM = 35,

    /* ac3::io::ScanError — ac3forge_scan() only; split_frames()/split_access_units()/
     * stream_bsid() stay on ac3::DecodeError above, since ac3::io::scan() is the
     * only entry point in this header built on ac3::io's own error type. */
    AC3FORGE_ERROR_SCAN_EMPTY = 50,
    AC3FORGE_ERROR_SCAN_LOST_SYNC = 51,
    AC3FORGE_ERROR_SCAN_UNSUPPORTED_BSID = 52,
    AC3FORGE_ERROR_SCAN_RESERVED_VALUE = 53,
    AC3FORGE_ERROR_SCAN_TRUNCATED = 54,
    AC3FORGE_ERROR_SCAN_UNSUPPORTED_STRUCTURE = 55
} ac3forge_status_t;

/* A short, static, human-readable description of `status` — e.g. for a log
 * line. The returned pointer is to library-owned storage valid for the
 * process lifetime; never free() it. */
AC3FORGEC_EXPORT const char* ac3forge_status_message(ac3forge_status_t status);

/* --------------------------------------------------------------------- *
 * Version
 * --------------------------------------------------------------------- */

/* Tagged ac3forge_version_info rather than ac3forge_version: GCC's -Wshadow
 * (built C++) treats a struct tag and a same-named free function as the
 * function "hiding" the tag's implicit constructor-like name. The typedef
 * name below is what callers actually use. */
typedef struct ac3forge_version_info {
    int major;
    int minor;
    int patch;
    /* Semver plus prerelease suffix (e.g. "0.8.0-beta.1"), library-owned
     * storage valid for the process lifetime — mirrors ac3::version_full. */
    const char* full;
} ac3forge_version_t;

AC3FORGEC_EXPORT ac3forge_version_t ac3forge_version(void);

/* --------------------------------------------------------------------- *
 * Shared enums (ac3::SampleRate, ac3::Acmod)
 * --------------------------------------------------------------------- */

/* Ordinals match ac3::SampleRate exactly (A/52 Table 5.6 fscod, plus the
 * three Annex E fscod2 reduced rates). */
typedef enum ac3forge_sample_rate {
    AC3FORGE_SAMPLE_RATE_48000 = 0,
    AC3FORGE_SAMPLE_RATE_44100 = 1,
    AC3FORGE_SAMPLE_RATE_32000 = 2,
    AC3FORGE_SAMPLE_RATE_24000 = 3, /* E-AC-3 only */
    AC3FORGE_SAMPLE_RATE_22050 = 4, /* E-AC-3 only */
    AC3FORGE_SAMPLE_RATE_16000 = 5  /* E-AC-3 only */
} ac3forge_sample_rate_t;

/* Ordinals match ac3::Acmod exactly (A/52 Table 5.8). kDualMono (0) is 1+1:
 * two independent programmes sharing one syncframe, not a channel count. */
typedef enum ac3forge_acmod {
    AC3FORGE_ACMOD_DUAL_MONO = 0, /* 1+1: Ch1, Ch2 */
    AC3FORGE_ACMOD_1_0 = 1,       /* C */
    AC3FORGE_ACMOD_2_0 = 2,       /* L, R */
    AC3FORGE_ACMOD_3_0 = 3,       /* L, C, R */
    AC3FORGE_ACMOD_2_1 = 4,       /* L, R, S */
    AC3FORGE_ACMOD_3_1 = 5,       /* L, C, R, S */
    AC3FORGE_ACMOD_2_2 = 6,       /* L, R, SL, SR */
    AC3FORGE_ACMOD_3_2 = 7        /* L, C, R, SL, SR */
} ac3forge_acmod_t;

/* One audio block is always 256 samples (A/52 §4.1); one syncframe is always
 * six blocks. Exposed as constants because encode_frame()'s channel spans
 * and decoded-frame accessors are both sized against them. */
#define AC3FORGE_SAMPLES_PER_BLOCK 256
#define AC3FORGE_BLOCKS_PER_FRAME 6
#define AC3FORGE_SAMPLES_PER_FRAME 1536

/* Every AC-3 layout codes at most 5 full-bandwidth channels plus LFE - the
 * span count ac3forge_decoder_decode_frame_into() always requires,
 * regardless of what a given frame actually codes, since that is not known
 * until the frame's own header is parsed. */
#define AC3FORGE_DECODER_MAX_CHANNELS 6

/* --------------------------------------------------------------------- *
 * Latency (roadmap PF6)
 * --------------------------------------------------------------------- */

/* Mirrors ac3::LatencyBudget: the ALGORITHMIC delay of an encode -> decode
 * chain, in samples at the coded sample rate. Compute time is a separate
 * question (docs/performance-trend.md); transport, device buffers and
 * resampling are the integrator's own to add.
 *
 *   frame_samples      Input granularity. Nothing leaves the encoder until a
 *                      whole frame has gone in.
 *   transform_samples  The MDCT/IMDCT overlap. This is the one term that is a
 *                      sample-domain SHIFT: decoded output sample k is input
 *                      sample k - transform_samples. AC3FORGE_SAMPLES_PER_BLOCK
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
 * numbers and tests/decoder/test_latency.cpp for how they were measured. */
typedef struct ac3forge_latency {
    int frame_samples;
    int transform_samples;
    int lookahead_samples;
    int holdback_samples;
} ac3forge_latency_t;

/* The figure to budget with: the sum of the four terms above. No sample
 * entering the encoder is delayed by more than this many samples before the
 * matching decoded sample leaves the decoder. NULL returns 0. */
AC3FORGEC_EXPORT int ac3forge_latency_total_samples(const ac3forge_latency_t* latency);

/* Milliseconds for a sample count at a coded rate — e.g. 1792 samples at
 * AC3FORGE_SAMPLE_RATE_48000 is 37.33 ms. */
AC3FORGEC_EXPORT double ac3forge_latency_ms(int samples, ac3forge_sample_rate_t sample_rate);

/* Table 5.9 (§5.4.2.4) / Table 5.10 (§5.4.2.5). */
typedef enum ac3forge_centre_mix_level {
    AC3FORGE_CMIXLEV_MINUS_3DB = 0,
    AC3FORGE_CMIXLEV_MINUS_4_5DB = 1,
    AC3FORGE_CMIXLEV_MINUS_6DB = 2
} ac3forge_centre_mix_level_t;

typedef enum ac3forge_surround_mix_level {
    AC3FORGE_SURMIXLEV_MINUS_3DB = 0,
    AC3FORGE_SURMIXLEV_MINUS_6DB = 1,
    AC3FORGE_SURMIXLEV_SILENT = 2
} ac3forge_surround_mix_level_t;

/* The five conventional Dolby DRC curves (ac3::meta::ProfileId) — the same
 * named presets ac3cli's own --drc flag accepts. The full custom
 * ac3::meta::Profile curve (attack/release timing, boost ratios, ...) is an
 * internal tuning knob, not part of this minimal stable surface. */
typedef enum ac3forge_drc_profile {
    AC3FORGE_DRC_FILM_STANDARD = 0,
    AC3FORGE_DRC_FILM_LIGHT = 1,
    AC3FORGE_DRC_MUSIC_STANDARD = 2,
    AC3FORGE_DRC_MUSIC_LIGHT = 3,
    AC3FORGE_DRC_SPEECH = 4
} ac3forge_drc_profile_t;

/* ac3::meta::HeavyConfig verbatim (§7.7.2) — small enough, and specific
 * enough per-field, to expose directly rather than behind a preset. */
typedef struct ac3forge_heavy_config {
    double dialogue_target_dbfs; /* default -20.0 */
    double peak_ceiling_dbfs;    /* default -0.5 */
    double release_db_per_second; /* default 20.0 */
} ac3forge_heavy_config_t;

AC3FORGEC_EXPORT void ac3forge_heavy_config_init(ac3forge_heavy_config_t* config);

/* --------------------------------------------------------------------- *
 * AC-3 encoder (ac3::FrameEncoder)
 * --------------------------------------------------------------------- */

typedef struct ac3forge_encoder ac3forge_encoder_t;

/* Mirrors ac3::EncoderConfig. `has_*` flags stand in for std::optional<T>,
 * since C has no direct equivalent — the paired field is read only when its
 * flag is non-zero. Call ac3forge_encoder_config_init() first so every field
 * this struct doesn't set explicitly carries the same default EncoderConfig{}
 * does; a zero-initialized struct is NOT equivalent (e.g. dialnorm 0 is
 * invalid — §5.4.2.8 reserves it — where EncoderConfig's real default is 31). */
typedef struct ac3forge_encoder_config {
    ac3forge_sample_rate_t sample_rate;
    uint32_t bitrate_kbps;
    int dialnorm; /* 1..31, §5.4.2.8 */

    int has_dialnorm2; /* dual mono (acmod == AC3FORGE_ACMOD_DUAL_MONO) only */
    int dialnorm2;

    int chbwcod; /* 0..60, or -1 for auto-from-bitrate */
    ac3forge_acmod_t acmod;
    int lfe;
    int coupling;
    int cplbegf; /* -1 = auto */
    int cplendf; /* -1 = auto */
    int fast_mdct;

    int has_drc;
    ac3forge_drc_profile_t drc_profile;
    int has_heavy;
    ac3forge_heavy_config_t heavy;

    /* Ch2's own DRC/heavy — dual mono only, no fallback to the above. */
    int has_drc2;
    ac3forge_drc_profile_t drc2_profile;
    int has_heavy2;
    ac3forge_heavy_config_t heavy2;

    ac3forge_centre_mix_level_t cmixlev;
    ac3forge_surround_mix_level_t surmixlev;
} ac3forge_encoder_config_t;

/* Fills `config` with the same defaults as ac3::EncoderConfig{}. */
AC3FORGEC_EXPORT void ac3forge_encoder_config_init(ac3forge_encoder_config_t* config);

AC3FORGEC_EXPORT ac3forge_status_t ac3forge_encoder_create(const ac3forge_encoder_config_t* config,
                                                         ac3forge_encoder_t** out_encoder);
AC3FORGEC_EXPORT void ac3forge_encoder_destroy(ac3forge_encoder_t* encoder);

/* Full-bandwidth channels (per config.acmod) plus, when config.lfe is set,
 * the LFE channel last — the same count encode_frame() below expects. */
AC3FORGEC_EXPORT size_t ac3forge_encoder_channel_count(const ac3forge_encoder_t* encoder);

/* This encoder's latency budget. Constant for the encoder's whole life: no
 * field of ac3forge_encoder_config_t moves any term. `out_latency` is left
 * untouched when either pointer is NULL. */
AC3FORGEC_EXPORT void ac3forge_encoder_latency(const ac3forge_encoder_t* encoder,
                                              ac3forge_latency_t* out_latency);

/* The same budget's total — the single number an engine or conferencing
 * integrator asks for. 1792 samples (37.33 ms at 48 kHz) for every AC-3
 * configuration. NULL returns 0. */
AC3FORGEC_EXPORT int ac3forge_encoder_latency_samples(const ac3forge_encoder_t* encoder);

/* An owned, immutable byte buffer — the result type for every function here
 * that produces one encoded frame's worth of bytes. */
typedef struct ac3forge_bytes ac3forge_bytes_t;

AC3FORGEC_EXPORT const uint8_t* ac3forge_bytes_data(const ac3forge_bytes_t* bytes);
AC3FORGEC_EXPORT size_t ac3forge_bytes_size(const ac3forge_bytes_t* bytes);
AC3FORGEC_EXPORT void ac3forge_bytes_destroy(ac3forge_bytes_t* bytes);

/* channels: `channel_count` pointers (must equal
 * ac3forge_encoder_channel_count(encoder)), each to exactly
 * AC3FORGE_SAMPLES_PER_FRAME samples nominally in [-1, 1), in AC-3 channel
 * order (Table 5.8) with LFE last. On success, *out_frame receives one
 * complete syncframe; the caller must destroy it. *out_frame is left
 * untouched on failure. */
AC3FORGEC_EXPORT ac3forge_status_t ac3forge_encoder_encode_frame(ac3forge_encoder_t* encoder,
                                                               const float* const* channels,
                                                               size_t channel_count,
                                                               size_t samples_per_channel,
                                                               ac3forge_bytes_t** out_frame);

/* --------------------------------------------------------------------- *
 * AC-3 decoder (ac3::FrameDecoder)
 * --------------------------------------------------------------------- */

typedef struct ac3forge_decoder ac3forge_decoder_t;

/* Mirrors ac3::DecoderConfig. */
typedef struct ac3forge_decoder_config {
    double drc_scale;      /* 0.0..1.0, default 0.0 (§7.7.1's "Partial Compression") */
    int heavy_compression; /* default 0 */
} ac3forge_decoder_config_t;

AC3FORGEC_EXPORT void ac3forge_decoder_config_init(ac3forge_decoder_config_t* config);

AC3FORGEC_EXPORT ac3forge_status_t ac3forge_decoder_create(const ac3forge_decoder_config_t* config,
                                                         ac3forge_decoder_t** out_decoder);
AC3FORGEC_EXPORT void ac3forge_decoder_destroy(ac3forge_decoder_t* decoder);

/* The delay THIS decoder adds on top of the encoder's own budget. Always 0
 * for AC-3: decode_frame returns a frame's full PCM from the call that
 * supplies its bytes, and the IMDCT overlap those samples came from is
 * already the chain's transform term. Present so "encoder plus decoder" is a
 * sum a caller can write. */
AC3FORGEC_EXPORT int ac3forge_decoder_latency_samples(const ac3forge_decoder_t* decoder);

/* One decoded syncframe (ac3::DecodedFrame), read through the accessors
 * below. */
typedef struct ac3forge_decoded_frame ac3forge_decoded_frame_t;

/* `frame` must be exactly one syncframe, same precondition as
 * FrameDecoder::decode_frame(). On success, *out_frame receives the decode
 * result; the caller must destroy it. */
AC3FORGEC_EXPORT ac3forge_status_t ac3forge_decoder_decode_frame(ac3forge_decoder_t* decoder,
                                                               const uint8_t* frame,
                                                               size_t frame_size,
                                                               ac3forge_decoded_frame_t** out_frame);

/* As ac3forge_decoder_decode_frame, but the PCM lands in caller-owned planar
 * storage instead of an allocation this call would otherwise own - the C
 * mirror of FrameDecoder::decode_frame_into(), for a realtime embedder that
 * cannot allocate on the decode path. channels: exactly
 * AC3FORGE_DECODER_MAX_CHANNELS pointers, each exactly
 * AC3FORGE_SAMPLES_PER_FRAME samples - always six spans, regardless of what
 * this particular frame codes, since that is only known once its header is
 * parsed; a trailing span this frame's acmod/lfe do not need is left
 * untouched. On success, *out_frame carries every field decode_frame()
 * would EXCEPT the PCM itself (its channel_count() reports 0 - the samples
 * went to the caller's spans instead, in AC-3 channel order with LFE last).
 * On an error return the spans' contents are unspecified, exactly as
 * discarded as the value form's partial frame is. */
AC3FORGEC_EXPORT ac3forge_status_t ac3forge_decoder_decode_frame_into(
    ac3forge_decoder_t* decoder, const uint8_t* frame, size_t frame_size, float* const* channels,
    size_t channel_count, size_t samples_per_channel, ac3forge_decoded_frame_t** out_frame);

AC3FORGEC_EXPORT ac3forge_sample_rate_t ac3forge_decoded_frame_sample_rate(
    const ac3forge_decoded_frame_t* frame);
AC3FORGEC_EXPORT uint32_t ac3forge_decoded_frame_bitrate_kbps(const ac3forge_decoded_frame_t* frame);
AC3FORGEC_EXPORT ac3forge_acmod_t ac3forge_decoded_frame_acmod(const ac3forge_decoded_frame_t* frame);
AC3FORGEC_EXPORT int ac3forge_decoded_frame_lfe(const ac3forge_decoded_frame_t* frame);
AC3FORGEC_EXPORT int ac3forge_decoded_frame_dialnorm(const ac3forge_decoded_frame_t* frame);

AC3FORGEC_EXPORT int ac3forge_decoded_frame_has_compr(const ac3forge_decoded_frame_t* frame);
AC3FORGEC_EXPORT uint8_t ac3forge_decoded_frame_compr(const ac3forge_decoded_frame_t* frame);
/* block_index in [0, AC3FORGE_BLOCKS_PER_FRAME). */
AC3FORGEC_EXPORT uint8_t ac3forge_decoded_frame_dynrng(const ac3forge_decoded_frame_t* frame,
                                                     int block_index);

/* Ch2's words — meaningful only when acmod() == AC3FORGE_ACMOD_DUAL_MONO. */
AC3FORGEC_EXPORT int ac3forge_decoded_frame_has_dialnorm2(const ac3forge_decoded_frame_t* frame);
AC3FORGEC_EXPORT int ac3forge_decoded_frame_dialnorm2(const ac3forge_decoded_frame_t* frame);
AC3FORGEC_EXPORT int ac3forge_decoded_frame_has_compr2(const ac3forge_decoded_frame_t* frame);
AC3FORGEC_EXPORT uint8_t ac3forge_decoded_frame_compr2(const ac3forge_decoded_frame_t* frame);
AC3FORGEC_EXPORT uint8_t ac3forge_decoded_frame_dynrng2(const ac3forge_decoded_frame_t* frame,
                                                      int block_index);

AC3FORGEC_EXPORT size_t ac3forge_decoded_frame_channel_count(const ac3forge_decoded_frame_t* frame);
/* Always AC3FORGE_SAMPLES_PER_FRAME; exposed for a caller that would rather
 * not depend on the macro. */
AC3FORGEC_EXPORT size_t ac3forge_decoded_frame_samples_per_channel(
    const ac3forge_decoded_frame_t* frame);
/* channel_index in [0, channel_count()), AC-3 coded order (Table 5.8), LFE
 * last when lfe() is set. The returned pointer is valid until `frame` is
 * destroyed. */
AC3FORGEC_EXPORT const float* ac3forge_decoded_frame_channel_samples(
    const ac3forge_decoded_frame_t* frame, size_t channel_index);
/* True where that block used the short (block-switched) transform.
 * channel_index only ranges over the full-bandwidth channels (no LFE/coupling
 * entry — see DecodedFrame::blksw); out of range returns 0. */
AC3FORGEC_EXPORT int ac3forge_decoded_frame_block_switched(const ac3forge_decoded_frame_t* frame,
                                                         size_t channel_index, int block_index);

AC3FORGEC_EXPORT void ac3forge_decoded_frame_destroy(ac3forge_decoded_frame_t* frame);

/* --------------------------------------------------------------------- *
 * E-AC-3 encoder (ac3::eac3::FrameEncoder / AccessUnitEncoder)
 * --------------------------------------------------------------------- */

/* Mirrors ac3::eac3::StreamType (Table E1.2, §E2.3.1.2). This encoder only
 * ever emits kIndependent/kDependent; kConvertible/kReserved are accepted
 * here for a faithful mirror but never produced by anything below, the same
 * way ac3::eac3::FrameEncoder's own validate() refuses them. */
typedef enum ac3forge_stream_type {
    AC3FORGE_STREAM_TYPE_INDEPENDENT = 0,
    AC3FORGE_STREAM_TYPE_DEPENDENT = 1,
    AC3FORGE_STREAM_TYPE_CONVERTIBLE = 2,
    AC3FORGE_STREAM_TYPE_RESERVED = 3
} ac3forge_stream_type_t;

typedef struct ac3forge_eac3_encoder ac3forge_eac3_encoder_t;
typedef struct ac3forge_eac3_access_unit_encoder ac3forge_eac3_access_unit_encoder_t;

/* Mirrors ac3::eac3::FrameConfig's core surface - the fields needed to
 * produce a real E-AC-3 substream. `has_*` flags stand in for
 * std::optional<T>, same convention as ac3forge_encoder_config_t. Not
 * mirrored here: the mixmdate/infomdat metadata groups, dialnorm2/drc/heavy
 * (dual mono and DRC are exactly as useful on this side as on AC-3's, but
 * the broader Table E1.2 metadata surface is deliberately deferred - see
 * docs/library/c-api.md's "What is deliberately out of scope"), vbr and
 * numblkscod (CBR, six-block syncframes only), and the internal self-check
 * `trace` hook. Call ac3forge_eac3_frame_config_init() first so every field
 * this struct doesn't set explicitly carries the same default FrameConfig{}
 * does. */
typedef struct ac3forge_eac3_frame_config {
    ac3forge_sample_rate_t sample_rate; /* includes the 3 fscod2 reduced rates */
    uint32_t bitrate_kbps;
    int dialnorm; /* 1..31, §5.4.2.8 */
    ac3forge_acmod_t acmod;
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
     * several ac3forge_eac3_encoder_t instances.
     * ac3forge_eac3_access_unit_encoder_t assigns these itself the way
     * ac3::eac3::AccessUnitEncoder does, and does not read them from the
     * configs passed to it (see ac3::eac3::AccessUnitConfig's own comment) -
     * only chanmap/has_chanmap on a dependent matters there. */
    ac3forge_stream_type_t strmtyp;
    int substreamid;
    int has_chanmap; /* dependent substreams only */
    uint16_t chanmap; /* Table E2.5 bitmask; AC3FORGE_CHANMAP_* below name a few */
} ac3forge_eac3_frame_config_t;

AC3FORGEC_EXPORT void ac3forge_eac3_frame_config_init(ac3forge_eac3_frame_config_t* config);

/* A few of Table E2.5's chanmap combinations, matching
 * ac3::eac3::chanmap::k71Rear/k512Height/kTopQuad - what a dependent
 * substream needs to widen a 5.1 bed. See docs/library/encoding-eac3.md's
 * "Wide layouts" table. */
#define AC3FORGE_CHANMAP_71_REAR 0x1A00u    /* Ls, Rs, Lrs, Rrs -> 7.1 */
#define AC3FORGE_CHANMAP_512_HEIGHT 0x0010u /* Vhl, Vhr -> 5.1.2 */
/* Vhl, Vhr, Lts, Rts -> 5.1.4 (or 7.1.4 with 71_REAR in a second dependent) */
#define AC3FORGE_CHANMAP_TOP_QUAD 0x0014u

/* Mirrors ac3::eac3::FrameMetadata - the §7.7 words for one frame, shared
 * across every substream of one programme by
 * ac3forge_eac3_access_unit_encoder_encode() so they never disagree (see
 * ac3::eac3::FrameEncoder's own comment on why "measured per substream" and
 * "shared across substreams" give different answers). */
typedef struct ac3forge_eac3_frame_metadata {
    uint8_t dynrng[AC3FORGE_BLOCKS_PER_FRAME];
    int has_compr;
    uint8_t compr;
    /* Ch2's own words, meaningful only when acmod is AC3FORGE_ACMOD_DUAL_MONO. */
    uint8_t dynrng2[AC3FORGE_BLOCKS_PER_FRAME];
    int has_compr2;
    uint8_t compr2;
} ac3forge_eac3_frame_metadata_t;

/* All-zero dynrng (§7.7.1's "no change"), no compr - same defaults an
 * all-zero-initialized struct would already have, provided for symmetry with
 * every other _init() function here. */
AC3FORGEC_EXPORT void ac3forge_eac3_frame_metadata_init(ac3forge_eac3_frame_metadata_t* metadata);

AC3FORGEC_EXPORT ac3forge_status_t ac3forge_eac3_encoder_create(
    const ac3forge_eac3_frame_config_t* config, ac3forge_eac3_encoder_t** out_encoder);
AC3FORGEC_EXPORT void ac3forge_eac3_encoder_destroy(ac3forge_eac3_encoder_t* encoder);

/* Full-bandwidth channels (per config.acmod) plus, when config.lfe is set,
 * the LFE channel last - the same count encode_frame() below expects. */
AC3FORGEC_EXPORT size_t ac3forge_eac3_encoder_channel_count(const ac3forge_eac3_encoder_t* encoder);
/* Always AC3FORGE_SAMPLES_PER_FRAME today (numblkscod is not exposed above,
 * so every substream this API builds carries six blocks); exposed as its own
 * accessor rather than assumed so a caller never has to special-case this
 * encoder against the AC-3 one - ac3::eac3::FrameEncoder::samples_per_frame()
 * genuinely varies once a caller reaches numblkscod, even though nothing
 * here can ask for that yet. */
AC3FORGEC_EXPORT size_t ac3forge_eac3_encoder_samples_per_frame(
    const ac3forge_eac3_encoder_t* encoder);

AC3FORGEC_EXPORT void ac3forge_eac3_encoder_latency(const ac3forge_eac3_encoder_t* encoder,
                                                   ac3forge_latency_t* out_latency);
AC3FORGEC_EXPORT int ac3forge_eac3_encoder_latency_samples(const ac3forge_eac3_encoder_t* encoder);

/* channels: `channel_count` pointers (must equal
 * ac3forge_eac3_encoder_channel_count(encoder)), each to exactly
 * ac3forge_eac3_encoder_samples_per_frame(encoder) samples nominally in
 * [-1, 1), in AC-3 channel order (Table 5.8) with LFE last. `metadata`, when
 * non-NULL, supplies the §7.7 words explicitly (ac3::eac3::FrameEncoder's
 * second encode_frame() overload) instead of measuring them from `channels`
 * - the access-unit path needs this so every substream of one programme
 * agrees; NULL measures internally, matching a standalone stream. `aux`/
 * `aux_size` carry a caller-built EMDF container (ac3::emdf::build_container)
 * in the frame's aux data, or NULL/0 for none. On success, *out_frame
 * receives one complete syncframe; the caller must destroy it. */
AC3FORGEC_EXPORT ac3forge_status_t ac3forge_eac3_encoder_encode_frame(
    ac3forge_eac3_encoder_t* encoder, const float* const* channels, size_t channel_count,
    size_t samples_per_channel, const ac3forge_eac3_frame_metadata_t* metadata,
    const uint8_t* aux, size_t aux_size, ac3forge_bytes_t** out_frame);

/* --- wide layouts: ac3::eac3::AccessUnitEncoder ------------------------- */

/* An access unit's bytes plus per-substream boundaries - mirrors
 * ac3::eac3::AccessUnit. Unlike ac3forge_atmos_encoder_encode_frame() (which
 * always produces exactly one substream and so returns a plain
 * ac3forge_bytes_t), a general access-unit encoder can produce several, and a
 * caller re-deriving crc2 or demuxing substreams individually needs to know
 * where each one starts. */
typedef struct ac3forge_eac3_access_unit ac3forge_eac3_access_unit_t;

AC3FORGEC_EXPORT const uint8_t* ac3forge_eac3_access_unit_data(
    const ac3forge_eac3_access_unit_t* unit);
AC3FORGEC_EXPORT size_t ac3forge_eac3_access_unit_size(const ac3forge_eac3_access_unit_t* unit);
AC3FORGEC_EXPORT size_t ac3forge_eac3_access_unit_substream_count(
    const ac3forge_eac3_access_unit_t* unit);
/* Byte length of substream `index` (independent first); sums to
 * ac3forge_eac3_access_unit_size(). */
AC3FORGEC_EXPORT uint32_t ac3forge_eac3_access_unit_substream_bytes(
    const ac3forge_eac3_access_unit_t* unit, size_t index);
AC3FORGEC_EXPORT void ac3forge_eac3_access_unit_destroy(ac3forge_eac3_access_unit_t* unit);

/* `independent` is the bed's config; `dependents`/`dependent_count` are the
 * substreams that widen it (at most 8), in transmission order - see
 * ac3::eac3::AccessUnitConfig. Every substream must agree on sample_rate;
 * strmtyp/substreamid on `independent` and each of `dependents` are assigned
 * by this call the way ac3::eac3::AccessUnitEncoder's constructor does, so
 * whatever the caller set there is not read - only chanmap/has_chanmap on a
 * dependent matters (Table E2.5; AC3FORGE_CHANMAP_* above name a few
 * combinations). */
AC3FORGEC_EXPORT ac3forge_status_t ac3forge_eac3_access_unit_encoder_create(
    const ac3forge_eac3_frame_config_t* independent, const ac3forge_eac3_frame_config_t* dependents,
    size_t dependent_count, ac3forge_eac3_access_unit_encoder_t** out_encoder);
AC3FORGEC_EXPORT void ac3forge_eac3_access_unit_encoder_destroy(
    ac3forge_eac3_access_unit_encoder_t* encoder);

/* Summed across every substream - the span count encode() below expects. */
AC3FORGEC_EXPORT size_t ac3forge_eac3_access_unit_encoder_channel_count(
    const ac3forge_eac3_access_unit_encoder_t* encoder);

AC3FORGEC_EXPORT void ac3forge_eac3_access_unit_encoder_latency(
    const ac3forge_eac3_access_unit_encoder_t* encoder, ac3forge_latency_t* out_latency);
AC3FORGEC_EXPORT int ac3forge_eac3_access_unit_encoder_latency_samples(
    const ac3forge_eac3_access_unit_encoder_t* encoder);

/* channels: every channel of the access unit grouped by substream in
 * transmission order - the independent's first (AC-3 order, LFE last), then
 * each dependent's in the order its chanmap names them - channel_count()
 * spans total, each AC3FORGE_SAMPLES_PER_FRAME samples. NULL is accepted when
 * channel_count is 0, which happens when `independent`/`dependents` described
 * a config ac3::eac3::AccessUnitEncoder's constructor could not build any
 * substreams from (see ac3forge_eac3_access_unit_encoder_create()'s own
 * comment) - calling this then reports the real reason as a status code
 * rather than silently producing nothing. `aux`/`aux_size`: see
 * ac3forge_eac3_encoder_encode_frame(). On success, *out_unit receives the
 * whole access unit; the caller must destroy it. */
AC3FORGEC_EXPORT ac3forge_status_t ac3forge_eac3_access_unit_encoder_encode(
    ac3forge_eac3_access_unit_encoder_t* encoder, const float* const* channels,
    size_t channel_count, size_t samples_per_channel, const uint8_t* aux, size_t aux_size,
    ac3forge_eac3_access_unit_t** out_unit);

/* --------------------------------------------------------------------- *
 * E-AC-3 / Atmos decode (ac3::Eac3Decoder)
 * --------------------------------------------------------------------- */

typedef struct ac3forge_eac3_decoder ac3forge_eac3_decoder_t;

/* Table E2.5 caps one rendered programme (bed plus every dependent) at 16
 * channels (§E3.8.2) - the span count
 * ac3forge_eac3_decoder_decode_access_unit_into() always requires, for the
 * same "not known until parsed" reason as AC3FORGE_DECODER_MAX_CHANNELS. */
#define AC3FORGE_EAC3_DECODER_MAX_CHANNELS 16

AC3FORGEC_EXPORT ac3forge_status_t ac3forge_eac3_decoder_create(
    const ac3forge_decoder_config_t* config, ac3forge_eac3_decoder_t** out_decoder);
AC3FORGEC_EXPORT void ac3forge_eac3_decoder_destroy(ac3forge_eac3_decoder_t* decoder);

/* The delay THIS decoder adds, same contract as
 * ac3forge_decoder_latency_samples(). 0 until some substream's frame sets
 * transproce, AC3FORGE_SAMPLES_PER_FRAME from then on — §3.7's hold-back is a
 * property of the stream, not of the decoder, and once engaged it stays
 * engaged. A caller sizing buffers BEFORE the stream starts should ask the
 * encoder instead; this reports what has actually happened so far. */
AC3FORGEC_EXPORT int ac3forge_eac3_decoder_latency_samples(
    const ac3forge_eac3_decoder_t* decoder);

typedef struct ac3forge_decoded_substream ac3forge_decoded_substream_t;
typedef struct ac3forge_decoded_access_unit ac3forge_decoded_access_unit_t;

/* Same std::nullopt-via-out-parameter convention as the C++ API: a return of
 * AC3FORGE_OK with *out_substream left NULL means this frame's PCM is being
 * held back pending transient pre-noise processing (§3.7) — not an error.
 * Call ac3forge_eac3_decoder_flush() at end of stream to collect it. */
AC3FORGEC_EXPORT ac3forge_status_t ac3forge_eac3_decoder_decode_substream(
    ac3forge_eac3_decoder_t* decoder, const uint8_t* frame, size_t frame_size,
    ac3forge_decoded_substream_t** out_substream);

/* Same held-back convention as decode_substream, for the same reason (see
 * ac3::Eac3Decoder::decode_access_unit's own comment). `unit` must be
 * delimited exactly as ac3forge_split_access_units() would delimit it. */
AC3FORGEC_EXPORT ac3forge_status_t ac3forge_eac3_decoder_decode_access_unit(
    ac3forge_eac3_decoder_t* decoder, const uint8_t* unit, size_t unit_size,
    ac3forge_decoded_access_unit_t** out_unit);

/* As ac3forge_eac3_decoder_decode_access_unit, but the rendered programme's
 * PCM lands in caller-owned planar storage - the C mirror of
 * Eac3Decoder::decode_access_unit_into(). channels: exactly
 * AC3FORGE_EAC3_DECODER_MAX_CHANNELS pointers, each exactly
 * AC3FORGE_SAMPLES_PER_FRAME samples, written in the rendered layout's own
 * slot order (coded order for dual mono); a trailing span this programme
 * does not render is left untouched. Same held-back convention as the value
 * form: AC3FORGE_OK with *out_unit left NULL means the §3.7 hold-back - and
 * the spans are left untouched for that call too, not partially written,
 * because a held-back frame's PCM is buffered internally either way and
 * only copied out (to the caller's spans this time) at the call that
 * releases it. ac3forge_eac3_decoder_flush() is still the release path at
 * end of stream and still returns library-owned data even for a decoder
 * driven entirely through this form - there is no flush_into, since flush's
 * own per-substream results were never assembled into one programme to
 * begin with (see its own comment). On an error return the spans' contents
 * are unspecified. */
AC3FORGEC_EXPORT ac3forge_status_t ac3forge_eac3_decoder_decode_access_unit_into(
    ac3forge_eac3_decoder_t* decoder, const uint8_t* unit, size_t unit_size,
    float* const* channels, size_t channel_count, size_t samples_per_channel,
    ac3forge_decoded_access_unit_t** out_unit);

/* Releases whichever frames transient pre-noise processing is still holding
 * back. *out_substreams receives a library-owned array of *out_count owned
 * handles (each must still be individually destroyed); *out_count is 0 (and
 * *out_substreams NULL) for a stream that never used the tool. Free the array
 * itself with ac3forge_decoded_substream_array_destroy(). */
AC3FORGEC_EXPORT ac3forge_status_t ac3forge_eac3_decoder_flush(
    ac3forge_eac3_decoder_t* decoder, ac3forge_decoded_substream_t*** out_substreams,
    size_t* out_count);
AC3FORGEC_EXPORT void ac3forge_decoded_substream_array_destroy(
    ac3forge_decoded_substream_t** substreams, size_t count);

/* --- ac3::DecodedSubstream accessors ------------------------------------ */

AC3FORGEC_EXPORT int ac3forge_decoded_substream_is_independent(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_EXPORT int ac3forge_decoded_substream_id(const ac3forge_decoded_substream_t* substream);
AC3FORGEC_EXPORT ac3forge_sample_rate_t ac3forge_decoded_substream_sample_rate(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_EXPORT ac3forge_acmod_t ac3forge_decoded_substream_acmod(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_EXPORT int ac3forge_decoded_substream_lfe(const ac3forge_decoded_substream_t* substream);
AC3FORGEC_EXPORT int ac3forge_decoded_substream_dialnorm(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_EXPORT int ac3forge_decoded_substream_has_compr(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_EXPORT uint8_t ac3forge_decoded_substream_compr(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_EXPORT uint8_t ac3forge_decoded_substream_dynrng(
    const ac3forge_decoded_substream_t* substream, int block_index);
AC3FORGEC_EXPORT int ac3forge_decoded_substream_has_dialnorm2(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_EXPORT int ac3forge_decoded_substream_dialnorm2(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_EXPORT int ac3forge_decoded_substream_has_compr2(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_EXPORT uint8_t ac3forge_decoded_substream_compr2(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_EXPORT uint8_t ac3forge_decoded_substream_dynrng2(
    const ac3forge_decoded_substream_t* substream, int block_index);
AC3FORGEC_EXPORT int ac3forge_decoded_substream_numblkscod(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_EXPORT int ac3forge_decoded_substream_has_chanmap(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_EXPORT uint16_t ac3forge_decoded_substream_chanmap(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_EXPORT int ac3forge_decoded_substream_last_dependent(
    const ac3forge_decoded_substream_t* substream);
/* Table E2.5 location map this substream's channels occupy — chanmap() when
 * present, the acmod/lfe-derived map otherwise. */
AC3FORGEC_EXPORT uint16_t ac3forge_decoded_substream_location_map(
    const ac3forge_decoded_substream_t* substream);

AC3FORGEC_EXPORT size_t ac3forge_decoded_substream_channel_count(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_EXPORT size_t ac3forge_decoded_substream_samples_per_channel(
    const ac3forge_decoded_substream_t* substream);
/* channel_index in [0, channel_count()). The returned pointer is valid until `substream` is
 * destroyed, same convention as ac3forge_decoded_frame_channel_samples() above. */
AC3FORGEC_EXPORT const float* ac3forge_decoded_substream_channel_samples(
    const ac3forge_decoded_substream_t* substream, size_t channel_index);
AC3FORGEC_EXPORT int ac3forge_decoded_substream_block_switched(
    const ac3forge_decoded_substream_t* substream, size_t channel_index, int block_index);

/* --- object audio: OAMD (ac3::oba::DecodedProgram) + JOC reconstruction -- */

AC3FORGEC_EXPORT int ac3forge_decoded_substream_has_object_metadata(
    const ac3forge_decoded_substream_t* substream);
/* Below are only meaningful when has_object_metadata() is non-zero. */
AC3FORGEC_EXPORT int ac3forge_decoded_substream_program_dynamic_only(
    const ac3forge_decoded_substream_t* substream);
/* b_lfe_present — dynamic_only programmes only; see program_bed() for the
 * bed-instance branch's own LFE flag (bit AC3FORGE_BED_LFE). */
AC3FORGEC_EXPORT int ac3forge_decoded_substream_program_lfe(
    const ac3forge_decoded_substream_t* substream);
/* Table 12 bed-instance channel assignment bitmask (0 when dynamic_only). */
AC3FORGEC_EXPORT uint16_t ac3forge_decoded_substream_program_bed(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_EXPORT int ac3forge_decoded_substream_program_dynamic_object_count(
    const ac3forge_decoded_substream_t* substream);
/* object_index in [0, program_dynamic_object_count()). Position is §4.2.1's
 * room-anchored, left-handed, normalized-to-the-room-cuboid system: x in
 * [0, 1] left to right, y in [0, 1] front to back, z in [-1, 1] floor to
 * ceiling. gain_db is §5.6.1.4's object gain. */
AC3FORGEC_EXPORT void ac3forge_decoded_substream_dynamic_object(
    const ac3forge_decoded_substream_t* substream, int object_index, double* out_x, double* out_y,
    double* out_z, double* out_gain_db);

/* JOC's reconstructed per-object audio, parallel to the dynamic objects
 * above (same index = same object) — 0 when no JOC payload rode alongside
 * the OAMD one. Each waveform is samples_per_channel() samples long, and the returned pointer is
 * valid until `substream` is destroyed, same convention as
 * ac3forge_decoded_frame_channel_samples() above. */
AC3FORGEC_EXPORT size_t ac3forge_decoded_substream_object_audio_count(
    const ac3forge_decoded_substream_t* substream);
AC3FORGEC_EXPORT const float* ac3forge_decoded_substream_object_audio(
    const ac3forge_decoded_substream_t* substream, size_t object_index);

AC3FORGEC_EXPORT void ac3forge_decoded_substream_destroy(ac3forge_decoded_substream_t* substream);

/* --- Table 12 bed-instance channel assignment bits (program_bed()) ------ */
#define AC3FORGE_BED_LR (1u << 9)
#define AC3FORGE_BED_C (1u << 8)
#define AC3FORGE_BED_LFE (1u << 7)
#define AC3FORGE_BED_LS_RS (1u << 6)
#define AC3FORGE_BED_LB_RB (1u << 5)
#define AC3FORGE_BED_TFL_TFR (1u << 4)
#define AC3FORGE_BED_TSL_TSR (1u << 3)
#define AC3FORGE_BED_TBL_TBR (1u << 2)
#define AC3FORGE_BED_LW_RW (1u << 1)
#define AC3FORGE_BED_LFE2 (1u << 0)

/* --- ac3::eac3::Location (Table E2.5), used by location_map()/layout ---- */
typedef enum ac3forge_location {
    AC3FORGE_LOCATION_L = 0,
    AC3FORGE_LOCATION_C = 1,
    AC3FORGE_LOCATION_R = 2,
    AC3FORGE_LOCATION_LS = 3,
    AC3FORGE_LOCATION_RS = 4,
    AC3FORGE_LOCATION_LC = 5,
    AC3FORGE_LOCATION_RC = 6,
    AC3FORGE_LOCATION_LRS = 7,
    AC3FORGE_LOCATION_RRS = 8,
    AC3FORGE_LOCATION_CS = 9,
    AC3FORGE_LOCATION_TS = 10,
    AC3FORGE_LOCATION_LSD = 11,
    AC3FORGE_LOCATION_RSD = 12,
    AC3FORGE_LOCATION_LW = 13,
    AC3FORGE_LOCATION_RW = 14,
    AC3FORGE_LOCATION_VHL = 15,
    AC3FORGE_LOCATION_VHR = 16,
    AC3FORGE_LOCATION_VHC = 17,
    AC3FORGE_LOCATION_LTS = 18,
    AC3FORGE_LOCATION_RTS = 19,
    AC3FORGE_LOCATION_LFE2 = 20,
    AC3FORGE_LOCATION_LFE = 21
} ac3forge_location_t;

/* --- ac3::DecodedAccessUnit accessors ----------------------------------- */

/* Same held-back-frame convention as decode_substream/decode_access_unit
 * above; see ac3::Eac3Decoder::decode_access_unit's own comment. */
AC3FORGEC_EXPORT ac3forge_sample_rate_t ac3forge_decoded_access_unit_sample_rate(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_EXPORT ac3forge_acmod_t ac3forge_decoded_access_unit_acmod(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_EXPORT int ac3forge_decoded_access_unit_dialnorm(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_EXPORT int ac3forge_decoded_access_unit_has_compr(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_EXPORT uint8_t ac3forge_decoded_access_unit_compr(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_EXPORT uint8_t ac3forge_decoded_access_unit_dynrng(
    const ac3forge_decoded_access_unit_t* unit, int block_index);
AC3FORGEC_EXPORT int ac3forge_decoded_access_unit_numblkscod(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_EXPORT int ac3forge_decoded_access_unit_substream_count(
    const ac3forge_decoded_access_unit_t* unit);

/* The rendered programme, laid out in Table E2.5 location order — parallel
 * to layout() below, except for dual mono (see layout_count()'s own
 * comment). */
AC3FORGEC_EXPORT size_t ac3forge_decoded_access_unit_channel_count(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_EXPORT size_t ac3forge_decoded_access_unit_samples_per_channel(
    const ac3forge_decoded_access_unit_t* unit);
/* channel_index in [0, channel_count()). The returned pointer is valid until `unit` is
 * destroyed, same convention as ac3forge_decoded_frame_channel_samples() above. */
AC3FORGEC_EXPORT const float* ac3forge_decoded_access_unit_channel_samples(
    const ac3forge_decoded_access_unit_t* unit, size_t channel_index);

/* 0 for dual mono (acmod == AC3FORGE_ACMOD_DUAL_MONO): 1+1 has no Table
 * E2.5 layout, its two channels are unrelated programmes — see
 * ac3::DecodedAccessUnit::layout's own comment. Otherwise equal to
 * channel_count() above. */
AC3FORGEC_EXPORT size_t ac3forge_decoded_access_unit_layout_count(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_EXPORT ac3forge_location_t ac3forge_decoded_access_unit_layout_location(
    const ac3forge_decoded_access_unit_t* unit, size_t index);

AC3FORGEC_EXPORT int ac3forge_decoded_access_unit_has_object_metadata(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_EXPORT int ac3forge_decoded_access_unit_program_dynamic_only(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_EXPORT int ac3forge_decoded_access_unit_program_lfe(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_EXPORT uint16_t ac3forge_decoded_access_unit_program_bed(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_EXPORT int ac3forge_decoded_access_unit_program_dynamic_object_count(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_EXPORT void ac3forge_decoded_access_unit_dynamic_object(
    const ac3forge_decoded_access_unit_t* unit, int object_index, double* out_x, double* out_y,
    double* out_z, double* out_gain_db);
/* The returned pointer is valid until `unit` is destroyed, same convention as
 * ac3forge_decoded_frame_channel_samples() above. */
AC3FORGEC_EXPORT size_t ac3forge_decoded_access_unit_object_audio_count(
    const ac3forge_decoded_access_unit_t* unit);
AC3FORGEC_EXPORT const float* ac3forge_decoded_access_unit_object_audio(
    const ac3forge_decoded_access_unit_t* unit, size_t object_index);

AC3FORGEC_EXPORT void ac3forge_decoded_access_unit_destroy(ac3forge_decoded_access_unit_t* unit);

/* --------------------------------------------------------------------- *
 * Stream framing helpers (ac3::split_frames / split_access_units / stream_bsid)
 * --------------------------------------------------------------------- */

/* A library-owned array of (offset, length) spans into the SAME buffer the
 * caller passed to ac3forge_split_frames()/ac3forge_split_access_units() —
 * the caller must keep that buffer alive and unmodified for as long as this
 * result is in use. */
typedef struct ac3forge_span {
    size_t offset;
    size_t length;
} ac3forge_span_t;

typedef struct ac3forge_spans ac3forge_spans_t;

AC3FORGEC_EXPORT size_t ac3forge_spans_count(const ac3forge_spans_t* spans);
AC3FORGEC_EXPORT ac3forge_span_t ac3forge_spans_get(const ac3forge_spans_t* spans, size_t index);
AC3FORGEC_EXPORT void ac3forge_spans_destroy(ac3forge_spans_t* spans);

/* Splits a raw elementary stream into syncframes by sync word and declared
 * size. Handles both AC-3 and E-AC-3. */
AC3FORGEC_EXPORT ac3forge_status_t ac3forge_split_frames(const uint8_t* stream, size_t stream_size,
                                                       ac3forge_spans_t** out_spans);
/* Groups those syncframes into access units — a new one begins at each
 * independent substream. */
AC3FORGEC_EXPORT ac3forge_status_t ac3forge_split_access_units(const uint8_t* stream,
                                                             size_t stream_size,
                                                             ac3forge_spans_t** out_spans);
/* bsid at bit 40, without committing to either generation. Fails only if
 * `frame` is too short to hold a header. */
AC3FORGEC_EXPORT ac3forge_status_t ac3forge_stream_bsid(const uint8_t* frame, size_t frame_size,
                                                      int* out_bsid);

/* --------------------------------------------------------------------- *
 * Stream scan (ac3::io::scan / ac3::io::ScannedStream)
 * --------------------------------------------------------------------- */

/* split_frames()/split_access_units()/stream_bsid() above only delimit a
 * stream; ac3forge_scan() actually reads what it contains - sample rate,
 * layout, every programme it carries, the Annex G/DVB service fields a
 * muxer's descriptors want - without decoding any audio. Mirrors
 * ac3::io::scan()/ScannedStream. */

/* Mirrors ac3::io::StreamKind. */
typedef enum ac3forge_stream_kind {
    AC3FORGE_STREAM_KIND_AC3 = 0,   /* bsid <= 10 */
    AC3FORGE_STREAM_KIND_EAC3 = 1,  /* bsid 16 (Annex E) */
    /* §E2.3.1.2 legacy-core delivery: an AC-3 syncframe carrying the 5.1 bed,
     * immediately followed by one or more Annex E dependent substreams that
     * extend it - see ac3::io::StreamKind's own comment on why this is its
     * own kind rather than folded into either of the two above. */
    AC3FORGE_STREAM_KIND_AC3_CORE_EAC3_EXTENSION = 2
} ac3forge_stream_kind_t;

typedef struct ac3forge_scanned_stream ac3forge_scanned_stream_t;

/* On success, *out_stream receives the scan result; the caller must destroy
 * it. `stream`'s bytes must stay valid and unmodified for as long as any
 * access-unit span this result reports is still in use - same convention as
 * ac3forge_split_frames()/ac3forge_split_access_units(). */
AC3FORGEC_EXPORT ac3forge_status_t ac3forge_scan(const uint8_t* stream, size_t stream_size,
                                               ac3forge_scanned_stream_t** out_stream);

AC3FORGEC_EXPORT ac3forge_stream_kind_t ac3forge_scanned_stream_kind(
    const ac3forge_scanned_stream_t* stream);
AC3FORGEC_EXPORT ac3forge_sample_rate_t ac3forge_scanned_stream_sample_rate(
    const ac3forge_scanned_stream_t* stream);
/* Of the first (or only) substream - see ac3forge_scanned_stream_channels()
 * for what the stream as a whole RENDERS. */
AC3FORGEC_EXPORT ac3forge_acmod_t ac3forge_scanned_stream_acmod(
    const ac3forge_scanned_stream_t* stream);
AC3FORGEC_EXPORT int ac3forge_scanned_stream_lfe(const ac3forge_scanned_stream_t* stream);
/* Channels the stream RENDERS - for E-AC-3 this folds in every dependent
 * substream's chanmap, so it is not the bed's own channel count. */
AC3FORGEC_EXPORT int ac3forge_scanned_stream_channels(const ac3forge_scanned_stream_t* stream);

/* The FIRST programme's access units only (offset/length into the `stream`
 * buffer passed to ac3forge_scan()) - see ScannedStream::access_units's own
 * comment on why a second programme's units are not appended here. Use the
 * programme accessors below to reach any others. */
AC3FORGEC_EXPORT size_t ac3forge_scanned_stream_access_unit_count(
    const ac3forge_scanned_stream_t* stream);
AC3FORGEC_EXPORT ac3forge_span_t ac3forge_scanned_stream_access_unit(
    const ac3forge_scanned_stream_t* stream, size_t index);
/* Samples access unit `index` codes - parallel to the count above. Always
 * AC3FORGE_SAMPLES_PER_FRAME for AC-3; an E-AC-3 independent substream's own
 * numblkscod (§E2.3.1.4) lets this be 256, 512, 768 or 1536, and a stream may
 * mix lengths. */
AC3FORGEC_EXPORT uint32_t ac3forge_scanned_stream_access_unit_samples(
    const ac3forge_scanned_stream_t* stream, size_t index);
/* Substreams in the first access unit; always 1 for AC-3. */
AC3FORGEC_EXPORT size_t ac3forge_scanned_stream_substreams_per_unit(
    const ac3forge_scanned_stream_t* stream);

/* --- raw syntax fields (ac3::io::dec3.hpp's codec-config boxes / MPEG-TS
 * descriptors want these straight off the bitstream) --------------------- */

AC3FORGEC_EXPORT int ac3forge_scanned_stream_bsid(const ac3forge_scanned_stream_t* stream);
/* 0 when the stream never carried bsmod - see bsmod_present() below, which
 * is what distinguishes "the stream said complete main" from "never said". */
AC3FORGEC_EXPORT int ac3forge_scanned_stream_bsmod(const ac3forge_scanned_stream_t* stream);
/* AC-3 only (a kAc3CoreEac3Extension stream's core included): Table 5.18's
 * index into kBitratesKbps. Meaningless for plain E-AC-3, which has no
 * equivalent fixed-table field. */
AC3FORGEC_EXPORT int ac3forge_scanned_stream_bit_rate_code(const ac3forge_scanned_stream_t* stream);
/* TS 103 420 §8.3.2.2's complexity_index_type_a - the Atmos/JOC marker
 * readable without decoding the EMDF container itself. */
AC3FORGEC_EXPORT int ac3forge_scanned_stream_has_oba_complexity_index(
    const ac3forge_scanned_stream_t* stream);
AC3FORGEC_EXPORT int ac3forge_scanned_stream_oba_complexity_index(
    const ac3forge_scanned_stream_t* stream);
/* Whether bsmod was actually transmitted - always true for AC-3, only when
 * infomdate was set for E-AC-3. */
AC3FORGEC_EXPORT int ac3forge_scanned_stream_bsmod_present(const ac3forge_scanned_stream_t* stream);
/* §5.4.2.8/§E2.3.2.3 dsurmod: 0 = not indicated, 1 = NOT Dolby Surround
 * encoded, 2 = Dolby Surround encoded. Only transmitted when acmod is 2/0. */
AC3FORGEC_EXPORT int ac3forge_scanned_stream_dsurmod(const ac3forge_scanned_stream_t* stream);
/* The Annex G §3.5 mixinfoexists conditions for independent substream 0. */
AC3FORGEC_EXPORT int ac3forge_scanned_stream_mix_metadata(const ac3forge_scanned_stream_t* stream);
/* Bit n set when an independent substream with substreamid n appears
 * ANYWHERE in the stream - an observation over the whole stream, computed
 * independently of how the programme list below groups them. */
AC3FORGEC_EXPORT uint8_t ac3forge_scanned_stream_independent_substreams(
    const ac3forge_scanned_stream_t* stream);
/* The FIRST programme's rendered channel LOCATIONS as one Table E2.5
 * custom-channel-map word (bit 0 = Left in the MSB through bit 15 = LFE in
 * the LSB) - see ac3::io::ScannedStream::channel_map's own comment. Written
 * for ac3::io::dash_channel_configuration()'s DASH @value. */
AC3FORGEC_EXPORT uint16_t ac3forge_scanned_stream_channel_map(
    const ac3forge_scanned_stream_t* stream);

/* --- independent substreams 1-3 (index 0-2), for the DVB/ATSC descriptors
 * that name them individually - substream 0 is the stream's main service,
 * already described by acmod/lfe/bsmod/mix_metadata above. `present` false
 * means that substream id was never seen. -------------------------------- */

AC3FORGEC_EXPORT int ac3forge_scanned_stream_associated_substream_present(
    const ac3forge_scanned_stream_t* stream, int index);
AC3FORGEC_EXPORT int ac3forge_scanned_stream_associated_substream_bsmod(
    const ac3forge_scanned_stream_t* stream, int index);
AC3FORGEC_EXPORT int ac3forge_scanned_stream_associated_substream_bsmod_present(
    const ac3forge_scanned_stream_t* stream, int index);
AC3FORGEC_EXPORT ac3forge_acmod_t ac3forge_scanned_stream_associated_substream_acmod(
    const ac3forge_scanned_stream_t* stream, int index);
AC3FORGEC_EXPORT int ac3forge_scanned_stream_associated_substream_lfe(
    const ac3forge_scanned_stream_t* stream, int index);
AC3FORGEC_EXPORT int ac3forge_scanned_stream_associated_substream_mix_metadata(
    const ac3forge_scanned_stream_t* stream, int index);

/* --- ac3::io::ScannedProgramme, by index - ascending substreamid order,
 * never empty on a successful scan. Entry 0 is the same programme every
 * scalar accessor above describes. §E2.3.1.2 allows up to 8 for E-AC-3;
 * always exactly 1 for AC-3 and AC3FORGE_STREAM_KIND_AC3_CORE_EAC3_EXTENSION,
 * neither of which has a second independent substream to number away from. */

AC3FORGEC_EXPORT size_t ac3forge_scanned_stream_programme_count(
    const ac3forge_scanned_stream_t* stream);
AC3FORGEC_EXPORT int ac3forge_scanned_stream_programme_substream_id(
    const ac3forge_scanned_stream_t* stream, size_t programme_index);
AC3FORGEC_EXPORT ac3forge_acmod_t ac3forge_scanned_stream_programme_acmod(
    const ac3forge_scanned_stream_t* stream, size_t programme_index);
AC3FORGEC_EXPORT int ac3forge_scanned_stream_programme_lfe(const ac3forge_scanned_stream_t* stream,
                                                         size_t programme_index);
/* Channels this PROGRAMME renders, folding in every dependent's chanmap. */
AC3FORGEC_EXPORT int ac3forge_scanned_stream_programme_channels(
    const ac3forge_scanned_stream_t* stream, size_t programme_index);
AC3FORGEC_EXPORT int ac3forge_scanned_stream_programme_bsid(const ac3forge_scanned_stream_t* stream,
                                                          size_t programme_index);
/* §5.4.2.2's service type - what tells a receiver this programme is a
 * complete main service (0-1) rather than one to be mixed against another
 * (2-7). */
AC3FORGEC_EXPORT int ac3forge_scanned_stream_programme_bsmod(const ac3forge_scanned_stream_t* stream,
                                                           size_t programme_index);
AC3FORGEC_EXPORT size_t ac3forge_scanned_stream_programme_substreams_per_unit(
    const ac3forge_scanned_stream_t* stream, size_t programme_index);
AC3FORGEC_EXPORT int ac3forge_scanned_stream_programme_has_oba_complexity_index(
    const ac3forge_scanned_stream_t* stream, size_t programme_index);
AC3FORGEC_EXPORT int ac3forge_scanned_stream_programme_oba_complexity_index(
    const ac3forge_scanned_stream_t* stream, size_t programme_index);
/* One entry per frame period, offset/length into the `stream` buffer passed
 * to ac3forge_scan() - same span convention as
 * ac3forge_scanned_stream_access_unit() above. */
AC3FORGEC_EXPORT size_t ac3forge_scanned_stream_programme_access_unit_count(
    const ac3forge_scanned_stream_t* stream, size_t programme_index);
AC3FORGEC_EXPORT ac3forge_span_t ac3forge_scanned_stream_programme_access_unit(
    const ac3forge_scanned_stream_t* stream, size_t programme_index, size_t au_index);

AC3FORGEC_EXPORT void ac3forge_scanned_stream_destroy(ac3forge_scanned_stream_t* stream);

/* --- timing (ac3::io::access_unit_timing() and neighbours) - over the FIRST
 * programme's access units, same convention as the scalar fields above. --- */

/* Access unit `index`'s absolute position - returns 0 (out-parameters left
 * untouched) when there is no such unit, 1 otherwise. A caller wanting
 * seconds divides start_sample/duration_samples by sample_rate itself (or an
 * arbitrary-timescale tick by multiplying before dividing) - deliberately not
 * wrapped here, unlike AccessUnitTiming's own start_seconds()/
 * start_in_timescale(), since it is one line either caller side and this
 * keeps the entry point count down. */
AC3FORGEC_EXPORT int ac3forge_scanned_stream_access_unit_timing(
    const ac3forge_scanned_stream_t* stream, size_t index, uint64_t* out_start_sample,
    uint32_t* out_duration_samples, uint32_t* out_sample_rate);
AC3FORGEC_EXPORT uint64_t ac3forge_scanned_stream_duration_samples(
    const ac3forge_scanned_stream_t* stream);
AC3FORGEC_EXPORT double ac3forge_scanned_stream_duration_seconds(
    const ac3forge_scanned_stream_t* stream);
/* The access unit covering `sample`/`seconds` - i.e. the one to cut at for a
 * given position. Returns 0 (out_index untouched) past the end of the
 * stream, 1 otherwise. */
AC3FORGEC_EXPORT int ac3forge_scanned_stream_access_unit_at_sample(
    const ac3forge_scanned_stream_t* stream, uint64_t sample, size_t* out_index);
AC3FORGEC_EXPORT int ac3forge_scanned_stream_access_unit_at_seconds(
    const ac3forge_scanned_stream_t* stream, double seconds, size_t* out_index);
/* The one length every access unit shares - returns 0 (out_samples
 * untouched) when they differ, 1 otherwise. mp4::AudioTrack/mpegts::
 * AudioTrack/matroska::AudioTrack each need exactly this before a stream can
 * be muxed into a fixed-duration track. */
AC3FORGEC_EXPORT int ac3forge_scanned_stream_uniform_access_unit_samples(
    const ac3forge_scanned_stream_t* stream, uint32_t* out_samples);

/* --------------------------------------------------------------------- *
 * Atmos encode (ac3::oba::AtmosEncoder)
 * --------------------------------------------------------------------- */

typedef struct ac3forge_atmos_encoder ac3forge_atmos_encoder_t;

/* Mirrors ac3::oba::AtmosConfig. */
typedef struct ac3forge_atmos_config {
    ac3forge_sample_rate_t sample_rate;
    uint32_t bitrate_kbps; /* default 448 */
    int dialnorm;
    int num_bands_idx; /* index into ac3::oba::joc::kNumBands (Table 50); default 4 */
    int fine_quant;
    int emit_object_metadata; /* default 1 — see AtmosConfig's own comment on turning this off */
    int fast_mdct;
} ac3forge_atmos_config_t;

AC3FORGEC_EXPORT void ac3forge_atmos_config_init(ac3forge_atmos_config_t* config);

/* One object's placement for one frame — mirrors ac3::oba::ObjectPlacement.
 * Position is §4.2.1's room-anchored system, same ranges as
 * ac3forge_decoded_substream_dynamic_object()'s out_x/out_y/out_z above. */
typedef struct ac3forge_object_placement {
    double x, y, z;
    double gain;     /* linear, default 1.0 */
    double lfe_send; /* linear, default 0.0 — the only route an object reaches the LFE */
} ac3forge_object_placement_t;

/* Fills `placement` with the same defaults ac3::oba::ObjectPlacement's own default member
 * initializers give — room-centre position (x 0.5, y 0.5, z 0.0), unity gain, no LFE send —
 * call this before setting only the fields you need, the same convention every
 * ac3forge_*_config_init() above follows. Without it, a zero-initialized
 * ac3forge_object_placement_t silently encodes gain 0.0 (a muted object) rather than the
 * documented default of unity gain, and a position at the room's front-left-floor corner rather
 * than its centre. */
AC3FORGEC_EXPORT void ac3forge_object_placement_init(ac3forge_object_placement_t* placement);

AC3FORGEC_EXPORT ac3forge_status_t ac3forge_atmos_encoder_create(
    const ac3forge_atmos_config_t* config, int object_count,
    ac3forge_atmos_encoder_t** out_encoder);
AC3FORGEC_EXPORT void ac3forge_atmos_encoder_destroy(ac3forge_atmos_encoder_t* encoder);
AC3FORGEC_EXPORT int ac3forge_atmos_encoder_dynamic_object_count(
    const ac3forge_atmos_encoder_t* encoder);

/* The OBJECT path's latency budget — what this encoder is for. Its
 * transform_samples is AC3FORGE_SAMPLES_PER_BLOCK plus the §7.1 QMF
 * filterbank's own delay (576 samples, ac3::dsp::kQmfDelay): JOC codes a
 * matrix that pulls objects back out of the decoded bed in a 64-band complex
 * QMF domain rather than the MDCT's, and analysis plus synthesis costs that
 * much on top of the bed's own overlap. With config.emit_object_metadata
 * clear there is no container, no JOC and no filterbank, and this collapses
 * to the bed's own budget below. */
AC3FORGEC_EXPORT void ac3forge_atmos_encoder_latency(const ac3forge_atmos_encoder_t* encoder,
                                                    ac3forge_latency_t* out_latency);
AC3FORGEC_EXPORT int ac3forge_atmos_encoder_latency_samples(
    const ac3forge_atmos_encoder_t* encoder);

/* The 5.1 BED's budget: what a legacy decoder that ignores the container
 * hears. One transform overlap, like any other E-AC-3 stream. */
AC3FORGEC_EXPORT void ac3forge_atmos_encoder_bed_latency(const ac3forge_atmos_encoder_t* encoder,
                                                        ac3forge_latency_t* out_latency);

/* objects: `object_count` (as given to _create) mono spans, each exactly
 * AC3FORGE_SAMPLES_PER_FRAME samples; placements: the same count, one per
 * object in the same order. On success, *out_unit receives one complete
 * E-AC-3 access unit (a single independent substream carrying the 5.1 bed,
 * with the EMDF object container in its aux data when
 * config.emit_object_metadata was set); the caller must destroy it. */
AC3FORGEC_EXPORT ac3forge_status_t ac3forge_atmos_encoder_encode_frame(
    ac3forge_atmos_encoder_t* encoder, const float* const* objects, size_t object_count,
    size_t samples_per_object, const ac3forge_object_placement_t* placements,
    size_t placement_count, ac3forge_bytes_t** out_unit);

/* --------------------------------------------------------------------- *
 * Loudness metering (ac3::meta::LoudnessMeter)
 * --------------------------------------------------------------------- */

typedef struct ac3forge_loudness_meter ac3forge_loudness_meter_t;

/* BS.1770-4 Annex 1's basic algorithm, keyed on acmod/lfe exactly like
 * ac3::meta::LoudnessMeter's own first constructor - the lone surround of
 * 2/1 and 3/1 is weighted as the surround FIELD collapsed to one channel
 * (see the C++ class's own comment on how this differs from the chanmap
 * form below for that one case). push() below expects spans in the coded
 * order this acmod implies (Table 5.8), LFE last. */
AC3FORGEC_EXPORT ac3forge_status_t ac3forge_loudness_meter_create(
    ac3forge_sample_rate_t sample_rate, ac3forge_acmod_t acmod, int lfe,
    ac3forge_loudness_meter_t** out_meter);

/* BS.1770-5 Annex 3's extended algorithm over a rendered Table E2.5 layout -
 * the wide layouts (7.1, 5.1.2, 5.1.4, 7.1.4) an acmod cannot name, because a
 * dependent substream's height/wide/rear channels are not members of Table
 * 5.8 at all. `chanmap` is the same Table E2.5 bitmask
 * ac3forge_decoded_substream_location_map()/AC3FORGE_CHANMAP_* use; push()
 * then expects spans in that map's own bit order (ac3::eac3::chanmap::expand()'s
 * order). Fails with AC3FORGE_ERROR_INVALID_ARGUMENT for a chanmap with no
 * channels set. */
AC3FORGEC_EXPORT ac3forge_status_t ac3forge_loudness_meter_create_for_chanmap(
    ac3forge_sample_rate_t sample_rate, uint16_t chanmap, ac3forge_loudness_meter_t** out_meter);

AC3FORGEC_EXPORT void ac3forge_loudness_meter_destroy(ac3forge_loudness_meter_t* meter);
AC3FORGEC_EXPORT int ac3forge_loudness_meter_channel_count(const ac3forge_loudness_meter_t* meter);

/* channels: channel_count() planar spans, coded order with LFE last, each
 * samples_per_channel samples - any length works, unlike encode_frame()'s
 * fixed frame size, since a meter is fed incrementally over a whole
 * programme rather than one frame at a time. */
AC3FORGEC_EXPORT ac3forge_status_t ac3forge_loudness_meter_push(
    ac3forge_loudness_meter_t* meter, const float* const* channels, size_t channel_count,
    size_t samples_per_channel);

/* std::nullopt-via-has_* convention, same as every optional field elsewhere
 * in this header (e.g. ac3forge_decoded_frame_has_compr()) - false before
 * enough audio has been pushed for that measurement to mean anything (see
 * ac3::meta::LoudnessMeter's own per-accessor comments on how much). */
AC3FORGEC_EXPORT int ac3forge_loudness_meter_has_integrated_lkfs(
    const ac3forge_loudness_meter_t* meter);
AC3FORGEC_EXPORT double ac3forge_loudness_meter_integrated_lkfs(
    const ac3forge_loudness_meter_t* meter);
AC3FORGEC_EXPORT int ac3forge_loudness_meter_has_momentary_lkfs(
    const ac3forge_loudness_meter_t* meter);
AC3FORGEC_EXPORT double ac3forge_loudness_meter_momentary_lkfs(
    const ac3forge_loudness_meter_t* meter);
AC3FORGEC_EXPORT int ac3forge_loudness_meter_has_short_term_lkfs(
    const ac3forge_loudness_meter_t* meter);
AC3FORGEC_EXPORT double ac3forge_loudness_meter_short_term_lkfs(
    const ac3forge_loudness_meter_t* meter);
AC3FORGEC_EXPORT int ac3forge_loudness_meter_has_loudness_range(
    const ac3forge_loudness_meter_t* meter);
AC3FORGEC_EXPORT double ac3forge_loudness_meter_loudness_range(
    const ac3forge_loudness_meter_t* meter);
AC3FORGEC_EXPORT int ac3forge_loudness_meter_has_true_peak_dbtp(
    const ac3forge_loudness_meter_t* meter);
AC3FORGEC_EXPORT double ac3forge_loudness_meter_true_peak_dbtp(
    const ac3forge_loudness_meter_t* meter);

/* §5.4.2.8: dialnorm is how many dB dialogue sits below digital 100 percent,
 * valid 1..31 - a programme louder than -1 LKFS or quieter than -31 clamps,
 * which is why a stream that never measured anything reports 31. */
AC3FORGEC_EXPORT int ac3forge_dialnorm_from_lkfs(double lkfs);

/* --------------------------------------------------------------------- *
 * Level metering (ac3::analysis::LevelMeter)
 * --------------------------------------------------------------------- */

/* Everything at or below this reports as this on
 * ac3forge_channel_level_t/ac3forge_channel_summary_t's *_db fields, so a
 * caller never meets log10(0) - mirrors ac3::analysis::kFloorDb. */
#define AC3FORGE_LEVEL_METER_FLOOR_DB (-120.0)

typedef struct ac3forge_level_meter_ballistics {
    double rms_integration_ms;  /* default 300.0 */
    double peak_decay_db_per_s; /* default 20.0 */
    double peak_hold_ms;        /* default 1200.0 */
} ac3forge_level_meter_ballistics_t;

/* Fills `ballistics` with the same defaults as ac3::analysis::MeterBallistics{}. */
AC3FORGEC_EXPORT void ac3forge_level_meter_ballistics_init(
    ac3forge_level_meter_ballistics_t* ballistics);

typedef struct ac3forge_level_meter ac3forge_level_meter_t;

/* `channels` 0 meters exactly acmod's own channel count (mirrors LevelMeter's
 * first constructor); a larger value meters a wider layout no acmod can name
 * - E-AC-3's dependent substreams add speakers Table 5.8 has no word for -
 * with the acmod still naming and placing the first channels of them as the
 * bed (see LevelMeter's second constructor's own comment); a value below
 * acmod's own count is raised to it rather than truncating a layout the
 * caller has already committed to. `ballistics` NULL uses the same defaults
 * ac3forge_level_meter_ballistics_init() fills in. */
AC3FORGEC_EXPORT ac3forge_status_t ac3forge_level_meter_create(
    ac3forge_acmod_t acmod, int lfe, uint32_t sample_rate, int channels,
    const ac3forge_level_meter_ballistics_t* ballistics, ac3forge_level_meter_t** out_meter);

AC3FORGEC_EXPORT void ac3forge_level_meter_destroy(ac3forge_level_meter_t* meter);
AC3FORGEC_EXPORT ac3forge_acmod_t ac3forge_level_meter_acmod(const ac3forge_level_meter_t* meter);
AC3FORGEC_EXPORT int ac3forge_level_meter_lfe(const ac3forge_level_meter_t* meter);
AC3FORGEC_EXPORT int ac3forge_level_meter_channel_count(const ac3forge_level_meter_t* meter);
AC3FORGEC_EXPORT uint32_t ac3forge_level_meter_sample_rate(const ac3forge_level_meter_t* meter);

/* Planar, one span per channel in A/52 order. The shortest span sets the
 * length; channels beyond the ones supplied are metered as silence, so a
 * caller that hands over fewer spans sees the rest fall away rather than
 * freeze. */
AC3FORGEC_EXPORT ac3forge_status_t ac3forge_level_meter_process(
    ac3forge_level_meter_t* meter, const float* const* channels, size_t channel_count,
    size_t samples_per_channel);

/* Drops both the ballistic state and the accumulated summary. */
AC3FORGEC_EXPORT void ac3forge_level_meter_reset(ac3forge_level_meter_t* meter);

typedef struct ac3forge_channel_level {
    double peak_db; /* ballistic peak */
    double hold_db; /* held maximum */
    double rms_db;  /* integrated RMS */
    int clipped;    /* reached full scale since the last reset */
} ac3forge_channel_level_t;

/* The live ballistic view - channel_index in [0, channel_count()); every
 * field is AC3FORGE_LEVEL_METER_FLOOR_DB/clipped=0 out of range. */
AC3FORGEC_EXPORT ac3forge_channel_level_t ac3forge_level_meter_level(
    const ac3forge_level_meter_t* meter, size_t channel_index);

typedef struct ac3forge_channel_summary {
    double peak; /* linear */
    double rms;  /* linear - ac3::analysis::ChannelSummary::rms() */
    double peak_db;
    double rms_db;
    uint64_t samples;
    uint64_t clipped_samples;
} ac3forge_channel_summary_t;

/* Unweighted, exact statistics over everything processed so far - what a
 * file report wants; levels() above exists to make a moving display readable
 * and would only smear a question this has an exact answer to. */
AC3FORGEC_EXPORT ac3forge_channel_summary_t ac3forge_level_meter_summary(
    const ac3forge_level_meter_t* meter, size_t channel_index);

/* --------------------------------------------------------------------- *
 * QC gate (ac3::meta::qc)
 * --------------------------------------------------------------------- */

typedef enum ac3forge_qc_loudness_limit {
    AC3FORGE_QC_LOUDNESS_BAND = 0,   /* |measured - target| <= tolerance_lu */
    AC3FORGE_QC_LOUDNESS_CEILING = 1 /* measured <= target; tolerance_lu unused */
} ac3forge_qc_loudness_limit_t;

/* Mirrors ac3::meta::QcPresetId, ordinals matching kQcPresetIds's own
 * declaration order. */
typedef enum ac3forge_qc_preset_id {
    AC3FORGE_QC_PRESET_EBU_R128_S2 = 0,
    AC3FORGE_QC_PRESET_ATSC_A85 = 1,
    AC3FORGE_QC_PRESET_ATSC_A85_STREAMING = 2,
    AC3FORGE_QC_PRESET_NETFLIX = 3,
    AC3FORGE_QC_PRESET_APPLE_MUSIC_ATMOS = 4
} ac3forge_qc_preset_id_t;

/* Every preset id is valid in [0, ac3forge_qc_preset_count()) - for a caller
 * that wants to check a measurement against all of them (mirrors
 * ac3::meta::kQcPresetIds's own size). */
AC3FORGEC_EXPORT size_t ac3forge_qc_preset_count(void);

/* Tagged ac3forge_qc_preset_info rather than ac3forge_qc_preset: GCC's
 * -Wshadow (built C++) treats a struct tag and a same-named free function as
 * the function "hiding" the tag's implicit constructor-like name - the same
 * reason ac3forge_version_info/ac3forge_version() above are split. The
 * typedef name below is what callers actually use. */
typedef struct ac3forge_qc_preset_info {
    double target_lkfs;
    double tolerance_lu;       /* +/- around target_lkfs, BAND only */
    double max_true_peak_dbtp; /* a ceiling, not a tolerance band */
    ac3forge_qc_loudness_limit_t loudness_limit;
    /* The primary document this row was read out of, with its version and
     * date - library-owned storage valid for the process lifetime; never
     * free() it. */
    const char* source;
} ac3forge_qc_preset_t;

AC3FORGEC_EXPORT ac3forge_qc_preset_t ac3forge_qc_preset(ac3forge_qc_preset_id_t id);
/* Library-owned storage valid for the process lifetime. */
AC3FORGEC_EXPORT const char* ac3forge_qc_preset_name(ac3forge_qc_preset_id_t id);
/* 1 and *out_id set on a recognized name (e.g. "ebu-r128-s2"), 0 otherwise -
 * *out_id is left untouched when this returns 0. */
AC3FORGEC_EXPORT int ac3forge_parse_qc_preset(const char* name, ac3forge_qc_preset_id_t* out_id);

/* One preset's verdict against one measurement - mirrors ac3::meta::QcVerdict.
 * Either half is left at its not-passing default when the corresponding
 * measurement was itself unavailable (has_integrated_lkfs/has_true_peak_dbtp
 * false below) - material this gate cannot actually judge, not a false
 * pass. */
typedef struct ac3forge_qc_verdict {
    int has_loudness_delta_lu;
    double loudness_delta_lu; /* measured - target */
    int loudness_pass;
    int has_true_peak_margin_dbtp;
    double true_peak_margin_dbtp; /* ceiling - measured; >= 0 passes */
    int true_peak_pass;
} ac3forge_qc_verdict_t;

AC3FORGEC_EXPORT int ac3forge_qc_verdict_pass(const ac3forge_qc_verdict_t* verdict);

/* has_integrated_lkfs/has_true_peak_dbtp: pass 0 exactly when
 * ac3forge_loudness_meter_has_integrated_lkfs()/..._has_true_peak_dbtp()
 * would - see ac3forge_qc_verdict_t's own comment on what that leaves in the
 * verdict. */
AC3FORGEC_EXPORT ac3forge_qc_verdict_t ac3forge_evaluate_qc_gate(
    const ac3forge_qc_preset_t* preset, int has_integrated_lkfs, double integrated_lkfs,
    int has_true_peak_dbtp, double true_peak_dbtp);

#ifdef __cplusplus
} /* extern "C" */
#endif

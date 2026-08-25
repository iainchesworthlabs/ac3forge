#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/mantissas.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/decoder/syntax_trace.hpp"
#include "ac3/encoder/eac3_tools.hpp"  // eac3::BandLayout, for BlockTail below
#include "ac3/export.hpp"
#include "ac3/latency.hpp"
#include "ac3/meta/bsi.hpp"
#include "ac3/meta/mixing.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/verify/eac3_mirror.hpp"
#include "ac3/verify/mirror.hpp"

// The in-repo AC-3 / E-AC-3 decoder — the validation pyramid's strongest
// correctness anchor (fully normative, shares tables/bit-allocation/exponents/
// IMDCT with the encoder core).
//
// AC-3 scope (bsid <= 8): any acmod 0/0..3/2 plus LFE, long blocks,
// D15/D25/D45/reuse exponents, full bit allocation including delta bit
// allocation (§7.2.2.6), mantissa ungrouping, coupling (strategy, banded
// coordinates, phase flags and leak parameters) and 2/0 rematrixing.
// acmod 0 (1+1 dual mono) is two independent programmes sharing one
// syncframe — Ch2's dialnorm2/compr2/dynrng2 are parsed and reported
// alongside Ch1's, and each programme's §7.7 gain is applied to its own
// channel only. Block switching (§8.2.2/§7.9) is decoded too — DecodedFrame::
// blksw reports which blocks used the short transform. dynrng words are
// parsed but not applied; bap-0 bins reconstruct per §7.3.4's dithflag - a
// true zero when it is clear, a dither sample (DitherGenerator, deterministic
// per decoder instance) when it is set. A coupled channel's shared bap-0
// bins are dithered independently per RECEIVING channel, after decoupling,
// per §7.3.4's own "uncorrelated" requirement - never by dithering the
// shared coupling-channel coefficient itself.
//
// E-AC-3 scope (Annex E, bsid 11-16): the whole of Tables E1.2/E1.3/E1.4 as
// syntax — every metadata payload is walked correctly whether or not its
// contents are used — plus dependent substreams, chanmap and the §E3.8.2
// render. Every coding tool Annex E adds on top of AC-3 is implemented: AHT,
// spectral extension, enhanced coupling (§E3.5) and transient pre-noise
// processing (§3.7) - individually or all stacked together. Annex E's
// default coupling band structures decode too: standard coupling falls back
// to Table E2.12, enhanced coupling to Table E2.13. Two syntax corners are
// still recognised and refused rather than mis-decoded - enhanced coupling's
// angle-interpolation flag, and a transient pre-noise correction reaching
// further back or forward than the one frame of history/lookahead buffered
// here - because no stream this project's own encoder produces exercises
// them. Transient pre-noise processing has one
// consequence for this class's own API: see decode_substream and flush()
// below. This is the only oracle 7.1.4 has: FFmpeg rejects any frame with
// substreamid != 0, so a stream with two dependent substreams cannot be
// checked against it in any container. Every substream's own dynrng/dynrng2
// words are reported on DecodedSubstream, same convention as DecodedFrame,
// and optionally applied per Eac3Decoder's own constructor — see
// DecoderConfig below.
//
// The §7.7 dynamic range words are always reported and optionally applied —
// see DecoderConfig. Reporting them separately from applying them is what
// makes this useful as a check on the encoder: a test can assert on the words
// the encoder chose AND on the level change they cause, and those are two
// different claims.

namespace ac3 {

enum class DecodeError : std::uint8_t {
    kTruncated,
    kBadSyncWord,
    kBadCrc,
    kReservedValue,
    kUnsupported,  // legal AC-3, but syntax this decoder declines to read
    kInvalidStream,
};

[[nodiscard]] AC3FORGE_EXPORT std::string_view describe(DecodeError error);

// --- §7.10 error concealment ------------------------------------------------

// What to do about a frame that cannot be decoded. kNone - the default - is
// what every caller got before this existed: decode_frame returns the error
// and the caller decides. The other two produce a frame's worth of audio
// instead, so a stream with a damaged frame in it stays continuous rather
// than gaining a hard discontinuity where that frame should have been.
//
// Both work in the overlap-add domain rather than on finished PCM, which is
// what keeps them coherent with the frames either side: the decoders retain
// the last successfully decoded BLOCK's windowed transform output, and
// synthesise the concealed frame's blocks from it through the same
// overlap-add the real ones went through. A concealed frame therefore leaves
// the delay state in exactly the shape the next good frame expects, and the
// fade-in and fade-out at each end are the codec's own window rather than a
// ramp invented here.
enum class ConcealmentPolicy : std::uint8_t {
    kNone,
    // Repeat the last good block, decaying towards silence across the frame
    // and on into any further consecutive losses. Preserves the programme's
    // texture across a short dropout, at the cost of one block of material
    // being heard twice.
    kRepeatFade,
    // Substitute silence. The last good block's own overlap tail still plays
    // out through the first block, so this fades rather than cuts.
    kMute,
};

// What a concealed result actually did, reported on the result so a test can
// assert on the concealment itself and not only on "the decode did not fail".
enum class ConcealmentAction : std::uint8_t {
    kRepeatFade,
    kMute,
    // E-AC-3 only: the access unit's independent (bed) substream decoded, but
    // at least one dependent did not, so the program is rendered from the bed
    // alone. Nothing was substituted - the channels that arrived are real -
    // the layout is simply narrower than the stream promised.
    kBedOnly,
};

struct Concealment {
    DecodeError error = DecodeError::kInvalidStream;
    ConcealmentAction action = ConcealmentAction::kMute;
};

struct DecoderConfig {
    // §7.7.1's "Partial Compression": the dynrng word may be scaled so that a
    // fraction of the coded compression is applied. 0 ignores dynrng entirely
    // and reproduces the full dynamic range; 1 applies it as the encoder
    // intended. A/52 §7.7.1.1 says a consumer decoder "shall, by default,
    // implement the compression characteristic" — this one defaults to 0
    // because it exists to check what the encoder wrote, and a decoder that
    // silently rescales its output cannot be the reference for that.
    double drc_scale = 0.0;
    // §7.9.4 step 3's complex transform evaluated via the same FFT core the
    // encoder's fast MDCT fold uses, instead of the pseudocode's direct
    // O(N^2) sum against a 320 KiB tabulated matrix - see mdct.hpp's
    // inverse doc comment. Applies to every inverse transform a DECODE
    // runs: both decoders' PCM reconstruction, the three per-block
    // inverses inside eac3::ecpl_channel_spectrum's enhanced-coupling
    // reconstruction, and joc::reconstruct's per-object synthesis. It
    // never reaches an encoder: the encoder-internal inverse uses
    // (spx/ecpl copy-source reconstruction) read eac3::FrameConfig's own
    // fast_mdct instead, so nothing about ENCODED output depends on this
    // flag. Default ON since the owner accepted the quality evidence (the
    // same gate EncoderConfig::fast_mdct passed through): worst
    // transform-level relative error 7.8e-14 against the direct form, 180 s
    // stream agreement 214.9 dB SNR (AC-3) / 284.7 dB (E-AC-3), decodes
    // 4.5-4.7x faster. false selects the pseudocode's own direct evaluation
    // - the REFERENCE form, and the oracle the fast path's tests validate
    // against; ac3cli exposes the pair as mode=performance|reference for
    // exactly the runs where bit-for-bit agreement with the spec's stated
    // arithmetic matters more than speed.
    bool fast_imdct = true;
    // §7.7.2: prefer compr over dynrng wherever a compr word exists, which is
    // what a set-top box's RF mode does. §7.7.2.1 requires falling back on
    // dynrng for any syncframe that carries no compr, so this composes with
    // drc_scale rather than replacing it.
    bool heavy_compression = false;
    // --- output stage (ac3/decoder/output.hpp) -----------------------------
    // dialnorm normalisation, the §7.8 downmix and §7.7's two canonical
    // operating modes. Every field defaults to off, so a decoder configured
    // the way every existing caller configures it emits the coded channels
    // untouched, sample for sample. OperatingMode::kLine/kRf override
    // drc_scale/heavy_compression above rather than composing with them -
    // that is what makes them modes rather than two more switches; see
    // internal::resolve_operating_mode().
    OutputConfig output{};
    // §7.10: what to do with a frame that will not decode. kNone returns the
    // error, exactly as before. See ConcealmentPolicy.
    ConcealmentPolicy concealment = ConcealmentPolicy::kNone;
    // Which domain JOC object reconstruction applies §6.6.6's matrix in.
    // joc::Domain::kQmf is what the clause describes and what a licensed
    // decoder runs: §7.1's 64-band complex QMF, ac3::dsp::QmfAnalysis.
    // joc::Domain::kMdctBand is the cheaper approximation over 256 MDCT
    // bins that predates this tree having a filterbank at all - correct
    // only for a stream whose matrix was estimated the same way, which in
    // practice means one this project's own encoder produced with
    // AtmosConfig::joc_domain to match. Note the two domains do not have
    // the same latency: object audio lags the bed by
    // joc::reconstruction_delay(domain), 256 samples against 576.
    //
    // Default kQmf: it is both what the clause says and, measured, the
    // cheaper of the two here - 0.70 ms/frame against 0.88 for four
    // objects, because the MDCT path's inverse is deliberately pinned to
    // §7.9.4's direct form while the filterbank has only the one
    // evaluation.
    joc::Domain joc_domain = joc::Domain::kQmf;
    // --- self-check (ac3/verify/mirror.hpp) --------------------------------
    // The decoder's half of EncoderConfig::trace: when set, decode_frame()
    // records the same per-block, per-stream state it derived from the wire,
    // so the two models can be diffed. Null by default, at the same cost as
    // the encoder's - one branch per block. Filled INCREMENTALLY, so a frame
    // the decoder ends up refusing still leaves behind everything it read
    // before the refusal, which is the case the comparison is most useful in.
    // AC-3 only (FrameDecoder) - see eac3_trace below for Eac3Decoder's own.
    verify::FrameTrace* trace = nullptr;
    // The Annex E counterpart (ac3/verify/eac3_mirror.hpp), and a whole
    // ACCESS UNIT rather than one frame: decode_substream appends one
    // substream's view per call, starting a fresh unit at each independent
    // substream, so a caller stepping through syncframes by hand and one
    // calling decode_access_unit both end up with the same accumulated
    // trace. Filled incrementally and null by default, exactly as `trace`
    // above.
    verify::Eac3AccessUnitTrace* eac3_trace = nullptr;
    // --- programme selection (§E2.3.1.2) -----------------------------------
    // Which independent substream's programme Eac3Decoder::decode_access_unit
    // renders. A stream may carry up to eight — a main service plus the
    // second language, audio description or commentary a broadcaster mixes
    // against it — and they are alternatives, not layers: only one is played
    // at a time.
    //
    // std::nullopt renders whichever programme each call's access unit
    // happens to belong to, which is what every caller got before this field
    // existed and is exactly right for the single-programme case. Set to an
    // id and an access unit belonging to any OTHER programme is skipped
    // without being decoded at all — see decode_access_unit's own doc comment
    // for what it returns then, and DecodedAccessUnit::programme for telling
    // the results apart under std::nullopt.
    //
    // Ignored by decode_substream, which is deliberately below the programme
    // layer: it decodes the frame it is handed.
    std::optional<int> programme = std::nullopt;
    // --- syntax trace (ac3/decoder/syntax_trace.hpp) ------------------------
    // Which coding tools each block used and what exponent strategy each
    // stream carried, recorded on the way past. Null by default, at the same
    // one-branch-per-block cost `trace` above already sets the precedent for,
    // and written by BOTH decoders rather than just the AC-3 one - the
    // Annex E tools are most of what makes it worth having. Filled
    // incrementally: a frame the decoder ends up refusing leaves behind
    // everything it read before the refusal.
    FrameSyntax* syntax = nullptr;
    // Parse every field exactly as a full decode does, but stop short of
    // turning the coefficients into audio: no inverse transform, no
    // overlap-add, no JOC object reconstruction and, for Eac3Decoder, no
    // per-access-unit channel combination. The returned metadata - and any
    // trace above - is identical to a full decode's; `channels` and
    // `object_audio` come back empty.
    //
    // This exists because inspecting a stream and rendering it are different
    // jobs with very different costs. Everything a reader wants to know about
    // a frame (its metadata, its tool usage, its object layer) is settled by
    // the parse; the transform is the expensive part and answers none of it.
    // `ac3cli probe` runs the whole of a file this way. Note what it does NOT
    // skip: the mantissas are still read, because the bit position of every
    // subsequent field depends on them - a "parse" that skipped those would
    // not be parsing the same stream.
    bool skip_reconstruction = false;
};

struct DecodedFrame {
    SampleRate sample_rate = SampleRate::k48000;
    std::uint32_t bitrate_kbps = 0;
    // §5.4.1.3/§5.4.2.1, reported rather than merely checked: an inspection
    // tool wants both off the wire, and nothing else here carries them. bsid
    // is 8 for the syntax in the body of A/52, 6 for Annex D's alternate one
    // - anything else is refused, so those are the only two values this ever
    // reports. bsmod is the same 3-bit code info.bsmod below decodes into
    // BitstreamMode; both are populated from the one read, this one for a
    // caller (an inspection tool's JSON output) that wants the raw code.
    int bsid = 8;
    int bsmod = 0;
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    // §5.4.2's informational fields, whatever this frame carried. Fields the
    // layout gives no home to keep their defaults - a 3/2 frame sends no
    // dsurmod, so `info.dsurmod` stays "not indicated" rather than reporting
    // a bit that was never on the wire.
    meta::BsiInfo info{};
    // Annex D's xbsi1/xbsi2, present exactly when bsid is 6. A bsid-8 frame
    // carries the time code in the same 28 bits instead, and reports it as
    // info.timecod1/timecod2 above.
    std::optional<meta::AlternateBsi> alternate_bsi = std::nullopt;
    int dialnorm = 31;
    // §5.4.2.4/§5.4.2.5, the two downmix levels bsi carries: std::nullopt for
    // any acmod whose bsi does not carry that field at all (cmixlev needs
    // three front channels, surmixlev needs surrounds), which is a different
    // statement from "carried, and says the default". ac3::mix_levels() turns
    // the pair into the coefficients the §7.8 output stage needs, applying
    // §7.8's own defaults where a field is absent.
    std::optional<meta::CentreMixLevel> cmixlev = std::nullopt;
    std::optional<meta::SurroundMixLevel> surmixlev = std::nullopt;
    // §5.4.2.9: std::nullopt when compre was clear, so "no word" and "a word
    // that happens to say unity" stay distinguishable.
    std::optional<std::uint8_t> compr = std::nullopt;
    // §7.7.1.2: the EFFECTIVE word for each block, with the persistence rule
    // already resolved — a block that transmitted nothing reports what it
    // inherited, and block 0 without a word reports unity rather than
    // whatever the previous frame ended on.
    std::array<std::uint8_t, kBlocksPerFrame> dynrng{};
    // Ch2's own dialnorm/compr/dynrng (§5.4.2.16-22), present only when acmod
    // is kDualMono — the second of the two independent programmes 1+1 codes.
    std::optional<int> dialnorm2 = std::nullopt;
    std::optional<std::uint8_t> compr2 = std::nullopt;
    std::array<std::uint8_t, kBlocksPerFrame> dynrng2{};
    // §8.2.2/§7.9: per full-bandwidth channel, per block - true where that
    // block used the short (block-switched) transform. Sized to nfchans; the
    // LFE and any coupling channel never switch, so they carry no entry.
    std::vector<std::array<bool, kBlocksPerFrame>> blksw;
    // nchans x kSamplesPerFrame, AC-3 channel order, LFE last when present -
    // unless DecoderConfig::output asked for a fold, in which case this holds
    // the folded output (Lo/Ro, Lt/Rt or mono) while acmod/lfe above still
    // describe what was CODED. ac3::output_channel_count() says how many
    // channels a given config leaves behind.
    std::vector<std::vector<float>> channels;
    // §7.10: set only when this frame was concealed rather than decoded -
    // std::nullopt on every ordinary frame, including when concealment is
    // enabled and nothing went wrong. Every field above describes the last
    // frame that DID decode, since a damaged frame's own bsi cannot be
    // trusted, and `dynrng` reads unity throughout because no word arrived.
    std::optional<Concealment> concealed = std::nullopt;
};

class AC3FORGE_EXPORT FrameDecoder {
   public:
    // Real work, not =default, because Impl below is incomplete here - same
    // reason ac3::io::WavStreamReader's default ctor gives. A default-
    // constructed decoder is only ever a move-assignment target (see
    // io::Prober::Impl), never decoded through directly, but it still needs
    // a valid impl_ to be safely destructible and move-assignable.
    FrameDecoder();
    explicit FrameDecoder(const DecoderConfig& config);
    // Declared (and defined in decoder.cpp, where Impl below is complete)
    // rather than implicit: a dllexport class generates every implicit
    // special member whether or not called, and the unique_ptr member makes
    // the implicit copy deleted - which is fine - but the moves must be
    // spelled out or the declared destructor suppresses them.
    ~FrameDecoder();
    FrameDecoder(const FrameDecoder&) = delete;
    FrameDecoder& operator=(const FrameDecoder&) = delete;
    FrameDecoder(FrameDecoder&&) noexcept;
    FrameDecoder& operator=(FrameDecoder&&) noexcept;

    // Decodes exactly one syncframe (the span must be exactly one frame).
    //
    // With DecoderConfig::concealment set, a frame that will not decode comes
    // back as a SUCCESSFUL result carrying DecodedFrame::concealed instead of
    // as an error - except for a failure before any frame has decoded at all,
    // which still returns the error: concealment reconstructs from what came
    // before it, and at the head of a stream there is nothing to reconstruct
    // from.
    [[nodiscard]] std::expected<DecodedFrame, DecodeError> decode_frame(
        std::span<const std::byte> frame);

    // As decode_frame, but the PCM lands in caller-owned planar storage
    // instead of freshly allocated vectors - the per-call cost drops from
    // one vector per channel (~37 KB a frame at 5.1) to nothing, which is
    // what a realtime consumer or the WASM demo wants. channels[ch] must
    // each hold kSamplesPerFrame floats and there must be a span for every
    // channel the frame codes - six covers every AC-3 layout; the returned
    // metadata's acmod/lfe say how many were written. The returned
    // DecodedFrame carries everything EXCEPT the PCM (its `channels` is
    // left empty). On an error return the spans' contents are unspecified
    // - exactly as discarded as the value form's partial frame was.
    [[nodiscard]] std::expected<DecodedFrame, DecodeError> decode_frame_into(
        std::span<const std::byte> frame, std::span<const std::span<float>> channels);

    // Roadmap PF6: the delay THIS decoder adds on top of whatever the
    // encoder's own budget (ac3/latency.hpp) already accounts for. Exactly
    // zero, and structurally so rather than by luck: decode_frame returns a
    // frame's full kSamplesPerFrame of PCM from the call that supplies that
    // frame's bytes, and the IMDCT overlap those samples came out of is
    // already charged as the chain's transform term. AC-3 has no §3.7
    // hold-back to add - see Eac3Decoder::latency_samples(), which does.
    //
    // Present rather than left implicit so "encoder latency plus decoder
    // latency" is a sum a caller can actually write, with both halves
    // answering from the object that owns the behaviour. Distinct from
    // output_latency_samples() below: that is the OUTPUT STAGE's own delay
    // (Lt/Rt phase shift), a separate term from this decoder's own overlap
    // contribution and additive with it, not a duplicate of it.
    [[nodiscard]] static constexpr int latency_samples() { return 0; }

    // The output stage's own added delay, in samples - see
    // OutputStage::latency_samples(). Zero for every configuration except
    // Lt/Rt with its phase shift left on.
    [[nodiscard]] int output_latency_samples() const;

   private:
    // Both public forms above: `channels` empty means allocate the PCM into
    // the returned DecodedFrame, non-empty means write through the spans.
    [[nodiscard]] std::expected<DecodedFrame, DecodeError> decode_frame_core(
        std::span<const std::byte> frame, std::span<const std::span<float>> channels);
    // §7.10: a frame's worth of audio built out of retained_ under the
    // configured policy. std::nullopt when there is nothing retained yet to
    // build it from, which is the whole of the "damaged first frame" case.
    [[nodiscard]] std::optional<DecodedFrame> conceal(DecodeError error,
                                                      std::span<const std::span<float>> channels);

    // Every private data member - config, overlap-add state, dither, the
    // output stage, the §7.10 concealment buffers - lives behind this one
    // pimpl, following the same pattern as ac3::io::WavStreamReader/Writer
    // and ac3::FrameEncoder. Impl is defined in decoder.cpp.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// --- E-AC-3 ----------------------------------------------------------------

// One decoded syncframe of an E-AC-3 stream. `channels` are in the substream's
// own coded order (Table 5.8, LFE last); where those channels BELONG is
// `chanmap` when a dependent sent one and acmod/lfeon otherwise.
struct DecodedSubstream {
    eac3::StreamType strmtyp = eac3::StreamType::kIndependent;
    int substreamid = 0;
    // §E2.3.1.6 and Annex E's infomdate payload. bsmod is 0 ("not indicated")
    // where infomdate was clear, matching io::ScannedStream::bsmod.
    int bsid = eac3::kBsid;
    int bsmod = 0;
    SampleRate sample_rate = SampleRate::k48000;
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    int dialnorm = 31;
    // §5.4.2.9/§E3.8.5: std::nullopt when compre was clear OR this substream
    // is a dependent one - a dependent's compre bit is repurposed to mark the
    // LAST dependent of the program rather than announce a compression word
    // (see parse_bsi's own comment), so there is no meaningful compr value to
    // report there even though the 8 bits are still present on the wire.
    std::optional<std::uint8_t> compr = std::nullopt;
    // §7.7.1.2: the EFFECTIVE word for each block, with the persistence rule
    // already resolved, same convention as DecodedFrame::dynrng - a block
    // that transmitted nothing reports what it inherited, and block 0
    // without a word reports unity. Sized to kBlocksPerFrame regardless of
    // how many blocks this syncframe actually codes (numblkscod), matching
    // blksw's own fixed-size convention above; entries at index >= nblks are
    // never written.
    std::array<std::uint8_t, kBlocksPerFrame> dynrng{};
    // Ch2's own dialnorm/compr, present only when acmod is kDualMono (1+1) -
    // the second of the two independent programmes 1+1 codes.
    std::optional<int> dialnorm2 = std::nullopt;
    std::optional<std::uint8_t> compr2 = std::nullopt;
    std::array<std::uint8_t, kBlocksPerFrame> dynrng2{};
    int numblkscod = 3;
    // Table E1.2's mixmdate group, std::nullopt when mixmdate was clear -
    // separate Lt/Rt and Lo/Ro centre and surround levels plus the LFE mix
    // level, none of which AC-3's bsi can express, through the rest of the
    // group DC4 added (programme scale factors, the mixdef block, pan info).
    // A DEPENDENT substream's copy stops after the levels - Table E1.2 gates
    // everything past lfemixlevcod on strmtyp == 0x0 - so those fields keep
    // their defaults there rather than reporting bits that were never sent.
    // ac3::mix_levels() turns the downmix levels alone into the coefficients
    // the §7.8 output stage needs.
    std::optional<meta::MixMetadata> mixing = std::nullopt;
    // Table E1.2's infomdat group, std::nullopt when infomdate was clear.
    // BsiInfo's langcod/langcod2 and timecod1/timecod2 have no Annex E field
    // and are never set here.
    std::optional<meta::BsiInfo> info = std::nullopt;
    // §E2.3.1.8: only a dependent substream may carry one.
    std::optional<std::uint16_t> chanmap;
    // §E3.8.5: in a dependent substream compre does not announce a compression
    // word so much as mark the LAST dependent of the program — the point at
    // which a decoder knows every channel has arrived.
    bool last_dependent = false;
    // §8.2.2/§7.9: per full-bandwidth channel, per block - true where that
    // block used the short (block-switched) transform. Sized to nfchans; the
    // LFE and any coupling channel never switch, so they carry no entry.
    std::vector<std::array<bool, kBlocksPerFrame>> blksw;
    std::vector<std::vector<float>> channels;
    // §H.1/TS 103 420 §5.5: the OAMD payload found in one of this substream's
    // block skip fields, if any - std::nullopt for plain E-AC-3 with no
    // object audio at all, and equally for a skip field this decoder found
    // but declined to interpret (see oba::parse_payload's own comment on
    // what it refuses). Which block actually carries the container is not
    // fixed (emdf::build_container's own comment), so every block's skip
    // field is a candidate; the first one that parses wins.
    std::optional<oba::DecodedProgram> object_metadata = std::nullopt;
    // JOC's (§6) reconstructed per-object audio, one waveform per JOC output
    // - empty when object_metadata is unset, when no JOC payload rode
    // alongside the OAMD one, or when the downmix JOC asks for is not the
    // five channels this substream carries (Table 47's 7-channel
    // configurations need a dependent substream's Lb/Rb).
    std::vector<std::vector<float>> object_audio;
    // §7.10, same convention as DecodedFrame::concealed: set only when this
    // substream was concealed rather than decoded. A concealed substream
    // never carries an object layer - OAMD and JOC describe the frame that
    // did not arrive, and repeating the previous frame's positions would put
    // moving objects somewhere they demonstrably are not.
    std::optional<Concealment> concealed = std::nullopt;
    // What each object_audio entry IS: an index into the payload's own object
    // order (bed channels, then ISF, then dynamic objects), which is
    // oba::joc_object_indices() for this program. For the
    // dynamic-object-only program AtmosEncoder writes, entry i is
    // object_metadata->objects[i] - the identity this used to assume - and
    // for a bed program it names the bed channel instead, which
    // oba::bed_labels() turns into a speaker label. Same length as
    // object_audio, and empty exactly when it is.
    std::vector<int> object_indices;

    // The Table E2.5 map this substream's channels occupy.
    [[nodiscard]] std::uint16_t location_map() const {
        return chanmap ? *chanmap : eac3::chanmap::acmod_map(acmod, lfe);
    }
};

// One program's channels after §E3.8.2: the independent substream's bed with
// each dependent's channels laid over it, in Table E2.5 location order (which
// for a lone 5.1 bed is exactly the AC-3 channel order).
//
// Dual mono (acmod kDualMono) is the one exception: 1+1 is always a single
// substream with no bed/dependent split, and its two channels are unrelated
// programmes with no Table E2.5 location - Ch1 and Ch2, not L and R. `layout`
// is left empty (count 0) in that case, matching ac3::meta::layout_of()'s own
// "not a layout" stance, and `channels` holds Ch1 then Ch2 in coded order.
struct DecodedAccessUnit {
    SampleRate sample_rate = SampleRate::k48000;
    Acmod acmod = Acmod::k2_0;
    int dialnorm = 31;
    // The independent substream's own compr, when it carries one - see
    // DecodedSubstream::compr's own comment; a dependent substream's compre
    // bit means something else entirely, so only the independent (bed)
    // substream's word is ever meaningful at the access-unit level.
    std::optional<std::uint8_t> compr = std::nullopt;
    // The independent substream's own dynrng, same reasoning as compr above -
    // every substream carries its own words and a decoder applies each to
    // that substream's own channels (see Eac3Decoder's DecoderConfig-driven
    // gain), but the bed's is the one figure worth surfacing at the
    // access-unit level for a status report. Only entries below
    // eac3::blocks_per_syncframe(numblkscod) were ever written - see
    // DecodedSubstream::dynrng's own comment on the fixed-size convention.
    std::array<std::uint8_t, kBlocksPerFrame> dynrng{};
    // The independent substream's own numblkscod (§E2.3.1.4), needed to know
    // how many of the kBlocksPerFrame entries in `dynrng` above are real
    // rather than the fixed array's unwritten tail - see
    // eac3::blocks_per_syncframe.
    int numblkscod = 3;
    // The independent substream's own mixmdate and infomdat groups, same
    // reasoning as compr and dynrng above: every substream carries its own,
    // but only the bed's describes the programme. A dependent's mixmdate is
    // the levels alone anyway, and Table E1.2 gives a dependent no infomdat
    // gate of its own worth surfacing at this level.
    std::optional<meta::MixMetadata> mixing = std::nullopt;
    std::optional<meta::BsiInfo> info = std::nullopt;
    // object_metadata/object_audio from whichever substream of the access
    // unit carries them, first one wins - see DecodedSubstream's own comments
    // on both. TS 103 420 §8.3.1 leaves the choice of substream to the
    // encoder; this project's own AtmosEncoder always uses the bed (it never
    // sends a dependent substream at all), but §E2.3.1.2's legacy-core
    // delivery cannot - an AC-3 core has nowhere to put an EMDF container -
    // so there the objects arrive in a dependent. Not unioned the way
    // `layout` is below: one EMDF container describes the whole programme, so
    // the question is which substream carries it, not how to merge several.
    std::optional<oba::DecodedProgram> object_metadata = std::nullopt;
    std::vector<std::vector<float>> object_audio;
    std::vector<int> object_indices;
    // §E2.3.1.2's substreamid of the independent substream this programme was
    // rendered from — 0 for every single-programme stream, and the only way
    // to tell one programme's units from another's when DecoderConfig::
    // programme is left unset and the decoder renders whatever arrives.
    int programme = 0;
    int substream_count = 0;
    eac3::chanmap::Layout layout;
    // Parallel to `layout`, except for dual mono - unless DecoderConfig::
    // output asked for a fold, in which case this holds the folded output and
    // `layout` still describes what was rendered before it. See
    // ac3::output_channel_count().
    std::vector<std::vector<float>> channels;
    // §7.10, same convention as DecodedFrame::concealed: std::nullopt on any
    // access unit that assembled normally. ConcealmentAction::kBedOnly is the
    // one that only happens here - the bed decoded and a dependent did not,
    // so the program is real but narrower than the stream promised.
    std::optional<Concealment> concealed = std::nullopt;
};

class AC3FORGE_EXPORT Eac3Decoder {
   public:
    // Real work, not =default, because Impl below is incomplete here - same
    // reason FrameDecoder's default ctor gives.
    Eac3Decoder();
    explicit Eac3Decoder(const DecoderConfig& config);
    // Declared (and defined in eac3_decoder.cpp, where Impl below is
    // complete) rather than implicit - same dllexport/unique_ptr reasoning
    // as FrameDecoder above.
    ~Eac3Decoder();
    Eac3Decoder(const Eac3Decoder&) = delete;
    Eac3Decoder& operator=(const Eac3Decoder&) = delete;
    Eac3Decoder(Eac3Decoder&&) noexcept;
    Eac3Decoder& operator=(Eac3Decoder&&) noexcept;

    // Decodes one syncframe. Overlap-add state is kept per substream identity,
    // so the substreams of successive access units stay independent of each
    // other; a caller stepping through syncframes by hand gets the same audio
    // as one calling decode_access_unit.
    //
    // An AC-3 syncframe (bsid <= 8) is accepted here too, and comes back as
    // substream (kIndependent, 0) with numblkscod 3 - §E2.3.1.2 assigns an
    // AC-3 bit stream present in an E-AC-3 bit stream exactly that identity,
    // and a legacy-core delivery is built on it: an AC-3 frame carrying the
    // 5.1 bed with Annex E dependents extending it to 7.1 or beyond. See
    // core_. Note this means Eac3Decoder accepts a plain AC-3 stream as well,
    // one access unit per syncframe; FrameDecoder remains the direct way to
    // read one, and reports AC-3's own DecodedFrame rather than a substream.
    //
    // Returns std::nullopt exactly when a frame's PCM is being held back
    // pending transient pre-noise processing (§3.7): a stream's very first
    // frame that turns transproce on has nothing ready to return yet, because
    // whether a correction reaches back into it is only known once the NEXT
    // frame has been parsed. A stream that never uses the tool always gets a
    // populated result immediately - this holding-back is the exception, not
    // the common case. Call flush() once at end-of-stream to collect
    // whichever frame is still held back, if any.
    [[nodiscard]] std::expected<std::optional<DecodedSubstream>, DecodeError> decode_substream(
        std::span<const std::byte> frame);

    // Decodes one access unit — an independent substream followed by its
    // dependents, exactly as split_access_units delimits them — and renders it.
    //
    // Same std::nullopt convention as decode_substream, for the same reason:
    // assembling one access unit needs every one of its substreams ready in
    // the SAME call, and decode_substream can hold one back independently of
    // the others (§3.7's transient pre-noise processing is a per-substream
    // flag). When that happens, whichever OTHER substreams of this access
    // unit already released this call are held in an internal per-identity
    // cache until the rest catch up - so nothing already-ready is discarded,
    // and a later call finishes the assembly once every identity this
    // program uses has a result waiting. A stream that never uses the tool
    // is unaffected: every substream releases every call, so the cache never
    // holds more than one call's worth at a time and every call returns a
    // populated result immediately.
    //
    // std::nullopt has one further cause here that decode_substream has none
    // of: a DecoderConfig::programme was set and this unit belongs to a
    // DIFFERENT programme (§E2.3.1.2), so there is nothing of the selected
    // one to return. The unit is skipped before any decoding, leaving no
    // per-substream state behind. Both causes call for the same thing from a
    // caller — take nothing from this call and go on to the next unit — so
    // they are one return value rather than two.
    [[nodiscard]] std::expected<std::optional<DecodedAccessUnit>, DecodeError> decode_access_unit(
        std::span<const std::byte> unit);

    // As decode_access_unit, but the rendered program's PCM lands in
    // caller-owned planar storage - FrameDecoder::decode_frame_into's
    // E-AC-3 counterpart, same span contract by assert. channels[slot] is
    // written in the returned layout's slot order (coded order for dual
    // mono), and the returned DecodedAccessUnit carries everything EXCEPT
    // that PCM (its `channels` stays empty; object_audio, which only an
    // Atmos bed carries, stays by value). There must be a span for every
    // slot the assembled layout renders - 16 covers §E3.8.2's cap - and
    // each must hold the unit's blocks*256 samples (kSamplesPerFrame covers
    // every numblkscod). std::nullopt - the §3.7 hold-back - leaves the
    // spans untouched; on an error return their contents are unspecified.
    //
    // What this form removes is the assembly's own allocation (up to 16
    // channels of 1536 samples, every unit) - the term that dominates a
    // stream that never uses transient pre-noise processing. A held-back
    // frame is by definition decoded before the call whose spans would
    // receive it, so its PCM is buffered internally either way and only
    // copied out here at release.
    [[nodiscard]] std::expected<std::optional<DecodedAccessUnit>, DecodeError>
    decode_access_unit_into(std::span<const std::byte> unit,
                            std::span<const std::span<float>> channels);

    // Releases whichever frames transient pre-noise processing is still
    // holding back, one per substream identity that has one pending - empty
    // if none does, which covers every stream that never used the tool.
    // Call once, after the last decode_substream/decode_access_unit call for
    // a stream, to avoid silently dropping its final frame(s). Drains BOTH
    // decode_substream's own pending frame and decode_access_unit's
    // assembly cache (see its own doc comment) - a caller that only ever
    // used decode_access_unit and wants the very last program's worth of
    // audio out of a stream that ends mid-hold-back gets raw per-substream
    // results here rather than one final assembled DecodedAccessUnit,
    // since by definition the assembly never completed.
    [[nodiscard]] std::vector<DecodedSubstream> flush();

    // Roadmap PF6: the delay THIS decoder adds, same contract as
    // FrameDecoder::latency_samples(). Zero until some substream's frame sets
    // transproce, kSamplesPerFrame from then on - §3.7's hold-back is not a
    // property of the decoder but of the stream it is fed, and once a
    // substream identity's slot engages it stays engaged for the rest of the
    // stream (decode_substream's own doc comment). Not const-foldable for
    // that reason, unlike the AC-3 form.
    //
    // A caller sizing buffers before the stream starts should ask the ENCODER
    // instead (eac3::eac3_latency), which knows from its own configuration
    // whether the tool will ever be used; this reports what has actually
    // happened so far. Distinct from output_latency_samples() below and
    // additive with it - see FrameDecoder's own pair of these for why.
    [[nodiscard]] int latency_samples() const;

    // The output stage's own added delay, in samples - see
    // OutputStage::latency_samples(). Zero for every configuration except
    // Lt/Rt with its phase shift left on.
    [[nodiscard]] int output_latency_samples() const;

   private:
    // decode_substream without the §7.10 concealment wrapper around it.
    [[nodiscard]] std::expected<std::optional<DecodedSubstream>, DecodeError>
    decode_substream_core(std::span<const std::byte> frame);
    // §7.10: a substream's worth of audio built out of retained_[slot] under
    // the configured policy, or std::nullopt when that identity has nothing
    // retained yet. `slot` is the identity key described below.
    [[nodiscard]] std::optional<DecodedSubstream> conceal(DecodeError error, std::size_t slot);
    // The §7.8 fold over an assembled access unit, in whichever storage it
    // landed - the result's own vectors or the caller's spans. The fold
    // itself is OutputStage's; this only decides what to hand it.
    void apply_output(DecodedAccessUnit& out, std::span<const std::span<float>> external);
    // Both public access-unit forms above: `external` empty means allocate
    // the program PCM into the returned DecodedAccessUnit, non-empty means
    // write through the spans - the same split decode_frame_core makes.
    [[nodiscard]] std::expected<std::optional<DecodedAccessUnit>, DecodeError>
    decode_access_unit_core(std::span<const std::byte> unit,
                            std::span<const std::span<float>> external);

    // §E2.3.1.2's legacy core, presented as substream (kIndependent, 0) - see
    // core_ below and decode_substream's own doc comment.
    [[nodiscard]] std::expected<DecodedSubstream, DecodeError> decode_ac3_core(
        std::span<const std::byte> frame);

    // Every private data member - config, the output stage, the per-
    // substream-identity delay/JOC/pending state, the decode scratch, the
    // §7.10 concealment buffers, all of it - lives behind this one pimpl,
    // following the same pattern as ac3::io::WavStreamReader/Writer and
    // ac3::FrameEncoder. Impl is defined in eac3_decoder.cpp. The lazy
    // per-substream-slot unique_ptr arrays (delay/JOC/retained state) stay
    // exactly as they were - that is a laziness optimization independent of
    // this pimpl, not something the sweep should flatten.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Split a raw elementary stream into syncframes by sync word and declared
// size. Handles both generations; bsid at bit 40 decides which.
[[nodiscard]] AC3FORGE_EXPORT std::expected<std::vector<std::span<const std::byte>>, DecodeError>
split_frames(std::span<const std::byte> stream);

// Group those syncframes into access units. A new one begins at each
// independent substream, and the spans returned are the concatenations the
// bitstream itself defines.
//
// This DELIMITS; it does not select. A stream carrying more than one
// programme (§E2.3.1.2 allows eight independent substreams, and broadcast DD+
// uses them for a second language or an associated service) yields the
// programmes' units interleaved, one frame period's worth of each in turn -
// so consecutive entries are NOT consecutive in time. Feeding them straight
// to a decoder in that order splices two programmes together; use the
// programme-selecting overload below, or read
// DecodedAccessUnit::programme, to keep them apart.
[[nodiscard]] AC3FORGE_EXPORT std::expected<std::vector<std::span<const std::byte>>, DecodeError>
split_access_units(std::span<const std::byte> stream);

// The access units of ONE programme, in order: those beginning with an
// independent substream whose §E2.3.1.2 substreamid is `programme`, together
// with the dependents that follow each. Consecutive entries here ARE
// consecutive frame periods, which is what a decoder, a muxer or a level
// meter needs.
//
// An empty result means the stream carries no such programme - not an error,
// since asking is how a caller finds out. Use programme_ids() to enumerate
// what is actually there.
[[nodiscard]] AC3FORGE_EXPORT std::expected<std::vector<std::span<const std::byte>>, DecodeError>
split_access_units(std::span<const std::byte> stream, int programme);

// The substreamid of every independent substream the stream carries, ascending
// and without duplicates - one entry per programme. Always {0} for AC-3, which
// has no substream layer, and for the single-programme E-AC-3 case.
[[nodiscard]] AC3FORGE_EXPORT std::expected<std::vector<int>, DecodeError> programme_ids(
    std::span<const std::byte> stream);

// bsid at bit 40, without committing to either layout. Fails only if the span
// is too short to hold a header.
[[nodiscard]] AC3FORGE_EXPORT std::expected<int, DecodeError> stream_bsid(
    std::span<const std::byte> frame);

// True for A/52 §E2.3.1.2's legacy-core delivery: an AC-3 syncframe standing
// in as independent substream 0, with an Annex E DEPENDENT substream
// immediately behind it extending the layout per §E3.8.2 - a 5.1 AC-3 bed
// plus, typically, the two rear surrounds that make it 7.1.
//
// The point of it is that stream_bsid() alone cannot tell such a stream from
// plain AC-3: both open with an AC-3 syncframe, and a caller that dispatches
// on that one value sends this one down the frame-at-a-time AC-3 path, which
// reaches the dependent and refuses it as "valid AC-3 this decoder does not
// implement". These streams need split_access_units + Eac3Decoder, which
// handle the core natively (see Eac3Decoder::decode_substream).
//
// Reads the first two syncframes and no further. That is all the arrangement
// needs to be recognised, and it keeps the check O(1) on a caller that is
// only trying to pick a code path; a stream that contradicts itself later is
// the decode's problem to report, not this predicate's to pre-empt. False
// for anything it cannot read, including a truncated or desynchronised
// stream - "not this arrangement" is the safe answer, and every caller has a
// real decode behind it to produce the actual error.
[[nodiscard]] AC3FORGE_EXPORT bool has_eac3_extension_substreams(
    std::span<const std::byte> stream);

}  // namespace ac3

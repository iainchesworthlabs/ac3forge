#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "ac3/core/bitalloc.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/encoder/silent_frame.hpp"  // FrameError
#include "ac3/encoder/transient.hpp"
#include "ac3/export.hpp"
#include "ac3/latency.hpp"
#include "ac3/meta/bsi.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/mixing.hpp"
#include "ac3/quality/distortion.hpp"
#include "ac3/verify/eac3_mirror.hpp"

// E-AC-3 (Dolby Digital Plus) framing - ATSC A/52:2018 Annex E, bsid 16.
//
// E-AC-3 is not a variant of the AC-3 frame; it is a different container for
// the same coding tools:
//   - syncinfo is ONLY the sync word. There is no crc1, so the GF(2) leading
//     -CRC solver AC-3 needs has no counterpart here.
//   - frmsiz is an arbitrary 11-bit word count rather than a table lookup, so
//     any frame size is directly expressible and the 44.1 kHz padding
//     alternation AC-3 needs disappears.
//   - Exponent strategies and coupling-in-use for EVERY block are hoisted
//     into a frame-level audfrm element ahead of the blocks, and several
//     per-block fields become conditional on frame-level flags.
//
// This first step emits a valid, decodable bsid-16 frame carrying digital
// silence, the same way the AC-3 work started: all SNR offsets zero, which
// §7.2.2.1.1 defines as an all-zero bit allocation, so no mantissa data
// exists and the frame is pure syntax.

namespace ac3::eac3 {

// kBsid, StreamType and the Table E2.5 chanmap live in
// ac3/core/eac3_tables.hpp: the decoder reads the same fields this writes, and
// one definition is what keeps the two agreeing on what a bit pattern means.
// This encoder writes only strmtyp 0x0 and 0x1 - 0x2, an independent substream
// whose program was previously coded as AC-3, drags in a blkid/frmsizecod
// branch nothing here would ever emit, so validate() refuses it.

// Average bit rate: a long-run rate target that still lets each frame's size
// move with the content. CBR holds every frame to the same size; VBR holds
// every frame to the same quality and lets the rate go where it likes;
// neither delivers what a streaming ladder rung or a DVB mux contracts for,
// which is a deliverable AVERAGE at a frame size still free to move.
//
// Set VbrConfig::abr and the encoder holds one composite SNR offset across
// frames and steers it - up while the stream is running under its target,
// down while it is running over - so a quiet frame stays cheap and a busy
// one is allowed to cost more, with the average landing where it was asked
// to. Underneath that, `window_frames` consecutive frames pool one budget as
// a hard ceiling, so no window can overrun whatever the offset is doing.
//
// VbrConfig::quality is NOT read under ABR. The two are different rate
// controls: quality fixes the offset, ABR's whole job is to move it, and the
// stream's first frame seeds the offset from its own budget search rather
// than from a number a caller guessed. VbrConfig::min_kbps/max_kbps do still
// apply - they bound each individual frame, which composes with a long-run
// average rather than competing with it.
struct AbrConfig {
    // The long-run average, same unit and meaning as FrameConfig::bitrate_kbps.
    std::uint32_t target_kbps = 192;
    // How many consecutive frames share one pooled budget. At 48 kHz a frame
    // is 1536 samples (32 ms), so the default holds the average over about a
    // second - long enough for a bar of music or a spoken phrase to borrow
    // from its neighbours, short enough that a mux's own buffer model still
    // recognises the result. 1 pools nothing, which pins every frame to one
    // frame's share and makes ABR behave as CBR; 0 is rejected by validate().
    std::uint32_t window_frames = 32;
};

inline constexpr std::uint32_t kAbrDefaultWindowFrames = 32;

// Variable bit rate: instead of fixing the frame's word count and searching
// for the best quality that fits it (CBR's rate control, see FrameConfig's
// own comment below), fix the quality and let the word count follow the
// content. Only meaningful for E-AC-3: AC-3's frame size is a lookup into
// Table 5.18 (frmsizecod), not a free word count, so it has no equivalent.
struct VbrConfig {
    // [0, 1]: linearly maps onto the encoder's own composite SNR-offset
    // search space (composite = round(quality * 1023)). Not read at all when
    // `abr` below is set - see AbrConfig. This is not a
    // perceptual or cross-encoder quality scale - it is exactly as
    // meaningful as the search space is, which is to say it is monotonic in
    // "how good" for THIS encoder and nothing more, the same caveat every
    // encoder's own CRF/-V knob carries.
    //
    // The search space is NOT linear in bit cost - masking-model allocators
    // spend roughly twice the bits for a fixed step up in SNR margin, so bit
    // cost rises steeply in the top third or so of this range. For ordinary
    // multi-channel programme material that means a high quality with no
    // max_kbps bound will often demand more bits than any legal E-AC-3 frame
    // can hold at all (encode_frame then reports FrameError::kInvalidBitrate
    // rather than silently truncating) - pair a high quality with max_kbps
    // unless the content is known to be quiet or sparse.
    double quality = 0.5;

    // Optional hard bounds, same unit and meaning as FrameConfig::bitrate_kbps:
    // the chosen word count is clamped so the frame's own rate never leaves
    // [min_kbps, max_kbps]. std::nullopt on either side means unbounded there.
    std::optional<std::uint32_t> min_kbps = std::nullopt;
    // When the quality target would need more words than max_kbps allows,
    // the encoder falls back to the same binary search CBR uses, budgeted
    // against max_kbps instead of a fixed target - so a bounded VBR frame is
    // never worse than "the best CBR could do at that ceiling".
    std::optional<std::uint32_t> max_kbps = std::nullopt;

    // Drives the coupling/spx begin-frequency heuristics (default_cplbegf,
    // default_spxbegf) in place of bitrate_kbps, which VBR has nothing fixed
    // to offer them. std::nullopt resolves to abr->target_kbps if set, then
    // max_kbps if set, else kVbrDefaultNominalKbps - a caller who wants
    // exactly today's CBR tool behaviour at some quality supplies the same
    // number they would have passed as bitrate_kbps.
    std::optional<std::uint32_t> nominal_kbps = std::nullopt;

    // Set to hold a long-run average rate instead of letting the rate run
    // free: the offset is steered frame to frame rather than read off
    // `quality`, which is then unused. See AbrConfig. Unset is plain VBR,
    // exactly as before.
    std::optional<AbrConfig> abr = std::nullopt;
};

inline constexpr std::uint32_t kVbrDefaultNominalKbps = 192;

struct FrameConfig {
    SampleRate sample_rate = SampleRate::k48000;
    std::uint32_t bitrate_kbps = 192;
    // std::nullopt: CBR, sized from bitrate_kbps (frame_words() below). Set:
    // VBR: bitrate_kbps is not read on the encode path at all - the
    // cplbegf/spxbegf frequency defaults use vbr->nominal_kbps in its place
    // (falling back to max_kbps, then kVbrDefaultNominalKbps).
    std::optional<VbrConfig> vbr = std::nullopt;
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    // §E2.3.1.4, Table E2.4: how many 256-sample audio blocks one syncframe
    // carries - code 0 is one block (5.3 ms), 1 is two, 2 is three, and 3 (the
    // default) is the usual six (32 ms). A short syncframe is a shorter
    // FRAME, not a lower sample rate: encode_frame then wants
    // samples_per_frame() samples per channel instead of kSamplesPerFrame,
    // and the frame's own byte count falls with it.
    //
    // What it buys is granularity at the cost of repeating the whole
    // bsi/audfrm header that much more often, which at a fixed bit rate comes
    // straight out of the mantissas. Annex E also takes several shortcuts
    // away below code 3 (Table E1.3): expstre is implied 1, so exponent
    // strategies are always stated per block and never hoisted into a Table
    // E2.10 code; ahte is implied 0, so the adaptive hybrid transform is
    // unavailable; an independent substream carries convsync, which this
    // encoder sets on the first frame of every group of 6 / blocks_per_frame
    // frames - the point from which a converter to classic six-block AC-3 can
    // start accumulating (§8.2 of Annex E's own PES-packaging text); and
    // blkstrtinfoe disappears entirely at code 0 (there is only one block to
    // start).
    //
    // Not available at the three fscod2 reduced rates: §E2.3.1.3 spends
    // numblkscod's own bits on fscod2 there, so such a frame is implicitly
    // always six blocks. validate() refuses the combination rather than
    // writing a header that says one thing and a payload that says another.
    int numblkscod = 3;
    int dialnorm = 31;
    // Annex E Table E1.2: Ch2's dialnorm, required when acmod is kDualMono
    // (1+1) — the two programmes are levelled independently.
    std::optional<int> dialnorm2 = std::nullopt;
    // fbw bandwidth code 0..60, or -1 for the encoder's own choice: the rate
    // ceiling AC-3 has always used, with the frame's own spectrum narrowing
    // it under that (ac3/encoder/bandwidth.hpp). This used to default to a
    // fixed 60 - the whole 23.7 kHz at every rate - which at 96 kbit/s per
    // channel, where neither coupling nor spectral extension runs, spread
    // the frame's bits across 253 mantissas that could not each afford two.
    // Meaningful only for a channel carrying its own high band: with either
    // tool in use the tool's start frequency IS the coded bandwidth and
    // chbwcod is not transmitted at all (§E3.3.3).
    int chbwcod = -1;

    // --- substream identity (Table E1.2) -----------------------------------
    // The defaults describe the lone independent substream this encoder has
    // always emitted, so existing callers keep their exact bit layout.
    StreamType strmtyp = StreamType::kIndependent;
    // §E2.3.1.2. Independent substreams number from 0; the dependents of one
    // independent substream number from 0 in their OWN space, so a dependent's
    // id does not continue its parent's.
    int substreamid = 0;
    // Sent only by dependent substreams. std::nullopt clears chanmape, which
    // lets acmod and lfeon speak for themselves - the dependent's channels
    // then simply overwrite the matching ones in the independent substream.
    std::optional<std::uint16_t> chanmap = std::nullopt;
    // §E3.8.5. In a dependent substream compre stops meaning "a compression
    // word follows" and becomes the marker for the LAST dependent of the
    // program - it is how a decoder knows the program is complete. Set by the
    // access-unit builder; meaningless on an independent substream.
    bool last_dependent = false;

    // --- dynamic range and mixing metadata ---------------------------------
    // As in AC-3: std::nullopt keeps dynrnge clear in every block and compre
    // clear in bsi, so a metadata-free stream is bit-identical to before.
    std::optional<meta::Profile> drc = std::nullopt;
    // §7.7.2 heavy compression. Only an INDEPENDENT substream can carry it:
    // §E3.8.5 repurposes a dependent's compre as the end-of-programme marker,
    // so the eight bits it drags in there are not a gain any decoder will use.
    //
    // This is the one part of the metadata layer with no external oracle.
    // FFmpeg's E-AC-3 header parser reads compre and then SKIPS the word, so
    // -heavy_compr changes nothing on an E-AC-3 stream however good the
    // metadata is - unlike -drc_scale, which honours dynrng here as it does in
    // AC-3. What holds it up instead: the word format and the generator are
    // shared with the AC-3 path, which ffmpeg does verify, and the field's
    // placement is checked bit by bit (tests/meta/test_drc.cpp, tools/references/eac3_parse.py).
    std::optional<meta::HeavyConfig> heavy = std::nullopt;
    // Ch2's own drc/heavy, meaningful only under kDualMono - no fallback to
    // drc/heavy when unset. See ac3::EncoderConfig::drc2 (the AC-3 sibling of
    // this field) for why: dialnorm2 is already independent of dialnorm the
    // same way, so this follows the same all-or-nothing precedent.
    std::optional<meta::Profile> drc2 = std::nullopt;
    std::optional<meta::HeavyConfig> heavy2 = std::nullopt;
    // The mixmdate group (Table E1.2). E-AC-3 dropped bsi's cmixlev and
    // surmixlev entirely, so without this a stream carries no downmix levels
    // at all and a receiver falls back on its own defaults.
    //
    // Everything in MixMetadata past the five levels and lfemixlevcod is
    // written only by an INDEPENDENT substream - Table E1.2 gates the
    // programme scale factors, the mixing-parameter block, the pan
    // information and the per-block configuration on strmtyp == 0x0, because
    // all four describe how to combine this programme with another one and a
    // dependent substream is only ever part of someone else's. Set them on a
    // dependent and they are silently not written, exactly as the syntax
    // requires.
    std::optional<meta::MixMetadata> mixing = std::nullopt;
    // The infomdat group (Table E1.2, §E2.3.1.62): what service this is, the
    // Dolby Surround / Surround EX / Headphone flags, the mixing room, the
    // copyright and original-bitstream bits and sourcefscod. std::nullopt
    // clears infomdate, which is what this encoder always did before.
    // BsiInfo's langcod/langcod2 and timecod1/timecod2 have no home in Annex
    // E and are not read here.
    std::optional<meta::BsiInfo> info = std::nullopt;
    // --- Annex E coding tools -----------------------------------------------
    // Let the encoder choose the tool set from the per-channel rate, instead
    // of taking the `coupling`/`spx`/`aht` flags below as given.
    //
    // Every one of these tools trades waveform fidelity for a band it can
    // describe more cheaply than it can code, so each is a win below some
    // per-channel rate and a loss above it - and the crossovers are far apart
    // (coupling's moves with the channel count, spectral extension's does
    // not). Turning them all on at every rate is what "all" does, and at
    // 192 kbit/s stereo that costs about 10 dB of SNR against simply not
    // using them; turning them all off gives up about the same at
    // 256 kbit/s 5.1. This asks for neither, and picks per rate.
    //
    // It overrides the individual flags rather than combining with them: when
    // it is set they are not read at all. cplbegf/spxbegf/gaqmod still apply
    // to whatever it does turn on, so a caller can steer the geometry without
    // taking over the on/off decision.
    bool auto_tools = false;
    // Channel coupling (§E3.3, §7.4). Needs two full-bandwidth channels to
    // share anything, so it is ignored for mono. Above the coupling frequency
    // the coupled channels stop carrying coefficients of their own and a
    // single shared channel plus per-channel per-band coordinates stands in
    // for them; chbwcod then disappears, because the coupling frequency IS
    // the coded bandwidth of every coupled channel.
    bool coupling = false;
    // Coupling begin frequency code (§5.4.3.11), 0-15: the region starts at
    // coefficient 37 + 12 * cplbegf. Negative picks a rate-dependent default,
    // which is the useful behaviour - the whole point of coupling is to buy
    // bits at rates that cannot afford two full-bandwidth channels, so the
    // right frequency falls as the rate does.
    int cplbegf = -1;
    // §E3.5: enhanced coupling instead of standard - 22 sub-bands, amplitude/
    // angle/chaos-quantized coordinates and a phase-restoring reconstruction
    // built on a full DFT, rather than a single per-band scale factor. Only
    // meaningful together with `coupling`.
    bool enhanced = false;

    // Spectral extension (§E3.6). Above the extension frequency nothing is
    // coded at all: the decoder copies a lower band up, blends it with noise
    // and scales it to the banded envelope the encoder measured. It is
    // cheaper than coupling - scale factors only, no shared channel - and
    // correspondingly cruder, so the two stack: independent low, coupled mid,
    // synthesized high.
    bool spx = false;
    // Spectral extension begin frequency code (§E2.3.3.5), 0-7. Synthesis
    // starts at coefficient 25 + 12 * spx_begin_subbnd(spxbegf), which is
    // non-linear in spxbegf. Negative picks a rate-dependent default.
    //
    // With coupling also in use this value FIXES the coupling end frequency:
    // §E3.3.1 stops transmitting cplendf and derives it from spxbegf, so that
    // coupling ends exactly where synthesis begins. When that derived end
    // lands below where cplbegf asks coupling to start there is no coupling
    // region at that frequency, and coupling is dropped for the frame -
    // rather than slid down to meet it, which would couple lower than either
    // the caller or the rate default asked for.
    int spxbegf = -1;
    // Spectral extension attenuation (§E3.6.4.2.3): a five-tap notch across
    // the seam where the coded band meets the synthesized one, and across
    // every point where the copy wraps back to its source. Only meaningful
    // when spx is set. It costs six bits per channel per frame.
    bool spx_atten = true;
    // The attenuation depth (§E2.3.2.25), 0-31: deeper with the code. Negative
    // matches the notch to how big a step the seam actually is.
    int spxattencod = -1;

    // Adaptive hybrid transform (§E3.4). A second transform stage - a 6-point
    // DCT down each spectral bin across the frame's six blocks - which for
    // stationary material collapses six coefficients into essentially one.
    // It brings a finer allocation table and vector quantisation with it, and
    // it restructures the frame: an AHT channel's whole frame of mantissas is
    // packed into block 0 and the other five carry nothing for it.
    //
    // It is not free for material that moves between blocks, so it is decided
    // per channel per frame; setting this permits it rather than forces it.
    bool aht = false;
    // Gain-adaptive quantization mode (§E3.4.4.2, Table E3.3), 0-3. Negative
    // lets the encoder pick the cheapest per channel, which is the useful
    // behaviour; pinning it to 0 turns GAQ off while leaving the rest of AHT
    // alone, which is how the tool's contribution gets measured on its own.
    int gaqmod = -1;

    // Transient pre-noise processing (§3.7): a post-IMDCT correction that
    // overwrites the pre-echo ahead of a detected transient with a
    // synthesized copy of the clean audio just before it. This encoder
    // reuses TransientDetector - the same detector blksw already relies on -
    // rather than a second, independent one, so this only has an effect on
    // channels/frames that also block-switch (§8.2.2/§7.9).
    bool transient_prenoise = false;

    // §7.9.4 fast N/4-FFT forward MDCT (see mdct.hpp's mdct512_forward), on
    // by default since the owner accepted its quality evidence (verified max
    // relative error ~3e-12 against the direct form on random data and real
    // audio, 331 dB direct-vs-fast end-to-end SNR, 0.000 dB delta against an
    // independent oracle at 192-448 kbps; see tests/core/test_mdct_fast.cpp and
    // `tools/ci/quality_race.py fast-mdct`). false forces the direct §8.2.3.2
    // reference form, which stays maintained as the oracle the fast path is
    // validated against. All three forward transforms accelerate - the long
    // one and both halves of a block-switched pair, each down its own fold
    // (see mdct.hpp). It also selects the form of the three
    // inverse transforms an ENHANCED-COUPLING encode runs per block inside
    // eac3::ecpl_channel_spectrum, reconstructing the spectrum the decoder
    // will hold: encoding is the only reason an encoder runs an inverse at
    // all, so this one field is the encoder's fast-transform switch in both
    // directions, and ac3cli's mode=reference (which clears it) keeps a
    // reference-mode encode direct end to end.
    bool fast_mdct = true;

    // §7.2.2's transmitted bit allocation parameters (BitAllocCodes,
    // ac3/core/bitalloc.hpp), searched per frame from the reconstruction
    // error a decoder will produce, instead of the fixed dbpbcod == 3 EQ3
    // measured its way to on average (roadmap EQ13; AC-3's own
    // EncoderConfig::search, encoder.cpp's step 9a, is the model this
    // mirrors). search=distortion minimises ac3::quality::accumulate_block's
    // decoded-domain noise, per stream, over the frame's six blocks.
    //
    // search=perceptual is accepted but has no effect here: AC-3's own
    // measurements found that criterion uncompetitive at every rate tried
    // (docs/library/quality.md), so wiring ac3::quality::PerceptualModel a
    // second time to chase a criterion already known not to win was scoped
    // out rather than rushed.
    //
    // CBR only (config_.vbr unset): VBR/ABR's own budget-fitting search is a
    // materially bigger unit to wrap in an outer candidate loop than AC-3's
    // settle() is - the delta-segment with/without comparison and ABR's
    // stateful reservoir both assume one committed codes value per frame -
    // and untangling that was scoped out too; see ROADMAP.md EQ13. Silently
    // inert under VBR, the same way delta bit allocation is silently inert
    // on an AHT stream (EQ5) - a documented scope boundary, not a rejected
    // configuration.
    //
    // Only dbpbcod varies, between kAllocCodes' 3 and Table E1.4's 2 (the
    // only two values baie can carry that this encoder ever chooses
    // between): fgaincod is the AC-3 search's other axis, and E-AC-3 has no
    // per-frame fgaincod to search yet - frmfgaincode stays 0 unconditionally
    // (EQ7's own remaining gap, a prerequisite this does not also solve).
    // AHT streams are excluded from the measurement, on the same grounds
    // EQ5 excludes them from delta bit allocation: the concentration AHT's
    // own DCT performs reads as quantization error in accumulate_block's
    // per-block model. Off by default, like every other decision knob here.
    quality::Criterion search = quality::Criterion::kNone;

    // §7.3.4 dithflag, decided per channel per block from content (see
    // src/forge/src/encoder/dither.hpp) - on by default, matching every other
    // config field here, except a frame using spectral extension, which
    // always dithers off (see the note where step 8a decides it). false pins
    // dithflag at 0 unconditionally in every frame, the deterministic
    // behaviour from before this existed: real dither values are
    // decoder-defined (the spec's own "any reasonably random sequence"), so
    // two independent, spec-correct decoders given the same dithered stream
    // diverge in the dithered bins by design - which is exactly what breaks
    // a bit-for-bit comparison between this project's own decoder and an
    // external one (tools/checks/verify_gold_reference.sh). That gate sets
    // this false; nothing else needs to.
    bool dither = true;

    // TS 103 420 §8.3. An object-audio stream sets flag_ec3_extension_type_a in
    // the addbsi field of whichever substream carries the EMDF container, and
    // follows it with the number of bed, ISF and dynamic objects (§8.3.2.2 caps
    // it at 16). This is the only Atmos marker a decoder can read without
    // hunting through the aux data for the container, and it is what FFmpeg
    // keys its "Dolby Digital Plus + Dolby Atmos" report off. std::nullopt
    // writes addbsie == 0, which is what every stream here did before.
    std::optional<int> oba_complexity_index = std::nullopt;

    // --- self-check (ac3/verify/eac3_mirror.hpp) -----------------------------
    // When set, the encoder records the per-block model it wrote this frame
    // for - bit offsets, exponents, bit allocation, delta correction, AHT
    // gains and the coupling/spectral-extension coordinates - into this
    // trace, for comparison against a decoder's own reading of the same
    // frame. One trace per SUBSTREAM: an AccessUnitEncoder's substreams each
    // carry their own FrameConfig and so their own pointer, and
    // verify::Eac3AccessUnitTrace is what holds a whole access unit's worth.
    //
    // Null by default, which costs one branch per block and no allocation.
    // Nothing about the encoded output depends on it: the trace reads state
    // the encoder already has and never steers a decision.
    verify::Eac3SubstreamTrace* trace = nullptr;
};

// Words per syncframe at a given rate. E-AC-3 signals the size directly, so
// this is just the exact bit budget rounded to whole 16-bit words. `blocks` is
// blocks_per_syncframe(numblkscod): a short syncframe carries proportionally
// fewer samples, and so proportionally fewer words at the same bit rate.
[[nodiscard]] constexpr std::uint32_t frame_words(SampleRate sample_rate,
                                                  std::uint32_t bitrate_kbps,
                                                  int blocks = kBlocksPerFrame) {
    const std::uint64_t bits = static_cast<std::uint64_t>(bitrate_kbps) * 1000 *
                               static_cast<std::uint64_t>(blocks) * kSamplesPerBlock /
                               sample_rate_hz(sample_rate);
    return static_cast<std::uint32_t>(bits / 16);
}

// §E2.3.1.3: frmsiz is 11 bits and holds (words - 1), so 2048 is the largest
// word count this format can ever signal, whatever bitrate produced it.
inline constexpr std::uint32_t kMaxFrameWords = 2048;

// An EMDF container (ac3::emdf::build_container) to carry in this frame's aux
// data, or an empty span for none.
//
// A/52 §5.4.4.1 puts aux user data at the END of the auxbits field, immediately
// before auxdatal, "so a decoder can find and unpack the auxdatal user bits
// without knowing the value of nauxbits" - nauxbits being unknowable until the
// whole frame has been decoded. So the container is not appended after the
// padding; the padding is what gets pushed in front of it.
using AuxPayload = std::span<const std::byte>;

// The latency budget a stream from this configuration imposes end to end
// (roadmap PF6; ac3/latency.hpp documents the four terms).
//
// Only transient_prenoise moves anything. Every other Annex E tool - AHT,
// coupling, enhanced coupling, spectral extension - is a different way of
// coding the SAME frame's coefficients and adds no delay on either side: AHT
// packs a channel's six blocks into block 0 rather than looking ahead of the
// frame, spx and coupling reconstruct within the block they arrive in, and
// none of them change how many samples the decoder must hold. §3.7 is the
// exception because its correction reaches backwards ACROSS a frame boundary,
// which is only realizable by a decoder that still has the previous frame -
// hence one frame period of hold-back, charged here because it is the
// encoder's tool choice that imposes it.
//
// Free function rather than a FrameEncoder member alone so a caller can price
// a configuration before building an encoder for it, which is what a live
// pipeline sizing its buffers actually needs.
[[nodiscard]] constexpr LatencyBudget eac3_latency(const FrameConfig& config) {
    return LatencyBudget{
        .frame_samples = kSamplesPerFrame,
        .transform_samples = kTransformDelaySamples,
        .lookahead_samples = 0,
        .holdback_samples = config.transient_prenoise ? kSamplesPerFrame : 0};
}

[[nodiscard]] AC3FORGE_EXPORT std::expected<std::vector<std::byte>, FrameError> build_silent_frame(
    const FrameConfig& config, AuxPayload aux = {});

// The §7.7 words for one frame, separated from FrameConfig because they change
// every frame and from the encoder because every substream of an access unit
// has to carry the SAME ones - see AccessUnitEncoder.
struct FrameMetadata {
    std::array<std::uint8_t, kBlocksPerFrame> dynrng{};
    std::optional<std::uint8_t> compr = std::nullopt;
    // Ch2's own words, present only when the substream's acmod is kDualMono.
    std::array<std::uint8_t, kBlocksPerFrame> dynrng2{};
    std::optional<std::uint8_t> compr2 = std::nullopt;
};

// Real audio through the same container. The coding profile is deliberately
// the one reference encoders use, because those are the paths reference
// decoders are exercised on: exponent strategies and SNR offsets planned per
// frame from content (EQ1) rather than fixed. Long blocks only; the Annex E
// tools and FrameConfig::numblkscod are opt-in.
class AC3FORGE_EXPORT FrameEncoder {
   public:
    explicit FrameEncoder(const FrameConfig& config);
    // Out of line because state_ below is an incomplete type here; movable
    // (AccessUnitEncoder keeps these in a vector), not copyable.
    ~FrameEncoder();
    FrameEncoder(FrameEncoder&&) noexcept;
    FrameEncoder& operator=(FrameEncoder&&) noexcept;

    // channels: the full-bandwidth channels in AC-3 order (Table 5.8),
    // followed by LFE last when config.lfe is set. Each span holds exactly
    // samples_per_frame() samples, nominally in [-1, 1) - kSamplesPerFrame
    // unless config.numblkscod shortens the syncframe.
    [[nodiscard]] std::expected<std::vector<std::byte>, FrameError> encode_frame(
        std::span<const std::span<const float>> channels, AuxPayload aux = {});

    // As above, but with the §7.7 words supplied instead of derived. This is
    // the access-unit path: a substream that measured only its own channels
    // would reach a different gain from its siblings, and a decoder applies
    // each substream's word to that substream's channels - so disagreement
    // does not average out, it tilts the mix.
    [[nodiscard]] std::expected<std::vector<std::byte>, FrameError> encode_frame(
        std::span<const std::span<const float>> channels, const FrameMetadata& metadata,
        AuxPayload aux = {});

    [[nodiscard]] const FrameConfig& config() const;
    [[nodiscard]] int channel_count() const;
    // How many samples per channel one call to encode_frame consumes.
    [[nodiscard]] int samples_per_frame() const;

    // Roadmap PF6 - see ac3/latency.hpp for what each term means and
    // eac3_latency() below for why transient_prenoise is the only field of
    // FrameConfig that moves any of them.
    [[nodiscard]] LatencyBudget latency() const;
    [[nodiscard]] int latency_samples() const { return latency().total_samples(); }

   private:
    // Every private data member - config, MDCT history/scratch, the enhanced-
    // coupling scratch, the per-frame plan, the DRC controllers, EQ13's
    // codes-search incumbent, all of it - lives behind this one pimpl,
    // following the same pattern as ac3::io::WavStreamReader/Writer and
    // ac3::FrameEncoder. Impl is defined in eac3_frame.cpp, so a dllexport
    // class instantiating every implicit special member is why the
    // destructor and moves above are declared (not defaulted inline) here:
    // move-assignment's implicit reset() needs Impl complete.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One programme: an independent substream and the dependents that extend it.
// Every substream codes the same samples of the same programme, so a
// dependent contributes only its own channels, its chanmap and its share of
// the bit rate - and, since they are the same samples, every substream must
// carry the same numblkscod. AccessUnitEncoder's constructor refuses a
// mixture rather than building substreams a decoder would have no way to
// align against each other.
struct ProgrammeConfig {
    FrameConfig independent{};
    std::vector<FrameConfig> dependents{};
};

// An access unit: the first programme, plus any further ones sharing the same
// frame period.
//
// §E2.3.1.2 allows eight independent substreams (I0-I7) in one elementary
// stream. Broadcast DD+ uses the extra ones for the services A/52 §5.4.2.2
// names - a second language, an audio description, a commentary - so that one
// stream carries the main programme and its alternatives and a receiver picks
// between them. They are not layers of a soundfield the way dependents are:
// each is self-sufficient, each has its own layout and its own metadata, and
// only one is rendered at a time.
//
// `independent`/`dependents` are the first programme, kept spelled out at this
// level rather than moved into `programmes[0]` so that every caller that ever
// built a single-programme config still does.
struct AccessUnitConfig {
    FrameConfig independent{};
    std::vector<FrameConfig> dependents{};
    // I1-I7: further programmes, in transmission order, each with its own
    // dependents. Empty for the ordinary single-programme stream. substreamid
    // is assigned by position - the first programme is I0, additional[0] is
    // I1 and so on - the same way a dependent's id is assigned by its
    // position in `dependents`; FrameConfig::substreamid is not read here.
    std::vector<ProgrammeConfig> additional{};
};

// One access unit: the independent substream's frame followed by its
// dependents' in transmission order, concatenated exactly as they go on the
// wire. Nothing may sit between them and they may not be reordered - a decoder
// finds each substream by walking sync word and frmsiz, so the concatenation
// IS the framing.
struct AC3FORGE_EXPORT AccessUnit {
    std::vector<std::byte> bytes;
    // Byte length of each substream frame, independent first; sums to
    // bytes.size(). Retained because crc2 is per substream, so anything that
    // re-checks a written stream has to find these boundaries again.
    std::vector<std::uint32_t> substream_bytes;

    [[nodiscard]] std::size_t substream_count() const { return substream_bytes.size(); }
    [[nodiscard]] std::span<const std::byte> substream(std::size_t index) const;
};

// Words in a whole access unit. bitrate_kbps is PER SUBSTREAM - the substreams
// share one frame period, not one frame - so the total is the sum.
//
// CBR only. Under VBR the word count follows the content, so it cannot be
// known before a frame is actually encoded - callers must not call this when
// any substream's FrameConfig::vbr is set.
[[nodiscard]] AC3FORGE_EXPORT std::uint32_t access_unit_words(const AccessUnitConfig& config);

// TS 103 420 §8.2 fixes which substream carries the container: the LAST
// dependent substream if the access unit has any, otherwise the independent
// one. The object metadata describes the whole program, so it may not arrive
// before every substream that contributes to it.
//
// "The programme", specifically - so with additional programmes present the
// container rides in the last substream of the FIRST one, not the last
// substream on the wire. The objects belong to a programme; a later
// programme's substreams are a different piece of audio entirely and putting
// the container behind them would describe one programme with another's
// metadata position.
[[nodiscard]] AC3FORGE_EXPORT std::expected<AccessUnit, FrameError> build_silent_access_unit(
    const AccessUnitConfig& config, AuxPayload aux = {});

// Real audio across an independent substream and its dependents. One
// FrameEncoder per substream: each keeps its own MDCT overlap and runs its own
// SNR search against its own share of the rate.
class AC3FORGE_EXPORT AccessUnitEncoder {
   public:
    explicit AccessUnitEncoder(const AccessUnitConfig& config);
    // Declared (and defined in eac3_frame.cpp, where Impl below is complete)
    // rather than implicit/inline-defaulted: a dllexport class generates
    // every implicit special member whether or not called, and the
    // unique_ptr member makes the implicit copy deleted - which is fine -
    // but move-assignment's implicit reset() needs Impl complete, so it
    // cannot stay inline once Impl is only forward-declared here. Move-only,
    // following FrameEncoder above (substreams_ holds those).
    ~AccessUnitEncoder();
    AccessUnitEncoder(const AccessUnitEncoder&) = delete;
    AccessUnitEncoder& operator=(const AccessUnitEncoder&) = delete;
    AccessUnitEncoder(AccessUnitEncoder&&) noexcept;
    AccessUnitEncoder& operator=(AccessUnitEncoder&&) noexcept;

    // channels: every channel of the access unit grouped by substream in
    // transmission order - the independent's first (AC-3 order, Table 5.8,
    // LFE last), then each dependent's in the order its chanmap names them.
    // With additional programmes configured, every substream of the first
    // programme comes first, then every substream of the second, and so on:
    // the same order the substreams themselves go on the wire in.
    [[nodiscard]] std::expected<AccessUnit, FrameError> encode_access_unit(
        std::span<const std::span<const float>> channels, AuxPayload aux = {});

    [[nodiscard]] const AccessUnitConfig& config() const;
    // Summed across every substream of every programme: the span count
    // encode_access_unit expects.
    [[nodiscard]] int channel_count() const;

    // Roadmap PF6. Every substream of an access unit codes the same 1536
    // samples of the same program, so the frame and transform terms are
    // shared rather than summed - what a dependent substream CAN add is its
    // own §3.7 hold-back, since transproce is a per-substream flag and a
    // decoder holds back per substream identity. The worst term across the
    // whole unit is therefore the unit's own, and decode_access_unit's
    // assembly cache means one substream holding back delays the assembled
    // program, not just that substream.
    [[nodiscard]] LatencyBudget latency() const;
    [[nodiscard]] int latency_samples() const { return latency().total_samples(); }

   private:
    // Every private data member - config and the per-programme encoders/
    // metadata state - lives behind this one pimpl, following the same
    // pattern as ac3::io::WavStreamReader/Writer and ac3::FrameEncoder. Impl
    // is defined in eac3_frame.cpp.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3::eac3

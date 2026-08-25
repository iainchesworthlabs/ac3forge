#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "ac3/core/bitalloc.hpp"  // BitAllocCodes, for previous_codes_ below
#include "ac3/core/mantissas.hpp"  // MantissaToken, for the token scratch below
#include "ac3/core/tables.hpp"
#include "ac3/quality/distortion.hpp"
#include "ac3/encoder/silent_frame.hpp"  // FrameError, SkipPlan/plan_padding
#include "ac3/encoder/transient.hpp"
#include "ac3/export.hpp"
#include "ac3/latency.hpp"
#include "ac3/meta/bsi.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/mixing.hpp"
#include "ac3/verify/mirror.hpp"

// The AC-3 encoder: any audio coding mode (mono through 3/2) plus optional
// LFE, long blocks, optional channel coupling, 2/0 rematrixing, adaptive
// D15/D25/D45 exponents re-sent mid-frame when a channel's exponents drift
// (§8.2.8; the coupling channel is always D15, the LFE one D15 set per
// frame), static bit-allocation parameters (A/52 §8.2.12 basic-encoder
// defaults), global SNR-offset search to fill the frame.
//
// CBR at 44.1 kHz needs non-integral frame sizes: a Bresenham accumulator
// alternates between the two Table 5.18 lengths (even/odd frmsizecod) so the
// long-run rate is exact. At 32/48 kHz the same accumulator degenerates to
// the constant frame size.

namespace ac3 {

struct EncoderConfig {
    SampleRate sample_rate = SampleRate::k48000;
    std::uint32_t bitrate_kbps = 192;
    int dialnorm = 31;  // 1..31 (§5.4.2.8)
    // §5.4.2.16: Ch2's dialnorm, required when acmod is kDualMono (1+1) and
    // meaningless otherwise — the two programmes are levelled independently.
    std::optional<int> dialnorm2 = std::nullopt;
    int chbwcod = -1;  // fbw bandwidth code 0..60; -1 = auto from bitrate
    // §7.2.2.4 fast gain, Table 7.11. -1 asks for the encoder's own choice,
    // which is rate-dependent (see encoder.cpp step 0's measurement table);
    // 0..7 pins it. Pinned here rather than searched per frame for the reason
    // that comment gives: two code sets produce two different masking curves,
    // so the encoder's own composite SNR offset cannot compare them.
    int fgaincod = -1;
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    // Channel coupling (§7.4): above the coupling frequency the fbw channels
    // share one channel plus per-band coordinates. Needs >= 2 fbw channels;
    // the win shows up at low bit rates, where the saved coefficients buy
    // precision everywhere else. cplbegf/cplendf are sub-band indices; -1
    // asks the encoder to choose, which it does from the per-channel rate
    // (start) and from the bandwidth it would have coded anyway (end).
    bool coupling = false;
    int cplbegf = -1;
    int cplendf = -1;
    // §7.9.4 fast N/4-FFT forward MDCT (see mdct.hpp's mdct512_forward), on
    // by default since the owner accepted its quality evidence (verified max
    // relative error ~3e-12 against the direct form on random data and real
    // audio, 331 dB direct-vs-fast end-to-end SNR, 0.000 dB delta against an
    // independent oracle at 192-448 kbps; see tests/core/test_mdct_fast.cpp and
    // `tools/ci/quality_race.py fast-mdct`). false forces the direct §8.2.3.2
    // reference form, which stays maintained as the oracle the fast path is
    // validated against. All three forward transforms accelerate - the long
    // one and both halves of a block-switched pair, each down its own fold
    // (see mdct.hpp).
    bool fast_mdct = true;

    // §7.3.4 dithflag, decided per channel per block from content (see
    // src/forge/src/encoder/dither.hpp) - on by default, matching every other
    // config field here. false pins dithflag at 0 unconditionally, the
    // deterministic behaviour from before this existed: real dither values
    // are decoder-defined (the spec's own "any reasonably random sequence"),
    // so two independent, spec-correct decoders given the same dithered
    // stream diverge in the dithered bins by design - which is exactly what
    // breaks a bit-for-bit comparison between this project's own decoder and
    // an external one (tools/checks/verify_gold_reference.sh). That gate sets
    // this false; nothing else needs to.
    bool dither = true;

    // --- dynamic range and downmix metadata (§7.7, §7.8) -------------------
    // Dynamic range control. std::nullopt leaves dynrnge clear in every block,
    // which is what §7.7.1.2 says an encoder applying no compression does, and
    // keeps a DRC-free stream bit-identical to one from before this existed.
    std::optional<meta::Profile> drc = std::nullopt;
    // Heavy compression, independent of drc: the two answer different
    // questions (§7.7.2.1), so a stream may carry either, both or neither.
    std::optional<meta::HeavyConfig> heavy = std::nullopt;
    // Ch2's own drc/heavy, meaningful only under kDualMono. No fallback to
    // drc/heavy when unset - see plan::Metadata::drc2 for why: dialnorm2 is
    // already independent of dialnorm the same way.
    std::optional<meta::Profile> drc2 = std::nullopt;
    std::optional<meta::HeavyConfig> heavy2 = std::nullopt;
    // Table 5.9 / Table 5.10. Transmitted only when the layout has the
    // channels they describe, but they always define the §7.8 downmix, so the
    // heavy-compression peak detector consults them whatever acmod is.
    meta::CentreMixLevel cmixlev = meta::CentreMixLevel::kMinus4_5dB;
    meta::SurroundMixLevel surmixlev = meta::SurroundMixLevel::kMinus6dB;

    // --- bit stream information (§5.4.2, Annex D) --------------------------
    // The informational fields: what service this is, whether the 2/0 pair is
    // a Dolby Surround matrix, the mixing room it was judged in, the copyright
    // and original-bitstream bits, and the time code. Every default matches
    // the constant this encoder wrote before any of it was configurable, so a
    // config that leaves this alone produces the same bits it always did.
    // BsiInfo's dheadphonmod/dsurexmod/sourcefscod have no home in AC-3 bsi
    // and are not read here - Annex D's xbsi2 below carries the first two.
    meta::BsiInfo info{};
    // Annex D (§D2.2): std::nullopt writes bsid 8 with info.timecod1/2 in the
    // two 14-bit fields; set writes bsid 6 and spends those same 28 bits on
    // xbsi1/xbsi2 instead, at which point info.timecod1/2 are unwritable and
    // encode_frame() refuses rather than dropping them silently.
    std::optional<meta::AlternateBsi> alternate_bsi = std::nullopt;

    // --- decision search (ac3/quality) -------------------------------------
    // §7.2.2's bit allocation parameters are chosen once, from the bit rate,
    // and written into every frame of the encode. The comment at their
    // declaration in encoder.cpp records why they were never searched: the
    // only in-loop criterion this encoder had was the composite SNR offset,
    // and that number is not comparable between two code sets because each
    // produces a different masking curve for the offset to sit on.
    //
    // ac3::quality supplies the criterion that was missing - the error the
    // decoder will actually reconstruct - so with this set the encoder tries
    // a small set of candidate BitAllocCodes per frame, and decides the
    // delta-bit-allocation race on measured error rather than on the
    // composite offset each pass happened to reach.
    //
    // kNone by default, and not just because the search costs real time.
    // Validated on real CC0/CC-BY programme material against FFmpeg's decode
    // (SNR, log-spectral distance, ViSQOL MOS-LQO -
    // docs/library/encoding-ac3.md's own table has the numbers): kDistortion
    // is a real, repeatable win from 448 kbit/s up, but at 192 kbit/s its own
    // criterion still improves while LSD and MOS both worsen - redistributing
    // bits away from dbpbcod's quiet-band floor buys back less SNR than it
    // costs in per-band spectral shape at that budget. kPerceptual
    // loses outright at every rate tested, despite its psychoacoustic model
    // being independently validated (tests/quality/test_perceptual.cpp): its
    // objective correctly discounts already-masked headroom, which leaves it
    // much thinner decision margins than raw distortion, and on real stereo
    // material with rematrixing active those margins are landing on the
    // wrong side of external metrics. This project does not turn a decision
    // knob on without the numbers to justify it, and right now only
    // kDistortion at the higher rates has them. `ac3cli encode search=...`
    // sets it.
    quality::Criterion search = quality::Criterion::kNone;

    // --- self-check (ac3/verify/mirror.hpp) --------------------------------
    // When set, encode_frame() records its own model of the decoder - the bit
    // offset at each block boundary, and each stream's decoded exponents, bit
    // allocation and delta correction - into this trace, for comparison
    // against a real decode of the same frame. Null by default, and null
    // costs one branch per block and no allocation: the encoder's behaviour
    // and output are identical either way, this only reads state it already
    // has. A runtime pointer rather than a build-time switch because that is
    // how every other optional behaviour in this struct is expressed, and
    // because a check that needs a special build is a check nobody runs.
    // MirrorEncoder (ac3/verify/selfcheck.hpp) drives the whole comparison;
    // this is for a caller wanting to place the decode themselves.
    verify::FrameTrace* trace = nullptr;
};

class AC3FORGE_EXPORT FrameEncoder {
   public:
    explicit FrameEncoder(const EncoderConfig& config);
    // Declared (and defaulted in encoder.cpp, where PlanScratch below is
    // complete) rather than implicit: a dllexport class generates every
    // implicit member whether or not called, and the unique_ptr member
    // makes the implicit copy deleted - which is fine - but the moves must
    // be spelled out or the declared destructor suppresses them. The same
    // move-only shape eac3::FrameEncoder took for its FrameState.
    ~FrameEncoder();
    FrameEncoder(FrameEncoder&&) noexcept;
    FrameEncoder& operator=(FrameEncoder&&) noexcept;

    // channels: the full-bandwidth channels in AC-3 order (Table 5.8: e.g.
    // 3/2 = L, C, R, SL, SR), followed by the LFE channel last when
    // config.lfe is set. Each span holds exactly kSamplesPerFrame samples,
    // nominally in [-1, 1). Returns one complete syncframe.
    [[nodiscard]] std::expected<std::vector<std::byte>, FrameError> encode_frame(
        std::span<const std::span<const float>> channels);

    [[nodiscard]] const EncoderConfig& config() const;
    [[nodiscard]] int channel_count() const;

    // Roadmap PF6. Constant for the life of the encoder - nothing in
    // EncoderConfig moves any term (see ac3/latency.hpp for what each one
    // is): AC-3 has one frame length, this encoder needs no lookahead, and
    // §3.7's hold-back is an Annex E tool AC-3 does not have. Reported as a
    // member function rather than a free constant so a caller holding an
    // encoder can ask it directly, and so the E-AC-3 and Atmos encoders -
    // where the answer DOES depend on the configuration - answer the same
    // question the same way.
    [[nodiscard]] LatencyBudget latency() const {
        return LatencyBudget{.frame_samples = kSamplesPerFrame,
                             .transform_samples = kTransformDelaySamples,
                             .lookahead_samples = 0,
                             .holdback_samples = 0};
    }
    [[nodiscard]] int latency_samples() const { return latency().total_samples(); }

   private:
    // Every private data member - the MDCT history/scratch, the §8.2.8
    // exponent-strategy plan, the coupling work buffers, the DRC controllers,
    // all of it - lives behind this one pimpl, following the same pattern as
    // ac3::io::WavStreamReader/Writer. Impl is defined in encoder.cpp, so a
    // dllexport class instantiating every implicit special member is why the
    // destructor and moves above are declared (not defaulted inline) here:
    // move-assignment's implicit reset() needs Impl complete.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3

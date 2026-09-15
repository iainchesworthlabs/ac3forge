#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/export.hpp"
#include "ac3/meta/bsi.hpp"
#include "ac3/meta/mixing.hpp"

// The decoder's output stage: what happens between "the coded channels have
// been reconstructed" and "these are the samples a listener hears".
//
// Everything here is off by default. The decoders exist first as a check on
// the encoder, and a stage that silently re-levelled or re-folded their
// output would destroy that - see DecoderConfig::drc_scale's own comment for
// the same reasoning applied to §7.7. A caller that wants a listenable
// rendering rather than the coded channels asks for one.
//
// Three things live here, in the order §7.7/§7.8 apply them:
//
//   1. dialnorm normalisation (§5.4.2.8). The stream says where its dialogue
//      sits; a decoder normalising to the -31 dBFS reference attenuates by
//      the difference, which is what makes two programmes cut together at a
//      consistent loudness. Dual mono (acmod 0) codes two UNRELATED
//      programmes in one syncframe, each with its own dialnorm - §5.4.2.16's
//      dialnorm2 for Ch2 - so this step takes an optional second dialnorm
//      and normalises Ch2 by its own reference rather than by Ch1's.
//   2. The §7.8 downmix, to Lo/Ro stereo, Lt/Rt stereo or mono, driven by
//      the stream's OWN mix levels (AC-3's cmixlev/surmixlev or Annex D's
//      xbsi1 group, E-AC-3's mixmdate group) rather than by constants chosen
//      here.
//   3. RF mode's overload protection, which only exists because §7.7.2's
//      compr guarantee is about the mono downmix and not about whichever
//      fold this stage was actually asked for.
//
// §7.7's dynrng/compr gain itself is NOT here: both decoders apply it to the
// COEFFICIENTS, before the IMDCT, so the overlap-add window cross-fades a
// per-block gain change instead of stepping it (see decoder.cpp's own comment
// at that site). OperatingMode below selects which of the two words is used,
// and whether a compr word comes with RF mode's 11 dB; the arithmetic stays
// where it belongs. So an RF-mode decode's level is only partly this stage's:
// the dialnorm normalisation is, the 11 dB above it are the decoders'.

namespace ac3 {

// Which fold to produce. kAsCoded is the default and does nothing at all -
// the coded channels come out exactly as they went in.
enum class DownmixTarget : std::uint8_t {
    kAsCoded,
    kLoRo,  // §7.8.1's plain stereo fold
    kLtRt,  // §7.8.2's Dolby Surround compatible fold
    kMono,  // §7.8's "output_mode == 1/0" branch
};

// §7.7's two canonical consumer decoder modes, as one choice rather than as
// two independent switches a caller can set to a combination that means
// nothing. kCustom leaves DecoderConfig::drc_scale/heavy_compression exactly
// as the caller set them, which is what every existing caller gets.
enum class OperatingMode : std::uint8_t {
    // Whatever drc_scale/heavy_compression already say. The default.
    kCustom,
    // §7.7.1: dialnorm normalisation plus the full transmitted dynrng. What
    // a decoder feeding a wide-dynamic-range playback system does.
    kLine,
    // §7.7.2: dialnorm normalisation plus compr (falling back on dynrng for
    // any syncframe carrying no compr word, per §7.7.2.1), and the downmix
    // overload protection below. What a set-top box feeding an RF modulator
    // does, where the whole point is that nothing ever clips.
    //
    // Every compr word is applied with RF mode's 11 dB (meta::kRfModeGainDb),
    // which puts dialogue at -20 dBFS against line mode's -31. A syncframe
    // that falls back on dynrng gets no 11 dB and plays at line mode's level,
    // so a stream with no compr words at all takes the same gains as kLine,
    // the overload protection aside. The Dolby Reference Player's RF mode
    // does both, measured frame by frame, and Dolby's encoder writes its
    // compr words for this decode, as meta::HeavyCompressor does.
    kRf,
};

struct OutputConfig {
    DownmixTarget target = DownmixTarget::kAsCoded;
    OperatingMode mode = OperatingMode::kCustom;
    // §5.4.2.8 normalisation onto the -31 dBFS reference. kLine and kRf both
    // imply it (that is what makes them the canonical modes rather than two
    // more knobs), so this only has to be set for kCustom. kRf's -20 dBFS
    // dialogue level is this normalisation plus the 11 dB its compr words
    // are applied with (see kRf above).
    bool apply_dialnorm = false;
    // §7.8 makes the LFE's contribution to a downmix optional, and decoders
    // drop it by default - it is the channel most likely to overload a fold
    // and the least likely to be missed. When this is set the LFE is mixed
    // in at the stream's own lfemixlevcod where it has one (E-AC-3), and at
    // §7.8's stated ideal of +10 dB relative to left and right where it does
    // not (AC-3, which has no field for it). An E-AC-3 stream that
    // deliberately sent NO lfemixlevcod has disabled LFE mixing in the
    // bitstream (§E2.3.1.10) and is honoured: this flag cannot override it.
    bool mix_lfe = false;
    // Whether Lt/Rt's surround sum really is phase shifted 90 degrees before
    // it is matrixed, or only polarity-inverted into Lt. The shift is what
    // §7.8.2 describes and what a Dolby Surround decoder steers on, so it is
    // the default; it costs latency_samples() of delay on the whole output,
    // because the direct path has to be delayed to stay aligned with the
    // shifted one. Clearing it gives the sign-only matrix - no latency, and
    // what a lot of hardware actually implements - at the cost of the
    // surround sum no longer being in quadrature.
    //
    // One consequence worth knowing before choosing between them: a phase
    // shifter preserves ENERGY, not peak. §7.8.1's normalisation bounds a sum
    // of plain COEFFICIENTS, so it bounds Lo/Ro, mono and the sign-only Lt/Rt
    // by the loudest coded sample - but it cannot bound the shifted path,
    // whose response at a discontinuity is unbounded. A shifted Lt/Rt fold can
    // therefore come out louder than its inputs were. That follows from what
    // §7.8.2 asks for rather than from anything decided here; kRf below is
    // what does guarantee a ceiling.
    bool ltrt_phase_shift = true;
    // kRf's ceiling, as a linear sample magnitude. Full scale by default: RF
    // mode's promise is that the fold does not clip, and a decoder that held
    // back more headroom than it was asked for would just be quieter than it
    // needed to be.
    double rf_ceiling = 1.0;
};

// What the stream itself says about folding down, resolved from whichever
// syntax carried it. AC-3 carries two coarse levels in bsi and nothing about
// the LFE, and an Annex D (bsid 6) stream can add separate Lt/Rt and Lo/Ro
// levels in xbsi1; E-AC-3 carries separate Lt/Rt and Lo/Ro levels plus an LFE
// level inside mixmdate. Resolving all of them into one shape here is what
// lets the output stage be written once - see mix_levels() below for the
// conversions, including what each syntax's defaults are when a field is
// simply not present.
struct MixLevels {
    double loro_clev = meta::level::kMinus4_5dB;
    double loro_slev = meta::level::kMinus6dB;
    double ltrt_clev = meta::level::kMinus3dB;
    double ltrt_slev = meta::level::kMinus3dB;
    // std::nullopt means the stream disabled LFE mixing (§E2.3.1.10's absent
    // lfemixlevcod), which OutputConfig::mix_lfe deliberately cannot override.
    std::optional<double> lfe_mix_level_db = meta::lfe_mix_level_db(meta::kLfeMixLevelIdeal);
    // Table D2.2's dmixmod - which fold the CONTENT was authored to be heard
    // through, when it says. Advisory: a caller asking for a specific
    // DownmixTarget gets that target. It is what a UI would offer as the
    // stream's own preference; automatic_stereo_target() below turns it into
    // one. mix_levels() reads it from E-AC-3's mixmdate, or from an Annex D
    // (bsid 6) AC-3 stream's own xbsi1 group where the stream carries one -
    // bsi's two coarse levels say nothing about it. Either source, a reserved
    // '11' is reported here as kReserved, as sent.
    meta::DownmixMode preferred = meta::DownmixMode::kNotIndicated;
};

// §D3.1.1's third choice for a two-channel output: "automatic selection of
// either Lt/Rt or Lo/Ro based on the preferred downmix mode parameter
// dmixmod". '01' gives Lt/Rt and '10' gives Lo/Ro, both folds this stage
// produces. Every other case gets Lo/Ro, the plain fold a two-channel output
// gets when nothing is preferred: '00', a stream that sends no dmixmod at all,
// and the reserved '11', which §D2.3.1.2 allows a decoder to read as "not
// indicated". Neither A/52 nor TS 102 366 V1.4.1 assigns '11' a downmix, so no
// code can ask for a fold this stage lacks. Should a later revision define
// one, it gets an enumerator of its own and -Wswitch flags the switch below
// until the new code is given a fold.
//
// `acmod` gates the whole field, not just the reserved code: Table D2.2's own
// NOTE says dmixmod's meaning "is only defined ... if the audio coding mode is
// 3/0, 2/1, 3/1, 2/2 or 3/2. If the audio coding mode is 1+1, 1/0 or 2/0 then
// the meaning of this field is reserved" - whatever code it carries. That is
// the same acmod > 0x2 boundary Table E1.2 already gates mixmdate's own
// dmixmod on (E-AC-3 simply never transmits the field below it), so this only
// changes behaviour for AC-3's Annex D xbsi1, whose fixed Table D2.1 layout
// carries all five levels unconditionally and has no wire-level gate of its
// own - FrameHeader::dmixmod reports whatever xbsi1 said even at a narrow
// acmod, exactly as transmitted, and it is this function's job to then treat
// that as no preference rather than act on a code the standard does not
// define there.
//
// ac3cli's downmix=auto is this function applied to the dmixmod and acmod of
// the first syncframe of the programme it decodes.
[[nodiscard]] constexpr DownmixTarget automatic_stereo_target(Acmod acmod,
                                                               meta::DownmixMode preferred) {
    if (static_cast<std::uint8_t>(acmod) <= 0x2) {
        return DownmixTarget::kLoRo;
    }
    switch (preferred) {
        case meta::DownmixMode::kLtRt:
            return DownmixTarget::kLtRt;
        case meta::DownmixMode::kNotIndicated:
        case meta::DownmixMode::kLoRo:
        case meta::DownmixMode::kReserved:
            break;
    }
    return DownmixTarget::kLoRo;
}

// AC-3 (§5.4.2.4/§5.4.2.5). Both arguments are std::nullopt for any acmod
// whose bsi does not carry that field, and the §7.8 defaults stand in: -4.5 dB
// centre and -6 dB surround, the mid-range choices a decoder makes when it has
// not been told. bsi has no Lt/Rt levels at all, so those keep §7.8.2's own
// -3 dB; and no LFE mix level, so §7.8's stated +10 dB ideal stands. That is
// the whole of a bsid-8 stream's downmix information; the overload below adds
// what an Annex D stream can say on top of it.
[[nodiscard]] AC3FORGE_EXPORT MixLevels mix_levels(
    std::optional<meta::CentreMixLevel> cmixlev, std::optional<meta::SurroundMixLevel> surmixlev);

// AC-3 including Annex D's xbsi1 group (bsid 6). `alternate` is
// DecodedFrame::alternate_bsi, std::nullopt for bsid 8. §D3 makes compliant
// decoding of the alternate syntax optional; this library implements it, and
// FrameDecoder folds with this overload.
//
// Without xbsi1 (bsid 8, or bsid 6 with xbsi1e clear) the result is the
// overload above, field for field: §D3.1.2 has a decoder downmix as the
// original specification defines when the parameters are not in the stream.
//
// With xbsi1, §D3.1.2 has a compliant decoder use the levels associated with
// the two-channel downmix it has selected: ltrtcmixlev/ltrtsurmixlev for Lt/Rt,
// lorocmixlev/lorosurmixlev for Lo/Ro. They replace cmixlev/surmixlev, which
// §D4.2.1 says they override (a bsid-6 encoder still has to send valid bsi
// levels for legacy decoders). The mono fold takes the Lo/Ro pair, because
// §7.8.2 defines mono as Lo/Ro summed. ETSI TS 102 366 V1.4.1 clause D.2.1.2
// says the same.
//
// `preferred` is xbsi1's dmixmod for the acmods Table D2.2's note defines it
// for: 3/0, 2/1, 3/1, 2/2 and 3/2. For 1+1, 1/0 and 2/0 the note leaves the
// field's meaning reserved, so it stays kNotIndicated, which is also what
// E-AC-3 gives those acmods by not sending dmixmod. The LFE keeps §7.8's
// +10 dB ideal: Annex D has no LFE mix level, and reading an absent
// lfemixlevcod as "LFE mixing disabled" is §E2.3.1.10's rule for Annex E.
//
// The four levels are converted as they are given. FrameDecoder has already
// read a reserved surround level (Tables D2.4/D2.6) as -1.5 dB by then, the
// same substitution the E-AC-3 reader makes for mixmdate.
[[nodiscard]] AC3FORGE_EXPORT MixLevels mix_levels(
    Acmod acmod, std::optional<meta::CentreMixLevel> cmixlev,
    std::optional<meta::SurroundMixLevel> surmixlev,
    const std::optional<meta::AlternateBsi>& alternate);

// E-AC-3 (Table E1.2's mixmdate group). std::nullopt - no mixmdate on the
// wire at all - falls back on the AC-3 defaults above rather than on zero, so
// a stream that says nothing folds down the same way either generation of it
// would.
[[nodiscard]] AC3FORGE_EXPORT MixLevels mix_levels(const std::optional<meta::MixMetadata>& mix);

// How many channels apply() will leave behind for a given coded programme.
// kAsCoded reports the coded count unchanged.
[[nodiscard]] AC3FORGE_EXPORT std::size_t output_channel_count(const OutputConfig& config,
                                                               Acmod acmod, bool lfe);

// The stage itself. Stateful: the Lt/Rt phase shift carries a filter tail
// across frames and RF mode carries its protection gain, so one instance
// belongs to one stream and frames go through it in order - the same
// contract the decoders' own overlap-add state has.
class AC3FORGE_EXPORT OutputStage {
   public:
    OutputStage() = default;
    explicit OutputStage(const OutputConfig& config) : config_(config) {}

    // Folds one frame in place. `channels` is the coded order of Table 5.8
    // with the LFE last, exactly as DecodedFrame::channels holds it, and is
    // resized down to output_channel_count() on return. A kAsCoded stage
    // returns without touching anything.
    //
    // Dual mono (acmod 0) is left alone whatever the target says: 1+1 is two
    // unrelated programmes and there is no fold of "both at once" that means
    // anything - §7.8's own dual-mono branch is a choice of WHICH programme
    // to listen to, which is a routing decision above this layer rather than
    // a matrix. output_channel_count() reports 2 for it for the same reason.
    // Dual mono IS still normalised, though, whenever kLine/kRf/apply_dialnorm
    // ask for it - which is what `dialnorm2` is for.
    //
    // `dialnorm2` is §5.4.2.16's own dialnorm for Ch2, meaningful only under
    // acmod kDualMono: channels[1] (Ch2) is normalised by its own reference
    // rather than by `dialnorm` (Ch1's) - the two programmes are unrelated,
    // and an encoder sizes Ch2's compr2 on the assumption Ch2 IS levelled by
    // dialnorm2. Left at std::nullopt, Ch2 falls back to `dialnorm` like
    // every other channel - the pre-existing behaviour, wrong for 1+1 but the
    // only sane default when a caller has no dialnorm2 to give.
    void apply(std::vector<std::vector<float>>& channels, Acmod acmod, bool lfe,
               const MixLevels& levels, int dialnorm, std::optional<int> dialnorm2 = std::nullopt);

    // The same fold over caller-owned planar storage, for the decoders'
    // *_into forms. Writes the fold into the first output_channel_count()
    // spans and leaves the rest untouched - it does not zero the channels a
    // fold has consumed, because the caller owns that storage and knows from
    // the same function how much of it is now meaningful. `dialnorm2` is as
    // above.
    void apply(std::span<const std::span<float>> channels, Acmod acmod, bool lfe,
               const MixLevels& levels, int dialnorm, std::optional<int> dialnorm2 = std::nullopt);

    // The fold over a RENDERED E-AC-3 program: `channels` parallel to
    // `layout` (Table E2.5 order), rather than in an acmod's Table 5.8 coded
    // order. §7.8 defines folds FROM the eight AC-3 acmods and says nothing
    // about the wide layouts Annex E's chanmap can express, so a layout with
    // no acmod of its own is reduced to the nearest one first - every extra
    // location seated where it obviously belongs (a wide left is a left, a
    // rear surround is a surround, a top front left is a left), at -3 dB
    // where it shares a seat. That reduction is an extension beyond §7.8 and
    // the .cpp says so at the point it happens; it is an exact identity for
    // every plain acmod bed, which is the case that must not change.
    //
    // Writes the fold into the first spans exactly as the overload above
    // does. A layout with no locations at all (dual mono, which
    // DecodedAccessUnit leaves empty) falls through to that overload, and
    // `dialnorm2` reaches it unchanged - see that overload's own comment.
    void apply(std::span<const std::span<float>> channels,
               const eac3::chanmap::Layout& layout, Acmod acmod, bool lfe,
               const MixLevels& levels, int dialnorm, std::optional<int> dialnorm2 = std::nullopt);

    // Samples of delay the stage adds, all of it the Lt/Rt phase shift's -
    // zero for every other target, and zero for Lt/Rt with the shift off.
    // A caller lining decoded output up against its source accounts for this
    // the same way it would for any filter.
    [[nodiscard]] int latency_samples() const;

    // Drops the filter tail and the protection gain. For reuse across
    // streams; a stream decoded in order never needs it.
    void reset();

    [[nodiscard]] const OutputConfig& config() const { return config_; }

    // The protection attenuation RF mode is currently holding, in dB (0.0
    // when nothing is being held back, and never positive). Reported so a
    // test can assert the limiter engaged rather than only that the output
    // stayed under the ceiling, which silence also satisfies.
    [[nodiscard]] double rf_protection_db() const;

   private:
    OutputConfig config_{};
    // The 90-degree phase shifter's history, and the matched delay line the
    // direct path runs through so the two stay aligned. Sized lazily at first
    // Lt/Rt use - a stage that never folds to Lt/Rt never allocates either,
    // the same reasoning Eac3Decoder's own aht_coeffs_/ecpl_all_coeffs_ use.
    std::vector<float> shift_history_;
    std::vector<std::vector<float>> direct_history_;
    std::vector<float> delay_scratch_;
    // The vector form's views onto its own argument, so lending them to the
    // span form costs no allocation after the first frame.
    std::vector<std::span<float>> views_;
    // The rendered-layout form's own working storage: the wide Table E2.5
    // layout reduced to the §7.8 acmod layout nearest it, a block of each
    // seat at a time, and views onto the seats that reduction filled. Members
    // so a steady-state decode allocates nothing; empty unless that overload
    // is actually used.
    std::vector<std::vector<float>> fold_scratch_;
    std::vector<std::span<float>> fold_views_;
    // Reused across frames so a steady-state decode allocates nothing: a
    // block of the fold's two output channels, and of the surround sum
    // feeding the shifter. A block, not a frame - see the .cpp's kFoldBlock.
    std::vector<float> out_left_;
    std::vector<float> out_right_;
    std::vector<float> surround_sum_;
    // kRf's smoothed attenuation, carried between frames - see the .cpp's own
    // comment on why a per-frame gain is ramped rather than stepped.
    double protection_gain_ = 1.0;
};

}  // namespace ac3

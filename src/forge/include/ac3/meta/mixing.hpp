#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "ac3/core/tables.hpp"
#include "ac3/export.hpp"

// Mixing and downmix metadata, and the §7.8 downmix the values feed.
//
// A receiver almost never has as many loudspeakers as the stream has
// channels, so these few bits decide what most listeners actually hear.
// AC-3 carries two coarse 2-bit levels in bsi (§5.4.2.4, §5.4.2.5); E-AC-3
// drops them entirely and carries a richer group inside mixmdate instead —
// separate levels for the matrixed Lt/Rt and the plain Lo/Ro downmix, so a
// mix that folds down badly one way can be corrected without spoiling the
// other, plus an LFE mix level AC-3 has no way to express.

namespace ac3::meta {

// The printed table values (0.707, 0.595, 0.841 …) are rounded quarter-powers
// of two; these are the exact ones, so that a chain of them is exact.
namespace level {
inline constexpr double kPlus3dB = 1.4142135623730951;    // 2^(1/2)
inline constexpr double kPlus1_5dB = 1.1892071150027210;  // 2^(1/4)
inline constexpr double kUnity = 1.0;
inline constexpr double kMinus1_5dB = 0.8408964152537145;  // 2^(-1/4)
inline constexpr double kMinus3dB = 0.7071067811865476;    // 2^(-1/2)
inline constexpr double kMinus4_5dB = 0.5946035575013605;  // 2^(-3/4)
inline constexpr double kMinus6dB = 0.5;
inline constexpr double kSilent = 0.0;
}  // namespace level

// §5.4.2.4, Table 5.9. '11' is reserved; §5.4.2.4 tells a decoder receiving it
// to fall back on the intermediate −4.5 dB, so it is not offered here.
enum class CentreMixLevel : std::uint8_t {
    kMinus3dB = 0,
    kMinus4_5dB = 1,
    kMinus6dB = 2,
};

// §5.4.2.5, Table 5.10. '10' is a genuine value — surround channels dropped
// from the downmix altogether — not a reserved code.
enum class SurroundMixLevel : std::uint8_t {
    kMinus3dB = 0,
    kMinus6dB = 1,
    kSilent = 2,
};

// Tables D2.3 / D2.5, the 3-bit levels E-AC-3 carries inside mixmdate. The
// surround variants (Tables D2.4 / D2.6) reserve codes 0–2, so only
// kMinus1_5dB and below are legal there — see valid_surround_mix_level().
enum class MixLevel : std::uint8_t {
    kPlus3dB = 0,
    kPlus1_5dB = 1,
    kUnity = 2,
    kMinus1_5dB = 3,
    kMinus3dB = 4,
    kMinus4_5dB = 5,
    kMinus6dB = 6,
    kSilent = 7,
};

// Table D2.2. '11' is reserved and reads as "not indicated".
enum class DownmixMode : std::uint8_t {
    kNotIndicated = 0,
    kLtRt = 1,
    kLoRo = 2,
};

[[nodiscard]] constexpr double coefficient(CentreMixLevel value) {
    switch (value) {
        case CentreMixLevel::kMinus3dB:
            return level::kMinus3dB;
        case CentreMixLevel::kMinus4_5dB:
            return level::kMinus4_5dB;
        case CentreMixLevel::kMinus6dB:
            return level::kMinus6dB;
    }
    return level::kMinus4_5dB;
}

[[nodiscard]] constexpr double coefficient(SurroundMixLevel value) {
    switch (value) {
        case SurroundMixLevel::kMinus3dB:
            return level::kMinus3dB;
        case SurroundMixLevel::kMinus6dB:
            return level::kMinus6dB;
        case SurroundMixLevel::kSilent:
            return level::kSilent;
    }
    return level::kMinus6dB;
}

[[nodiscard]] constexpr double coefficient(MixLevel value) {
    switch (value) {
        case MixLevel::kPlus3dB:
            return level::kPlus3dB;
        case MixLevel::kPlus1_5dB:
            return level::kPlus1_5dB;
        case MixLevel::kUnity:
            return level::kUnity;
        case MixLevel::kMinus1_5dB:
            return level::kMinus1_5dB;
        case MixLevel::kMinus3dB:
            return level::kMinus3dB;
        case MixLevel::kMinus4_5dB:
            return level::kMinus4_5dB;
        case MixLevel::kMinus6dB:
            return level::kMinus6dB;
        case MixLevel::kSilent:
            return level::kSilent;
    }
    return level::kMinus3dB;
}

// Tables D2.4 / D2.6 reserve '000'..'010'; a decoder receiving one substitutes
// 0.841. Writing a reserved code would therefore mean the level we intended is
// silently not the level applied, so the encoder refuses instead.
[[nodiscard]] constexpr bool valid_surround_mix_level(MixLevel value) {
    return static_cast<std::uint8_t>(value) >= static_cast<std::uint8_t>(MixLevel::kMinus1_5dB);
}

// §E2.3.1.11: LFE mix level (dB) = 10 − lfemixlevcod, so codes 0..31 span
// +10 dB down to −21 dB. §7.8 calls an LFE contribution of +10 dB relative to
// left and right the ideal, which is code 0.
[[nodiscard]] constexpr double lfe_mix_level_db(int code) {
    return 10.0 - static_cast<double>(code);
}
inline constexpr int kLfeMixLevelIdeal = 0;

// The whole mixmdate group an E-AC-3 substream can carry. Which fields are
// actually written depends on acmod and lfeon exactly as Table E1.2 says; the
// values here are what goes out when the corresponding field exists.
struct MixMetadata {
    DownmixMode dmixmod = DownmixMode::kNotIndicated;
    MixLevel ltrtcmixlev = MixLevel::kMinus3dB;
    MixLevel lorocmixlev = MixLevel::kMinus3dB;
    MixLevel ltrtsurmixlev = MixLevel::kMinus3dB;
    MixLevel lorosurmixlev = MixLevel::kMinus3dB;
    // §E2.3.1.10: absent means LFE mixing is DISABLED, which is a decision in
    // its own right and not the same as sending code 31.
    std::optional<int> lfemixlevcod = std::nullopt;
};

// --- §7.8 downmixing -------------------------------------------------------

// Un-normalised then normalised per §7.8.1: "attenuating all downmix
// coefficients equally, such that the sum of coefficients used to create any
// single output channel never exceeds 1". Indices follow the coded order of
// Table 5.8; entries past the acmod's channel count are zero. The LFE never
// appears — §7.8 makes its downmix optional and decoders drop it by default.
struct DownmixCoefficients {
    std::array<double, 5> left{};
    std::array<double, 5> right{};
};

// Lo/Ro: the plain stereo fold-down, and the one a mono sum is taken from.
[[nodiscard]] AC3FORGE_EXPORT DownmixCoefficients stereo_downmix(Acmod acmod, double clev,
                                                                 double slev);

// §7.8.2's Dolby Surround compatible fold: Lt = L + clev·C − slev·S,
// Rt = R + clev·C + slev·S, where S is the surround SUM phase shifted by 90
// degrees. A coefficient cannot express a phase shift, so the surround path
// is kept separate from the direct one here rather than folded into a single
// DownmixCoefficients: `direct` holds the coefficients applied to the
// channels as coded (its surround entries are zero), `surround` holds the
// coefficients of the channels that form the sum, and the caller applies the
// shift to that sum before adding it — negated into Lt, positive into Rt.
// See ac3::OutputStage, which owns the shift itself.
//
// Normalisation (§7.8.1) is over the WORST CASE of the two paths together,
// since the shifted sum is not generally in quadrature with everything at
// once: a coefficient set whose direct and surround magnitudes could sum
// above 1 is attenuated until it cannot.
struct LtRtCoefficients {
    DownmixCoefficients direct{};
    std::array<double, 5> surround{};
};

[[nodiscard]] AC3FORGE_EXPORT LtRtCoefficients ltrt_downmix(Acmod acmod, double clev,
                                                            double slev);

// §7.8's "output_mode == 1/0" branch: left and right at −3 dB, centre at
// clev + 3 dB, each surround at slev − 3 dB, then normalised. This is the
// signal §7.7.2 promises to keep under a ceiling.
[[nodiscard]] AC3FORGE_EXPORT std::array<double, 5> mono_downmix(Acmod acmod, double clev,
                                                                 double slev);

// True peak of the mono downmix, in dBFS. channels holds the full-bandwidth
// channels in coded order; any LFE span is ignored.
//
// history is the previous frame's last 256 samples per channel - the MDCT
// overlap. Those samples are windowed into THIS frame's block 0, so this
// frame's compr is what a decoder applies to them, and leaving them out is
// precisely how a hard loud-to-quiet transition breaks §7.7.2's ceiling: the
// frame that has just gone quiet carries a generous gain over a block that
// still holds the loud tail. Pass an empty span when there is no history to
// account for (the first frame, or a caller that only wants this frame).
[[nodiscard]] AC3FORGE_EXPORT double mono_downmix_peak_dbfs(
    std::span<const std::array<double, 256>> history,
    std::span<const std::span<const float>> channels, Acmod acmod, double clev, double slev);

[[nodiscard]] AC3FORGE_EXPORT double mono_downmix_peak_dbfs(
    std::span<const std::span<const float>> channels, Acmod acmod, double clev, double slev);

// --- §7.7/§7.8 output-stage levels -----------------------------------------

// §5.4.2.8: dialnorm says where average dialogue sits, as −dialnorm dBFS, and
// a decoder normalising to the −31 dBFS reference attenuates by the
// difference. dialnorm 31 is already the reference and returns exactly 1.0;
// every legal value below it is an attenuation, never a boost, so this can
// only ever reduce level. The reserved value 0 is treated as 31 (no change) -
// §5.4.2.8 forbids emitting it and a decoder has no better reading of it than
// "no information", which is what leaving the audio alone says.
[[nodiscard]] AC3FORGE_EXPORT double dialnorm_gain(int dialnorm);
inline constexpr int kReferenceDialnorm = 31;

// The linear gain of §E2.3.1.11's LFE mix level, for the §7.8 LFE
// contribution a decoder may fold in. lfe_mix_level_db(kLfeMixLevelIdeal) is
// §7.8's stated ideal of +10 dB relative to left and right.
[[nodiscard]] AC3FORGE_EXPORT double lfe_mix_gain(double level_db);

}  // namespace ac3::meta

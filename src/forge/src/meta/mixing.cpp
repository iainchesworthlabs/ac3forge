#include "ac3/meta/mixing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>

#include "ac3/core/tables.hpp"
#include "ac3/meta/drc.hpp"  // to_db

namespace ac3::meta {

namespace {

// Where each acmod's channels sit in the coded order of Table 5.8. −1 means
// the layout has no such channel.
struct Layout {
    int left = -1;
    int centre = -1;
    int right = -1;
    int surround = -1;        // the single surround of 2/1 and 3/1
    int left_surround = -1;
    int right_surround = -1;
};

Layout layout_of(Acmod acmod) {
    switch (acmod) {
        case Acmod::k1_0: return {.centre = 0};
        case Acmod::k2_0: return {.left = 0, .right = 1};
        case Acmod::k3_0: return {.left = 0, .centre = 1, .right = 2};
        case Acmod::k2_1: return {.left = 0, .right = 1, .surround = 2};
        case Acmod::k3_1: return {.left = 0, .centre = 1, .right = 2, .surround = 3};
        case Acmod::k2_2:
            return {.left = 0, .right = 1, .left_surround = 2, .right_surround = 3};
        case Acmod::k3_2:
            return {.left = 0, .centre = 1, .right = 2, .left_surround = 3,
                    .right_surround = 4};
        case Acmod::kDualMono:
            // 1+1 is two programmes, not a layout; §7.8's dual-mono branch
            // depends on which programme the listener asked for, so there is
            // no single set of coefficients to return.
            return {};
    }
    return {};
}

// §7.8.1: "Normalization is achieved by attenuating all downmix coefficients
// equally, such that the sum of coefficients used to create any single output
// channel never exceeds 1."
//
// ATTENUATING, and only to satisfy a bound. A sum already at or below 1 is left
// alone rather than scaled up to 1: the clause exists to prevent overload, not
// to equalise gain. It matters for a 1/0 source folded to two speakers, where
// scaling up would put the centre channel at unity in BOTH outputs and so add
// 3 dB of acoustic power that was not in the programme.
void normalize(std::array<double, 5>& coeffs) {
    double sum = 0.0;
    for (const double c : coeffs) {
        sum += c;
    }
    if (sum <= 1.0) {
        return;
    }
    for (double& c : coeffs) {
        c /= sum;
    }
}

// §7.8.1 for the two-path Lt/Rt form. The bound has to hold for the direct
// and shifted paths TOGETHER: a 90-degree shift keeps the surround sum out of
// phase with a steady tone in the fronts, but says nothing about a transient,
// where both paths can peak at once. Summing the magnitudes is the only bound
// that holds for every input, and it is the same "never exceeds 1" clause -
// applied to the real worst case rather than to a convenient subset of it.
void normalize_ltrt(LtRtCoefficients& coeffs) {
    double left = 0.0;
    double right = 0.0;
    for (std::size_t i = 0; i < coeffs.direct.left.size(); ++i) {
        left += coeffs.direct.left[i] + coeffs.surround[i];
        right += coeffs.direct.right[i] + coeffs.surround[i];
    }
    const double sum = std::max(left, right);
    if (sum <= 1.0) {
        return;
    }
    for (std::size_t i = 0; i < coeffs.direct.left.size(); ++i) {
        coeffs.direct.left[i] /= sum;
        coeffs.direct.right[i] /= sum;
        coeffs.surround[i] /= sum;
    }
}

}  // namespace

DownmixCoefficients stereo_downmix(Acmod acmod, double clev, double slev) {
    const Layout layout = layout_of(acmod);
    DownmixCoefficients out;
    const auto set = [&](int index, double left, double right) {
        if (index < 0) {
            return;
        }
        out.left[static_cast<std::size_t>(index)] = left;
        out.right[static_cast<std::size_t>(index)] = right;
    };

    // "route input_chan[i] into output_chan[i]" for the channels that exist in
    // both, then the mixes §7.8's output_nfront == 2 branch adds.
    set(layout.left, 1.0, 0.0);
    set(layout.right, 0.0, 1.0);
    // A 1/0 source has no left or right at all: its centre becomes both, at
    // −3 dB, which normalisation then lifts back to unity.
    set(layout.centre, layout.left < 0 ? level::kMinus3dB : clev,
        layout.left < 0 ? level::kMinus3dB : clev);
    // A single coded surround with no surround loudspeakers spreads across
    // both fronts at slev − 3 dB; a coded pair keeps its sides.
    set(layout.surround, slev * level::kMinus3dB, slev * level::kMinus3dB);
    set(layout.left_surround, slev, 0.0);
    set(layout.right_surround, 0.0, slev);

    normalize(out.left);
    normalize(out.right);
    return out;
}

LtRtCoefficients ltrt_downmix(Acmod acmod, double clev, double slev) {
    const Layout layout = layout_of(acmod);
    LtRtCoefficients out;
    const auto direct = [&](int index, double left, double right) {
        if (index < 0) {
            return;
        }
        out.direct.left[static_cast<std::size_t>(index)] = left;
        out.direct.right[static_cast<std::size_t>(index)] = right;
    };
    const auto shifted = [&](int index, double gain) {
        if (index >= 0) {
            out.surround[static_cast<std::size_t>(index)] = gain;
        }
    };

    direct(layout.left, 1.0, 0.0);
    direct(layout.right, 0.0, 1.0);
    // A 1/0 source has no left or right of its own, exactly as in Lo/Ro: its
    // centre becomes both at -3 dB. A Dolby Surround decoder recovers that as
    // a phantom centre, which is the correct place for it.
    direct(layout.centre, layout.left < 0 ? level::kMinus3dB : clev,
           layout.left < 0 ? level::kMinus3dB : clev);
    // Every coded surround feeds ONE sum, then that sum is shifted and
    // matrixed. A 2/1 or 3/1 source's single surround is already the sum; a
    // coded pair is summed at slev, which is what a Dolby Surround encoder
    // does with discrete surrounds and what a Pro Logic decoder expects to
    // pull back out of the difference signal. Their individual sides are
    // deliberately lost here - Lt/Rt has one surround channel, by definition.
    shifted(layout.surround, slev);
    shifted(layout.left_surround, slev);
    shifted(layout.right_surround, slev);

    normalize_ltrt(out);
    return out;
}

std::array<double, 5> mono_downmix(Acmod acmod, double clev, double slev) {
    const Layout layout = layout_of(acmod);
    std::array<double, 5> out{};
    const auto set = [&](int index, double value) {
        if (index >= 0) {
            out[static_cast<std::size_t>(index)] = value;
        }
    };

    // §7.8's output_mode == 1/0 branch, term by term.
    set(layout.left, level::kMinus3dB);
    set(layout.right, level::kMinus3dB);
    // "mix center into center using clev and +3 dB gain" only when the source
    // has three front channels; a 1/0 source routes its centre straight
    // through.
    set(layout.centre, layout.left < 0 ? 1.0 : clev * level::kPlus3dB);
    set(layout.surround, slev * level::kMinus3dB);
    set(layout.left_surround, slev * level::kMinus3dB);
    set(layout.right_surround, slev * level::kMinus3dB);

    normalize(out);
    return out;
}

double mono_downmix_peak_dbfs(std::span<const std::array<double, 256>> history,
                              std::span<const std::span<const float>> channels, Acmod acmod,
                              double clev, double slev) {
    const auto coeffs = mono_downmix(acmod, clev, slev);
    const int nfchans = fullbw_channel_count(acmod);
    const auto usable =
        std::min(static_cast<std::size_t>(nfchans), channels.size());

    double peak = 0.0;
    // The history block first, then the frame: one continuous signal as far as
    // the peak is concerned.
    if (!history.empty()) {
        for (std::size_t n = 0; n < history[0].size(); ++n) {
            double sum = 0.0;
            for (std::size_t ch = 0; ch < usable && ch < history.size(); ++ch) {
                sum += coeffs[ch] * history[ch][n];
            }
            peak = std::max(peak, std::abs(sum));
        }
    }
    std::size_t length = 0;
    for (std::size_t ch = 0; ch < usable; ++ch) {
        length = std::max(length, channels[ch].size());
    }
    for (std::size_t n = 0; n < length; ++n) {
        double sum = 0.0;
        for (std::size_t ch = 0; ch < usable; ++ch) {
            if (n < channels[ch].size()) {
                sum += coeffs[ch] * static_cast<double>(channels[ch][n]);
            }
        }
        peak = std::max(peak, std::abs(sum));
    }
    return to_db(peak);
}

double mono_downmix_peak_dbfs(std::span<const std::span<const float>> channels, Acmod acmod,
                              double clev, double slev) {
    return mono_downmix_peak_dbfs({}, channels, acmod, clev, slev);
}

double dialnorm_gain(int dialnorm) {
    // §5.4.2.8 reserves 0; every other value is 1..31 dB below full scale.
    // Anything outside that range means the field was never read (or was read
    // wrong), and leaving the audio alone is the only reading that cannot
    // make things worse.
    if (dialnorm <= 0 || dialnorm > kReferenceDialnorm) {
        return 1.0;
    }
    return std::pow(10.0, static_cast<double>(dialnorm - kReferenceDialnorm) / 20.0);
}

double lfe_mix_gain(double level_db) { return std::pow(10.0, level_db / 20.0); }

namespace {

bool in_range(const std::optional<int>& value, int high) {
    return !value || (*value >= 0 && *value <= high);
}

bool valid_external_scales(const ExternalScales& value) {
    if (value.premix.premixcmpscl < 0 || value.premix.premixcmpscl > 7) {
        return false;
    }
    // Table E2.8 is a 4-bit code for every one of them, auxiliaries included.
    for (const auto& scale : {value.left, value.centre, value.right, value.left_surround,
                              value.right_surround, value.lfe, value.dmixscl}) {
        if (!in_range(scale, 15)) {
            return false;
        }
    }
    if (value.auxiliary) {
        for (const auto& scale : *value.auxiliary) {
            if (!in_range(scale, 15)) {
                return false;
            }
        }
    }
    return true;
}

bool valid_speech_enhancement(const SpeechEnhancement& value) {
    if (value.spchdat < 0 || value.spchdat > 31) {
        return false;
    }
    if (!value.additional) {
        return true;
    }
    const auto& additional = *value.additional;
    if (additional.spchdat1 < 0 || additional.spchdat1 > 31 || additional.spchan1att < 0 ||
        additional.spchan1att > 3) {
        return false;
    }
    if (!additional.more) {
        return true;
    }
    return additional.more->spchdat2 >= 0 && additional.more->spchdat2 <= 31 &&
           additional.more->spchan2att >= 0 && additional.more->spchan2att <= 7;
}

bool valid_pan(const std::optional<PanInfo>& value) {
    return !value || (value->panmean >= 0 && value->panmean <= kPanMeanMax &&
                      value->paninfo >= 0 && value->paninfo <= 63);
}

}  // namespace

bool valid_mix_metadata(const MixMetadata& value) {
    if (!valid_surround_mix_level(value.ltrtsurmixlev) ||
        !valid_surround_mix_level(value.lorosurmixlev)) {
        return false;
    }
    if (!in_range(value.lfemixlevcod, 31) || !in_range(value.pgmscl, kPgmScaleMax) ||
        !in_range(value.pgmscl2, kPgmScaleMax) || !in_range(value.extpgmscl, kPgmScaleMax)) {
        return false;
    }
    const auto& mixing = value.mixing;
    if (mixing.mixdef == MixDefinition::kPremix &&
        (mixing.premix.premixcmpscl < 0 || mixing.premix.premixcmpscl > 7)) {
        return false;
    }
    // Table E2.6: mixdef 0x2 reserves exactly twelve bits.
    if (mixing.mixdef == MixDefinition::kReserved && mixing.reserved > 0x0FFF) {
        return false;
    }
    if (mixing.mixdef == MixDefinition::kExtended) {
        if (mixing.external && !valid_external_scales(*mixing.external)) {
            return false;
        }
        if (mixing.speech && !valid_speech_enhancement(*mixing.speech)) {
            return false;
        }
    }
    if (!valid_pan(value.pan) || !valid_pan(value.pan2)) {
        return false;
    }
    if (value.blkmixcfginfo) {
        for (const auto& word : *value.blkmixcfginfo) {
            if (!in_range(word, 31)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace ac3::meta

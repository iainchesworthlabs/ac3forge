#include "pcm/drc.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>

#include "tables/qmf_tables.hpp"

namespace ac4::detail {
namespace {

constexpr int kSubbands = 64;
// The QMF domain works at the inverse transform's scale, full scale 2^15
// (substream_pcm.cpp, kFullScale).
constexpr double kFullScalePower = 32768.0 * 32768.0;
// BS.1770's offset from K-weighted mean square to LKFS.
constexpr double kLkfsOffset = -0.691;
// A floor for the level of silence, in the power's units.
constexpr double kPowerFloor = 1e-15;

[[nodiscard]] double db2_to_linear(double gain) noexcept {
    return std::exp2(gain / 6.0);
}

// |H(f)|^2 of a biquad at 48 kHz.
[[nodiscard]] double biquad_power(const std::array<double, 3>& b, const std::array<double, 3>& a,
                                  double hz) {
    const double w = 2.0 * std::numbers::pi * hz / 48000.0;
    const std::complex<double> z1 = std::polar(1.0, -w);
    const std::complex<double> z2 = z1 * z1;
    const std::complex<double> h = (b[0] + b[1] * z1 + b[2] * z2) / (a[0] + a[1] * z1 + a[2] * z2);
    return std::norm(h);
}

// ITU-R BS.1770's K-weighting at 48 kHz, the shelf and the high-pass, as power
// at `hz` (read at 23.5 kHz above it: the shelf is flat there).
[[nodiscard]] double k_weight(double hz) {
    constexpr std::array<double, 3> kShelfB = {1.53512485958697, -2.69169618940638,
                                               1.19839281085285};
    constexpr std::array<double, 3> kShelfA = {1.0, -1.69065929318241, 0.73248077421585};
    constexpr std::array<double, 3> kHighPassB = {1.0, -2.0, 1.0};
    constexpr std::array<double, 3> kHighPassA = {1.0, -1.99004745483398, 0.99007225036621};
    const double f = std::min(hz, 23500.0);
    return biquad_power(kShelfB, kShelfA, f) * biquad_power(kHighPassB, kHighPassA, f);
}

// BS.1770's channel weights: 1.41 at the sides (60 to 120 degrees), none for
// the LFE, 1 elsewhere.
[[nodiscard]] double loudness_weight(Speaker speaker) noexcept {
    switch (speaker) {
        case Speaker::kLfe:
            return 0.0;
        case Speaker::kLeftSurround:
        case Speaker::kRightSurround:
        case Speaker::kLeftWide:
        case Speaker::kRightWide:
            return 1.41;
        default:
            return 1.0;
    }
}

// Table 168's channel group, by the channel's speaker; the pair a 7.X mode adds
// joins group 0 or 2 by add_ch_base, and the back pair is always group 2.
[[nodiscard]] int drc_group(Speaker speaker, bool add_ch_base, bool mono_or_stereo) noexcept {
    if (mono_or_stereo) {
        return 0;
    }
    switch (speaker) {
        case Speaker::kCentre:
            return 1;
        case Speaker::kLeftSurround:
        case Speaker::kRightSurround:
        case Speaker::kLeftBack:
        case Speaker::kRightBack:
            return 2;
        case Speaker::kLeftWide:
        case Speaker::kRightWide:
        case Speaker::kTopFrontLeft:
        case Speaker::kTopFrontRight:
            return add_ch_base ? 2 : 0;
        default:
            return 0;  // L, R and the LFE
    }
}

// Table 164's first QMF subband of each DRC band, by drc_gains_config, and
// the end (Qmax, Table 165: 64 at 48 kHz).
[[nodiscard]] int band_of(int gains_config, int subband) noexcept {
    switch (gains_config) {
        case 2:
            return subband <= 4 ? 0 : 1;
        case 3:
            if (subband == 0) {
                return 0;
            }
            if (subband <= 4) {
                return 1;
            }
            return subband <= 16 ? 2 : 3;
        default:
            return 0;
    }
}

}  // namespace

double DrcCurve::gain(double level) const noexcept {
    const auto between = [](double from_gain, double to_gain, double t) {
        return from_gain + (to_gain - from_gain) * t;
    };
    if (level < max_boost_level) {
        return max_boost_gain;
    }
    if (level <= section_boost_level) {
        const double span = section_boost_level - max_boost_level;
        return span > 0.0 ? between(section_boost_gain, max_boost_gain,
                                    (section_boost_level - level) / span)
                          : section_boost_gain;
    }
    if (level <= null_low) {
        const double span = null_low - section_boost_level;
        return span > 0.0 ? section_boost_gain * (null_low - level) / span : 0.0;
    }
    if (level <= null_high) {
        return 0.0;
    }
    if (level <= section_cut_level) {
        const double span = section_cut_level - null_high;
        return span > 0.0 ? section_cut_gain * (level - null_high) / span : 0.0;
    }
    if (level <= max_cut_level) {
        const double span = max_cut_level - section_cut_level;
        return span > 0.0
                   ? between(section_cut_gain, max_cut_gain, (level - section_cut_level) / span)
                   : section_cut_gain;
    }
    return max_cut_gain;
}

DrcCurve drc_curve(const DrcCompressionCurve& t) noexcept {
    DrcCurve c;
    c.null_low = -static_cast<double>(t.drc_lev_nullband_low);
    c.null_high = static_cast<double>(t.drc_lev_nullband_high);
    c.max_boost_gain = static_cast<double>(t.drc_gain_max_boost);
    // Table 166. Without a section of its own, a boost or cut section runs
    // from the null band to its maximum, its section gain 0.
    if (t.drc_nr_boost_sections == 1) {
        c.section_boost_gain = 1.0 + static_cast<double>(t.drc_gain_section_boost);
        c.section_boost_level = c.null_low - (1.0 + static_cast<double>(t.drc_lev_section_boost));
    } else {
        c.section_boost_gain = 0.0;
        c.section_boost_level = c.null_low;
    }
    c.max_boost_level =
        c.max_boost_gain > 0.0
            ? c.section_boost_level - (1.0 + static_cast<double>(t.drc_lev_max_boost))
            : c.null_low;
    c.max_cut_gain = -static_cast<double>(t.drc_gain_max_cut);
    if (t.drc_nr_cut_sections == 1) {
        c.section_cut_gain = -(1.0 + static_cast<double>(t.drc_gain_section_cut));
        c.section_cut_level = c.null_high + (1.0 + static_cast<double>(t.drc_lev_section_cut));
    } else {
        c.section_cut_gain = 0.0;
        c.section_cut_level = c.null_high;
    }
    c.max_cut_level = c.max_cut_gain < 0.0
                          ? c.section_cut_level + (1.0 + static_cast<double>(t.drc_lev_max_cut))
                          : c.null_high;
    if (!t.drc_tc_default_flag) {
        // Clauses 4.3.13.4.15 to 4.3.13.4.21.
        c.attack_ms = 5.0 * t.drc_tc_attack;
        c.release_ms = 40.0 * t.drc_tc_release;
        c.attack_fast_ms = 5.0 * t.drc_tc_attack_fast;
        c.release_fast_ms = 20.0 * t.drc_tc_release_fast;
        c.adaptive = t.drc_adaptive_smoothing_flag;
        c.attack_threshold = static_cast<double>(t.drc_attack_threshold);
        c.release_threshold = static_cast<double>(t.drc_release_threshold);
    }
    // Table 167 otherwise: the defaults DrcCurve starts with, without adaptive
    // smoothing.
    return c;
}

std::optional<DrcCurve> drc_default_curve(int drc_eac3_profile) noexcept {
    // Table 162, by drc_eac3_profile (Table 160): None, Film standard, Film
    // light, Music standard, Music light, Speech.
    struct Profile {
        double null_low, null_high, max_boost_gain, max_boost_level, max_cut_gain;
        bool cut_section;
        double section_cut_level, max_cut_level, section_cut_gain;
        double release_ms, release_fast_ms, attack_threshold, release_threshold;
    };
    static constexpr std::array<Profile, 6> kProfiles{{
        {0, 0, 0, 0, 0, false, 0, 0, 0, 3000, 1000, 15, 20},
        {0, 5, 6, -12, -24, true, 15, 35, -5, 3000, 1000, 15, 20},
        {-10, 10, 6, -22, -24, true, 20, 40, -5, 3000, 1000, 15, 20},
        {0, 5, 12, -24, -24, true, 15, 35, -5, 10000, 1000, 15, 20},
        {-10, 10, 12, -34, -15, false, 0, 40, 0, 3000, 1000, 15, 20},
        {0, 5, 15, -19, -24, true, 15, 35, -5, 1000, 200, 10, 10},
    }};
    if (drc_eac3_profile < 0 || static_cast<std::size_t>(drc_eac3_profile) >= kProfiles.size()) {
        return std::nullopt;
    }
    const Profile& p = kProfiles[static_cast<std::size_t>(drc_eac3_profile)];
    DrcCurve c;
    c.null_low = p.null_low;
    c.null_high = p.null_high;
    c.max_boost_gain = p.max_boost_gain;
    c.max_boost_level = p.max_boost_gain > 0.0 ? p.max_boost_level : p.null_low;
    c.section_boost_gain = 0.0;  // no profile has a boost section
    c.section_boost_level = p.null_low;
    c.max_cut_gain = p.max_cut_gain;
    c.section_cut_gain = p.cut_section ? p.section_cut_gain : 0.0;
    c.section_cut_level = p.cut_section ? p.section_cut_level : p.null_high;
    c.max_cut_level = p.max_cut_gain < 0.0 ? p.max_cut_level : p.null_high;
    c.attack_ms = 100.0;
    c.release_ms = p.release_ms;
    c.attack_fast_ms = 10.0;
    c.release_fast_ms = p.release_fast_ms;
    // Table 162 gives every profile but None its fast time constants and
    // thresholds; the profiles are taken as smoothing adaptively with them.
    c.adaptive = drc_eac3_profile != 0;
    c.attack_threshold = p.attack_threshold;
    c.release_threshold = p.release_threshold;
    return c;
}

std::optional<int> drc_mode_for(const DrcConfig& config, DrcMode wanted, double output_level,
                                bool headphones) noexcept {
    const auto configured = [&config](int id) {
        return config.mode[static_cast<std::size_t>(id)].configured;
    };
    switch (wanted) {
        case DrcMode::kOff:
            return std::nullopt;
        case DrcMode::kHomeTheatre:
        case DrcMode::kFlatPanelTv:
        case DrcMode::kPortableSpeakers:
        case DrcMode::kPortableHeadphones: {
            const int id = static_cast<int>(wanted) - static_cast<int>(DrcMode::kHomeTheatre);
            return configured(id) ? std::optional<int>(id) : std::nullopt;
        }
        case DrcMode::kDefault:
            break;
    }
    // Table 161's ranges, and the transmitted ones of modes 4 to 7.
    const double level = std::round(output_level);
    const auto in_range = [&](int id) {
        switch (id) {
            case 0:
                return level >= -31.0 && level <= -27.0;
            case 1:
                return level >= -26.0 && level <= -17.0;
            case 2:
                return !headphones && level >= -16.0 && level <= 0.0;
            case 3:
                return headphones && level >= -16.0 && level <= 0.0;
            default: {
                const DrcDecoderModeConfig& mode = config.mode[static_cast<std::size_t>(id)];
                const double low = -static_cast<double>(mode.drc_output_level_from);
                const double high = -static_cast<double>(mode.drc_output_level_to);
                return level >= std::min(low, high) && level <= std::max(low, high);
            }
        }
    };
    for (int id = kMaxDrcModes - 1; id >= 0; --id) {
        if (configured(id) && in_range(id)) {
            return id;
        }
    }
    return std::nullopt;
}

DrcFrameValues drc_frame_values(const OutputConfig& output, std::optional<double> dialnorm,
                                const DrcState* state, const DrcFrame* frame) {
    DrcFrameValues values;
    values.dialnorm = dialnorm;
    if (frame != nullptr && frame->b_drc_present) {
        values.reset = frame->drc_reset_flag;
    }
    if (!output.output_level_dbfs || state == nullptr || !state->config_valid) {
        return values;
    }
    const std::optional<int> id =
        drc_mode_for(state->config, output.drc, *output.output_level_dbfs, output.headphones);
    if (!id) {
        return values;
    }
    const DrcDecoderModeConfig& mode = state->config.mode[static_cast<std::size_t>(*id)];
    if (mode.drc_default_profile_flag) {
        values.curve = drc_default_curve(state->config.drc_eac3_profile);
    } else if (mode.drc_compression_curve_flag) {
        if (mode.curve) {
            values.curve = drc_curve(*mode.curve);
        }
    } else if (frame != nullptr && frame->b_drc_present) {
        for (int g = 0; g < frame->n_gainsets; ++g) {
            const DrcGainset& set = frame->gainsets[static_cast<std::size_t>(g)];
            if (set.drc_decoder_mode_id == *id && set.gains_present) {
                values.gains = set;
                break;
            }
        }
    }
    return values;
}

void DrcStage::configure(double rate_hz, int slots, std::span<const Speaker> speakers,
                         bool add_ch_base) {
    rate_hz_ = rate_hz;
    slots_ = slots;
    speakers_.assign(speakers.begin(), speakers.end());
    const bool small = speakers.size() <= 2;
    loudness_weight_.clear();
    group_.clear();
    for (const Speaker speaker : speakers) {
        loudness_weight_.push_back(loudness_weight(speaker));
        group_.push_back(drc_group(speaker, add_ch_base, small));
    }
    for (int k = 0; k < kSubbands; ++k) {
        k_weight_[static_cast<std::size_t>(k)] =
            k_weight((static_cast<double>(k) + 0.5) * rate_hz / (2.0 * kSubbands));
    }
    qmf_gain_ = 0.0;
    for (const float w : tables::kQwin) {
        qmf_gain_ += static_cast<double>(w) * static_cast<double>(w);
    }
    reset();
}

void DrcStage::reset() noexcept {
    dialnorm_.reset();
    primed_ = false;
    level_smoothed_ = 0.0;
    gain_smoothed_ = 1.0;
    last_gain_ = 1.0;
}

double DrcStage::slot_level(std::span<std::vector<QmfValue>* const> side, int slot) const {
    double power = 0.0;
    for (std::size_t c = 0; c < side.size() && c < loudness_weight_.size(); ++c) {
        const double weight = loudness_weight_[c];
        if (weight == 0.0) {
            continue;
        }
        const QmfValue* row = side[c]->data() + static_cast<std::size_t>(slot) * kSubbands;
        double channel = 0.0;
        for (std::size_t k = 0; k < kSubbands; ++k) {
            channel += k_weight_[k] * std::norm(row[k]);
        }
        power += weight * channel;
    }
    // The mean square per sample at full scale 1.0: the slot's 64 samples
    // carried power * sum(QWIN^2) into the subbands.
    return power / (static_cast<double>(kSubbands) * qmf_gain_ * kFullScalePower);
}

void DrcStage::process(const OutputConfig& output, const DrcFrameValues& values,
                       std::span<std::vector<QmfValue>* const> matrices,
                       std::span<std::vector<QmfValue>* const> side) {
    if (values.dialnorm) {
        dialnorm_ = values.dialnorm;
    }
    if (!output.output_level_dbfs) {
        last_gain_ = 1.0;
        return;
    }
    // Clause 5.7.9.3.3: dialnorm to the output level, 2^((Lout - Lin) / 6); a
    // stream that has sent no dialnorm yet is taken to be at the output level.
    const double lin = dialnorm_.value_or(*output.output_level_dbfs);
    const double level_gain = db2_to_linear(*output.output_level_dbfs - lin);
    const double slot_ms = static_cast<double>(kSubbands) * 1000.0 / rate_hz_;
    if (values.reset) {
        primed_ = false;
    }
    for (int n = 0; n < slots_; ++n) {
        double gain = 1.0;
        if (values.curve) {
            const DrcCurve& curve = *values.curve;
            const double power = std::max(slot_level(side, n), kPowerFloor);
            const double level = 10.0 * std::log10(power) + kLkfsOffset - lin;
            const double target = db2_to_linear(curve.gain(level));
            if (!primed_) {
                level_smoothed_ = power;
                gain_smoothed_ = target;
                primed_ = true;
            } else {
                double tau = level_smoothed_ < power ? curve.attack_ms : curve.release_ms;
                if (curve.adaptive) {
                    const double change =
                        10.0 * std::log10(power / std::max(level_smoothed_, kPowerFloor));
                    if (change > curve.attack_threshold) {
                        tau = curve.attack_fast_ms;
                    } else if (change > 0.0) {
                        tau = curve.attack_ms;
                    } else if (-change <= curve.release_threshold) {
                        tau = curve.release_ms;
                    } else {
                        tau = curve.release_fast_ms;
                    }
                }
                const double alpha = tau > 0.0 ? std::exp2(-slot_ms / tau) : 0.0;
                level_smoothed_ = alpha * level_smoothed_ + (1.0 - alpha) * power;
                gain_smoothed_ = alpha * gain_smoothed_ + (1.0 - alpha) * target;
            }
            gain = gain_smoothed_;
        }
        for (std::size_t c = 0; c < matrices.size(); ++c) {
            QmfValue* row = matrices[c]->data() + static_cast<std::size_t>(n) * kSubbands;
            if (values.gains) {
                // Clause 5.7.9.3.2: the gain of the channel's group, its band
                // and the slot's subframe, constant across each.
                const DrcGainset& set = *values.gains;
                const int subframes = std::max(1, set.nr_drc_subframes);
                const int sf = std::min(subframes - 1, n * subframes / std::max(1, slots_));
                const int group =
                    set.nr_drc_channels > 1 ? std::min(group_[c], set.nr_drc_channels - 1) : 0;
                for (int k = 0; k < kSubbands; ++k) {
                    const int band = std::min(band_of(set.drc_gains_config, k),
                                              std::max(1, set.nr_drc_bands) - 1);
                    row[k] *=
                        level_gain * db2_to_linear(static_cast<double>(set.gain(group, sf, band)));
                }
                continue;
            }
            const double total = level_gain * gain;
            for (int k = 0; k < kSubbands; ++k) {
                row[k] *= total;
            }
        }
        last_gain_ = level_gain * gain;
    }
}

}  // namespace ac4::detail

#include "pcm/downmix.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <utility>

namespace ac4::detail {
namespace {

constexpr double kMinus3Db = 0.70794578438413791;  // 10^(-3/20), no mix gain sent
constexpr double kFold = 0.707;                    // Tables 219 and 6.2.17.6, as printed

[[nodiscard]] double from_db(double db) noexcept {
    return std::pow(10.0, db / 20.0);
}

// A loudness correction code: (15 - x) / 2 dB2, 31 reading as 0 dB (4.3.12.2.11).
[[nodiscard]] double loud_corr_gain(std::optional<int> code) noexcept {
    if (!code || *code == 31) {
        return 1.0;
    }
    return std::exp2((15.0 - static_cast<double>(*code)) / 2.0 / 6.0);
}

using Mix = std::vector<double>;

[[nodiscard]] Mix unit(std::size_t count, std::size_t c) {
    Mix m(count, 0.0);
    m[c] = 1.0;
    return m;
}

[[nodiscard]] Mix add(Mix a, const Mix& b, double w) {
    for (std::size_t c = 0; c < a.size(); ++c) {
        a[c] += w * b[c];
    }
    return a;
}

[[nodiscard]] Mix scaled(Mix a, double w) {
    for (double& v : a) {
        v *= w;
    }
    return a;
}

[[nodiscard]] bool is_identity(const std::vector<std::vector<double>>& m) noexcept {
    for (std::size_t o = 0; o < m.size(); ++o) {
        for (std::size_t c = 0; c < m[o].size(); ++c) {
            if (m[o][c] != (o == c ? 1.0 : 0.0)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

double centre_mix_gain(int code) noexcept {
    // Table 149: +3, +1.5, 0, -1.5, -3, -4.5, -6 dB and silence.
    static constexpr std::array<double, 7> kDb = {3.0, 1.5, 0.0, -1.5, -3.0, -4.5, -6.0};
    if (code == 7) {
        return 0.0;
    }
    return code >= 0 && code < 7 ? from_db(kDb[static_cast<std::size_t>(code)]) : kMinus3Db;
}

double surround_mix_gain(int code) noexcept {
    // Table 149a: codes 0 and 1 reserved, then 0, -1.5, -3, -4.5, -6 dB and silence.
    static constexpr std::array<double, 5> kDb = {0.0, -1.5, -3.0, -4.5, -6.0};
    if (code == 7) {
        return 0.0;
    }
    return code >= 2 && code < 7 ? from_db(kDb[static_cast<std::size_t>(code - 2)]) : kMinus3Db;
}

DownmixValues downmix_values(const PresentationSubstream* presentation, const Metadata& metadata) {
    DownmixValues values;
    if (presentation != nullptr) {
        const LoudCorr& corr = presentation->loud_corr;
        values.coeff = presentation->custom_dmx_data.stereo_dmx_coeff;
        values.loro_loud_corr = corr.loro_dmx_loud_corr;
        values.ltrt_loud_corr = corr.ltrt_dmx_loud_corr;
        values.loud_corr_5x = corr.loud_corr_5_X;
        values.loud_corr_5x2 = corr.loud_corr_5_X_2;
        values.loud_corr_5x4 = corr.loud_corr_5_X_4;
        values.loud_corr_7x = corr.loud_corr_7_X;
        values.loud_corr_7x2 = corr.loud_corr_7_X_2;
        values.loud_corr_7x4 = corr.loud_corr_7_X_4;
        values.loud_corr_core_5x2 = corr.loud_corr_core_5_X_2;
        values.loud_corr_core_5x = corr.loud_corr_core_5_X;
        values.loud_corr_core_loro = corr.loud_corr_core_loro;
        values.loud_corr_core_ltrt = corr.loud_corr_core_ltrt;
        if (presentation->custom_dmx_data.b_cdmx_data_present) {
            values.cdmx = presentation->custom_dmx_data;
        }
    } else if (metadata.basic.stereo_dmx_coeff) {
        values.coeff = metadata.basic.stereo_dmx_coeff;
        values.loro_loud_corr = metadata.basic.stereo_dmx_coeff->loro_dmx_loud_corr;
        values.ltrt_loud_corr = metadata.basic.stereo_dmx_coeff->ltrt_dmx_loud_corr;
    }
    return values;
}

void DownmixStage::configure(std::span<const Speaker> speakers, bool add_ch_base,
                             DownmixTarget target, bool mix_lfe,
                             const std::optional<ImmersiveLayout>& immersive) {
    in_speakers_.assign(speakers.begin(), speakers.end());
    add_ch_base_ = add_ch_base;
    target_ = target;
    mix_lfe_ = mix_lfe;
    immersive_ = immersive;
    if (immersive_) {
        plan_ = render_plan(*immersive_, target);
        if (!plan_.stereo) {
            out_speakers_ = plan_.speakers;
        } else if (target == DownmixTarget::kMono) {
            out_speakers_ = {Speaker::kCentre};
        } else {
            out_speakers_ = {Speaker::kLeft, Speaker::kRight};
        }
        // As coded where the renderer takes each channel to itself, which in
        // full decoding no custom downmix parameter or correction touches;
        // core decoding's Table 45 takes both.
        pass_through_ =
            immersive_->decoding == DecodingMode::kFull && out_speakers_ == in_speakers_ &&
            is_identity(render_matrix(*immersive_, in_speakers_, plan_, render_gains(nullptr, -1)));
        reset();
        return;
    }
    plan_ = {};
    const auto has = [&](Speaker s) {
        return std::ranges::find(in_speakers_, s) != in_speakers_.end();
    };
    const bool seven =
        has(Speaker::kLeftBack) || has(Speaker::kLeftWide) || has(Speaker::kTopFrontLeft);
    const std::size_t count = in_speakers_.size();
    switch (target) {
        case DownmixTarget::kAsCoded:
        // The immersive layouts are the immersive element's; the other
        // elements come out as coded for them.
        case DownmixTarget::k7X4:
        case DownmixTarget::k7X2:
        case DownmixTarget::k7X0:
        case DownmixTarget::k5X4:
        case DownmixTarget::k5X2:
            out_speakers_ = in_speakers_;
            break;
        case DownmixTarget::k5X:
            if (seven) {
                out_speakers_ = {Speaker::kLeft, Speaker::kRight, Speaker::kCentre};
                if (has(Speaker::kLfe)) {
                    out_speakers_.push_back(Speaker::kLfe);
                }
                out_speakers_.push_back(Speaker::kLeftSurround);
                out_speakers_.push_back(Speaker::kRightSurround);
            } else {
                out_speakers_ = in_speakers_;
            }
            break;
        case DownmixTarget::kStereo:
        case DownmixTarget::kLoRo:
        case DownmixTarget::kLtRt:
            out_speakers_ =
                count == 2 ? in_speakers_ : std::vector<Speaker>{Speaker::kLeft, Speaker::kRight};
            break;
        case DownmixTarget::kMono:
            out_speakers_ = {Speaker::kCentre};
            break;
    }
    pass_through_ = out_speakers_ == in_speakers_;
    reset();
}

void DownmixStage::reset() {
    coeff_.reset();
    for (std::optional<int>* value :
         {&loro_loud_corr_, &ltrt_loud_corr_, &loud_corr_5x_, &loud_corr_5x2_, &loud_corr_5x4_,
          &loud_corr_7x_, &loud_corr_7x2_, &loud_corr_7x4_, &loud_corr_core_5x2_,
          &loud_corr_core_5x_, &loud_corr_core_loro_, &loud_corr_core_ltrt_}) {
        value->reset();
    }
    cdmx_.reset();
    rebuild();
}

std::optional<int> DownmixStage::correction(LoudCorrOutput output) const noexcept {
    switch (output) {
        case LoudCorrOutput::kNone:
            return std::nullopt;
        case LoudCorrOutput::k5X:
            return loud_corr_5x_;
        case LoudCorrOutput::k5X2:
            return loud_corr_5x2_;
        case LoudCorrOutput::k5X4:
            return loud_corr_5x4_;
        case LoudCorrOutput::k7X:
            return loud_corr_7x_;
        case LoudCorrOutput::k7X2:
            return loud_corr_7x2_;
        case LoudCorrOutput::k7X4:
            return loud_corr_7x4_;
        case LoudCorrOutput::kCore5X2:
            return loud_corr_core_5x2_;
        case LoudCorrOutput::kCore5X:
            return loud_corr_core_5x_;
    }
    return std::nullopt;
}

void DownmixStage::rebuild() {
    const std::size_t count = in_speakers_.size();
    matrix_.clear();
    if (pass_through_) {
        for (std::size_t c = 0; c < count; ++c) {
            matrix_.push_back(unit(count, c));
        }
        return;
    }
    if (immersive_) {
        rebuild_immersive();
        return;
    }
    const auto index = [this](Speaker s) -> std::optional<std::size_t> {
        const auto it = std::ranges::find(in_speakers_, s);
        return it == in_speakers_.end() ? std::nullopt
                                        : std::optional<std::size_t>(
                                              static_cast<std::size_t>(it - in_speakers_.begin()));
    };
    const auto channel = [&](Speaker s) {
        return index(s) ? unit(count, *index(s)) : Mix(count, 0.0);
    };
    // Step 1, Table 219: a 7.X element's pair into 5.X; C and the LFE through.
    Mix l = channel(Speaker::kLeft);
    Mix r = channel(Speaker::kRight);
    const Mix c = channel(Speaker::kCentre);
    const Mix lfe = channel(Speaker::kLfe);
    Mix ls = channel(Speaker::kLeftSurround);
    Mix rs = channel(Speaker::kRightSurround);
    if (index(Speaker::kLeftBack)) {
        ls = add(scaled(ls, kFold), channel(Speaker::kLeftBack), kFold);
        rs = add(scaled(rs, kFold), channel(Speaker::kRightBack), kFold);
    }
    for (const auto& [left, right] : {std::pair{Speaker::kLeftWide, Speaker::kRightWide},
                                      std::pair{Speaker::kTopFrontLeft, Speaker::kTopFrontRight}}) {
        if (!index(left)) {
            continue;
        }
        if (add_ch_base_) {
            ls = add(scaled(ls, kFold), channel(left), kFold);
            rs = add(scaled(rs, kFold), channel(right), kFold);
        } else {
            l = add(l, channel(left), kFold);
            r = add(r, channel(right), kFold);
        }
    }
    const bool folded_seven =
        index(Speaker::kLeftBack) || index(Speaker::kLeftWide) || index(Speaker::kTopFrontLeft);
    if (target_ == DownmixTarget::k5X) {
        // Part 2 clause 4.8.5.3: the fold to 5.X's loudness correction.
        const double gain = folded_seven ? loud_corr_gain(loud_corr_5x_) : 1.0;
        for (const Speaker s : out_speakers_) {
            switch (s) {
                case Speaker::kLeft:
                    matrix_.push_back(scaled(l, gain));
                    break;
                case Speaker::kRight:
                    matrix_.push_back(scaled(r, gain));
                    break;
                case Speaker::kCentre:
                    matrix_.push_back(scaled(c, gain));
                    break;
                case Speaker::kLfe:
                    matrix_.push_back(scaled(lfe, gain));
                    break;
                case Speaker::kLeftSurround:
                    matrix_.push_back(scaled(ls, gain));
                    break;
                default:
                    matrix_.push_back(scaled(rs, gain));
                    break;
            }
        }
        return;
    }
    // Step 2, Tables 217 and 218: to two channels.
    if (count == 1) {
        // 6.2.17.6: a mono channel to both, at 0.707.
        push_two(scaled(c, kFold), scaled(c, kFold));
    } else if (count == 2) {
        push_two(l, r);
    } else {
        const auto [lo, ro] = two_channels(l, r, c, lfe, ls, rs, loro_loud_corr_, ltrt_loud_corr_);
        push_two(lo, ro);
    }
}

void DownmixStage::rebuild_immersive() {
    const ImmersiveLayout& layout = *immersive_;
    const RenderGains gains =
        render_gains(cdmx_ ? &*cdmx_ : nullptr, out_ch_config(plan_.output).value_or(-1));
    std::vector<std::vector<double>> rendered = render_matrix(layout, in_speakers_, plan_, gains);
    if (!plan_.stereo) {
        const double gain = loud_corr_gain(correction(loud_corr_output(layout, plan_.output)));
        for (Mix& row : rendered) {
            row = scaled(std::move(row), gain);
        }
        matrix_ = std::move(rendered);
        return;
    }
    // Step 2 from the 5.X.0 the renderer gives, with the Lo/Ro or Lt/Rt
    // correction alone, the core's in core decoding (src/ac4dec/ERRATA.md,
    // "The renderer's two-channel output").
    const auto row = [&](Speaker s) {
        const auto it = std::ranges::find(plan_.speakers, s);
        return it == plan_.speakers.end()
                   ? Mix(in_speakers_.size(), 0.0)
                   : rendered[static_cast<std::size_t>(it - plan_.speakers.begin())];
    };
    const bool core = layout.decoding == DecodingMode::kCore;
    const auto [lo, ro] =
        two_channels(row(Speaker::kLeft), row(Speaker::kRight), row(Speaker::kCentre),
                     row(Speaker::kLfe), row(Speaker::kLeftSurround), row(Speaker::kRightSurround),
                     core ? loud_corr_core_loro_ : loro_loud_corr_,
                     core ? loud_corr_core_ltrt_ : ltrt_loud_corr_);
    push_two(lo, ro);
}

std::pair<DownmixStage::Mix, DownmixStage::Mix> DownmixStage::two_channels(
    const Mix& l, const Mix& r, const Mix& c, const Mix& lfe, const Mix& ls, const Mix& rs,
    const std::optional<int>& loro_correction, const std::optional<int>& ltrt_correction) const {
    const StereoDmxCoeff coeff = coeff_.value_or(StereoDmxCoeff{});
    const int preferred = coeff_ ? coeff.preferred_dmx_method : 0;
    // 0 Lo/Ro, 2 Lt/Rt, 3 Lt/Rt's Pro Logic II form (Table 218's methods).
    int method = preferred == 2 || preferred == 3 ? preferred : 1;
    if (target_ == DownmixTarget::kLoRo) {
        method = 1;
    } else if (target_ == DownmixTarget::kLtRt) {
        method = preferred == 3 ? 3 : 2;
    }
    const bool loro = method == 1;
    // Without b_ltrt_mixinfo the Lt/Rt gains are the Lo/Ro gains.
    const int centre_code = !coeff_                         ? -1
                            : loro || !coeff.b_ltrt_mixinfo ? coeff.loro_centre_mixgain
                                                            : coeff.ltrt_centre_mixgain;
    const int surround_code = !coeff_                         ? -1
                              : loro || !coeff.b_ltrt_mixinfo ? coeff.loro_surround_mixgain
                                                              : coeff.ltrt_surround_mixgain;
    const double cmg = centre_mix_gain(centre_code);
    const double smg = surround_mix_gain(surround_code);
    Mix lo = add(l, c, cmg);
    Mix ro = add(r, c, cmg);
    if (method == 1) {
        lo = add(lo, ls, smg);
        ro = add(ro, rs, smg);
    } else {
        const double near = method == 3 ? smg * from_db(1.8) : smg;
        const double far = method == 3 ? smg * from_db(-3.2) : smg;
        lo = add(add(lo, ls, -near), rs, -far);
        ro = add(add(ro, rs, near), ls, far);
    }
    if (mix_lfe_ && coeff_ && coeff.lfe_mixgain) {
        // 4.3.12.2.18: lfe_mg = 5.5 - lfe_mixgain dB.
        const double lfe_mg = from_db(5.5 - static_cast<double>(*coeff.lfe_mixgain));
        lo = add(lo, lfe, lfe_mg);
        ro = add(ro, lfe, lfe_mg);
    }
    const double correction = loud_corr_gain(loro ? loro_correction : ltrt_correction);
    return {scaled(std::move(lo), correction), scaled(std::move(ro), correction)};
}

void DownmixStage::push_two(const Mix& lo, const Mix& ro) {
    if (target_ == DownmixTarget::kMono) {
        // 6.2.17.2: C = L + R.
        matrix_.push_back(add(lo, ro, 1.0));
        return;
    }
    matrix_.push_back(lo);
    matrix_.push_back(ro);
}

void DownmixStage::process(const DownmixValues& values, std::span<std::vector<QmfValue>* const> in,
                           std::vector<std::vector<QmfValue>>& out) {
    bool changed = false;
    if (values.coeff) {
        coeff_ = values.coeff;
        changed = true;
    }
    if (values.cdmx) {
        cdmx_ = values.cdmx;
        changed = true;
    }
    for (auto [from, to] : {std::pair{&values.loro_loud_corr, &loro_loud_corr_},
                            std::pair{&values.ltrt_loud_corr, &ltrt_loud_corr_},
                            std::pair{&values.loud_corr_5x, &loud_corr_5x_},
                            std::pair{&values.loud_corr_5x2, &loud_corr_5x2_},
                            std::pair{&values.loud_corr_5x4, &loud_corr_5x4_},
                            std::pair{&values.loud_corr_7x, &loud_corr_7x_},
                            std::pair{&values.loud_corr_7x2, &loud_corr_7x2_},
                            std::pair{&values.loud_corr_7x4, &loud_corr_7x4_},
                            std::pair{&values.loud_corr_core_5x2, &loud_corr_core_5x2_},
                            std::pair{&values.loud_corr_core_5x, &loud_corr_core_5x_},
                            std::pair{&values.loud_corr_core_loro, &loud_corr_core_loro_},
                            std::pair{&values.loud_corr_core_ltrt, &loud_corr_core_ltrt_}}) {
        if (from->has_value() && *from != *to) {
            *to = *from;
            changed = true;
        }
    }
    if (changed) {
        rebuild();
    }
    const std::size_t length = in.empty() ? 0 : in.front()->size();
    out.resize(matrix_.size());
    for (std::size_t o = 0; o < matrix_.size(); ++o) {
        out[o].assign(length, QmfValue{});
        for (std::size_t c = 0; c < in.size() && c < matrix_[o].size(); ++c) {
            const double w = matrix_[o][c];
            if (w == 0.0) {
                continue;
            }
            const std::vector<QmfValue>& source = *in[c];
            for (std::size_t i = 0; i < length; ++i) {
                out[o][i] += w * source[i];
            }
        }
    }
}

}  // namespace ac4::detail

#include "acpl/acpl_encoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <tuple>
#include <utility>

#include "ajcc/ajcc.hpp"

namespace ac4::detail {
namespace {

using acpl::kMaxParamBands;
using acpl::kSubbands;

constexpr double kRoot2 = std::numbers::sqrt2;
constexpr double kHalfRoot2 = std::numbers::sqrt2 / 2.0;

// Frame f's parameters apply to the A-CPL slots from first_slot(f) = S (f +
// d_ctrl) - ts_offset_hfgen, S = num_qmf_timeslots, for S slots (the header;
// at frame_rate_index 13 32 (f + 1) - 6 + ts, ts = 0 to 31), and smooth
// interpolation reaches them at the last of those (Pseudocode 109). The
// estimate reads the 48 slots centred there, from S - 24 to S + 24 past
// first_slot(f), through a Hann window. At index 13 the last of them is as
// far ahead as the input the encoder holds reaches. Measured on G0's music
// and film at 96 and 128 kbps against DEE's streams
// (tools/checks/score_ac4_encode.py --gold): the frame's own 32 slots,
// unwindowed, left the per-band level difference 0.3 dB further from the
// source's than 32 slots centred on the frame's end did; those, 0.07 to 0.14
// dB further than these 48 and the subbands' own bins below, with the
// correlation's distance within 0.005 of it.
constexpr int kWindowSlots = kAcplWindowSlots;
constexpr long long kWindowHalf = kWindowSlots / 2;

// The estimate works on each subband's own band. A QMF subband holds what
// lies within half a subband of its centre, and a strong component of the
// next subband's band too, where the prototype's transition band lets it in:
// left in, a tone near a band's edge pulls that band's parameters towards
// its own channel. Subband k's samples turn by 64 w a slot for a component
// at w, so its own band, k pi / 64 to (k + 1) pi / 64, lies in half of their
// spectrum, 0 to pi for an even k and pi to 2 pi for an odd one, centred on
// a quarter of the subband rate, and its neighbours' components in the other
// half. Over the window, a Hann-windowed DFT of the subband's slots has bins
// a kWindowSlots-th of the subband rate apart, and those within
// kBandCentreBin - 1 of the centre of the subband's own half are its own.
// On G0's 5.1 tones, whose L and Ls tones lie within 44 Hz of subband 1's
// edges, ASPX_ACPL_3's centre prediction in that band, which holds R and C,
// went from gamma5 0.5 and gamma6 0.2 to 1 and 0 (DEE's: 0.9 and 0), and the
// tones' routing margin from -3.7 dB to 9.7 (DEE's: 8.4).
constexpr int kBandCentreBin = kWindowSlots / 4;
constexpr int kOwnReach = kBandCentreBin - 1;
// A band whose own bins hold less than this share of its energy holds mostly
// its neighbours' components, which the decoder's upmix takes with the
// band's parameters as well; its estimate reads all of its bins. Without it,
// ASPX_ACPL_2 routes the 5.1 tones 5.5 dB less cleanly.
constexpr double kOwnShare = 0.5;

// Below this energy a band holds nothing to estimate from, and its values
// stay what the decoder holds.
constexpr double kSilence = 1e-20;

// A gamma of 1: ten steps of 1 638 / 16 384, or five of 3 276 (Table 208).
[[nodiscard]] int gamma_one(int quant_mode) noexcept {
    return quant_mode == 0 ? 10 : 5;
}

[[nodiscard]] acpl::Quant quant_of(int quant_mode) noexcept {
    return quant_mode == 0 ? acpl::Quant::kFine : acpl::Quant::kCoarse;
}

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

// The nearest alpha of Table 203 or 205.
[[nodiscard]] int quantise_alpha(double alpha, acpl::Quant quant) noexcept {
    const acpl::Range range = acpl::quantised_range(acpl::Kind::kAlpha, quant);
    int best = range.min;
    double best_error = std::numeric_limits<double>::max();
    for (int q = range.min; q <= range.max; ++q) {
        const double error = std::abs(acpl::dequantise_alpha(q, quant).alpha - alpha);
        if (error < best_error) {
            best_error = error;
            best = q;
        }
    }
    return best;
}

// The nearest beta of Table 204 or 206's column `ibeta`.
[[nodiscard]] int quantise_beta(double beta, int ibeta, acpl::Quant quant) noexcept {
    const acpl::Range range = acpl::quantised_range(acpl::Kind::kBeta, quant);
    int best = range.min;
    double best_error = std::numeric_limits<double>::max();
    for (int q = range.min; q <= range.max; ++q) {
        const double error = std::abs(acpl::dequantise_beta(q, ibeta, quant) - beta);
        if (error < best_error) {
            best_error = error;
            best = q;
        }
    }
    return best;
}

// One module's sums over a band: the group g = a + b and the half difference
// d = (a - b) / 2, <g, g>, Re<d, g> and <d, d>.
struct ModuleSums {
    double gg = 0.0;
    double dg = 0.0;
    double dd = 0.0;

    void add(std::complex<double> a, std::complex<double> b) noexcept {
        const std::complex<double> g = a + b;
        const std::complex<double> d = 0.5 * (a - b);
        gg += std::norm(g);
        dg += (d * std::conj(g)).real();
        dd += std::norm(d);
    }

    ModuleSums& operator+=(const ModuleSums& other) noexcept {
        gg += other.gg;
        dg += other.dg;
        dd += other.dd;
        return *this;
    }
};

// ASPX_ACPL_3's sums over a band: Lo, Ro and C / sqrt 2's energies and
// cross terms.
struct CouplingSums {
    double ll = 0.0;
    double rr = 0.0;
    double lr = 0.0;
    double cl = 0.0;
    double cr = 0.0;
    double cc = 0.0;

    void add(std::complex<double> lo, std::complex<double> ro, std::complex<double> c) noexcept {
        ll += std::norm(lo);
        rr += std::norm(ro);
        lr += (lo * std::conj(ro)).real();
        cl += (c * std::conj(lo)).real();
        cr += (c * std::conj(ro)).real();
        cc += std::norm(c);
    }

    CouplingSums& operator+=(const CouplingSums& other) noexcept {
        ll += other.ll;
        rr += other.rr;
        lr += other.lr;
        cl += other.cl;
        cr += other.cr;
        cc += other.cc;
        return *this;
    }
};

// ASPX_AJCC's back column over a band: the three channels a back module
// rebuilds from their sum x = a + b + c (Ls, Lb and Tbl over sqrt 2, or their
// mirrors): <x, x>, Re<a, x> and Re<b, x>, and Re<b, b>, Re<c, c> and Re<b, c>.
struct ColumnSums {
    double xx = 0.0;
    double ax = 0.0;
    double bx = 0.0;
    double bb = 0.0;
    double cc = 0.0;
    double bc = 0.0;

    void add(std::complex<double> a, std::complex<double> b, std::complex<double> c) noexcept {
        const std::complex<double> x = a + b + c;
        xx += std::norm(x);
        ax += (a * std::conj(x)).real();
        bx += (b * std::conj(x)).real();
        bb += std::norm(b);
        cc += std::norm(c);
        bc += (b * std::conj(c)).real();
    }

    ColumnSums& operator+=(const ColumnSums& other) noexcept {
        xx += other.xx;
        ax += other.ax;
        bx += other.bx;
        bb += other.bb;
        cc += other.cc;
        bc += other.bc;
        return *this;
    }
};

// ASPX_AJCC's parameters, in AjccDataFields::params' order.
enum JointParam : std::uint8_t {
    kAlpha1,
    kAlpha2,
    kBeta1,
    kBeta2,
    kDry1,
    kDry2,
    kDry3,
    kDry4,
    kWet1,
    kWet2,
    kWet3,
    kWet4,
    kWet5,
    kWet6,
    kJointParams,
};

// The nearest dry or wet value of Pseudocode 4 or 5.
[[nodiscard]] int quantise_step(ajcc::Kind kind, double value, acpl::Quant quant) noexcept {
    const acpl::Range range = ajcc::quantised_range(kind, quant);
    const double base = ajcc::dequantise(kind, 0, quant);
    const double step = ajcc::dequantise(kind, 1, quant) - base;
    return std::clamp(static_cast<int>(std::lround((value - base) / step)), range.min, range.max);
}

// The wet values (wet1, wet2, wet3) whose decorrelated signals give a back
// column's residuals the covariance `b`, `c` and `d` asks, each over the
// sum's energy and doubled: wet3^2 + wet2^2 = b, wet1^2 + wet3^2 = c and
// wet3 (wet1 + wet2) = d. With s = wet3, wet2 = sqrt(b - s^2) and wet1 =
// sqrt(c - s^2); s (wet1 + wet2) rises from 0 and falls again as |s| reaches
// the smaller root, so s is the root of the rising part found by bisection,
// or where that part peaks when d lies beyond it.
[[nodiscard]] std::array<double, 3> column_wets(double b, double c, double d) noexcept {
    b = std::max(b, 0.0);
    c = std::max(c, 0.0);
    const double limit = std::sqrt(std::min(b, c));
    const auto reach = [&](double s) { return s * (std::sqrt(c - s * s) + std::sqrt(b - s * s)); };
    const auto wets = [&](double s) {
        return std::array<double, 3>{std::sqrt(std::max(c - s * s, 0.0)),
                                     std::sqrt(std::max(b - s * s, 0.0)), s};
    };
    const double target = std::abs(d);
    if (limit <= 0.0 || target <= 0.0) {
        return wets(0.0);
    }
    // The peak of reach() on [0, limit]: a ternary search, the function
    // rising to it and falling after.
    double lo = 0.0;
    double hi = limit;
    for (int i = 0; i < 60; ++i) {
        const double m1 = lo + (hi - lo) / 3.0;
        const double m2 = hi - (hi - lo) / 3.0;
        if (reach(m1) < reach(m2)) {
            lo = m1;
        } else {
            hi = m2;
        }
    }
    const double peak = 0.5 * (lo + hi);
    double s = peak;
    if (reach(peak) > target) {
        lo = 0.0;
        hi = peak;
        for (int i = 0; i < 60; ++i) {
            const double mid = 0.5 * (lo + hi);
            (reach(mid) < target ? lo : hi) = mid;
        }
        s = 0.5 * (lo + hi);
    }
    return wets(d < 0.0 ? -s : s);
}

// Whether a band's estimate reads its subbands' own bins alone: where they
// hold kOwnShare of its energy, `own` of `all`.
[[nodiscard]] bool own_bins_carry(double own, double all) noexcept {
    return own >= kOwnShare * all;
}

// A module's alpha and beta for a band, quantised: alpha predicts d from g,
// and beta gives the decorrelated part the energy the quantised prediction
// leaves out, against `input`, the energy of the signal the decorrelator
// takes. Where g is silent, `held` stays.
[[nodiscard]] std::pair<int, int> estimate(const ModuleSums& s, double input, acpl::Quant quant,
                                           std::pair<int, int> held) noexcept {
    if (s.gg <= kSilence || input <= kSilence) {
        return held;
    }
    const int alpha_q = quantise_alpha(std::clamp(2.0 * s.dg / s.gg, -2.0, 2.0), quant);
    const acpl::AlphaValue alpha = acpl::dequantise_alpha(alpha_q, quant);
    const double residual = std::max(0.0, s.dd - alpha.alpha * s.dg + 0.25 * alpha.alpha * alpha.alpha * s.gg);
    const double beta = 2.0 * std::sqrt(residual / input);
    return {alpha_q, quantise_beta(beta, alpha.ibeta, quant)};
}

// The window's DFT weights: Hann, centred on the window's middle, times
// e^(-i 2 pi k t / kWindowSlots) for bin k.
using DftWeights = std::array<std::array<std::complex<double>, kWindowSlots>, kWindowSlots>;

[[nodiscard]] const DftWeights& dft_weights() {
    static const auto weights = [] {
        DftWeights w{};
        const auto n = static_cast<double>(kWindowSlots);
        for (int k = 0; k < kWindowSlots; ++k) {
            for (int t = 0; t < kWindowSlots; ++t) {
                const double tt = static_cast<double>(t);
                const double hann = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * (tt + 0.5) / n);
                w[at(k)][at(t)] = std::polar(hann, -2.0 * std::numbers::pi * k * tt / n);
            }
        }
        return w;
    }();
    return weights;
}

// Whether a DFT bin lies in subband sb's own band (kBandCentreBin's comment).
[[nodiscard]] bool own_bin(int sb, int bin) noexcept {
    const int centre = sb % 2 == 0 ? kBandCentreBin : kWindowSlots - kBandCentreBin;
    const int distance = std::abs(bin - centre);
    return std::min(distance, kWindowSlots - distance) <= kOwnReach;
}

// Pseudocode 121 for one set: its quantised values.
[[nodiscard]] std::array<int, kMaxParamBands> decode_set(const AcplSetFields& set, int start,
                                                        const std::array<int, kMaxParamBands>& previous) {
    std::array<int, kMaxParamBands> out{};
    for (std::size_t i = 0; i < set.values.size(); ++i) {
        const std::size_t band = at(start) + i;
        if (set.diff_type == 1) {
            out[band] = previous[band] + set.values[i];
        } else {
            out[band] = (i == 0 ? 0 : out[band - 1]) + set.values[i];
        }
    }
    return out;
}

// The input channels a layout's analysis reads.
[[nodiscard]] std::size_t analysed_channels(AcplLayout layout) noexcept {
    switch (layout) {
        case AcplLayout::kPair:
            return 2;
        case AcplLayout::kImmersive:
            return 8;
        case AcplLayout::kJoint:
            return 10;
        default:
            return 5;
    }
}

}  // namespace

std::size_t acpl_modules(AcplLayout layout) noexcept {
    switch (layout) {
        case AcplLayout::kPair:
            return 1;
        case AcplLayout::kFiveX:
            return 2;
        case AcplLayout::kImmersive:
            return 4;
        case AcplLayout::kCoupling:
        case AcplLayout::kJoint:
            break;
    }
    return 0;
}

AcplEncoder::AcplEncoder(AcplLayout layout, int num_param_bands_id, int quant_mode, int qmf_band,
                         const FrameTiming& timing)
    : layout_(layout),
      timing_(timing),
      num_param_bands_id_(num_param_bands_id),
      num_bands_(acpl_num_param_bands(num_param_bands_id)),
      quant_mode_(quant_mode),
      qmf_band_(qmf_band),
      analyses_(analysed_channels(layout)) {
    start_band_ = acpl_param_band(config_1ch());
}

AcplConfig1chFields AcplEncoder::config_1ch() const noexcept {
    return {.partial = qmf_band_ > 0,
            .num_param_bands_id = num_param_bands_id_,
            .quant_mode = quant_mode_,
            .qmf_band = std::max(qmf_band_, 1)};
}

AcplConfig2chFields AcplEncoder::config_2ch() const noexcept {
    return {.num_param_bands_id = num_param_bands_id_, .quant_mode_0 = quant_mode_, .quant_mode_1 = quant_mode_};
}

void AcplEncoder::push_slot(std::span<const std::array<double, dsp::kQmfSubbands>> samples) {
    std::vector<Slot>& analysed = slots_.emplace_back(analyses_.size());
    for (std::size_t c = 0; c < analyses_.size() && c < samples.size(); ++c) {
        analyses_[c].process(samples[c], analysed[c]);
    }
}

long long AcplEncoder::first_slot(long long frame) const noexcept {
    return static_cast<long long>(timing_.qmf_slots) * (frame + timing_.control_delay) -
           timing_.hfgen_slots;
}

long long AcplEncoder::slots_needed(long long frame) const noexcept {
    return first_slot(frame) + timing_.qmf_slots + kWindowHalf;
}

const AcplEncoder::Slot& AcplEncoder::slot(std::size_t channel, long long index) const {
    return slots_[static_cast<std::size_t>(index - first_slot_)][channel];
}

std::vector<AcplEncoder::Spectrum> AcplEncoder::spectra(long long first) const {
    const DftWeights& weights = dft_weights();
    const long long from = first + timing_.qmf_slots - kWindowHalf;
    std::vector<Spectrum> out(analyses_.size());
    for (std::size_t c = 0; c < out.size(); ++c) {
        for (std::size_t sb = 0; sb < kSubbands; ++sb) {
            for (std::size_t k = 0; k < at(kWindowSlots); ++k) {
                std::complex<double> sum{};
                for (std::size_t t = 0; t < at(kWindowSlots); ++t) {
                    sum += weights[k][t] * slot(c, from + static_cast<long long>(t))[sb];
                }
                out[c][sb][k] = sum;
            }
        }
    }
    return out;
}

AcplParamFields AcplEncoder::code(AcplKind kind, const Values& q, const Values& previous, bool iframe) const {
    AcplSetFields along_frequency;
    AcplSetFields along_time;
    along_time.diff_type = 1;
    for (int band = start_band_; band < num_bands_; ++band) {
        const std::size_t b = at(band);
        along_frequency.values.push_back(band == start_band_ ? q[b] : q[b] - q[b - 1]);
        along_time.values.push_back(q[b] - previous[b]);
    }
    if (iframe || acpl_set_bits(kind, quant_mode_, along_frequency) <= acpl_set_bits(kind, quant_mode_, along_time)) {
        return {along_frequency};
    }
    return {along_time};
}

AcplFrameFields AcplEncoder::propose(long long frame, bool iframe) const {
    if (layout_ == AcplLayout::kJoint) {
        return propose_joint(frame, iframe);
    }
    const acpl::Quant quant = quant_of(quant_mode_);
    const long long first = first_slot(frame);
    const auto band_of = [&](int sb) { return at(acpl::sb_to_pb(num_bands_, sb)); };
    AcplFrameFields out;

    // Each band's sums over its subbands' own bins, [0], and over all of
    // them, [1].
    const std::vector<Spectrum> x = spectra(first);
    if (layout_ != AcplLayout::kCoupling) {
        // One module on (L, R), two on (L, Ls / sqrt 2) and (R, Rs / sqrt 2),
        // or the immersive element's four on its coupled pairs, from
        // acpl_qmf_band up: below it the residuals rebuild the pairs. A module's
        // estimate takes its pair at any one scale: the immersive element's are
        // over sqrt 2 in the upmix, which scales both alike.
        const std::size_t count = acpl_modules(layout_);
        std::array<std::array<std::array<ModuleSums, kMaxParamBands>, 2>, 4> split{};
        for (int sb = qmf_band_; sb < kSubbands; ++sb) {
            const std::size_t pb = band_of(sb);
            const auto s = at(sb);
            for (int bin = 0; bin < kWindowSlots; ++bin) {
                const auto k = at(bin);
                std::array<ModuleSums, 4> one{};
                if (layout_ == AcplLayout::kFiveX) {
                    one[0].add(x[0][s][k], kHalfRoot2 * x[3][s][k]);
                    one[1].add(x[1][s][k], kHalfRoot2 * x[4][s][k]);
                } else {
                    for (std::size_t m = 0; m < count; ++m) {
                        one[m].add(x[2 * m][s][k], x[2 * m + 1][s][k]);
                    }
                }
                for (std::size_t m = 0; m < count; ++m) {
                    split[m][1][pb] += one[m];
                    if (own_bin(sb, bin)) {
                        split[m][0][pb] += one[m];
                    }
                }
            }
        }
        std::array<std::array<ModuleSums, kMaxParamBands>, 4> sums{};
        for (std::size_t m = 0; m < count; ++m) {
            for (std::size_t b = 0; b < kMaxParamBands; ++b) {
                const bool own = own_bins_carry(split[m][0][b].gg, split[m][1][b].gg);
                sums[m][b] = split[m][own ? 0 : 1][b];
            }
        }
        for (std::size_t m = 0; m < count; ++m) {
            Values alpha{};
            Values beta{};
            const Values& held_alpha = module_history_[m][0];
            const Values& held_beta = module_history_[m][1];
            for (int band = start_band_; band < num_bands_; ++band) {
                const std::size_t b = at(band);
                // The decorrelator takes 2 x0 = g.
                std::tie(alpha[b], beta[b]) = estimate(sums[m][b], sums[m][b].gg, quant, {held_alpha[b], held_beta[b]});
            }
            AcplData1chFields& data = out.modules[m];
            data.alpha1 = code(AcplKind::kAlpha, alpha, held_alpha, iframe);
            data.beta1 = code(AcplKind::kBeta, beta, held_beta, iframe);
        }
        return out;
    }

    // ASPX_ACPL_3: Lo, Ro and C / sqrt 2 per band, and each pair's module,
    // own bins or all by Lo and Ro's energy.
    std::array<std::array<CouplingSums, kMaxParamBands>, 2> centre_split{};
    std::array<std::array<std::array<ModuleSums, kMaxParamBands>, 2>, 2> split{};
    for (int sb = 0; sb < kSubbands; ++sb) {
        const std::size_t pb = band_of(sb);
        const auto s = at(sb);
        for (int bin = 0; bin < kWindowSlots; ++bin) {
            const auto k = at(bin);
            const std::complex<double> l = x[0][s][k];
            const std::complex<double> r = x[1][s][k];
            const std::complex<double> c = kHalfRoot2 * x[2][s][k];
            const std::complex<double> ls = kHalfRoot2 * x[3][s][k];
            const std::complex<double> rs = kHalfRoot2 * x[4][s][k];
            CouplingSums one;
            one.add(l + c + ls, r + c + rs, c);
            std::array<ModuleSums, 2> modules{};
            modules[0].add(l, ls);
            modules[1].add(r, rs);
            for (const std::size_t part : {std::size_t{0}, std::size_t{1}}) {
                if (part == 0 && !own_bin(sb, bin)) {
                    continue;
                }
                centre_split[part][pb] += one;
                split[0][part][pb] += modules[0];
                split[1][part][pb] += modules[1];
            }
        }
    }
    std::array<CouplingSums, kMaxParamBands> centre{};
    std::array<std::array<ModuleSums, kMaxParamBands>, 2> sums{};
    for (std::size_t b = 0; b < kMaxParamBands; ++b) {
        const CouplingSums& own = centre_split[0][b];
        const CouplingSums& all = centre_split[1][b];
        const std::size_t part = own_bins_carry(own.ll + own.rr, all.ll + all.rr) ? 0 : 1;
        centre[b] = centre_split[part][b];
        sums[0][b] = split[0][part][b];
        sums[1][b] = split[1][part][b];
    }
    const int one = gamma_one(quant_mode_);
    const acpl::Range gamma_range = acpl::quantised_range(acpl::Kind::kGamma, quant);
    const double step = acpl::gamma_step(quant);
    const acpl::Range beta3_range = acpl::quantised_range(acpl::Kind::kBeta3, quant);
    // In Table 62's order: alpha1, alpha2, beta1, beta2, beta3, gamma1 to gamma6.
    std::array<Values, 11> q{};
    for (int band = 0; band < num_bands_; ++band) {
        const std::size_t b = at(band);
        const CouplingSums& k = centre[b];
        for (std::size_t p = 0; p < q.size(); ++p) {
            q[p][b] = coupling_history_[p][b];
        }
        if (k.ll + k.rr <= kSilence) {
            continue;
        }
        // gamma5 and gamma6: the least squares prediction of C / sqrt 2 from Lo
        // and Ro, kept where gamma1 = 1 - gamma5 and gamma4 = 1 - gamma6 stay
        // in range.
        const double det = k.ll * k.rr - k.lr * k.lr;
        double g5 = 0.0;
        double g6 = 0.0;
        if (det > 1e-9 * k.ll * k.rr) {
            g5 = (k.cl * k.rr - k.cr * k.lr) / det;
            g6 = (k.cr * k.ll - k.cl * k.lr) / det;
        } else {
            // Lo and Ro alike: the centre from their sum, split evenly.
            const double both = k.ll + k.rr + 2.0 * k.lr;
            g5 = g6 = both > kSilence ? (k.cl + k.cr) / both : 0.0;
        }
        const int low = std::max(gamma_range.min, one - gamma_range.max);
        const int high = std::min(gamma_range.max, one - gamma_range.min);
        const int q5 = std::clamp(static_cast<int>(std::lround(g5 / step)), low, high);
        const int q6 = std::clamp(static_cast<int>(std::lround(g6 / step)), low, high);
        const std::array<int, 6> gamma = {one - q5, -q6, -q5, one - q6, q5, q6};
        std::array<double, 6> g{};
        for (std::size_t i = 0; i < 6; ++i) {
            q[5 + i][b] = gamma[i];
            g[i] = gamma[i] * step;
        }
        // beta3: the centre's unpredicted energy, as -beta3 y2 / 2 carries it,
        // y2 decorrelating v3 = gamma1 Lo + gamma4 Ro.
        const double error = std::max(0.0, k.cc - 2.0 * g[4] * k.cl - 2.0 * g[5] * k.cr + g[4] * g[4] * k.ll +
                                               2.0 * g[4] * g[5] * k.lr + g[5] * g[5] * k.rr);
        // The energy of on_lo Lo + on_ro Ro.
        const auto energy_of = [&](double on_lo, double on_ro) {
            return on_lo * on_lo * k.ll + 2.0 * on_lo * on_ro * k.lr + on_ro * on_ro * k.rr;
        };
        const double v3 = energy_of(g[0], g[3]);
        if (v3 > kSilence) {
            q[4][b] = std::clamp(static_cast<int>(std::lround(2.0 * std::sqrt(error / v3) / acpl::beta3_step(quant))),
                                 beta3_range.min, beta3_range.max);
        }
        // Each pair's module on its group, its decorrelator taking v1 =
        // gamma1 Lo + gamma2 Ro, or v2 = gamma3 Lo + gamma4 Ro.
        const std::array<double, 2> input = {energy_of(g[0], g[1]), energy_of(g[2], g[3])};
        for (std::size_t m = 0; m < 2; ++m) {
            std::tie(q[m][b], q[2 + m][b]) = estimate(sums[m][b], input[m], quant, {q[m][b], q[2 + m][b]});
        }
    }
    AcplData2chFields& data = out.coupling;
    const std::array<AcplKind, 11> kinds = {AcplKind::kAlpha, AcplKind::kAlpha, AcplKind::kBeta,  AcplKind::kBeta,
                                            AcplKind::kBeta3, AcplKind::kGamma, AcplKind::kGamma, AcplKind::kGamma,
                                            AcplKind::kGamma, AcplKind::kGamma, AcplKind::kGamma};
    std::array<AcplParamFields*, 11> fields = {&data.alpha[0], &data.alpha[1], &data.beta[0],  &data.beta[1],
                                               &data.beta3,    &data.gamma[0], &data.gamma[1], &data.gamma[2],
                                               &data.gamma[3], &data.gamma[4], &data.gamma[5]};
    for (std::size_t p = 0; p < q.size(); ++p) {
        *fields[p] = code(kinds[p], q[p], coupling_history_[p], iframe);
    }
    return out;
}

AcplFrameFields AcplEncoder::propose_joint(long long frame, bool iframe) const {
    const acpl::Quant quant = quant_of(quant_mode_);
    const auto band_of = [&](int sb) { return at(acpl::sb_to_pb(num_bands_, sb)); };
    const std::vector<Spectrum> x = spectra(first_slot(frame));
    // Per side, the front module's sums on (L, Tfl / sqrt 2) and the back
    // column's on (Ls, Lb, Tbl) / sqrt 2, over each band's own bins, [0], and
    // all of them, [1]: kJoint's channels L Tfl Ls Lb Tbl, then the right
    // side's.
    std::array<std::array<std::array<ModuleSums, kMaxParamBands>, 2>, 2> front_split{};
    std::array<std::array<std::array<ColumnSums, kMaxParamBands>, 2>, 2> back_split{};
    for (int sb = 0; sb < kSubbands; ++sb) {
        const std::size_t pb = band_of(sb);
        const auto s = at(sb);
        for (int bin = 0; bin < kWindowSlots; ++bin) {
            const auto k = at(bin);
            for (std::size_t side = 0; side < 2; ++side) {
                const std::size_t c = 5 * side;
                ModuleSums front;
                front.add(x[c][s][k], kHalfRoot2 * x[c + 1][s][k]);
                ColumnSums back;
                back.add(kHalfRoot2 * x[c + 2][s][k], kHalfRoot2 * x[c + 3][s][k],
                         kHalfRoot2 * x[c + 4][s][k]);
                front_split[side][1][pb] += front;
                back_split[side][1][pb] += back;
                if (own_bin(sb, bin)) {
                    front_split[side][0][pb] += front;
                    back_split[side][0][pb] += back;
                }
            }
        }
    }
    std::array<Values, 14> q = joint_history_;
    const std::array<std::array<std::size_t, 7>, 2> params = {
        {{kAlpha1, kBeta1, kDry1, kDry2, kWet1, kWet2, kWet3},
         {kAlpha2, kBeta2, kDry3, kDry4, kWet4, kWet5, kWet6}}};
    for (std::size_t side = 0; side < 2; ++side) {
        const auto& [alpha_p, beta_p, dry_a, dry_b, wet_1, wet_2, wet_3] = params[side];
        for (int band = 0; band < num_bands_; ++band) {
            const std::size_t b = at(band);
            // The front module: A-CPL's on the pair, its decorrelator taking
            // the sum, x0in.
            const ModuleSums& own_front = front_split[side][0][b];
            const ModuleSums& front = own_bins_carry(own_front.gg, front_split[side][1][b].gg)
                                          ? own_front
                                          : front_split[side][1][b];
            std::tie(q[alpha_p][b], q[beta_p][b]) =
                estimate(front, front.gg, quant, {q[alpha_p][b], q[beta_p][b]});
            // The back module: the dry shares, then the wet values for what
            // the quantised shares leave, both decorrelators taking the sum,
            // x3in.
            const ColumnSums& own_back = back_split[side][0][b];
            const ColumnSums& back = own_bins_carry(own_back.xx, back_split[side][1][b].xx)
                                         ? own_back
                                         : back_split[side][1][b];
            if (back.xx <= kSilence) {
                continue;
            }
            q[dry_a][b] = quantise_step(ajcc::Kind::kDry, back.ax / back.xx, quant);
            q[dry_b][b] = quantise_step(ajcc::Kind::kDry, back.bx / back.xx, quant);
            const double da = ajcc::dequantise(ajcc::Kind::kDry, q[dry_a][b], quant);
            const double db = ajcc::dequantise(ajcc::Kind::kDry, q[dry_b][b], quant);
            const double dc = 1.0 - da - db;
            const double cx = back.xx - back.ax - back.bx;
            const double rbb = back.bb - 2.0 * db * back.bx + db * db * back.xx;
            const double rcc = back.cc - 2.0 * dc * cx + dc * dc * back.xx;
            const double rbc = back.bc - dc * back.bx - db * cx + db * dc * back.xx;
            const std::array<double, 3> wet =
                column_wets(2.0 * rbb / back.xx, 2.0 * rcc / back.xx, 2.0 * rbc / back.xx);
            q[wet_1][b] = quantise_step(ajcc::Kind::kWet, wet[0], quant);
            q[wet_2][b] = quantise_step(ajcc::Kind::kWet, wet[1], quant);
            q[wet_3][b] = quantise_step(ajcc::Kind::kWet, wet[2], quant);
        }
    }
    AcplFrameFields out;
    out.joint = joint_sent_as(q, iframe);
    return out;
}

AjccDataFields AcplEncoder::joint_sent_as(const std::array<Values, 14>& values, bool iframe) const {
    AjccDataFields data;
    data.no_dt = iframe;
    data.num_param_bands_id = num_param_bands_id_;
    data.core_mode = 0;
    data.qm_ab = quant_mode_;
    data.qm_dw = quant_mode_;
    for (std::size_t p = 0; p < values.size(); ++p) {
        AjccSetFields along_frequency;
        AjccSetFields along_time;
        along_time.diff_type = 1;
        for (int band = 0; band < num_bands_; ++band) {
            const std::size_t b = at(band);
            along_frequency.values.push_back(band == 0 ? values[p][b]
                                                       : values[p][b] - values[p][b - 1]);
            along_time.values.push_back(values[p][b] - joint_history_[p][b]);
        }
        // Along time where the codebook holds every step and that costs less.
        const bool codable = std::ranges::all_of(along_time.values, [&](int value) {
            return ajcc_codable(p, quant_mode_, 1, false, value);
        });
        const bool by_time = !iframe && codable &&
                             ajcc_set_bits(p, quant_mode_, false, along_time) <
                                 ajcc_set_bits(p, quant_mode_, false, along_frequency);
        data.params[p] = {by_time ? along_time : along_frequency};
    }
    return data;
}

AcplFrameFields AcplEncoder::held(bool iframe) const {
    if (layout_ == AcplLayout::kJoint) {
        AcplFrameFields out;
        out.joint = joint_sent_as(joint_history_, iframe);
        return out;
    }
    return sent_as(module_history_, coupling_history_, iframe);
}

AcplFrameFields AcplEncoder::least(bool iframe) const {
    if (layout_ == AcplLayout::kJoint && iframe) {
        // Each side's front all in L and its back all in Ls, with nothing
        // decorrelated: alpha 1, beta 0, dry1 1, dry2 0 and every wet 0.
        const acpl::Quant quant = quant_of(quant_mode_);
        const int alpha = quantise_alpha(1.0, quant);
        const int beta = quantise_beta(0.0, acpl::dequantise_alpha(alpha, quant).ibeta, quant);
        std::array<Values, 14> values{};
        for (int band = 0; band < num_bands_; ++band) {
            const std::size_t b = at(band);
            values[kAlpha1][b] = values[kAlpha2][b] = alpha;
            values[kBeta1][b] = values[kBeta2][b] = beta;
            values[kDry1][b] = values[kDry3][b] = quantise_step(ajcc::Kind::kDry, 1.0, quant);
            values[kDry2][b] = values[kDry4][b] = quantise_step(ajcc::Kind::kDry, 0.0, quant);
            for (const std::size_t wet : {kWet1, kWet2, kWet3, kWet4, kWet5, kWet6}) {
                values[wet][b] = quantise_step(ajcc::Kind::kWet, 0.0, quant);
            }
        }
        AcplFrameFields out;
        out.joint = joint_sent_as(values, true);
        return out;
    }
    return iframe ? sent_as({}, {}, true) : held(false);
}

AcplFrameFields AcplEncoder::sent_as(const std::array<std::array<Values, 2>, 4>& modules,
                                     const std::array<Values, 11>& coupling, bool iframe) const {
    AcplFrameFields out;
    if (layout_ != AcplLayout::kCoupling) {
        const std::size_t count = acpl_modules(layout_);
        for (std::size_t m = 0; m < count; ++m) {
            out.modules[m].alpha1 =
                code(AcplKind::kAlpha, modules[m][0], module_history_[m][0], iframe);
            out.modules[m].beta1 =
                code(AcplKind::kBeta, modules[m][1], module_history_[m][1], iframe);
        }
        return out;
    }
    AcplData2chFields& data = out.coupling;
    const std::array<AcplKind, 11> kinds = {AcplKind::kAlpha, AcplKind::kAlpha, AcplKind::kBeta,  AcplKind::kBeta,
                                            AcplKind::kBeta3, AcplKind::kGamma, AcplKind::kGamma, AcplKind::kGamma,
                                            AcplKind::kGamma, AcplKind::kGamma, AcplKind::kGamma};
    std::array<AcplParamFields*, 11> fields = {&data.alpha[0], &data.alpha[1], &data.beta[0],  &data.beta[1],
                                               &data.beta3,    &data.gamma[0], &data.gamma[1], &data.gamma[2],
                                               &data.gamma[3], &data.gamma[4], &data.gamma[5]};
    for (std::size_t p = 0; p < fields.size(); ++p) {
        *fields[p] = code(kinds[p], coupling[p], coupling_history_[p], iframe);
    }
    return out;
}

void AcplEncoder::commit(const AcplFrameFields& sent) {
    if (layout_ == AcplLayout::kJoint) {
        for (std::size_t p = 0; p < joint_history_.size(); ++p) {
            const AjccSetFields& set = sent.joint.params[p].front();
            AcplSetFields as_acpl;
            as_acpl.diff_type = set.diff_type;
            as_acpl.values = set.values;
            joint_history_[p] = decode_set(as_acpl, 0, joint_history_[p]);
        }
        return;
    }
    if (layout_ != AcplLayout::kCoupling) {
        const std::size_t count = acpl_modules(layout_);
        for (std::size_t m = 0; m < count; ++m) {
            module_history_[m][0] = decode_set(sent.modules[m].alpha1.front(), start_band_, module_history_[m][0]);
            module_history_[m][1] = decode_set(sent.modules[m].beta1.front(), start_band_, module_history_[m][1]);
        }
        return;
    }
    const AcplData2chFields& data = sent.coupling;
    const std::array<const AcplParamFields*, 11> fields = {&data.alpha[0], &data.alpha[1], &data.beta[0],
                                                           &data.beta[1],  &data.beta3,    &data.gamma[0],
                                                           &data.gamma[1], &data.gamma[2], &data.gamma[3],
                                                           &data.gamma[4], &data.gamma[5]};
    for (std::size_t p = 0; p < fields.size(); ++p) {
        coupling_history_[p] = decode_set(fields[p]->front(), 0, coupling_history_[p]);
    }
}

void AcplEncoder::drop_before_frame(long long frame) {
    const long long keep = first_slot(frame) + timing_.qmf_slots - kWindowHalf;
    while (first_slot_ < keep && !slots_.empty()) {
        slots_.pop_front();
        ++first_slot_;
    }
}

std::vector<double> acpl_downmix(AcplLayout layout, std::span<const double> input) {
    // The immersive element's pairs over 2 sqrt 2 (Pseudocode 2's x5in = 2 x5
    // and sqrt 2 on the outputs).
    constexpr double kCoupled = 1.0 / (2.0 * kRoot2);
    switch (layout) {
        case AcplLayout::kPair:
            return {0.5 * (input[0] + input[1])};
        case AcplLayout::kFiveX:
            return {0.5 * (input[0] + kHalfRoot2 * input[3]), 0.5 * (input[1] + kHalfRoot2 * input[4]), input[2]};
        case AcplLayout::kImmersive:
            return {kCoupled * (input[0] + input[1]), kCoupled * (input[2] + input[3]),
                    kCoupled * (input[4] + input[5]), kCoupled * (input[6] + input[7])};
        case AcplLayout::kCoupling:
            break;
    }
    const double centre = kHalfRoot2 * input[2];
    const double norm = 1.0 / (1.0 + kRoot2);
    return {norm * (input[0] + centre + kHalfRoot2 * input[3]), norm * (input[1] + centre + kHalfRoot2 * input[4])};
}

std::array<double, 5> ajcc_core(std::span<const double> input) {
    // Pseudocode 8's input gain, which the upmix applies to the core.
    constexpr double kInputGain = 2.0 + kHalfRoot2;
    constexpr double kFront = 1.0 / kInputGain;
    constexpr double kBack = kHalfRoot2 / kInputGain;
    return {kFront * (input[0] + kHalfRoot2 * input[1]),
            kFront * (input[5] + kHalfRoot2 * input[6]), kFront * input[10],
            kBack * (input[2] + input[3] + input[4]), kBack * (input[7] + input[8] + input[9])};
}

std::vector<double> acpl_residuals(AcplLayout layout, std::span<const double> input) {
    constexpr double kCoupled = 1.0 / (2.0 * kRoot2);
    if (layout == AcplLayout::kPair) {
        return {0.5 * (input[0] - input[1])};
    }
    if (layout == AcplLayout::kImmersive) {
        return {kCoupled * (input[0] - input[1]), kCoupled * (input[2] - input[3]),
                kCoupled * (input[4] - input[5]), kCoupled * (input[6] - input[7])};
    }
    return {0.5 * (input[0] - kHalfRoot2 * input[3]), 0.5 * (input[1] - kHalfRoot2 * input[4])};
}

}  // namespace ac4::detail

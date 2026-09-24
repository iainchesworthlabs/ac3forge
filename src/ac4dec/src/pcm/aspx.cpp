#include "pcm/aspx.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "tables/qmf_tables.hpp"

namespace ac4::detail {
namespace {

constexpr std::size_t kSubbands = 64;
constexpr int kMaxEnv = kAspxMaxSignalEnvelopes;
constexpr int kMaxNoiseEnv = kAspxMaxNoiseEnvelopes;

// Pseudocodes 83, 84 and 96 to 100.
constexpr double kNoiseFloorOffset = 6.0;
constexpr double kPanOffset = 12.0;
constexpr double kLimGain = 1.41254;
constexpr double kEpsilon0 = 1e-12;
constexpr double kMaxSigGain = 1e5;
constexpr double kMaxBoostFact = 1.584893192;

// Table 196.
constexpr std::array<double, 4> kSineRe = {1.0, 0.0, -1.0, 0.0};
constexpr std::array<double, 4> kSineIm = {0.0, 1.0, 0.0, -1.0};

// Exponents of 2 outside this range come only from streams that are not
// audio: real envelopes span a few hundred dB at most. Clamping keeps every
// value the adjuster computes finite (src/ac4dec/ERRATA.md, "Scale factors
// far out of range").
constexpr double kMinExponent = -96.0;
constexpr double kMaxExponent = 96.0;

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

[[nodiscard]] double exp2_clamped(double exponent) noexcept {
    return std::exp2(std::clamp(exponent, kMinExponent, kMaxExponent));
}

using SigQscf = std::array<std::array<int, aspx::kMaxSbgMaster>, kMaxEnv>;
using NoiseQscf = std::array<std::array<int, aspx::kMaxSbgNoise>, kMaxNoiseEnv>;

struct Envelopes {
    SigQscf qscf_sig{};
    NoiseQscf qscf_noise{};
    std::array<std::array<double, aspx::kMaxSbgMaster>, kMaxEnv> scf_sig{};
    std::array<std::array<double, aspx::kMaxSbgNoise>, kMaxNoiseEnv> scf_noise{};
};

// A value of aspx_data_sig or aspx_data_noise: huff_decode() for the first
// value of an envelope coded along frequency, huff_decode_diff() for the
// rest, which is the codeword's index less its codebook's cb_off (4.3.10.8.3).
[[nodiscard]] int envelope_value(const AspxEnvelope& e, int i, AspxDataType type, int quant_mode,
                                 AspxStereoMode stereo_mode) noexcept {
    AspxHcbType hcb = AspxHcbType::kDt;
    if (e.delta_dir == 0) {
        hcb = i == 0 ? AspxHcbType::kF0 : AspxHcbType::kDf;
    }
    const Codebook& codebook = aspx_codebook(type, quant_mode, stereo_mode, hcb);
    return static_cast<int>(e.huff_index[at(i)]) - codebook.cb_off;
}

[[nodiscard]] int signal_groups(const aspx::SubbandGroups& g, int freqres) noexcept {
    return freqres != 0 ? g.num_sbg_sig_highres : g.num_sbg_sig_lowres;
}

[[nodiscard]] std::span<const std::uint8_t> signal_table(const aspx::SubbandGroups& g,
                                                         int freqres) noexcept {
    return freqres != 0 ? std::span<const std::uint8_t>(g.sbg_sig_highres)
                        : std::span<const std::uint8_t>(g.sbg_sig_lowres);
}

// Pseudocode 80.
void signal_qscf(const AspxChannel& c, const aspx::SubbandGroups& g, int delta,
                 const AspxChannelState& state, SigQscf& q) {
    std::array<int, aspx::kMaxSbgMaster> high2low{};
    std::array<int, aspx::kMaxSbgMaster + 1> low2high{};
    int low = 0;
    for (int sbg = 0; sbg < g.num_sbg_sig_highres; ++sbg) {
        if (low < g.num_sbg_sig_lowres &&
            g.sbg_sig_lowres[at(low + 1)] == g.sbg_sig_highres[at(sbg)]) {
            ++low;
            low2high[at(low)] = sbg;
        }
        high2low[at(sbg)] = low;
    }
    const AspxFraming& f = c.framing;
    for (int atsg = 0; atsg < f.num_env; ++atsg) {
        const int res = f.atsg_freqres[at(atsg)];
        int res_prev = res;
        if (atsg > 0) {
            res_prev = f.atsg_freqres[at(atsg - 1)];
        } else if (state.have_previous) {
            res_prev = state.freqres_prev;
        }
        const AspxEnvelope& e = c.sig[at(atsg)];
        int sum = 0;
        for (int sbg = 0; sbg < signal_groups(g, res); ++sbg) {
            int sbg_prev = sbg;
            if (res == 0 && res_prev == 1) {
                sbg_prev = low2high[at(sbg)];
            } else if (res == 1 && res_prev == 0) {
                sbg_prev = high2low[at(sbg)];
            }
            const int value =
                envelope_value(e, sbg, AspxDataType::kSignal, c.qmode_env, c.stereo_mode);
            if (e.delta_dir == 0) {
                sum += delta * value;
                q[at(atsg)][at(sbg)] = sum;
            } else {
                const int prev =
                    atsg == 0 ? state.qscf_sig_prev[at(sbg_prev)] : q[at(atsg - 1)][at(sbg_prev)];
                q[at(atsg)][at(sbg)] = prev + delta * value;
            }
        }
    }
}

// Pseudocode 81.
void noise_qscf(const AspxChannel& c, const aspx::SubbandGroups& g, int delta,
                const AspxChannelState& state, NoiseQscf& q) {
    const AspxFraming& f = c.framing;
    for (int atsg = 0; atsg < f.num_noise; ++atsg) {
        const AspxEnvelope& e = c.noise[at(atsg)];
        int sum = 0;
        for (int sbg = 0; sbg < g.num_sbg_noise; ++sbg) {
            const int value = envelope_value(e, sbg, AspxDataType::kNoise, 0, c.stereo_mode);
            if (e.delta_dir == 0) {
                sum += delta * value;
                q[at(atsg)][at(sbg)] = sum;
            } else {
                const int prev =
                    atsg == 0 ? state.qscf_noise_prev[at(sbg)] : q[at(atsg - 1)][at(sbg)];
                q[at(atsg)][at(sbg)] = prev + delta * value;
            }
        }
    }
}

// Pseudocodes 82 and 83. Pseudocode 82 tests scf_sig_sbg[1][atsg] < 0, which
// no dequantised value is; the quantised qscf_sig_sbg is read
// (src/ac4dec/ERRATA.md, "The first signal scale factor below zero").
void dequantise(const AspxChannel& c, const aspx::SubbandGroups& g, Envelopes& e) {
    const double a = c.qmode_env == 0 ? 2.0 : 1.0;
    const AspxFraming& f = c.framing;
    for (int atsg = 0; atsg < f.num_env; ++atsg) {
        const int num = signal_groups(g, f.atsg_freqres[at(atsg)]);
        const auto& q = e.qscf_sig[at(atsg)];
        auto& scf = e.scf_sig[at(atsg)];
        for (int sbg = 0; sbg < num; ++sbg) {
            scf[at(sbg)] = 64.0 * exp2_clamped(q[at(sbg)] / a);
        }
        if (c.sig[at(atsg)].delta_dir == 0 && num > 1 && q[0] == 0 && q[1] < 0) {
            scf[0] = scf[1];
        }
    }
    for (int atsg = 0; atsg < f.num_noise; ++atsg) {
        for (int sbg = 0; sbg < g.num_sbg_noise; ++sbg) {
            e.scf_noise[at(atsg)][at(sbg)] =
                exp2_clamped(kNoiseFloorOffset - e.qscf_noise[at(atsg)][at(sbg)]);
        }
    }
}

// Pseudocode 84: channel 0 carries the sum, channel 1 the balance, with
// channel 0's framing and aspx_qmode_env.
void dequantise_balance(const AspxChannel& c, const aspx::SubbandGroups& g, Envelopes& sum,
                        Envelopes& balance) {
    const double a = c.qmode_env == 0 ? 2.0 : 1.0;
    const AspxFraming& f = c.framing;
    for (int atsg = 0; atsg < f.num_env; ++atsg) {
        for (int sbg = 0; sbg < signal_groups(g, f.atsg_freqres[at(atsg)]); ++sbg) {
            const double qa = sum.qscf_sig[at(atsg)][at(sbg)] / a;
            const double qb = balance.qscf_sig[at(atsg)][at(sbg)] / a;
            const double nom = exp2_clamped(qa + 1.0) * 64.0;
            sum.scf_sig[at(atsg)][at(sbg)] = nom / (1.0 + exp2_clamped(kPanOffset - qb));
            balance.scf_sig[at(atsg)][at(sbg)] = nom / (1.0 + exp2_clamped(qb - kPanOffset));
        }
    }
    for (int atsg = 0; atsg < f.num_noise; ++atsg) {
        for (int sbg = 0; sbg < g.num_sbg_noise; ++sbg) {
            const double qa = sum.qscf_noise[at(atsg)][at(sbg)];
            const double qb = balance.qscf_noise[at(atsg)][at(sbg)];
            const double nom = exp2_clamped(kNoiseFloorOffset - qa + 1.0);
            sum.scf_noise[at(atsg)][at(sbg)] = nom / (1.0 + exp2_clamped(kPanOffset - qb));
            balance.scf_noise[at(atsg)][at(sbg)] = nom / (1.0 + exp2_clamped(qb - kPanOffset));
        }
    }
}

// Borders that do not increase, or an interval Q_low does not hold, cannot
// be decoded.
[[nodiscard]] bool framing_fits(const AspxFraming& f, int num_ts_in_ats, int q_low_slots) noexcept {
    if (f.num_env < 1 || f.num_env > kMaxEnv || f.num_noise < 1 || f.num_noise > kMaxNoiseEnv) {
        return false;
    }
    if (f.atsg_sig[0] < 0 || f.atsg_sig[at(f.num_env)] * num_ts_in_ats > q_low_slots) {
        return false;
    }
    for (int atsg = 0; atsg < f.num_env; ++atsg) {
        if (f.atsg_sig[at(atsg + 1)] <= f.atsg_sig[at(atsg)]) {
            return false;
        }
    }
    for (int atsg = 0; atsg < f.num_noise; ++atsg) {
        if (f.atsg_noise[at(atsg + 1)] <= f.atsg_noise[at(atsg)]) {
            return false;
        }
    }
    return true;
}

// Per envelope and A-SPX subband (sb counted from sbx).
using EnvelopeMatrix = std::array<std::array<double, kSubbands>, kMaxEnv>;

// Pseudocodes 90 to 108 and clause 5.7.6.5.3 for one channel whose
// envelopes are decoded.
class ChannelAssembly {
   public:
    ChannelAssembly(const AspxFrame& frame, const aspx::SubbandGroups& groups,
                    const aspx::PatchTables& patches, AspxChannelIo& io, const Envelopes& envelopes)
        : frame_(frame),
          g_(groups),
          p_(patches),
          io_(io),
          c_(*io.data),
          f_(io.data->framing),
          st_(*io.state),
          e_(envelopes) {}

    void run(std::vector<QmfValue>& q_high, std::vector<QmfValue>& y);

   private:
    void estimate(std::span<const QmfValue> q_high);
    void map_scale_factors();
    void place_sinusoids();
    void compute_gains();
    void limit();
    void assemble(std::span<const QmfValue> q_high, std::span<QmfValue> y);
    void interleave(std::span<const QmfValue> y);
    void keep(std::span<const QmfValue> y);

    [[nodiscard]] int ts_begin() const noexcept { return f_.atsg_sig[0] * frame_.num_ts_in_ats; }
    [[nodiscard]] int ts_end() const noexcept {
        return f_.atsg_sig[at(f_.num_env)] * frame_.num_ts_in_ats;
    }
    [[nodiscard]] bool transient_envelope(int atsg) const noexcept {
        return atsg == f_.tsg_ptr || atsg == p_sine_at_end_;
    }

    const AspxFrame& frame_;
    const aspx::SubbandGroups& g_;
    const aspx::PatchTables& p_;
    AspxChannelIo& io_;
    const AspxChannel& c_;
    const AspxFraming& f_;
    AspxChannelState& st_;
    const Envelopes& e_;

    int p_sine_at_end_ = -1;
    EnvelopeMatrix est_sig_{};
    EnvelopeMatrix scf_sig_{};
    EnvelopeMatrix scf_noise_{};
    std::array<std::array<bool, kSubbands>, kMaxEnv> sine_idx_{};
    std::array<std::array<bool, kSubbands>, kMaxEnv> sine_area_{};
    EnvelopeMatrix sine_lev_{};
    EnvelopeMatrix noise_lev_{};
    EnvelopeMatrix sig_gain_{};
};

void ChannelAssembly::run(std::vector<QmfValue>& q_high, std::vector<QmfValue>& y) {
    const int q_low_slots = frame_.num_qmf_timeslots + frame_.ts_offset_hfgen;
    q_high.assign(at(q_low_slots) * kSubbands, QmfValue{});
    y.assign(at(q_low_slots) * kSubbands, QmfValue{});
    const aspx::HfGeneratorInput<double> in{
        .q_low_ext = io_.ext,
        .num_qmf_timeslots = frame_.num_qmf_timeslots,
        .ts_offset_hfgen = frame_.ts_offset_hfgen,
        .ts_begin = ts_begin(),
        .ts_end = ts_end(),
        .preflat = frame_.config->preflat,
        .tna_mode = std::span<const std::uint8_t>(c_.tna_mode).first(at(g_.num_sbg_noise)),
    };
    aspx::generate_high_band<double>(g_, p_, in, st_.hf, q_high);
    p_sine_at_end_ = st_.tsg_ptr_prev == st_.num_atsg_sig_prev ? 0 : -1;
    estimate(q_high);
    map_scale_factors();
    place_sinusoids();
    compute_gains();
    limit();
    assemble(q_high, y);
    interleave(y);
    keep(y);
}

// Pseudocode 90. The envelope's energy, summed over QMF slots, is divided by
// its length in QMF slots, where the text divides by A-SPX slots: the mean
// energy per QMF subsample that clause 3.1 makes a signal scale factor
// (src/ac4dec/ERRATA.md, "The estimated envelope's time divisor").
void ChannelAssembly::estimate(std::span<const QmfValue> q_high) {
    const int sbx = g_.sbx;
    for (int atsg = 0; atsg < f_.num_env; ++atsg) {
        const std::span<const std::uint8_t> table = signal_table(g_, f_.atsg_freqres[at(atsg)]);
        const int tsa = f_.atsg_sig[at(atsg)] * frame_.num_ts_in_ats;
        const int tsz = f_.atsg_sig[at(atsg + 1)] * frame_.num_ts_in_ats;
        const double length =
            (f_.atsg_sig[at(atsg + 1)] - f_.atsg_sig[at(atsg)]) * frame_.num_ts_in_ats;
        int sbg = 0;
        for (int sb = 0; sb < g_.num_sb_aspx; ++sb) {
            if (sb + sbx == table[at(sbg + 1)]) {
                ++sbg;
            }
            double est = 0.0;
            if (!frame_.config->interpolation) {
                const int lo = table[at(sbg)];
                const int hi = table[at(sbg + 1)];
                for (int ts = tsa; ts < tsz; ++ts) {
                    for (int j = lo; j < hi; ++j) {
                        est += std::norm(q_high[at(ts) * kSubbands + at(j)]);
                    }
                }
                est /= hi - lo;
            } else {
                for (int ts = tsa; ts < tsz; ++ts) {
                    est += std::norm(q_high[at(ts) * kSubbands + at(sb + sbx)]);
                }
            }
            est_sig_[at(atsg)][at(sb)] = est / length;
        }
    }
}

// Pseudocode 91.
void ChannelAssembly::map_scale_factors() {
    const int sbx = g_.sbx;
    int atsg_noise = 0;
    for (int atsg = 0; atsg < f_.num_env; ++atsg) {
        const int res = f_.atsg_freqres[at(atsg)];
        const std::span<const std::uint8_t> table = signal_table(g_, res);
        for (int sbg = 0; sbg < signal_groups(g_, res); ++sbg) {
            for (int sb = table[at(sbg)] - sbx; sb < table[at(sbg + 1)] - sbx; ++sb) {
                scf_sig_[at(atsg)][at(sb)] = e_.scf_sig[at(atsg)][at(sbg)];
            }
        }
        if (atsg_noise + 1 < f_.num_noise &&
            f_.atsg_sig[at(atsg)] == f_.atsg_noise[at(atsg_noise + 1)]) {
            ++atsg_noise;
        }
        for (int sbg = 0; sbg < g_.num_sbg_noise; ++sbg) {
            for (int sb = g_.sbg_noise[at(sbg)] - sbx; sb < g_.sbg_noise[at(sbg + 1)] - sbx; ++sb) {
                scf_noise_[at(atsg)][at(sb)] = e_.scf_noise[at(atsg_noise)][at(sbg)];
            }
        }
    }
}

// Pseudocodes 92 and 93. The middle of a group is (int)(0.5 * (sbz + sba)),
// the cast taken over the product (src/ac4dec/ERRATA.md, "The sinusoid's
// subband").
void ChannelAssembly::place_sinusoids() {
    const int sbx = g_.sbx;
    for (int atsg = 0; atsg < f_.num_env; ++atsg) {
        for (int sbg = 0; sbg < g_.num_sbg_sig_highres; ++sbg) {
            const int lo = g_.sbg_sig_highres[at(sbg)] - sbx;
            const int hi = g_.sbg_sig_highres[at(sbg + 1)] - sbx;
            const int mid = (hi + lo) / 2;
            for (int sb = lo; sb < hi; ++sb) {
                const bool starts =
                    atsg >= f_.tsg_ptr || p_sine_at_end_ == 0 || st_.sine_prev[at(sb + sbx)];
                sine_idx_[at(atsg)][at(sb)] = sb == mid && starts && c_.add_harmonic[at(sbg)];
            }
        }
        const int res = f_.atsg_freqres[at(atsg)];
        const std::span<const std::uint8_t> table = signal_table(g_, res);
        for (int sbg = 0; sbg < signal_groups(g_, res); ++sbg) {
            const int lo = table[at(sbg)] - sbx;
            const int hi = table[at(sbg + 1)] - sbx;
            bool present = false;
            for (int sb = lo; sb < hi; ++sb) {
                present = present || sine_idx_[at(atsg)][at(sb)];
            }
            for (int sb = lo; sb < hi; ++sb) {
                sine_area_[at(atsg)][at(sb)] = present;
            }
        }
    }
}

// Pseudocodes 94 and 95. Pseudocode 95 sets b_sine_at_end and then tests
// p_sine_at_end, Pseudocode 92's; that is the one used (src/ac4dec/ERRATA.md,
// "b_sine_at_end").
void ChannelAssembly::compute_gains() {
    constexpr double kEpsilon = 1.0;
    for (int atsg = 0; atsg < f_.num_env; ++atsg) {
        for (int sb = 0; sb < g_.num_sb_aspx; ++sb) {
            const double scf_sig = scf_sig_[at(atsg)][at(sb)];
            const double scf_noise = scf_noise_[at(atsg)][at(sb)];
            const double sig_noise_fact = scf_sig / (1.0 + scf_noise);
            const double sine = sine_idx_[at(atsg)][at(sb)] ? 1.0 : 0.0;
            sine_lev_[at(atsg)][at(sb)] = std::sqrt(sig_noise_fact * sine);
            noise_lev_[at(atsg)][at(sb)] = std::sqrt(sig_noise_fact * scf_noise);
            double denom = kEpsilon + est_sig_[at(atsg)][at(sb)];
            if (!sine_area_[at(atsg)][at(sb)]) {
                if (!transient_envelope(atsg)) {
                    denom *= 1.0 + scf_noise;
                }
                sig_gain_[at(atsg)][at(sb)] = std::sqrt(scf_sig / denom);
            } else {
                denom *= 1.0 + scf_noise;
                sig_gain_[at(atsg)][at(sb)] = std::sqrt(scf_sig * scf_noise / denom);
            }
        }
    }
}

// Pseudocodes 96 to 101, with aspx_limiter set; without it the gains and
// levels go on as they are (src/ac4dec/ERRATA.md, "aspx_limiter"). A subband
// above the limiter table's last border counts in its last group
// (src/ac4dec/ERRATA.md, "The limiter's last group").
void ChannelAssembly::limit() {
    if (!frame_.config->limiter) {
        return;
    }
    const int sbx = g_.sbx;
    const int num_lim = p_.num_sbg_lim;
    std::array<int, kSubbands> group{};
    int sbg = 0;
    for (int sb = 0; sb < g_.num_sb_aspx; ++sb) {
        while (sbg + 1 < num_lim && sb + sbx >= p_.sbg_lim[at(sbg + 1)]) {
            ++sbg;
        }
        group[at(sb)] = sbg;
    }
    for (int atsg = 0; atsg < f_.num_env; ++atsg) {
        auto& gain = sig_gain_[at(atsg)];
        auto& noise = noise_lev_[at(atsg)];
        auto& sine = sine_lev_[at(atsg)];
        const auto& scf = scf_sig_[at(atsg)];
        const auto& est = est_sig_[at(atsg)];
        // Pseudocode 96.
        std::array<double, aspx::kMaxSbgLim> nom{};
        std::array<double, aspx::kMaxSbgLim> denom{};
        denom.fill(kEpsilon0);
        for (int sb = 0; sb < g_.num_sb_aspx; ++sb) {
            nom[at(group[at(sb)])] += scf[at(sb)];
            denom[at(group[at(sb)])] += est[at(sb)];
        }
        std::array<double, kSubbands> max_gain{};
        for (int sb = 0; sb < g_.num_sb_aspx; ++sb) {
            const auto k = at(group[at(sb)]);
            max_gain[at(sb)] = std::min(std::sqrt(nom[k] / denom[k]) * kLimGain, kMaxSigGain);
        }
        // Pseudocodes 97 and 98.
        for (int sb = 0; sb < g_.num_sb_aspx; ++sb) {
            if (gain[at(sb)] > 0.0) {
                noise[at(sb)] =
                    std::min(noise[at(sb)], noise[at(sb)] * max_gain[at(sb)] / gain[at(sb)]);
            }
            gain[at(sb)] = std::min(gain[at(sb)], max_gain[at(sb)]);
        }
        // Pseudocode 99.
        std::array<double, aspx::kMaxSbgLim> boost_nom{};
        std::array<double, aspx::kMaxSbgLim> boost_denom{};
        boost_nom.fill(kEpsilon0);
        boost_denom.fill(kEpsilon0);
        for (int sb = 0; sb < g_.num_sb_aspx; ++sb) {
            const auto k = at(group[at(sb)]);
            boost_nom[k] += scf[at(sb)];
            boost_denom[k] +=
                est[at(sb)] * gain[at(sb)] * gain[at(sb)] + sine[at(sb)] * sine[at(sb)];
            if (!(sine[at(sb)] != 0.0 || transient_envelope(atsg))) {
                boost_denom[k] += noise[at(sb)] * noise[at(sb)];
            }
        }
        // Pseudocodes 100 and 101.
        for (int sb = 0; sb < g_.num_sb_aspx; ++sb) {
            const auto k = at(group[at(sb)]);
            const double boost = std::min(std::sqrt(boost_nom[k] / boost_denom[k]), kMaxBoostFact);
            gain[at(sb)] *= boost;
            noise[at(sb)] *= boost;
            sine[at(sb)] *= boost;
        }
    }
}

// Pseudocodes 102 to 108. The noise and sine indices count on from the last
// ones the previous interval used, whatever its borders, and time counts from
// the interval's first QMF slot (src/ac4dec/ERRATA.md, "The noise and tone
// generators' indices").
void ChannelAssembly::assemble(std::span<const QmfValue> q_high, std::span<QmfValue> y) {
    const int sbx = g_.sbx;
    const int nsb = g_.num_sb_aspx;
    const int first = ts_begin();
    const int last = ts_end();
    // Pseudocode 106: the slots before this interval are the last one's.
    for (int ts = 0; ts < first; ++ts) {
        if (ts < st_.y_prev_slots) {
            std::copy_n(st_.y_prev.begin() + static_cast<std::ptrdiff_t>(at(ts) * kSubbands),
                        kSubbands, y.begin() + static_cast<std::ptrdiff_t>(at(ts) * kSubbands));
        }
    }
    const int noise_base = frame_.master_reset ? 0 : st_.noise_index;
    const int sine_base = st_.first_frame ? 1 : (st_.sine_index + 1) % 4;
    const auto& noise_table = tables::kAspxNoise;
    int atsg = 0;
    int noise_index = st_.noise_index;
    int sine_index = st_.sine_index;
    for (int ts = first; ts < last; ++ts) {
        while (atsg + 1 < f_.num_env && ts >= f_.atsg_sig[at(atsg + 1)] * frame_.num_ts_in_ats) {
            ++atsg;
        }
        sine_index = (sine_base + ts - first) % 4;
        const std::span<const QmfValue> high = q_high.subspan(at(ts) * kSubbands, kSubbands);
        const std::span<QmfValue> out = y.subspan(at(ts) * kSubbands, kSubbands);
        for (int sb = 0; sb < nsb; ++sb) {
            noise_index = (noise_base + nsb * (ts - first) + sb + 1) % 512;
            const auto& noise = noise_table[at(noise_index)];
            const double noise_level = noise_lev_[at(atsg)][at(sb)];
            const double sine_level = sine_lev_[at(atsg)][at(sb)];
            const double sign = (sb + sbx) % 2 == 0 ? 1.0 : -1.0;
            const QmfValue noise_value(static_cast<double>(noise[0]),
                                       static_cast<double>(noise[1]));
            out[at(sb + sbx)] = sig_gain_[at(atsg)][at(sb)] * high[at(sb + sbx)] +
                                noise_level * noise_value +
                                QmfValue(sine_level * kSineRe[at(sine_index)],
                                         sine_level * sign * kSineIm[at(sine_index)]);
        }
    }
    if (last > first) {
        st_.noise_index = noise_index;
        st_.sine_index = sine_index;
        st_.first_frame = false;
    }
}

// Clause 5.7.6.5.3: below the crossover the delayed input, above it the
// assembled high band, with the waveform-coded components added where a
// subband group is frequency interleaved and in their place in a
// time-interleaved slot. Above the A-SPX range nothing is left but what a
// time-interleaved slot carries.
void ChannelAssembly::interleave(std::span<const QmfValue> y) {
    const int sbx = g_.sbx;
    const int sbz = g_.sbx + g_.num_sb_aspx;
    std::array<int, kSubbands> high_group{};
    int sbg = 0;
    for (int sb = sbx; sb < sbz; ++sb) {
        while (sbg + 1 < g_.num_sbg_sig_highres && sb >= g_.sbg_sig_highres[at(sbg + 1)]) {
            ++sbg;
        }
        high_group[at(sb)] = sbg;
    }
    for (int ts = 0; ts < frame_.num_qmf_timeslots; ++ts) {
        const std::span<const QmfValue> delayed =
            io_.ext.subspan(at(ts + aspx::kTsOffsetHfadj) * kSubbands, kSubbands);
        const std::span<const QmfValue> extension = y.subspan(at(ts) * kSubbands, kSubbands);
        const std::span<QmfValue> out = io_.out.subspan(at(ts) * kSubbands, kSubbands);
        const bool tic = c_.tic_used_in_slot[at(ts / frame_.num_ts_in_ats)];
        for (int sb = 0; sb < static_cast<int>(kSubbands); ++sb) {
            if (sb < sbx || tic) {
                out[at(sb)] = delayed[at(sb)];
            } else if (sb < sbz) {
                out[at(sb)] = c_.fic_used_in_sfb[at(high_group[at(sb)])]
                                  ? delayed[at(sb)] + extension[at(sb)]
                                  : extension[at(sb)];
            } else {
                out[at(sb)] = QmfValue{};
            }
        }
    }
}

void ChannelAssembly::keep(std::span<const QmfValue> y) {
    const int past = std::max(0, ts_end() - frame_.num_qmf_timeslots);
    const auto from = static_cast<std::ptrdiff_t>(at(frame_.num_qmf_timeslots) * kSubbands);
    const auto count = static_cast<std::ptrdiff_t>(at(past) * kSubbands);
    st_.y_prev.assign(y.begin() + from, y.begin() + from + count);
    st_.y_prev_slots = past;
    st_.tsg_ptr_prev = f_.tsg_ptr;
    st_.num_atsg_sig_prev = f_.num_env;
    st_.sine_prev.fill(false);
    for (int sb = 0; sb < g_.num_sb_aspx; ++sb) {
        st_.sine_prev[at(sb + g_.sbx)] = sine_idx_[at(f_.num_env - 1)][at(sb)];
    }
}

void keep_envelopes(const AspxChannel& c, const aspx::SubbandGroups& g, const Envelopes& e,
                    AspxChannelState& st) {
    const AspxFraming& f = c.framing;
    const int last = f.num_env - 1;
    st.freqres_prev = f.atsg_freqres[at(last)];
    st.qscf_sig_prev.fill(0);
    for (int sbg = 0; sbg < signal_groups(g, st.freqres_prev); ++sbg) {
        st.qscf_sig_prev[at(sbg)] = e.qscf_sig[at(last)][at(sbg)];
    }
    st.qscf_noise_prev.fill(0);
    for (int sbg = 0; sbg < g.num_sbg_noise; ++sbg) {
        st.qscf_noise_prev[at(sbg)] = e.qscf_noise[at(f.num_noise - 1)][at(sbg)];
    }
    st.have_previous = true;
}

}  // namespace

AspxInterval aspx_interval(const AspxFraming& framing, int num_ts_in_ats) noexcept {
    return {.first = framing.atsg_sig[0] * num_ts_in_ats,
            .last = framing.atsg_sig[at(framing.num_env)] * num_ts_in_ats};
}

ParseResult aspx_tables(const AspxFrame& frame, aspx::SubbandGroups& groups,
                        aspx::PatchTables& patches) {
    const AspxConfig& config = *frame.config;
    const aspx::FrequencyConfig frequency{.master_freq_scale = config.master_freq_scale,
                                          .start_freq = config.start_freq,
                                          .stop_freq = config.stop_freq,
                                          .noise_sbg = config.noise_sbg,
                                          .xover_subband_offset = frame.xover_subband_offset};
    if (aspx::derive_subband_groups(frequency, groups) != aspx::GroupsError::kNone) {
        return fail(DecodeError::kInvalidStream,
                    "A-SPX subband groups the syntax should have refused");
    }
    if (!aspx::derive_patch_tables(groups, config.master_freq_scale, frame.base_48k, patches)) {
        return fail(DecodeError::kInvalidStream,
                    "an A-SPX configuration whose patches cannot be built");
    }
    return {};
}

ParseResult check_aspx(const AspxFrame& frame, std::span<const AspxChannel* const> channels) {
    if (channels.empty() || channels.size() > 2 || frame.config == nullptr) {
        return fail(DecodeError::kInvalidStream,
                    "an A-SPX element of neither one nor two channels");
    }
    aspx::SubbandGroups groups;
    aspx::PatchTables patches;
    if (auto ok = aspx_tables(frame, groups, patches); !ok) {
        return ok;
    }
    const int q_low_slots = frame.num_qmf_timeslots + frame.ts_offset_hfgen;
    for (const AspxChannel* data : channels) {
        if (!framing_fits(data->framing, frame.num_ts_in_ats, q_low_slots)) {
            return fail(DecodeError::kInvalidStream,
                        "A-SPX interval borders out of order or out of range");
        }
    }
    return {};
}

ParseResult decode_aspx(const AspxFrame& frame, std::span<AspxChannelIo> channels) {
    std::array<const AspxChannel*, 2> parsed{};
    for (std::size_t c = 0; c < channels.size() && c < parsed.size(); ++c) {
        parsed[c] = channels[c].data;
    }
    if (auto ok = check_aspx(frame, std::span<const AspxChannel* const>(parsed).first(
                                        std::min(channels.size(), parsed.size())));
        !ok) {
        return ok;
    }
    aspx::SubbandGroups groups;
    aspx::PatchTables patches;
    (void)aspx_tables(frame, groups, patches);
    // Pseudocodes 80 to 84 for every channel first: a balanced pair's scale
    // factors come from both.
    std::array<Envelopes, 2> envelopes{};
    for (std::size_t c = 0; c < channels.size(); ++c) {
        const AspxChannel& data = *channels[c].data;
        const int delta = c == 1 && frame.balance ? 2 : 1;
        signal_qscf(data, groups, delta, *channels[c].state, envelopes[c].qscf_sig);
        noise_qscf(data, groups, delta, *channels[c].state, envelopes[c].qscf_noise);
    }
    if (channels.size() == 2 && frame.balance) {
        dequantise_balance(*channels[0].data, groups, envelopes[0], envelopes[1]);
    } else {
        for (std::size_t c = 0; c < channels.size(); ++c) {
            dequantise(*channels[c].data, groups, envelopes[c]);
        }
    }
    std::vector<QmfValue> q_high;
    std::vector<QmfValue> y;
    for (std::size_t c = 0; c < channels.size(); ++c) {
        ChannelAssembly(frame, groups, patches, channels[c], envelopes[c]).run(q_high, y);
        keep_envelopes(*channels[c].data, groups, envelopes[c], *channels[c].state);
    }
    return {};
}

}  // namespace ac4::detail

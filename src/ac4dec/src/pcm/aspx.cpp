#include "pcm/aspx.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>

#include "iclforge/ac4core/detail/profiling.hpp"
#include "iclforge/ac4core/dsp/real_functions.hpp"
#include "iclforge/ac4core/dsp/scalar_traits.hpp"
#include "iclforge/ac4core/tables/qmf_tables_fixed.hpp"
#include "iclforge/ac4core/tables/qmf_tables.hpp"

namespace iclforge::ac4::detail {
namespace {

constexpr std::size_t kSubbands = 64;
constexpr int kMaxEnv = kAspxMaxSignalEnvelopes;
constexpr int kMaxNoiseEnv = kAspxMaxNoiseEnvelopes;

// The envelopes' scale factors, the estimates, gains and levels: Real at double
// and float, and a mantissa and a power of two at Fixed32, whose QMF domain is
// below the double decoder's by dsp::kQmfShift (dsp/scalar_traits.hpp): a
// scale factor or an estimate is an energy, a level an amplitude, and a gain
// neither.
using Energy = dsp::Energy<Real>;

// Pseudocodes 83, 84 and 96 to 100.
constexpr Energy kNoiseFloorOffset{6};
constexpr Energy kPanOffset{12};
constexpr Energy kLimGain = Energy(1.41254);
constexpr Energy kEpsilon0 = dsp::qmf_energy<Real>(Energy(1e-12));
constexpr Energy kMaxSigGain = Energy(1e5);
constexpr Energy kMaxBoostFact = Energy(1.584893192);

// Table 196.
constexpr std::array<Real, 4> kSineRe = {Real{1}, Real{0}, Real{-1}, Real{0}};
constexpr std::array<Real, 4> kSineIm = {Real{0}, Real{1}, Real{0}, Real{-1}};

// Exponents of 2 outside this range come only from streams that are not
// audio: real envelopes span a few hundred dB at most. Clamping keeps every
// value the adjuster computes finite (src/ac4dec/ERRATA.md, "Scale factors
// far out of range").
constexpr Energy kMinExponent{-96};
constexpr Energy kMaxExponent{96};

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

// 2^exponent through dsp::exp2_of: at double that is std::exp2, as it always
// was, and at float the project's own function, which gives the same float on
// every platform where the C libraries' exp2f differ in the last bit
// (planning/ac4.md, D14a4).
[[nodiscard]] Energy exp2_clamped(Energy exponent) noexcept {
    return dsp::exp2_of(std::clamp(exponent, kMinExponent, kMaxExponent));
}

// One assembled value of Pseudocode 107: the gain on the high band, the noise at
// its level and the tone at its. At Fixed32 each level is applied to the value
// by the 64-bit product of dsp::apply_gain, and the noise is ASPX_NOISE in Q7.24
// (tables/qmf_tables_fixed.hpp).
template <typename R>
[[nodiscard]] dsp::Complex<R> assembled(const dsp::Energy<R>& sig_gain, dsp::Complex<R> high,
                                        const dsp::Energy<R>& noise_level, int noise_index,
                                        const dsp::Energy<R>& sine_level, R sign,
                                        int sine_index) noexcept {
    const auto i = static_cast<std::size_t>(noise_index);
    const auto t = static_cast<std::size_t>(sine_index);
    if constexpr (dsp::kFixed<R>) {
        const auto& noise = tables::kAspxNoiseQ24[i];
        const dsp::Complex<R> noise_value(R::from_raw(noise[0]), R::from_raw(noise[1]));
        const R tone = dsp::from_energy<R>(sine_level);
        return dsp::apply_gain<R>(sig_gain, high) + dsp::apply_gain<R>(noise_level, noise_value) +
               dsp::Complex<R>(tone * kSineRe[t], tone * sign * kSineIm[t]);
    } else {
        const auto& noise = tables::kAspxNoise[i];
        const dsp::Complex<R> noise_value(static_cast<R>(noise[0]), static_cast<R>(noise[1]));
        return sig_gain * high + noise_level * noise_value +
               dsp::Complex<R>(sine_level * kSineRe[t], sine_level * sign * kSineIm[t]);
    }
}

using SigQscf = std::array<std::array<int, aspx::kMaxSbgMaster>, kMaxEnv>;
using NoiseQscf = std::array<std::array<int, aspx::kMaxSbgNoise>, kMaxNoiseEnv>;

struct Envelopes {
    SigQscf qscf_sig{};
    NoiseQscf qscf_noise{};
    std::array<std::array<Energy, aspx::kMaxSbgMaster>, kMaxEnv> scf_sig{};
    std::array<std::array<Energy, aspx::kMaxSbgNoise>, kMaxNoiseEnv> scf_noise{};
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
    const Energy a = c.qmode_env == 0 ? Energy{2} : Energy{1};
    const AspxFraming& f = c.framing;
    for (int atsg = 0; atsg < f.num_env; ++atsg) {
        const int num = signal_groups(g, f.atsg_freqres[at(atsg)]);
        const auto& q = e.qscf_sig[at(atsg)];
        auto& scf = e.scf_sig[at(atsg)];
        for (int sbg = 0; sbg < num; ++sbg) {
            scf[at(sbg)] = dsp::qmf_energy<Real>(Energy(64) * exp2_clamped(static_cast<Energy>(q[at(sbg)]) / a));
        }
        if (c.sig[at(atsg)].delta_dir == 0 && num > 1 && q[0] == 0 && q[1] < 0) {
            scf[0] = scf[1];
        }
    }
    for (int atsg = 0; atsg < f.num_noise; ++atsg) {
        for (int sbg = 0; sbg < g.num_sbg_noise; ++sbg) {
            e.scf_noise[at(atsg)][at(sbg)] =
                exp2_clamped(kNoiseFloorOffset - static_cast<Energy>(e.qscf_noise[at(atsg)][at(sbg)]));
        }
    }
}

// Pseudocode 84: channel 0 carries the sum, channel 1 the balance, with
// channel 0's framing and aspx_qmode_env.
void dequantise_balance(const AspxChannel& c, const aspx::SubbandGroups& g, Envelopes& sum,
                        Envelopes& balance) {
    const Energy a = c.qmode_env == 0 ? Energy{2} : Energy{1};
    const AspxFraming& f = c.framing;
    for (int atsg = 0; atsg < f.num_env; ++atsg) {
        for (int sbg = 0; sbg < signal_groups(g, f.atsg_freqres[at(atsg)]); ++sbg) {
            const Energy qa = static_cast<Energy>(sum.qscf_sig[at(atsg)][at(sbg)]) / a;
            const Energy qb = static_cast<Energy>(balance.qscf_sig[at(atsg)][at(sbg)]) / a;
            const Energy nom = dsp::qmf_energy<Real>(exp2_clamped(qa + Energy{1}) * Energy(64));
            sum.scf_sig[at(atsg)][at(sbg)] = nom / (Energy{1} + exp2_clamped(kPanOffset - qb));
            balance.scf_sig[at(atsg)][at(sbg)] = nom / (Energy{1} + exp2_clamped(qb - kPanOffset));
        }
    }
    for (int atsg = 0; atsg < f.num_noise; ++atsg) {
        for (int sbg = 0; sbg < g.num_sbg_noise; ++sbg) {
            const Energy qa = static_cast<Energy>(sum.qscf_noise[at(atsg)][at(sbg)]);
            const Energy qb = static_cast<Energy>(balance.qscf_noise[at(atsg)][at(sbg)]);
            const Energy nom = exp2_clamped(kNoiseFloorOffset - qa + Energy{1});
            sum.scf_noise[at(atsg)][at(sbg)] = nom / (Energy{1} + exp2_clamped(kPanOffset - qb));
            balance.scf_noise[at(atsg)][at(sbg)] = nom / (Energy{1} + exp2_clamped(qb - kPanOffset));
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
using EnvelopeMatrix = AspxScratch::EnvelopeMatrix;

// Pseudocodes 90 to 108 and clause 5.7.6.5.3 for one channel whose
// envelopes are decoded.
class ChannelAssembly {
   public:
    ChannelAssembly(const AspxFrame& frame, const aspx::SubbandGroups& groups,
                    const aspx::PatchTables& patches, AspxChannelIo& io, const Envelopes& envelopes,
                    AspxScratch& scratch)
        : frame_(frame),
          g_(groups),
          p_(patches),
          io_(io),
          c_(*io.data),
          f_(io.data->framing),
          st_(*io.state),
          e_(envelopes),
          est_sig_(scratch.est_sig),
          scf_sig_(scratch.scf_sig),
          scf_noise_(scratch.scf_noise),
          sine_idx_(scratch.sine_idx),
          sine_area_(scratch.sine_area),
          sine_lev_(scratch.sine_lev),
          noise_lev_(scratch.noise_lev),
          sig_gain_(scratch.sig_gain) {
        // Each channel starts from zeros.
        est_sig_ = {};
        scf_sig_ = {};
        scf_noise_ = {};
        sine_idx_ = {};
        sine_area_ = {};
        sine_lev_ = {};
        noise_lev_ = {};
        sig_gain_ = {};
    }

    void run(std::vector<QmfValue>& q_high);

   private:
    void estimate(std::span<const QmfValue> q_high);
    void map_scale_factors();
    void place_sinusoids();
    void compute_gains();
    void limit();
    void assemble(std::span<QmfValue> q_high);
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
    // The caller's scratch (AspxScratch), by the names the assembly uses.
    EnvelopeMatrix& est_sig_;
    EnvelopeMatrix& scf_sig_;
    EnvelopeMatrix& scf_noise_;
    AspxScratch::FlagMatrix& sine_idx_;
    AspxScratch::FlagMatrix& sine_area_;
    EnvelopeMatrix& sine_lev_;
    EnvelopeMatrix& noise_lev_;
    EnvelopeMatrix& sig_gain_;
};

void ChannelAssembly::run(std::vector<QmfValue>& q_high) {
    AC4_ZONE_SCOPED_N("ac4_aspx");
    const int q_low_slots = frame_.num_qmf_timeslots + frame_.ts_offset_hfgen;
    q_high.assign(at(q_low_slots) * kSubbands, QmfValue{});
    const aspx::HfGeneratorInput<Real> in{
        .q_low_ext = io_.ext,
        .num_qmf_timeslots = frame_.num_qmf_timeslots,
        .ts_offset_hfgen = frame_.ts_offset_hfgen,
        .ts_begin = ts_begin(),
        .ts_end = ts_end(),
        .preflat = frame_.config->preflat,
        .tna_mode = std::span<const std::uint8_t>(c_.tna_mode).first(at(g_.num_sbg_noise)),
    };
    aspx::generate_high_band<Real>(g_, p_, in, st_.hf, q_high);
    p_sine_at_end_ = st_.tsg_ptr_prev == st_.num_atsg_sig_prev ? 0 : -1;
    estimate(q_high);
    map_scale_factors();
    place_sinusoids();
    compute_gains();
    limit();
    // From here the buffer holds Y, Pseudocode 106's output, in place of the high band.
    assemble(q_high);
    interleave(q_high);
    keep(q_high);
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
        const Energy length = static_cast<Energy>(
            (f_.atsg_sig[at(atsg + 1)] - f_.atsg_sig[at(atsg)]) * frame_.num_ts_in_ats);
        int sbg = 0;
        for (int sb = 0; sb < g_.num_sb_aspx; ++sb) {
            if (sb + sbx == table[at(sbg + 1)]) {
                ++sbg;
            }
            Energy est{};
            if (!frame_.config->interpolation) {
                const int lo = table[at(sbg)];
                const int hi = table[at(sbg + 1)];
                for (int ts = tsa; ts < tsz; ++ts) {
                    for (int j = lo; j < hi; ++j) {
                        est += dsp::energy_of(q_high[at(ts) * kSubbands + at(j)]);
                    }
                }
                est /= static_cast<Energy>(hi - lo);
            } else {
                for (int ts = tsa; ts < tsz; ++ts) {
                    est += dsp::energy_of(q_high[at(ts) * kSubbands + at(sb + sbx)]);
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
    constexpr Energy kEpsilon = dsp::qmf_energy<Real>(Energy{1});
    for (int atsg = 0; atsg < f_.num_env; ++atsg) {
        for (int sb = 0; sb < g_.num_sb_aspx; ++sb) {
            const Energy scf_sig = scf_sig_[at(atsg)][at(sb)];
            const Energy scf_noise = scf_noise_[at(atsg)][at(sb)];
            const Energy sig_noise_fact = scf_sig / (Energy{1} + scf_noise);
            const Energy sine = sine_idx_[at(atsg)][at(sb)] ? Energy{1} : Energy{};
            sine_lev_[at(atsg)][at(sb)] = dsp::sqrt_of(sig_noise_fact * sine);
            noise_lev_[at(atsg)][at(sb)] = dsp::sqrt_of(sig_noise_fact * scf_noise);
            Energy denom = kEpsilon + est_sig_[at(atsg)][at(sb)];
            if (!sine_area_[at(atsg)][at(sb)]) {
                if (!transient_envelope(atsg)) {
                    denom *= Energy{1} + scf_noise;
                }
                sig_gain_[at(atsg)][at(sb)] = dsp::sqrt_of(scf_sig / denom);
            } else {
                denom *= Energy{1} + scf_noise;
                sig_gain_[at(atsg)][at(sb)] = dsp::sqrt_of(scf_sig * scf_noise / denom);
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
        std::array<Energy, aspx::kMaxSbgLim> nom{};
        std::array<Energy, aspx::kMaxSbgLim> denom{};
        denom.fill(kEpsilon0);
        for (int sb = 0; sb < g_.num_sb_aspx; ++sb) {
            nom[at(group[at(sb)])] += scf[at(sb)];
            denom[at(group[at(sb)])] += est[at(sb)];
        }
        std::array<Energy, kSubbands> max_gain{};
        for (int sb = 0; sb < g_.num_sb_aspx; ++sb) {
            const auto k = at(group[at(sb)]);
            max_gain[at(sb)] = std::min(dsp::sqrt_of(nom[k] / denom[k]) * kLimGain, kMaxSigGain);
        }
        // Pseudocodes 97 and 98.
        for (int sb = 0; sb < g_.num_sb_aspx; ++sb) {
            if (gain[at(sb)] > Energy{}) {
                noise[at(sb)] =
                    std::min(noise[at(sb)], noise[at(sb)] * max_gain[at(sb)] / gain[at(sb)]);
            }
            gain[at(sb)] = std::min(gain[at(sb)], max_gain[at(sb)]);
        }
        // Pseudocode 99.
        std::array<Energy, aspx::kMaxSbgLim> boost_nom{};
        std::array<Energy, aspx::kMaxSbgLim> boost_denom{};
        boost_nom.fill(kEpsilon0);
        boost_denom.fill(kEpsilon0);
        for (int sb = 0; sb < g_.num_sb_aspx; ++sb) {
            const auto k = at(group[at(sb)]);
            boost_nom[k] += scf[at(sb)];
            boost_denom[k] +=
                est[at(sb)] * gain[at(sb)] * gain[at(sb)] + sine[at(sb)] * sine[at(sb)];
            if (!(sine[at(sb)] != Energy{} || transient_envelope(atsg))) {
                boost_denom[k] += noise[at(sb)] * noise[at(sb)];
            }
        }
        // Pseudocodes 100 and 101.
        for (int sb = 0; sb < g_.num_sb_aspx; ++sb) {
            const auto k = at(group[at(sb)]);
            const Energy boost = std::min(dsp::sqrt_of(boost_nom[k] / boost_denom[k]), kMaxBoostFact);
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
// In place: `buffer` holds the high band and leaves holding Y, which is zero wherever
// Pseudocode 106 writes nothing (each assembled value reads the high band only where it is
// written, and nothing reads the high band after this).
void ChannelAssembly::assemble(std::span<QmfValue> buffer) {
    const int sbx = g_.sbx;
    const int nsb = g_.num_sb_aspx;
    const int first = ts_begin();
    const int last = ts_end();
    const auto slot_of = [buffer](int ts) { return buffer.subspan(at(ts) * kSubbands, kSubbands); };
    // Pseudocode 106: the slots before this interval are the last one's.
    for (int ts = 0; ts < first; ++ts) {
        const std::span<QmfValue> slot = slot_of(ts);
        if (ts < st_.y_prev_slots) {
            std::copy_n(st_.y_prev.begin() + static_cast<std::ptrdiff_t>(at(ts) * kSubbands),
                        kSubbands, slot.begin());
        } else {
            std::ranges::fill(slot, QmfValue{});
        }
    }
    for (int ts = std::max(first, last); ts < static_cast<int>(buffer.size() / kSubbands); ++ts) {
        std::ranges::fill(slot_of(ts), QmfValue{});
    }
    const int noise_base = frame_.master_reset ? 0 : st_.noise_index;
    const int sine_base = st_.first_frame ? 1 : (st_.sine_index + 1) % 4;
    int atsg = 0;
    int noise_index = st_.noise_index;
    int sine_index = st_.sine_index;
    for (int ts = first; ts < last; ++ts) {
        while (atsg + 1 < f_.num_env && ts >= f_.atsg_sig[at(atsg + 1)] * frame_.num_ts_in_ats) {
            ++atsg;
        }
        sine_index = (sine_base + ts - first) % 4;
        const std::span<QmfValue> slot = slot_of(ts);
        for (int sb = 0; sb < nsb; ++sb) {
            noise_index = (noise_base + nsb * (ts - first) + sb + 1) % 512;
            const Real sign = (sb + sbx) % 2 == 0 ? Real{1} : Real{-1};
            slot[at(sb + sbx)] = assembled<Real>(sig_gain_[at(atsg)][at(sb)], slot[at(sb + sbx)],
                                                 noise_lev_[at(atsg)][at(sb)], noise_index,
                                                 sine_lev_[at(atsg)][at(sb)], sign, sine_index);
        }
        std::fill_n(slot.begin(), sbx, QmfValue{});
        std::fill(slot.begin() + sbx + nsb, slot.end(), QmfValue{});
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
    const auto scratch = std::make_unique<AspxScratch>();
    return decode_aspx(frame, channels, *scratch);
}

ParseResult decode_aspx(const AspxFrame& frame, std::span<AspxChannelIo> channels,
                        AspxScratch& scratch) {
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
    // The high band of Q_low's slots, generated and then assembled in place into Y (Pseudocode
    // 106 reads each value only where it writes it): 19 KB at a 2048-sample frame at the float
    // and fixed tiers, one buffer for the element's channels, freed when they are done.
    std::vector<QmfValue> high;
    for (std::size_t c = 0; c < channels.size(); ++c) {
        ChannelAssembly(frame, groups, patches, channels[c], envelopes[c], scratch).run(high);
        keep_envelopes(*channels[c].data, groups, envelopes[c], *channels[c].state);
    }
    return {};
}

}  // namespace iclforge::ac4::detail

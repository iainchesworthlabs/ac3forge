#include "ajoc/ajoc.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ac4::detail::ajoc {
namespace {

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

constexpr std::array<int, 8> kNumBands = {23, 15, 12, 9, 7, 5, 3, 1};

// Table 28 by rows: each row's last subband and its band for 23, 15, 12, 9,
// 7, 5, 3 and 1 bands.
struct MappingRow {
    int last_subband;
    std::array<std::uint8_t, 8> band;
};
// clang-format off
constexpr std::array<MappingRow, 23> kMapping = {{
    {0, {0, 0, 0, 0, 0, 0, 0, 0}},
    {1, {1, 1, 1, 1, 1, 1, 0, 0}},
    {2, {2, 2, 2, 2, 2, 1, 0, 0}},
    {3, {3, 3, 3, 3, 2, 2, 1, 0}},
    {4, {4, 4, 4, 3, 3, 2, 1, 0}},
    {5, {5, 5, 4, 4, 3, 2, 1, 0}},
    {6, {6, 6, 5, 4, 3, 2, 1, 0}},
    {7, {7, 7, 5, 5, 3, 2, 1, 0}},
    {8, {8, 8, 6, 5, 4, 2, 1, 0}},
    {9, {9, 9, 6, 6, 4, 3, 1, 0}},
    {10, {10, 9, 6, 6, 4, 3, 1, 0}},
    {11, {11, 10, 7, 6, 4, 3, 1, 0}},
    {13, {12, 10, 7, 6, 4, 3, 1, 0}},
    {15, {13, 11, 8, 7, 5, 3, 2, 0}},
    {17, {14, 11, 8, 7, 5, 3, 2, 0}},
    {19, {15, 12, 9, 7, 5, 3, 2, 0}},
    {22, {16, 12, 9, 7, 5, 3, 2, 0}},
    {25, {17, 13, 10, 8, 6, 4, 2, 0}},
    {29, {18, 13, 10, 8, 6, 4, 2, 0}},
    {34, {19, 13, 10, 8, 6, 4, 2, 0}},
    {40, {20, 14, 11, 8, 6, 4, 2, 0}},
    {47, {21, 14, 11, 8, 6, 4, 2, 0}},
    {63, {22, 14, 11, 8, 6, 4, 2, 0}},
}};
// clang-format on

// Tables 29 to 32's steps.
constexpr double kCoarseStep = 0.2001953125;
constexpr double kFineStep = 0.10009765625;

// 5.7.3.5's cyclic use of A-CPL's three decorrelators.
constexpr std::array<int, kMaxDecorrelators> kDecorrelatorOf = {0, 2, 1, 0, 2, 1, 0};

}  // namespace

int num_bands(int num_bands_code) noexcept {
    return num_bands_code >= 0 && num_bands_code < 8 ? kNumBands[at(num_bands_code)] : 0;
}

int sb_to_pb(int num_bands, int sb) noexcept {
    const auto column = std::ranges::find(kNumBands, num_bands);
    if (column == kNumBands.end()) {
        return 0;
    }
    const auto c = static_cast<std::size_t>(column - kNumBands.begin());
    for (const MappingRow& row : kMapping) {
        if (sb <= row.last_subband) {
            return row.band[c];
        }
    }
    return 0;
}

int nquant(bool wet, int quant_select) noexcept {
    if (wet) {
        return quant_select == 1 ? 21 : 41;
    }
    return quant_select == 1 ? 51 : 101;
}

double dequantise(bool wet, int quant_select, int q) noexcept {
    const int centre = (nquant(wet, quant_select) - 1) / 2;
    return static_cast<double>(q - centre) * (quant_select == 1 ? kCoarseStep : kFineStep);
}

bool differential_decode(bool diff_time, int nquant, int num_bands, std::span<const int> values,
                         std::span<const int, kMaxBands> previous,
                         std::span<int, kMaxBands> out) noexcept {
    if (num_bands <= 0 || num_bands > kMaxBands || values.size() < at(num_bands) || nquant <= 0) {
        return false;
    }
    for (int pb = 0; pb < num_bands; ++pb) {
        const int v = values[at(pb)];
        int q = 0;
        if (diff_time) {
            q = previous[at(pb)] + v;
            if (q < 0 || q >= nquant) {
                return false;
            }
        } else if (pb == 0) {
            q = v;
            if (q < 0 || q >= nquant) {
                return false;
            }
        } else {
            q = ((out[at(pb - 1)] + v) % nquant + nquant) % nquant;
        }
        out[at(pb)] = q;
    }
    return true;
}

int decorrelator_of(int de) noexcept {
    return de >= 0 && de < kMaxDecorrelators ? kDecorrelatorOf[at(de)] : 0;
}

void FrameParameters::resize(int dmx, int umx) {
    num_dmx = dmx;
    num_umx = umx;
    num_bands.assign(at(umx), 1);
    dry.assign(at(umx) * kMaxDataPoints * at(dmx) * kMaxBands, 0.0);
    wet.assign(at(umx) * kMaxDataPoints * kMaxDecorrelators * kMaxBands, 0.0);
}

double& FrameParameters::dry_at(int o, int dp, int ch, int pb) {
    return dry[((at(o) * kMaxDataPoints + at(dp)) * at(num_dmx) + at(ch)) * kMaxBands + at(pb)];
}

double& FrameParameters::wet_at(int o, int dp, int de, int pb) {
    return wet[((at(o) * kMaxDataPoints + at(dp)) * kMaxDecorrelators + at(de)) * kMaxBands +
               at(pb)];
}

double FrameParameters::dry_at(int o, int dp, int ch, int pb) const {
    return dry[((at(o) * kMaxDataPoints + at(dp)) * at(num_dmx) + at(ch)) * kMaxBands + at(pb)];
}

double FrameParameters::wet_at(int o, int dp, int de, int pb) const {
    return wet[((at(o) * kMaxDataPoints + at(dp)) * kMaxDecorrelators + at(de)) * kMaxBands +
               at(pb)];
}

template <typename Real>
void Reconstruction<Real>::Ramped::resize(std::size_t n) {
    prev.assign(n, Real{0});
    delta.assign(n, Real{0});
}

template <typename Real>
Reconstruction<Real>::Reconstruction()
    : decorrelators_{acpl::Decorrelator<Real>(kDecorrelatorOf[0]),
                     acpl::Decorrelator<Real>(kDecorrelatorOf[1]),
                     acpl::Decorrelator<Real>(kDecorrelatorOf[2]),
                     acpl::Decorrelator<Real>(kDecorrelatorOf[3]),
                     acpl::Decorrelator<Real>(kDecorrelatorOf[4]),
                     acpl::Decorrelator<Real>(kDecorrelatorOf[5]),
                     acpl::Decorrelator<Real>(kDecorrelatorOf[6])} {}

template <typename Real>
void Reconstruction<Real>::reset() {
    num_dmx_ = -1;
    num_umx_ = -1;
    curr_ramp_len_ = 0;
    target_ramp_len_ = 0;
    for (std::size_t de = 0; de < decorrelators_.size(); ++de) {
        decorrelators_[de].reset();
        duckers_[de].reset();
        ran_[de] = false;
    }
    h_m_prev_.clear();
}

template <typename Real>
void Reconstruction<Real>::configure(const FrameParameters& p) {
    if (p.num_dmx == num_dmx_ && p.num_umx == num_umx_) {
        return;
    }
    reset();
    num_dmx_ = p.num_dmx;
    num_umx_ = p.num_umx;
    const std::size_t m = at(num_dmx_);
    const std::size_t n = at(num_umx_);
    dry_.resize(at(kSubbands) * n * m);
    wet_.resize(at(kSubbands) * n * kMaxDecorrelators);
    pre_.resize(at(kSubbands) * kMaxDecorrelators * m);
    h_m_prev_.assign(n * m, Real{0});
    h_m_.assign(n * m, Real{0});
}

template <typename Real>
typename Reconstruction<Real>::Schedule Reconstruction<Real>::schedule(const FrameParameters& p,
                                                                       int num_ts) {
    // Pseudocode 18's counter, once per slot (src/ac4dec/ERRATA.md, "A-JOC's
    // ramp"): the ramp moves while the counter is below its length, and a data
    // point starting at the slot restarts it after the slot.
    Schedule s;
    int curr = curr_ramp_len_;
    int target = target_ramp_len_;
    for (int ts = 0; ts < num_ts && ts < kMaxSlots; ++ts) {
        s.moves[at(ts)] = curr < target;
        int start = -1;
        for (int dp = 0; dp < p.num_dpoints; ++dp) {
            if (ts == p.start_pos[at(dp)]) {
                start = dp;
            }
        }
        s.starts[at(ts)] = start;
        if (s.moves[at(ts)]) {
            ++curr;
        }
        if (start >= 0) {
            curr = 0;
            target = p.ramp_len[at(start)];
        }
    }
    curr_ramp_len_ = curr;
    target_ramp_len_ = target;
    return s;
}

template <typename Real>
void Reconstruction<Real>::reconstruct(const FrameParameters& p, int num_ts,
                                       std::span<const std::vector<Complex>* const> x,
                                       std::span<std::vector<Complex>* const> z, double de_gain,
                                       std::span<const std::uint8_t> dialogue) {
    configure(p);
    const int m = num_dmx_;
    const int n = num_umx_;
    const std::size_t slots = at(std::clamp(num_ts, 0, kMaxSlots)) * at(kSubbands);
    if (x.size() < at(m) || z.size() < at(n)) {
        return;
    }
    for (int o = 0; o < n; ++o) {
        z[at(o)]->assign(slots, Complex{});
    }
    const int decorr = std::clamp(p.num_decorr, 0, kMaxDecorrelators);

    // The decorrelation input matrix's parameters, D = |C_wet^T| C_dry per data
    // point, from the coefficients before dialogue enhancement; subband by
    // subband, each object's coefficients at its own band (src/ac4dec/
    // ERRATA.md, "The decorrelation input matrix").
    pre_param_.assign(at(p.num_dpoints) * at(kSubbands) * kMaxDecorrelators * at(m), 0.0);
    for (int dp = 0; dp < p.num_dpoints; ++dp) {
        for (int sb = 0; sb < kSubbands; ++sb) {
            for (int o = 0; o < n; ++o) {
                const int pb = sb_to_pb(p.num_bands[at(o)], sb);
                for (int de = 0; de < decorr; ++de) {
                    const double wet = std::abs(p.wet_at(o, dp, de, pb));
                    if (wet == 0.0) {
                        continue;
                    }
                    for (int ch = 0; ch < m; ++ch) {
                        pre_param_[((at(dp) * kSubbands + at(sb)) * kMaxDecorrelators + at(de)) *
                                       at(m) +
                                   at(ch)] += wet * p.dry_at(o, dp, ch, pb);
                    }
                }
            }
        }
    }

    // Pseudocode 22: the dialogue objects' coefficients times de_gain.
    const FrameParameters* params = &p;
    if (de_gain > 1.0 && std::ranges::any_of(dialogue, [](std::uint8_t d) { return d != 0; })) {
        scaled_ = p;
        FrameParameters& scaled = scaled_;
        for (int o = 0; o < n && at(o) < dialogue.size(); ++o) {
            if (dialogue[at(o)] == 0) {
                continue;
            }
            for (int dp = 0; dp < kMaxDataPoints; ++dp) {
                for (int pb = 0; pb < kMaxBands; ++pb) {
                    for (int ch = 0; ch < m; ++ch) {
                        scaled.dry_at(o, dp, ch, pb) *= de_gain;
                    }
                    for (int de = 0; de < kMaxDecorrelators; ++de) {
                        scaled.wet_at(o, dp, de, pb) *= de_gain;
                    }
                }
            }
        }
        params = &scaled_;
    }

    const Schedule s = schedule(p, num_ts);
    const auto step = [&p](Real& prev, Real& delta, bool moves, int start, double target) {
        const Real v = moves ? prev + delta : prev;
        prev = v;
        if (start >= 0) {
            delta = (static_cast<Real>(target) - v) / static_cast<Real>(p.ramp_len[at(start)]);
        }
        return v;
    };

    // The decorrelators' inputs, u = D x, slot by slot.
    for (int de = 0; de < decorr; ++de) {
        u_[at(de)].assign(slots, Complex{});
    }
    for (int ts = 0; ts < num_ts && ts < kMaxSlots; ++ts) {
        const bool moves = s.moves[at(ts)];
        const int start = s.starts[at(ts)];
        for (int sb = 0; sb < kSubbands; ++sb) {
            const std::size_t k = at(ts) * kSubbands + at(sb);
            for (int de = 0; de < decorr; ++de) {
                Complex u{};
                for (int ch = 0; ch < m; ++ch) {
                    const std::size_t i = (at(sb) * kMaxDecorrelators + at(de)) * at(m) + at(ch);
                    const double target =
                        start >= 0
                            ? pre_param_[((at(start) * kSubbands + at(sb)) * kMaxDecorrelators +
                                          at(de)) *
                                             at(m) +
                                         at(ch)]
                            : 0.0;
                    const Real d = step(pre_.prev[i], pre_.delta[i], moves, start, target);
                    u += d * (*x[at(ch)])[k];
                }
                u_[at(de)][k] = u;
            }
        }
    }
    // The decorrelators and their duckers (5.7.3.5); one not enabled, or not
    // used this frame, gives silence and starts afresh when next used.
    for (int de = 0; de < kMaxDecorrelators; ++de) {
        std::vector<Complex>& y = y_[at(de)];
        y.assign(slots, Complex{});
        const bool used = de < decorr && p.decorr_enable[at(de)];
        if (!used) {
            if (ran_[at(de)]) {
                decorrelators_[at(de)].reset();
                duckers_[at(de)].reset();
                ran_[at(de)] = false;
            }
            continue;
        }
        decorrelators_[at(de)].process(u_[at(de)], y, num_ts);
        duckers_[at(de)].process(y, num_ts);
        ran_[at(de)] = true;
    }

    // z = C_dry x + C_wet y, the coefficients interpolated slot by slot.
    for (int ts = 0; ts < num_ts && ts < kMaxSlots; ++ts) {
        const bool moves = s.moves[at(ts)];
        const int start = s.starts[at(ts)];
        for (int sb = 0; sb < kSubbands; ++sb) {
            const std::size_t k = at(ts) * kSubbands + at(sb);
            for (int o = 0; o < n; ++o) {
                const int pb = sb_to_pb(params->num_bands[at(o)], sb);
                Complex acc{};
                for (int ch = 0; ch < m; ++ch) {
                    const std::size_t i = (at(sb) * at(n) + at(o)) * at(m) + at(ch);
                    const double target = start >= 0 ? params->dry_at(o, start, ch, pb) : 0.0;
                    const Real c = step(dry_.prev[i], dry_.delta[i], moves, start, target);
                    acc += c * (*x[at(ch)])[k];
                }
                for (int de = 0; de < decorr; ++de) {
                    const std::size_t i = (at(sb) * at(n) + at(o)) * kMaxDecorrelators + at(de);
                    const double target = start >= 0 ? params->wet_at(o, start, de, pb) : 0.0;
                    const Real c = step(wet_.prev[i], wet_.delta[i], moves, start, target);
                    acc += c * y_[at(de)][k];
                }
                (*z[at(o)])[k] = acc;
            }
        }
    }
}

template <typename Real>
void Reconstruction<Real>::enhance_core(const FrameParameters& p, int num_ts,
                                        std::span<std::vector<Complex>* const> x, double de_gain,
                                        std::span<const std::uint8_t> dialogue,
                                        std::span<const double> coeff) {
    configure(p);
    const int m = num_dmx_;
    const int n = num_umx_;
    if (x.size() < at(m)) {
        return;
    }
    // H'_M by upmix object: the dialogue objects' downmix coefficients, 0
    // for the others (src/ac4dec/ERRATA.md, "Core decoding's H_M").
    std::ranges::fill(h_m_, Real{0});
    int dlg = 0;
    for (int o = 0; o < n && at(o) < dialogue.size(); ++o) {
        if (dialogue[at(o)] == 0) {
            continue;
        }
        for (int ch = 0; ch < m; ++ch) {
            const std::size_t c = at(dlg) * at(m) + at(ch);
            h_m_[at(o) * at(m) + at(ch)] = c < coeff.size() ? static_cast<Real>(coeff[c]) : Real{0};
        }
        ++dlg;
    }
    const Schedule s = schedule(p, num_ts);
    std::array<Complex, 64> a{};  // H_A x per upmix object, at most 64 of them
    for (int ts = 0; ts < num_ts && ts < kMaxSlots; ++ts) {
        const bool moves = s.moves[at(ts)];
        const int start = s.starts[at(ts)];
        const Real alpha = static_cast<Real>(ts + 1) / static_cast<Real>(num_ts);
        for (int sb = 0; sb < kSubbands; ++sb) {
            const std::size_t k = at(ts) * kSubbands + at(sb);
            for (int o = 0; o < n; ++o) {
                const int pb = sb_to_pb(p.num_bands[at(o)], sb);
                const bool is_dialogue = at(o) < dialogue.size() && dialogue[at(o)] != 0;
                // Pseudocode 22 with core decoding's de_gain, on the dialogue
                // objects' coefficients; every object's ramp moves on.
                const double gain = is_dialogue ? de_gain : 1.0;
                Complex acc{};
                for (int ch = 0; ch < m; ++ch) {
                    const std::size_t i = (at(sb) * at(n) + at(o)) * at(m) + at(ch);
                    const double target = start >= 0 ? gain * p.dry_at(o, start, ch, pb) : 0.0;
                    Real& prev = dry_.prev[i];
                    Real& delta = dry_.delta[i];
                    const Real v = moves ? prev + delta : prev;
                    prev = v;
                    if (start >= 0) {
                        delta = (static_cast<Real>(target) - v) /
                                static_cast<Real>(p.ramp_len[at(start)]);
                    }
                    acc += v * (*x[at(ch)])[k];
                }
                if (at(o) < a.size()) {
                    a[at(o)] = is_dialogue ? acc : Complex{};
                }
            }
            for (int ch = 0; ch < m; ++ch) {
                Complex add{};
                for (int o = 0; o < n && at(o) < a.size(); ++o) {
                    const std::size_t i = at(o) * at(m) + at(ch);
                    const Real h = (Real{1} - alpha) * h_m_prev_[i] + alpha * h_m_[i];
                    add += h * a[at(o)];
                }
                (*x[at(ch)])[k] += add;
            }
        }
    }
    h_m_prev_ = h_m_;
}

template class Reconstruction<double>;

}  // namespace ac4::detail::ajoc

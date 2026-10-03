#include "ajoc/ajoc_encoder.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

#include "bit_writer.hpp"

namespace iclforge::ac4::detail {
namespace {

using Complex = dsp::Complex<double>;

constexpr int kFrameSlots = 32;
// The centred window's lead past the frame's slots.
constexpr int kLookahead = 16;
constexpr int kSubbands = dsp::kQmfSubbands;

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

// Solves (a + eps I) h = b in place for the m-by-m symmetric matrix `a`
// (row-major), by elimination with partial pivoting; `b` becomes h.
void solve(std::vector<double>& a, std::vector<double>& b, int m) {
    double trace = 0.0;
    for (int i = 0; i < m; ++i) {
        trace += a[at(i * m + i)];
    }
    const double eps = 1e-9 * trace / std::max(m, 1) + 1e-30;
    for (int i = 0; i < m; ++i) {
        a[at(i * m + i)] += eps;
    }
    for (int col = 0; col < m; ++col) {
        int pivot = col;
        for (int row = col + 1; row < m; ++row) {
            if (std::abs(a[at(row * m + col)]) > std::abs(a[at(pivot * m + col)])) {
                pivot = row;
            }
        }
        if (pivot != col) {
            for (int k = 0; k < m; ++k) {
                std::swap(a[at(col * m + k)], a[at(pivot * m + k)]);
            }
            std::swap(b[at(col)], b[at(pivot)]);
        }
        const double d = a[at(col * m + col)];
        if (d == 0.0) {
            continue;
        }
        for (int row = col + 1; row < m; ++row) {
            const double f = a[at(row * m + col)] / d;
            if (f == 0.0) {
                continue;
            }
            for (int k = col; k < m; ++k) {
                a[at(row * m + k)] -= f * a[at(col * m + k)];
            }
            b[at(row)] -= f * b[at(col)];
        }
    }
    for (int row = m - 1; row >= 0; --row) {
        double s = b[at(row)];
        for (int k = row + 1; k < m; ++k) {
            s -= a[at(row * m + k)] * b[at(k)];
        }
        const double d = a[at(row * m + row)];
        b[at(row)] = d != 0.0 ? s / d : 0.0;
    }
}

}  // namespace

AjocEncoder::AjocEncoder(const AjocSetup& setup, const FrameTiming& timing)
    : setup_(setup),
      timing_(timing),
      dmx_analyses_(at(setup.num_dmx)),
      object_analyses_(at(setup.num_umx)),
      x_(at(setup.num_dmx)),
      z_(at(setup.num_umx)) {}

void AjocEncoder::push_slot(std::span<const std::array<double, dsp::kQmfSubbands>> dmx,
                            std::span<const std::array<double, dsp::kQmfSubbands>> objects) {
    std::vector<Slot>& analysed = slots_.emplace_back(at(setup_.num_dmx + setup_.num_umx));
    for (std::size_t c = 0; c < dmx_analyses_.size(); ++c) {
        dmx_analyses_[c].process(dmx[c], analysed[c]);
    }
    for (std::size_t o = 0; o < object_analyses_.size(); ++o) {
        object_analyses_[o].process(objects[o], analysed[dmx_analyses_.size() + o]);
    }
}

long long AjocEncoder::first_slot(long long frame) const noexcept {
    return static_cast<long long>(timing_.qmf_slots) * (frame + timing_.control_delay) -
           timing_.hfgen_slots;
}

long long AjocEncoder::slots_needed(long long frame) const noexcept {
    return first_slot(frame) + kFrameSlots + kLookahead;
}

int AjocEncoder::bands() const noexcept {
    return ajoc::num_bands(setup_.num_bands_code);
}

int AjocEncoder::centre(bool wet) const noexcept {
    return (ajoc::nquant(wet, setup_.quant_select) - 1) / 2;
}

double AjocEncoder::step_value(bool wet, int q) const noexcept {
    return ajoc::dequantise(wet, setup_.quant_select, q);
}

int AjocEncoder::quantise(bool wet, double v) const noexcept {
    const int n = ajoc::nquant(wet, setup_.quant_select);
    const double step = step_value(wet, centre(wet) + 1) - step_value(wet, centre(wet));
    const int guess = std::clamp(centre(wet) + static_cast<int>(std::lround(v / step)), 0, n - 1);
    int best = guess;
    for (const int q : {guess - 1, guess + 1}) {
        if (q >= 0 && q < n && std::abs(step_value(wet, q) - v) < std::abs(step_value(wet, best) - v)) {
            best = q;
        }
    }
    return best;
}

AjocEncoder::Values AjocEncoder::held_values() const {
    if (held_valid_) {
        return held_;
    }
    Values v;
    std::array<int, ajoc::kMaxBands> dry{};
    std::array<int, ajoc::kMaxBands> wet{};
    dry.fill(centre(false));
    wet.fill(centre(true));
    v.dry.assign(at(setup_.num_umx), std::vector(at(setup_.num_dmx), dry));
    v.wet.assign(at(setup_.num_umx), std::vector(at(setup_.num_decorr), wet));
    return v;
}

bool AjocEncoder::present(const Values& v, std::size_t o) const {
    const int b = bands();
    for (const auto& set : v.dry[o]) {
        for (int pb = 0; pb < b; ++pb) {
            if (set[at(pb)] != centre(false)) {
                return true;
            }
        }
    }
    for (const auto& set : v.wet[o]) {
        for (int pb = 0; pb < b; ++pb) {
            if (set[at(pb)] != centre(true)) {
                return true;
            }
        }
    }
    return false;
}

AjocFields AjocEncoder::fields_of(const Values& v, bool iframe) const {
    const int b = bands();
    const int quant = setup_.quant_select;
    AjocFields f;
    f.decorr_enable.assign(at(setup_.num_decorr), 1);
    f.num_dpoints = 1;
    f.start_pos = {0, 0};
    f.ramp_len = {v.ramp, 1};

    // Each set along frequency, and along time where the decoder holds the
    // values it differs from; the frame sends diff_type where any set is
    // cheaper along time, counted with it.
    struct Coded {
        AjocSetFields freq;
        std::optional<AjocSetFields> time;
    };
    std::vector<std::vector<Coded>> dry(at(setup_.num_umx));
    std::vector<std::vector<Coded>> wet(at(setup_.num_umx));
    std::size_t nodt_bits = 0;
    std::size_t dt_bits = 0;
    const auto code = [&](bool is_wet, const std::array<int, ajoc::kMaxBands>& q,
                          const std::array<int, ajoc::kMaxBands>* previous) {
        Coded c;
        const std::span<const int> values(q.data(), at(b));
        c.freq = ajoc_freq_set(is_wet, quant, values);
        if (previous != nullptr) {
            c.time = ajoc_time_set(is_wet, quant, values, std::span<const int>(previous->data(), at(b)));
        }
        const std::size_t freq_bits = ajoc_set_bits(is_wet, quant, true, c.freq);
        nodt_bits += freq_bits;
        std::size_t best = freq_bits + 1;
        if (c.time) {
            best = std::min(best, ajoc_set_bits(is_wet, quant, false, *c.time));
        }
        dt_bits += best;
        return c;
    };
    const Values held = held_values();
    for (std::size_t o = 0; o < dry.size(); ++o) {
        if (!present(v, o)) {
            continue;
        }
        const bool timed = !iframe && held_valid_;
        for (std::size_t ch = 0; ch < v.dry[o].size(); ++ch) {
            dry[o].push_back(code(false, v.dry[o][ch], timed ? &held.dry[o][ch] : nullptr));
        }
        for (std::size_t d = 0; d < v.wet[o].size(); ++d) {
            wet[o].push_back(code(true, v.wet[o][d], timed ? &held.wet[o][d] : nullptr));
        }
    }
    f.b_nodt = iframe || nodt_bits <= dt_bits;
    const auto pick = [&](bool is_wet, const Coded& c) {
        if (f.b_nodt || !c.time) {
            return c.freq;
        }
        const std::size_t freq_bits = ajoc_set_bits(is_wet, quant, false, c.freq);
        return ajoc_set_bits(is_wet, quant, false, *c.time) < freq_bits ? *c.time : c.freq;
    };
    for (std::size_t o = 0; o < dry.size(); ++o) {
        AjocObjectFields object;
        object.present = present(v, o);
        object.num_bands_code = setup_.num_bands_code;
        object.quant_select = quant;
        if (!object.present) {
            f.objects.push_back(std::move(object));
            continue;
        }
        std::vector<AjocSetFields>& dry_sets = object.dry.emplace_back();
        std::vector<AjocSetFields>& wet_sets = object.wet.emplace_back();
        std::size_t all = 0;
        std::size_t sparse = at(setup_.num_dmx + setup_.num_decorr);
        const auto zero = [&](bool is_wet, const std::array<int, ajoc::kMaxBands>& q) {
            return std::all_of(q.begin(), q.begin() + b, [&](int value) { return value == centre(is_wet); });
        };
        for (std::size_t ch = 0; ch < dry[o].size(); ++ch) {
            dry_sets.push_back(pick(false, dry[o][ch]));
            const std::size_t bits = ajoc_set_bits(false, quant, f.b_nodt, dry_sets.back());
            const bool sent = !zero(false, v.dry[o][ch]);
            object.dry_present.push_back(sent ? 1 : 0);
            all += bits;
            sparse += sent ? bits : 0;
        }
        for (std::size_t d = 0; d < wet[o].size(); ++d) {
            wet_sets.push_back(pick(true, wet[o][d]));
            const std::size_t bits = ajoc_set_bits(true, quant, f.b_nodt, wet_sets.back());
            const bool sent = !zero(true, v.wet[o][d]);
            object.wet_present.push_back(sent ? 1 : 0);
            all += bits;
            sparse += sent ? bits : 0;
        }
        object.sparse = sparse < all;
        f.objects.push_back(std::move(object));
    }
    return f;
}

ajoc::FrameParameters AjocEncoder::parameters_of(const Values& v) const {
    ajoc::FrameParameters p;
    p.resize(setup_.num_dmx, setup_.num_umx);
    p.num_decorr = setup_.num_decorr;
    for (int d = 0; d < setup_.num_decorr; ++d) {
        p.decorr_enable[at(d)] = true;
    }
    p.num_dpoints = 1;
    p.start_pos = {0, 0};
    p.ramp_len = {v.ramp, 1};
    const int b = bands();
    for (int o = 0; o < setup_.num_umx; ++o) {
        if (!present(v, at(o))) {
            continue;
        }
        p.num_bands[at(o)] = b;
        for (int ch = 0; ch < setup_.num_dmx; ++ch) {
            for (int pb = 0; pb < b; ++pb) {
                p.dry_at(o, 0, ch, pb) = step_value(false, v.dry[at(o)][at(ch)][at(pb)]);
            }
        }
        for (int d = 0; d < setup_.num_decorr; ++d) {
            for (int pb = 0; pb < b; ++pb) {
                p.wet_at(o, 0, d, pb) = step_value(true, v.wet[at(o)][at(d)][at(pb)]);
            }
        }
    }
    return p;
}

template <typename Weight>
AjocEncoder::Values AjocEncoder::fit(long long from, int ramp, const Weight& weight) const {
    const int m = setup_.num_dmx;
    const int n = setup_.num_umx;
    const int b = bands();
    Values out = held_values();
    out.ramp = ramp;
    const Values held = held_values();
    const auto slot = [&](long long s) -> const std::vector<Slot>& {
        return slots_[static_cast<std::size_t>(s - first_slot_)];
    };
    std::vector<std::vector<double>> r_xx(at(b), std::vector<double>(at(m * m), 0.0));
    std::vector<std::vector<std::vector<double>>> r_xz(
        at(n), std::vector<std::vector<double>>(at(b), std::vector<double>(at(m), 0.0)));
    std::vector<Complex> x(at(m));
    for (int t = 0; t < kFrameSlots; ++t) {
        const double a = weight(t);
        if (a == 0.0) {
            continue;
        }
        const std::vector<Slot>& s = slot(from + t);
        for (int sb = 0; sb < kSubbands; ++sb) {
            const int pb = ajoc::sb_to_pb(b, sb);
            std::vector<double>& rxx = r_xx[at(pb)];
            for (int i = 0; i < m; ++i) {
                x[at(i)] = s[at(i)][at(sb)];
            }
            for (int i = 0; i < m; ++i) {
                for (int j = i; j < m; ++j) {
                    const double v = a * a * (conj(x[at(i)]) * x[at(j)]).real();
                    rxx[at(i * m + j)] += v;
                    if (j != i) {
                        rxx[at(j * m + i)] += v;
                    }
                }
            }
            for (int o = 0; o < n; ++o) {
                // What the held matrix leaves the ramp's share to fit.
                Complex y = s[at(m + o)][at(sb)];
                if (a < 1.0) {
                    for (int j = 0; j < m; ++j) {
                        y -= (1.0 - a) * step_value(false, held.dry[at(o)][at(j)][at(pb)]) * x[at(j)];
                    }
                }
                std::vector<double>& rxz = r_xz[at(o)][at(pb)];
                for (int i = 0; i < m; ++i) {
                    rxz[at(i)] += a * (conj(x[at(i)]) * y).real();
                }
            }
        }
    }
    for (int o = 0; o < n; ++o) {
        for (int pb = 0; pb < b; ++pb) {
            std::vector<double> a = r_xx[at(pb)];
            std::vector<double> h = r_xz[at(o)][at(pb)];
            solve(a, h, m);
            for (int ch = 0; ch < m; ++ch) {
                out.dry[at(o)][at(ch)][at(pb)] = quantise(false, h[at(ch)]);
            }
        }
        for (auto& set : out.wet[at(o)]) {
            set.fill(centre(true));
        }
    }
    return out;
}

void AjocEncoder::gather(long long first) {
    for (std::size_t c = 0; c < x_.size() + z_.size(); ++c) {
        std::vector<Complex>& to = c < x_.size() ? x_[c] : z_[c - x_.size()];
        to.assign(at(kFrameSlots * kSubbands), Complex{});
        for (int t = 0; t < kFrameSlots; ++t) {
            const std::vector<Slot>& s = slots_[static_cast<std::size_t>(first + t - first_slot_)];
            std::copy(s[c].begin(), s[c].end(), to.begin() + static_cast<std::ptrdiff_t>(t * kSubbands));
        }
    }
}

double AjocEncoder::run(const Values& values, Reconstruction& state,
                        std::vector<std::vector<Complex>>& out) {
    const ajoc::FrameParameters p = parameters_of(values);
    std::vector<std::span<Complex>> x;
    for (std::vector<Complex>& signal : x_) {
        x.push_back(signal);
    }
    out.resize(z_.size());
    std::vector<std::vector<Complex>*> z;
    for (std::vector<Complex>& object : out) {
        z.push_back(&object);
    }
    state.reconstruct(p, kFrameSlots, x, z, 1.0, {});
    double error = 0.0;
    for (std::size_t o = 0; o < z_.size(); ++o) {
        for (std::size_t k = 0; k < z_[o].size(); ++k) {
            error += norm(z_[o][k] - out[o][k]);
        }
    }
    return error;
}

AjocFields AjocEncoder::propose(long long frame, bool iframe, std::size_t max_bits) {
    const long long first = first_slot(frame);
    gather(first);
    proposed_iframe_ = iframe;
    const auto bits_of = [&](const Values& v) {
        BitWriter w = BitWriter::buffered();
        write_ajoc(w, setup_.num_dmx, fields_of(v, iframe));
        return w.bit_position();
    };
    // A fit whose values take more than the frame allows keeps its larger
    // coefficients alone: those under each threshold in turn go to 0 until it
    // fits.
    const auto within = [&](Values v) -> std::optional<Values> {
        for (const double threshold : {0.0, 0.06, 0.12, 0.25, 0.5, 1.0, 2.0}) {
            Values trial = v;
            for (auto& object : trial.dry) {
                for (auto& set : object) {
                    for (int& q : set) {
                        if (std::abs(step_value(false, q)) < threshold) {
                            q = centre(false);
                        }
                    }
                }
            }
            if (bits_of(trial) <= max_bits) {
                return trial;
            }
        }
        return std::nullopt;
    };
    std::vector<Values> candidates;
    Values held = held_values();
    held.ramp = kFrameSlots;
    candidates.push_back(held);
    for (Values v : {fit(first, kFrameSlots, [](int t) { return static_cast<double>(t) / kFrameSlots; }),
                     fit(first, 1, [](int t) { return t >= 1 ? 1.0 : 0.0; }),
                     fit(first + kLookahead, kFrameSlots, [](int) { return 1.0; })}) {
        if (std::optional<Values> fitted = within(std::move(v))) {
            candidates.push_back(std::move(*fitted));
        }
    }

    double best_error = std::numeric_limits<double>::infinity();
    std::size_t cheapest_bits = std::numeric_limits<std::size_t>::max();
    std::optional<std::size_t> best;
    std::size_t cheapest = 0;
    std::vector<Reconstruction> states(candidates.size(), state_);
    std::vector<std::vector<Complex>> out;
    std::vector<std::vector<std::vector<Complex>>> outputs(candidates.size());
    for (std::size_t c = 0; c < candidates.size(); ++c) {
        const std::size_t bits = bits_of(candidates[c]);
        if (bits < cheapest_bits) {
            cheapest_bits = bits;
            cheapest = c;
        }
        const double error = run(candidates[c], states[c], outputs[c]);
        if (bits <= max_bits && error < best_error) {
            best_error = error;
            best = c;
        }
    }
    const std::size_t chosen = best.value_or(cheapest);
    proposed_ = candidates[chosen];
    proposed_state_ = states[chosen];

    if (setup_.num_decorr > 0) {
        // Each object's energy the dry matrix leaves out, band by band, and
        // what its decorrelator gives it at a coefficient of 1.
        const int b = bands();
        Values unit = proposed_;
        for (std::size_t o = 0; o < unit.wet.size(); ++o) {
            const std::size_t d = o % at(setup_.num_decorr);
            unit.wet[o][d].fill(quantise(true, 1.0));
        }
        // A Reconstruction is about 150 kB: these two work copies live on the heap, as the
        // candidates' do, and not in this function's frame.
        const auto probe = std::make_unique<Reconstruction>(state_);
        std::vector<std::vector<Complex>> with;
        (void)run(unit, *probe, with);
        const std::vector<std::vector<Complex>>& dry = outputs[chosen];
        Values wetted = proposed_;
        for (std::size_t o = 0; o < wetted.wet.size(); ++o) {
            std::vector<double> missing(at(b), 0.0);
            std::vector<double> decorrelated(at(b), 0.0);
            for (int t = 0; t < kFrameSlots; ++t) {
                for (int sb = 0; sb < kSubbands; ++sb) {
                    const std::size_t k = at(t * kSubbands + sb);
                    const auto pb = at(ajoc::sb_to_pb(b, sb));
                    missing[pb] += norm(z_[o][k]) - norm(dry[o][k]);
                    decorrelated[pb] += norm(with[o][k] - dry[o][k]);
                }
            }
            const std::size_t d = o % at(setup_.num_decorr);
            for (int pb = 0; pb < b; ++pb) {
                const double e = decorrelated[at(pb)];
                const double w = e > 0.0 && missing[at(pb)] > 0.0 ? std::sqrt(missing[at(pb)] / e) : 0.0;
                wetted.wet[o][d][at(pb)] = quantise(true, w);
            }
        }
        if (bits_of(wetted) <= max_bits) {
            const auto after = std::make_unique<Reconstruction>(state_);
            (void)run(wetted, *after, out);
            proposed_ = wetted;
            proposed_state_ = *after;
        }
    }
    return fields_of(proposed_, iframe);
}

AjocFields AjocEncoder::least(bool iframe) const {
    Values v = held_values();
    for (std::size_t o = 0; o < v.dry.size(); ++o) {
        for (auto& set : v.dry[o]) {
            set.fill(centre(false));
        }
        for (auto& set : v.wet[o]) {
            set.fill(centre(true));
        }
    }
    v.ramp = kFrameSlots;
    return fields_of(v, iframe);
}

void AjocEncoder::commit(bool least) {
    if (least) {
        Values v = held_values();
        for (std::size_t o = 0; o < v.dry.size(); ++o) {
            for (auto& set : v.dry[o]) {
                set.fill(centre(false));
            }
            for (auto& set : v.wet[o]) {
                set.fill(centre(true));
            }
        }
        v.ramp = kFrameSlots;
        std::vector<std::vector<Complex>> out;
        (void)run(v, state_, out);
        proposed_ = v;
    } else {
        state_ = proposed_state_;
    }
    // An object not present leaves the centre behind it (5.7.3.3), which its
    // values are.
    held_ = proposed_;
    held_valid_ = true;
}

void AjocEncoder::drop_before_frame(long long frame) {
    const long long keep = first_slot(frame);
    while (first_slot_ < keep && !slots_.empty()) {
        slots_.pop_front();
        ++first_slot_;
    }
}

}  // namespace iclforge::ac4::detail

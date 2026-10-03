// The decoder's A-CPL stage (src/ac4dec/src/pcm/acpl.cpp) against a verbatim copy of the stage it
// replaced (planning/ac4.md, D14e): one interpolated matrix of doubles for every parameter, product
// and sum, and every coefficient narrowed to Real at every element. The stage now interpolates once
// for each run of subbands that shares a band and its acpl_param_prev and narrows once for the run;
// this holds the QMF matrices it writes to the BITS of the old stage's, at the decoder's scalar,
// over frames that carry the decorrelators', the ducker's and acpl_param_prev's state, with every
// framing, band count and qmf_band.
//
// test_ac4dec_acpl.cpp holds the stage to the answers that can be worked by hand; this file holds
// it to what it did before the speed-up, so that a decoder's output moves by no bit. The
// decorrelators and the interpolation the copy calls are the core's own, which
// test_ac4core_acpl_exact.cpp holds to their old selves.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/ac4core/acpl/acpl.hpp"
#include "iclforge/ac4dec/decoder.hpp"
#include "pcm/acpl.hpp"
#include "syntax/channel_elements.hpp"

namespace {

namespace acpl = iclforge::ac4::detail::acpl;
namespace codec_mode = iclforge::ac4::detail::codec_mode;
using iclforge::ac4::Speaker;
using iclforge::ac4::detail::AcplChannels;
using iclforge::ac4::detail::AcplCouplingValues;
using iclforge::ac4::detail::AcplFrameValues;
using iclforge::ac4::detail::AcplModuleValues;
using iclforge::ac4::detail::AcplStage;
using iclforge::ac4::detail::ElementKind;
using iclforge::ac4::detail::QmfMatrix;
using iclforge::ac4::detail::QmfValue;
using iclforge::ac4::detail::Real;

constexpr int kSubbands = acpl::kSubbands;
constexpr double kSqrt2 = 1.4142135623730951;

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

// The stage as src/ac4dec/src/pcm/acpl.cpp had it, for the elements the test builds: a pair, and
// the 5.X element in ASPX_ACPL_1, 2 and 3.
namespace reference {

struct Param {
    acpl::ParamSets values{};
    acpl::ParamPrev prev{};
};

constexpr std::size_t kCouplingInterpolations = 17;

void scale(std::span<QmfValue> values, double gain) {
    const auto g = static_cast<Real>(gain);
    for (QmfValue& v : values) {
        v *= g;
    }
}

class Stage {
   public:
    Stage()
        : decorrelators_{acpl::Decorrelator<Real>(0), acpl::Decorrelator<Real>(1),
                         acpl::Decorrelator<Real>(2), acpl::Decorrelator<Real>(0),
                         acpl::Decorrelator<Real>(1)} {}

    void apply(ElementKind kind, int codec, const AcplFrameValues& values, int num_ts,
               const AcplChannels& channels) {
        const std::size_t n = at(num_ts) * kSubbands;
        const auto matrix_of = [&](Speaker speaker) -> QmfMatrix {
            for (std::size_t c = 0; c < channels.speakers.size(); ++c) {
                if (channels.speakers[c] == speaker && c < channels.matrices.size()) {
                    return channels.matrices[c];
                }
            }
            return {};
        };
        const auto input = [&](std::size_t slot, Speaker speaker) -> std::span<const QmfValue> {
            const QmfMatrix matrix = matrix_of(speaker);
            std::vector<QmfValue>& copy = in_[slot];
            copy.assign(n, QmfValue{});
            if (matrix.size() >= n) {
                std::copy_n(matrix.begin(), n, copy.begin());
            }
            return copy;
        };
        const auto output = [&](Speaker speaker) -> std::span<QmfValue> {
            const QmfMatrix matrix = matrix_of(speaker);
            if (matrix.size() < n) {
                return {};
            }
            return matrix.first(n);
        };
        using S = Speaker;
        if (codec == codec_mode::kAspxAcpl3) {
            std::array<std::span<QmfValue>, 5> z = {output(S::kLeft), output(S::kLeftSurround),
                                                    output(S::kRight), output(S::kRightSurround),
                                                    output(S::kCentre)};
            const std::span<const QmfValue> x0 = input(2, S::kLeft);
            const std::span<const QmfValue> x1 = input(3, S::kRight);
            coupling(*values.coupling, x0, x1, z, num_ts);
            return;
        }
        const bool residuals = codec == codec_mode::kAspxAcpl1;
        if (kind == ElementKind::kPair) {
            const std::span<const QmfValue> x0 = input(0, S::kLeft);
            const std::span<const QmfValue> x1 =
                residuals ? input(1, S::kRight) : std::span<const QmfValue>{};
            module(values.modules[0], 0, 0, x0, x1, output(S::kLeft), output(S::kRight), num_ts);
            return;
        }
        const std::span<const QmfValue> x0 = input(0, S::kLeft);
        const std::span<const QmfValue> x1 = input(1, S::kRight);
        const std::span<const QmfValue> x3 =
            residuals ? input(3, S::kLeftSurround) : std::span<const QmfValue>{};
        const std::span<const QmfValue> x4 =
            residuals ? input(4, S::kRightSurround) : std::span<const QmfValue>{};
        module(values.modules[0], 0, 0, x0, x3, output(S::kLeft), output(S::kLeftSurround), num_ts);
        module(values.modules[1], 1, 1, x1, x4, output(S::kRight), output(S::kRightSurround),
               num_ts);
        scale(output(S::kLeftSurround), kSqrt2);
        scale(output(S::kRightSurround), kSqrt2);
    }

   private:
    void interpolate(const acpl::Framing& framing, int num_bands, const Param& param, int num_ts,
                     std::vector<double>& out) const {
        out.resize(at(num_ts) * kSubbands);
        acpl::interpolate(framing, num_bands, param.values, param.prev, num_ts, out);
    }

    void decorrelate(int decorrelator, std::span<const QmfValue> in, std::span<QmfValue> out,
                     int num_ts) {
        decorrelators_[at(decorrelator)].process(in, out, num_ts);
        duckers_[at(decorrelator)].process(out, num_ts);
    }

    void module(const AcplModuleValues& values, int index, int decorrelator,
                std::span<const QmfValue> x0, std::span<const QmfValue> x1, std::span<QmfValue> z0,
                std::span<QmfValue> z1, int num_ts) {
        const std::size_t n = at(num_ts) * kSubbands;
        work_.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            work_[i] = Real{2} * x0[i];
        }
        std::vector<QmfValue>& y = decorrelated_[at(decorrelator)];
        y.resize(n);
        decorrelate(decorrelator, work_, y, num_ts);

        std::array<acpl::ParamPrev, 2>& prev = module_prev_[at(index)];
        const Param alpha{values.alpha, prev[0]};
        const Param beta{values.beta, prev[1]};
        interpolate(values.framing, values.num_bands, alpha, num_ts, interp_[0]);
        interpolate(values.framing, values.num_bands, beta, num_ts, interp_[1]);
        for (std::size_t ts = 0; ts < at(num_ts); ++ts) {
            for (std::size_t sb = 0; sb < kSubbands; ++sb) {
                const std::size_t i = ts * kSubbands + sb;
                const QmfValue x0in = work_[i];
                const QmfValue x1in = x1.empty() ? QmfValue{} : Real{2} * x1[i];
                if (static_cast<int>(sb) < values.qmf_band) {
                    z0[i] = Real(0.5) * (x0in + x1in);
                    z1[i] = Real(0.5) * (x0in - x1in);
                } else {
                    const auto a = static_cast<Real>(interp_[0][i]);
                    const auto b = static_cast<Real>(interp_[1][i]);
                    z0[i] = Real(0.5) * (x0in * (Real{1} + a) + y[i] * b);
                    z1[i] = Real(0.5) * (x0in * (Real{1} - a) - y[i] * b);
                }
            }
        }
        acpl::end_frame(values.framing, values.num_bands, values.alpha, prev[0]);
        acpl::end_frame(values.framing, values.num_bands, values.beta, prev[1]);
    }

    void coupling(const AcplCouplingValues& values, std::span<const QmfValue> x0,
                  std::span<const QmfValue> x1, std::span<std::span<QmfValue>, 5> z, int num_ts) {
        const std::size_t n = at(num_ts) * kSubbands;
        const auto input_gain = static_cast<Real>(1.0 + 2.0 * std::sqrt(0.5));
        std::vector<QmfValue>& x0in = in_[0];
        std::vector<QmfValue>& x1in = in_[1];
        x0in.resize(n);
        x1in.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            x0in[i] = input_gain * x0[i];
            x1in[i] = input_gain * x1[i];
        }

        const auto base = [&](std::size_t k, const acpl::ParamSets& sets) {
            return Param{sets, coupling_prev_[k]};
        };
        const Param a1 = base(0, values.alpha[0]);
        const Param a2 = base(1, values.alpha[1]);
        const Param b1 = base(2, values.beta[0]);
        const Param b2 = base(3, values.beta[1]);
        const Param b3 = base(4, values.beta3);
        std::array<Param, 6> g{};
        for (std::size_t k = 0; k < 6; ++k) {
            g[k] = base(5 + k, values.gamma[k]);
        }
        const auto combine = [](const Param& p, const Param& q, auto op) {
            Param out;
            for (std::size_t ps = 0; ps < acpl::kMaxParamSets; ++ps) {
                for (std::size_t pb = 0; pb < acpl::kMaxParamBands; ++pb) {
                    out.values[ps][pb] = op(p.values[ps][pb], q.values[ps][pb]);
                }
            }
            for (std::size_t sb = 0; sb < kSubbands; ++sb) {
                out.prev[sb] = op(p.prev[sb], q.prev[sb]);
            }
            return out;
        };
        const auto times = [&](const Param& p, const Param& q) {
            return combine(p, q, std::multiplies<>{});
        };
        const auto plus = [&](const Param& p, const Param& q) {
            return combine(p, q, std::plus<>{});
        };

        std::size_t slots_used = 0;
        interp_scratch_.resize(kCouplingInterpolations);
        const auto interp = [&](const Param& p) -> const std::vector<double>& {
            std::vector<double>& out = interp_scratch_[slots_used++];
            interpolate(values.framing, values.num_bands, p, num_ts, out);
            return out;
        };
        const std::vector<double>& ig1 = interp(g[0]);
        const std::vector<double>& ig2 = interp(g[1]);
        const std::vector<double>& ig3 = interp(g[2]);
        const std::vector<double>& ig4 = interp(g[3]);
        const std::vector<double>& ig5 = interp(g[4]);
        const std::vector<double>& ig6 = interp(g[5]);
        const std::vector<double>& ig135 = interp(plus(plus(g[0], g[2]), g[4]));
        const std::vector<double>& ig246 = interp(plus(plus(g[1], g[3]), g[5]));
        const std::vector<double>& ig1a1 = interp(times(g[0], a1));
        const std::vector<double>& ig2a1 = interp(times(g[1], a1));
        const std::vector<double>& ig3a2 = interp(times(g[2], a2));
        const std::vector<double>& ig4a2 = interp(times(g[3], a2));
        const std::vector<double>& ib1 = interp(b1);
        const std::vector<double>& ib2 = interp(b2);
        const std::vector<double>& ib3 = interp(b3);
        const std::vector<double>& ib3a1 = interp(times(b3, a1));
        const std::vector<double>& ib3a2 = interp(times(b3, a2));

        std::array<std::vector<QmfValue>, 3>& v = transformed_;
        for (auto& matrix : v) {
            matrix.resize(n);
        }
        for (std::size_t i = 0; i < n; ++i) {
            v[0][i] = x0in[i] * static_cast<Real>(ig1[i]) + x1in[i] * static_cast<Real>(ig2[i]);
            v[1][i] = x0in[i] * static_cast<Real>(ig3[i]) + x1in[i] * static_cast<Real>(ig4[i]);
            v[2][i] = x0in[i] * static_cast<Real>(ig135[i]) + x1in[i] * static_cast<Real>(ig246[i]);
        }
        for (int d = 0; d < acpl::kDecorrelators; ++d) {
            decorrelated_[at(d)].resize(n);
            decorrelate(d, v[at(d)], decorrelated_[at(d)], num_ts);
        }
        const std::vector<QmfValue>& y0 = decorrelated_[0];
        const std::vector<QmfValue>& y1 = decorrelated_[1];
        const std::vector<QmfValue>& y2 = decorrelated_[2];

        for (std::size_t i = 0; i < n; ++i) {
            const QmfValue l = x0in[i];
            const QmfValue r = x1in[i];
            QmfValue z0 = Real(0.5) * (l * static_cast<Real>(ig1[i] + ig1a1[i]) +
                                       r * static_cast<Real>(ig2[i] + ig2a1[i]) +
                                       y0[i] * static_cast<Real>(ib1[i]));
            QmfValue z1 = Real(0.5) * (l * static_cast<Real>(ig1[i] - ig1a1[i]) +
                                       r * static_cast<Real>(ig2[i] - ig2a1[i]) -
                                       y0[i] * static_cast<Real>(ib1[i]));
            QmfValue z2 = Real(0.5) * (l * static_cast<Real>(ig3[i] + ig3a2[i]) +
                                       r * static_cast<Real>(ig4[i] + ig4a2[i]) +
                                       y1[i] * static_cast<Real>(ib2[i]));
            QmfValue z3 = Real(0.5) * (l * static_cast<Real>(ig3[i] - ig3a2[i]) +
                                       r * static_cast<Real>(ig4[i] - ig4a2[i]) -
                                       y1[i] * static_cast<Real>(ib2[i]));
            QmfValue z4 = l * static_cast<Real>(ig5[i]) + r * static_cast<Real>(ig6[i]);
            z0 += Real(0.25) * y2[i] * static_cast<Real>(ib3[i] + ib3a1[i]);
            z1 += Real(0.25) * y2[i] * static_cast<Real>(ib3[i] - ib3a1[i]);
            z2 += Real(0.25) * y2[i] * static_cast<Real>(ib3[i] + ib3a2[i]);
            z3 += Real(0.25) * y2[i] * static_cast<Real>(ib3[i] - ib3a2[i]);
            z4 -= Real(0.5) * y2[i] * static_cast<Real>(ib3[i]);
            const auto sqrt2 = static_cast<Real>(kSqrt2);
            z[0][i] = z0;
            z[1][i] = sqrt2 * z1;
            z[2][i] = z2;
            z[3][i] = sqrt2 * z3;
            z[4][i] = sqrt2 * z4;
        }

        const std::array<const acpl::ParamSets*, 11> sets = {
            &values.alpha[0], &values.alpha[1], &values.beta[0],  &values.beta[1],
            &values.beta3,    &values.gamma[0], &values.gamma[1], &values.gamma[2],
            &values.gamma[3], &values.gamma[4], &values.gamma[5]};
        for (std::size_t k = 0; k < sets.size(); ++k) {
            acpl::end_frame(values.framing, values.num_bands, *sets[k], coupling_prev_[k]);
        }
    }

    std::array<acpl::Decorrelator<Real>, 5> decorrelators_;
    std::array<acpl::TransientDucker<Real>, 5> duckers_{};
    std::array<std::array<acpl::ParamPrev, 2>, 4> module_prev_{};
    std::array<acpl::ParamPrev, 11> coupling_prev_{};
    std::array<std::vector<QmfValue>, 5> in_{};
    std::array<std::vector<QmfValue>, 3> transformed_{};
    std::array<std::vector<QmfValue>, 5> decorrelated_{};
    std::vector<QmfValue> work_;
    std::array<std::vector<double>, 2> interp_{};
    std::vector<std::vector<double>> interp_scratch_;
};

}  // namespace reference

// Parameters as a stream sends them (the tables' values and sums of them) and as it can send them
// when it is corrupt or built to find a difference: zeros of either sign, equal sets, plain
// doubles.
class Source {
   public:
    explicit Source(unsigned seed) : rng_(seed) {}

    int below(int n) { return static_cast<int>(rng_() % static_cast<unsigned>(n)); }

    double value() {
        switch (below(6)) {
            case 0:
                return 0.0;
            case 1:
                return -0.0;
            case 2:
                return 0.0625 * static_cast<double>(below(65) - 32);
            case 3:
                return 0.190625 * static_cast<double>(below(21) - 10);
            case 4:
                return 4.0 * std::generate_canonical<double, 53>(rng_) - 2.0;
            default:
                return normal_(rng_);
        }
    }

    acpl::ParamSets sets() {
        acpl::ParamSets out{};
        for (auto& set : out) {
            for (double& v : set) {
                v = value();
            }
        }
        for (std::size_t pb = 0; pb < acpl::kMaxParamBands; ++pb) {
            if (below(4) == 0) {
                out[1][pb] = out[0][pb];
            }
        }
        return out;
    }

    acpl::Framing framing(int num_ts) {
        acpl::Framing out{
            .steep = below(3) == 0, .num_param_sets = 1 + below(2), .param_timeslot = {}};
        out.param_timeslot[0] = below(num_ts + 1);
        out.param_timeslot[1] = std::min(num_ts, out.param_timeslot[0] + below(num_ts + 1));
        return out;
    }

    // A QMF matrix: normal values, a share of zeros of either sign.
    std::vector<QmfValue> matrix(std::size_t count, double zero_fraction) {
        std::vector<QmfValue> out(count);
        std::bernoulli_distribution zero(zero_fraction);
        std::bernoulli_distribution negative(0.5);
        for (QmfValue& v : out) {
            const auto part = [&] {
                if (zero(rng_)) {
                    return negative(rng_) ? Real(-0.0) : Real(0.0);
                }
                return static_cast<Real>(normal_(rng_));
            };
            const Real re = part();
            const Real im = part();
            v = QmfValue(re, im);
        }
        return out;
    }

   private:
    std::mt19937 rng_;
    std::normal_distribution<double> normal_;
};

constexpr int kSlots = 32;
constexpr std::size_t kValues = static_cast<std::size_t>(kSlots) * kSubbands;
constexpr std::array<int, 4> kBandCounts = {15, 12, 9, 7};
constexpr std::array<int, 4> kQmfBands = {0, 7, 19, 64};

AcplModuleValues module_values(Source& source, int num_ts) {
    AcplModuleValues out;
    out.framing = source.framing(num_ts);
    out.num_bands = kBandCounts[at(source.below(4))];
    out.qmf_band = kQmfBands[at(source.below(4))];
    out.alpha = source.sets();
    out.beta = source.sets();
    return out;
}

AcplCouplingValues coupling_values(Source& source, int num_ts) {
    AcplCouplingValues out;
    out.framing = source.framing(num_ts);
    out.num_bands = kBandCounts[at(source.below(4))];
    for (auto& set : out.alpha) {
        set = source.sets();
    }
    for (auto& set : out.beta) {
        set = source.sets();
    }
    out.beta3 = source.sets();
    for (auto& set : out.gamma) {
        set = source.sets();
    }
    return out;
}

[[nodiscard]] bool same_bits(const std::vector<QmfValue>& a, const std::vector<QmfValue>& b) {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(QmfValue)) == 0;
}

// The same frames through the stage and through the old one, as each element builds them: the
// matrices the stage writes in place must come out alike.
void check(ElementKind kind, int codec, unsigned seed) {
    Source source(seed);
    AcplStage stage;
    reference::Stage original;
    const std::array<Speaker, 5> speakers = {Speaker::kLeft, Speaker::kRight, Speaker::kCentre,
                                             Speaker::kLeftSurround, Speaker::kRightSurround};
    const std::array<int, 6> slots = {32, 32, 30, 24, 32, 32};
    for (std::size_t frame = 0; frame < slots.size(); ++frame) {
        const int num_ts = slots[frame];
        AcplFrameValues values;
        if (codec == codec_mode::kAspxAcpl3) {
            values.coupling = coupling_values(source, num_ts);
        } else {
            values.module_count = kind == ElementKind::kPair ? 1 : 2;
            for (std::size_t m = 0; m < values.module_count; ++m) {
                values.modules[m] = module_values(source, num_ts);
            }
        }
        std::array<std::vector<QmfValue>, 5> actual;
        for (auto& m : actual) {
            m = source.matrix(kValues, frame == 3 ? 0.4 : 0.03);
        }
        std::array<std::vector<QmfValue>, 5> expected = actual;
        const std::array<QmfMatrix, 5> actual_matrices = {
            actual[0], actual[1], actual[2], actual[3], actual[4]};
        const std::array<QmfMatrix, 5> expected_matrices = {
            expected[0], expected[1], expected[2], expected[3], expected[4]};
        const std::size_t channels = kind == ElementKind::kPair ? 2 : 5;
        const int channel_mode = kind == ElementKind::kPair
                                     ? iclforge::ac4::detail::ch_mode::kStereo
                                     : iclforge::ac4::detail::ch_mode::k5_0;
        stage.apply(
            channel_mode, false, kind, codec, values, num_ts,
            {.speakers = std::span<const Speaker>(speakers).first(channels),
             .matrices = std::span<const QmfMatrix>(actual_matrices).first(channels)});
        original.apply(
            kind, codec, values, num_ts,
            {.speakers = std::span<const Speaker>(speakers).first(channels),
             .matrices =
                 std::span<const QmfMatrix>(expected_matrices).first(channels)});
        for (std::size_t c = 0; c < channels; ++c) {
            CAPTURE(frame, c);
            REQUIRE(same_bits(actual[c], expected[c]));
        }
    }
}

}  // namespace

TEST_CASE("the A-CPL stage gives the bits of the old stage: a channel pair in ASPX_ACPL_2",
          "[ac4dec][acpl][exact]") {
    for (unsigned seed = 1; seed <= 8; ++seed) {
        CAPTURE(seed);
        check(ElementKind::kPair, codec_mode::kAspxAcpl2, seed);
    }
}

TEST_CASE(
    "the A-CPL stage gives the bits of the old stage: a channel pair with residuals in ASPX_ACPL_1",
    "[ac4dec][acpl][exact]") {
    for (unsigned seed = 11; seed <= 18; ++seed) {
        CAPTURE(seed);
        check(ElementKind::kPair, codec_mode::kAspxAcpl1, seed);
    }
}

TEST_CASE("the A-CPL stage gives the bits of the old stage: the 5.X element's two modules",
          "[ac4dec][acpl][exact]") {
    for (unsigned seed = 21; seed <= 28; ++seed) {
        CAPTURE(seed);
        check(ElementKind::k5X, codec_mode::kAspxAcpl2, seed);
        check(ElementKind::k5X, codec_mode::kAspxAcpl1, seed + 100);
    }
}

TEST_CASE(
    "the A-CPL stage gives the bits of the old stage: the 5.X element's coupling in ASPX_ACPL_3",
    "[ac4dec][acpl][exact]") {
    for (unsigned seed = 31; seed <= 46; ++seed) {
        CAPTURE(seed);
        check(ElementKind::k5X, codec_mode::kAspxAcpl3, seed);
    }
}

#include "dsp/qmf.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

#include "tables/qmf_tables.hpp"

namespace ac4::detail::dsp {
namespace {

constexpr std::size_t kSubbands = kQmfSubbands;

// exp(i pi num / den), with num reduced modulo 2 den first so that a large
// product loses no precision.
std::complex<double> turn(long long num, long long den) {
    const long long period = 2 * den;
    const long long reduced = ((num % period) + period) % period;
    const double angle = std::numbers::pi * static_cast<double>(reduced) / static_cast<double>(den);
    return {std::cos(angle), std::sin(angle)};
}

}  // namespace

template <typename Real>
QmfAnalysis<Real>::QmfAnalysis() : fft_(128) {
    for (std::size_t n = 0; n < pre_.size(); ++n) {
        pre_[n] = Complex(turn(static_cast<long long>(n), 128));
    }
    for (std::size_t sb = 0; sb < post_.size(); ++sb) {
        post_[sb] = Complex(turn(-static_cast<long long>(2 * sb + 1), 256));
    }
}

template <typename Real>
void QmfAnalysis<Real>::reset() noexcept {
    filt_.fill(Real{});
}

template <typename Real>
void QmfAnalysis<Real>::process(std::span<const Real> pcm, std::span<Complex> out) {
    if (pcm.size() % kSubbands != 0 || out.size() < pcm.size()) {
        return;
    }
    const auto& qwin = tables::kQwin;
    const std::size_t slots = pcm.size() / kSubbands;
    for (std::size_t ts = 0; ts < slots; ++ts) {
        // Shift by 64 and feed the new samples, newest at [0].
        std::copy_backward(filt_.begin(), filt_.end() - kSubbands, filt_.end());
        for (std::size_t sb = 0; sb < kSubbands; ++sb) {
            filt_[sb] = pcm[ts * kSubbands + kSubbands - 1 - sb];
        }
        // Window, fold to u[n], and turn it by exp(i pi n / 128).
        for (std::size_t n = 0; n < 128; ++n) {
            Real u{};
            for (std::size_t k = 0; k < 5; ++k) {
                u += filt_[n + k * 128] * static_cast<Real>(qwin[n + k * 128]);
            }
            work_[n] = pre_[n] * u;
        }
        fft_.inverse(work_);
        std::span<Complex> slot = out.subspan(ts * kSubbands, kSubbands);
        for (std::size_t sb = 0; sb < kSubbands; ++sb) {
            slot[sb] = post_[sb] * work_[sb];
        }
    }
}

template <typename Real>
QmfSynthesis<Real>::QmfSynthesis() : fft_(128) {
    for (std::size_t sb = 0; sb < pre_.size(); ++sb) {
        pre_[sb] = Complex(turn(-255 * static_cast<long long>(2 * sb + 1), 256) / 64.0);
    }
    for (std::size_t n = 0; n < post_.size(); ++n) {
        post_[n] = Complex(turn(static_cast<long long>(n), 128));
    }
}

template <typename Real>
void QmfSynthesis<Real>::reset() noexcept {
    filt_.fill(Real{});
}

template <typename Real>
void QmfSynthesis<Real>::process(std::span<const Complex> in, std::span<Real> pcm) {
    if (in.size() % kSubbands != 0 || pcm.size() < in.size()) {
        return;
    }
    const auto& qwin = tables::kQwin;
    const std::size_t slots = in.size() / kSubbands;
    for (std::size_t ts = 0; ts < slots; ++ts) {
        // Shift by 128, then the 128 new values from the 64 subband samples.
        std::copy_backward(filt_.begin(), filt_.end() - 128, filt_.end());
        std::span<const Complex> slot = in.subspan(ts * kSubbands, kSubbands);
        for (std::size_t sb = 0; sb < kSubbands; ++sb) {
            work_[sb] = pre_[sb] * slot[sb];
        }
        std::fill(work_.begin() + kSubbands, work_.end(), Complex{});
        fft_.inverse(work_);
        for (std::size_t n = 0; n < 128; ++n) {
            filt_[n] = (post_[n] * work_[n]).real();
        }
        // g[128k + sb] = qsyn[256k + sb] and g[128k + 64 + sb] = qsyn[256k +
        // 192 + sb], windowed, and summed over the ten groups of 64.
        std::span<Real> out = pcm.subspan(ts * kSubbands, kSubbands);
        for (std::size_t sb = 0; sb < kSubbands; ++sb) {
            Real sum{};
            for (std::size_t k = 0; k < 5; ++k) {
                sum += filt_[256 * k + sb] * static_cast<Real>(qwin[128 * k + sb]);
                sum += filt_[256 * k + 192 + sb] * static_cast<Real>(qwin[128 * k + 64 + sb]);
            }
            out[sb] = sum;
        }
    }
}

template class QmfAnalysis<double>;
template class QmfSynthesis<double>;

}  // namespace ac4::detail::dsp

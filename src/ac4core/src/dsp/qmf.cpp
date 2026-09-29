#include "dsp/qmf.hpp"

#include <cstddef>

#include "dsp/qmf_kernels.hpp"

namespace ac4::detail::dsp {
namespace {

constexpr std::size_t kSubbands = kQmfSubbands;

}  // namespace

template <typename Real>
void QmfAnalysis<Real>::reset() noexcept {
    filt_.fill(Real{});
    head_ = 0;
}

template <typename Real>
void QmfAnalysis<Real>::process(std::span<const Real> pcm, std::span<Complex> out) {
    QmfScratch<Real> scratch{};
    process(pcm, out, scratch);
}

template <typename Real>
void QmfAnalysis<Real>::process(std::span<const Real> pcm, std::span<Complex> out,
                                QmfScratch<Real>& scratch) {
    if (pcm.size() % kSubbands != 0 || out.size() < pcm.size()) {
        return;
    }
    const std::size_t slots = pcm.size() / kSubbands;
    for (std::size_t ts = 0; ts < slots; ++ts) {
        // The new block goes over the oldest, and is the newest: qmf_filt[sb] =
        // pcm[63 - sb] within it.
        head_ = head_ == 0 ? 9 : head_ - 1;
        Real* block = filt_.data() + head_ * kSubbands;
        const Real* slot = pcm.data() + ts * kSubbands;
        for (std::size_t sb = 0; sb < kSubbands; ++sb) {
            block[sb] = slot[kSubbands - 1 - sb];
        }
        qmf::analysis_window(filt_.data(), head_, scratch.u.data());
        qmf::analysis_rotate(scratch.u.data(), scratch.a_re.data(), scratch.a_im.data());
        qmf::fft64(scratch.a_re.data(), scratch.a_im.data(), scratch.b_re.data(),
                   scratch.b_im.data());
        qmf::analysis_unpack(scratch.b_re.data(), scratch.b_im.data(), out.data() + ts * kSubbands);
    }
}

template <typename Real>
void QmfSynthesis<Real>::reset() noexcept {
    filt_.fill(Real{});
    head_ = 0;
}

template <typename Real>
void QmfSynthesis<Real>::process(std::span<const Complex> in, std::span<Real> pcm) {
    QmfScratch<Real> scratch{};
    process(in, pcm, scratch);
}

template <typename Real>
void QmfSynthesis<Real>::process(std::span<const Complex> in, std::span<Real> pcm,
                                 QmfScratch<Real>& scratch) {
    if (in.size() % kSubbands != 0 || pcm.size() < in.size()) {
        return;
    }
    const std::size_t slots = in.size() / kSubbands;
    for (std::size_t ts = 0; ts < slots; ++ts) {
        // The 128 new values go over the oldest block, and are the newest.
        head_ = head_ == 0 ? 9 : head_ - 1;
        qmf::synthesis_pack(in.data() + ts * kSubbands, scratch.a_re.data(), scratch.a_im.data());
        qmf::fft64(scratch.a_re.data(), scratch.a_im.data(), scratch.b_re.data(),
                   scratch.b_im.data());
        qmf::synthesis_rotate(scratch.b_re.data(), scratch.b_im.data(), filt_.data() + head_ * 128);
        qmf::synthesis_window(filt_.data(), head_, pcm.data() + ts * kSubbands);
    }
}

template class QmfAnalysis<Real>;
template class QmfSynthesis<Real>;
// The encoder's own QMF-domain code (src/ac4enc/src/acpl, src/ac4enc/src/aspx)
// calls these at double regardless of the decoder's scalar (see this target's
// CMakeLists.txt, AC4CORE_ALSO_AT_DOUBLE).
AC4CORE_ALSO_AT_DOUBLE(template class QmfAnalysis<double>; template class QmfSynthesis<double>;)

}  // namespace ac4::detail::dsp

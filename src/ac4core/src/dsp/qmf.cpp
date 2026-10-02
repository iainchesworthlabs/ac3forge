#include "iclforge/ac4core/dsp/qmf.hpp"

#include <cstddef>

#include "iclforge/ac4core/detail/profiling.hpp"
#include "iclforge/ac4core/dsp/qmf_fixed.hpp"
#include "iclforge/ac4core/dsp/qmf_kernels.hpp"
#include "iclforge/ac4core/dsp/qmf_vector.hpp"

namespace iclforge::ac4::detail::dsp {
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
    AC4_ZONE_SCOPED_N("ac4_qmf_analysis");
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
        if constexpr (kFixed<Real>) {
            qmf::fixed::analysis_slot(filt_.data(), head_, out.data() + ts * kSubbands, scratch);
        } else {
            qmf::vec::analysis_window(filt_.data(), head_, scratch.u.data());
            qmf::vec::analysis_rotate(scratch.u.data(), scratch.a_re.data(), scratch.a_im.data());
            qmf::vec::fft64(scratch.a_re.data(), scratch.a_im.data(), scratch.b_re.data(),
                            scratch.b_im.data());
            qmf::vec::analysis_unpack(scratch.b_re.data(), scratch.b_im.data(),
                                      out.data() + ts * kSubbands);
        }
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
    AC4_ZONE_SCOPED_N("ac4_qmf_synthesis");
    const std::size_t slots = in.size() / kSubbands;
    for (std::size_t ts = 0; ts < slots; ++ts) {
        // The 128 new values go over the oldest block, and are the newest.
        head_ = head_ == 0 ? 9 : head_ - 1;
        if constexpr (kFixed<Real>) {
            qmf::fixed::synthesis_slot(in.data() + ts * kSubbands, filt_.data(), head_,
                                       pcm.data() + ts * kSubbands, scratch);
        } else {
            qmf::vec::synthesis_pack(in.data() + ts * kSubbands, scratch.a_re.data(),
                                     scratch.a_im.data());
            qmf::vec::fft64(scratch.a_re.data(), scratch.a_im.data(), scratch.b_re.data(),
                            scratch.b_im.data());
            qmf::vec::synthesis_rotate(scratch.b_re.data(), scratch.b_im.data(),
                                       filt_.data() + head_ * 128);
            qmf::vec::synthesis_window(filt_.data(), head_, pcm.data() + ts * kSubbands);
        }
    }
}

template class QmfAnalysis<Real>;
template class QmfSynthesis<Real>;
// The encoder's own QMF-domain code (src/ac4enc/src/acpl, src/ac4enc/src/aspx)
// calls these at double regardless of the decoder's scalar (see this target's
// CMakeLists.txt, AC4CORE_ALSO_AT_DOUBLE).
AC4CORE_ALSO_AT_DOUBLE(template class QmfAnalysis<double>; template class QmfSynthesis<double>;)

}  // namespace iclforge::ac4::detail::dsp

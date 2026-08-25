#include "mdct_avx2.hpp"

#include <cstddef>

#include "ac3/core/window.hpp"

#include "simd_avx2.hpp"

namespace ac3::internal::avx2 {

void apply_analysis_window(std::span<const double, 512> x, std::span<double, 512> windowed) {
    const double* const in = x.data();
    double* const out = windowed.data();
    for (std::size_t n = 0; n < 512; n += 4) {
        (f64x4::load(in + n) * f64x4::load(&ac3::kAnalysisWindow[n])).store(out + n);
    }
}

}  // namespace ac3::internal::avx2

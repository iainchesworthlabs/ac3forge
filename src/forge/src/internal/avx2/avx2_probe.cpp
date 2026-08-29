#include "avx2_probe.hpp"

#include <array>
#include <cstddef>

#include <immintrin.h>

namespace ac3::internal::avx2 {

bool avx2_probe_matches_expected() noexcept {
    // Four independent lanes, plain add - no FMA, matching the project-wide
    // policy the real AVX2 kernels this proves the pipeline for will also
    // have to hold to (see the top-level CMakeLists.txt's -ffp-contract=off
    // comment). Nothing here is a codec kernel or carries the codec's own
    // bit-exactness promise; the point is only "did AVX2 code compile,
    // link and execute correctly on this machine", so an ordinary
    // fixed-point equality check is enough - no adversarial value set, no
    // tolerance, no rounding edge case to worry about.
    constexpr std::array<double, 4> a{1.0, 2.0, 3.0, 4.0};
    constexpr std::array<double, 4> b{10.0, 20.0, 30.0, 40.0};

    const __m256d va = _mm256_loadu_pd(a.data());
    const __m256d vb = _mm256_loadu_pd(b.data());
    const __m256d vsum = _mm256_add_pd(va, vb);

    std::array<double, 4> avx2_result{};
    _mm256_storeu_pd(avx2_result.data(), vsum);

    for (std::size_t i = 0; i < a.size(); ++i) {
        const double scalar_result = a[i] + b[i];
        if (avx2_result[i] != scalar_result) {
            return false;
        }
    }
    return true;
}

}  // namespace ac3::internal::avx2

#include "avx2_tier.hpp"

// The one definition tests/core/avx2/absent/avx2_tier.hpp declares, and the
// only thing the no-AVX2-tier build is missing: src/forge ships a `none/`
// variant for every mdct_avx2.hpp kernel but none for avx2_probe.cpp, since
// nothing in the library itself calls the probe - it exists purely so
// tests/core/test_simd_kernels.cpp can execute real AVX2 instructions.
//
// This body cannot run. ac3::test::avx2::kTierCompiled is false in this build
// and require_runnable_avx2() skips every AVX2 case on that before any of them
// reaches a call. std::unreachable() rather than a returned `false` for the
// same reason src/internal/avx2/none/mdct_avx2.cpp uses it: a caller can only
// arrive here by bypassing a guard, which is a bug to make loud rather than a
// case to answer plausibly.

#include <utility>

namespace ac3::internal::avx2 {

bool avx2_probe_matches_expected() noexcept { std::unreachable(); }

}  // namespace ac3::internal::avx2

static_assert(!ac3::test::avx2::kTierCompiled,
              "this translation unit is only for the build with no AVX2 tier; the present/ "
              "variant forwards to the real probe instead");

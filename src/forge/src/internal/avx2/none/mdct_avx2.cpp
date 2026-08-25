#include "mdct_avx2.hpp"

#include <utility>

// ---------------------------------------------------------------------------
// The AC3FORGE_AVX2=OFF / non-x86_64 half of mdct_avx2.hpp's declarations:
// compiled straight into forge_objects (no AVX2 flag needed - there is no
// AVX2 code here to need one), same "always linkable, directory-selected
// implementation" shape as cpu_features.hpp's own hardware_avx2.hpp probe,
// so mdct.cpp's call sites never need a preprocessor conditional
// (tools/checks/check_platform_macros.ps1 forbids one under src/ outright).
// A caller only ever reaches these bodies if ac3::internal::cpu::has_avx2()
// answered true - which this configuration's cpu_features.cpp build can
// never do (see src/forge/CMakeLists.txt's AC3FORGE_CPU_PROBE_DIR
// resolution: no AC3FORGE_AVX2/x86_64 means no forge_simd_avx2 target and
// no probe capable of returning true either) - so std::unreachable() is the
// correct body, not merely a defensive placeholder.
// ---------------------------------------------------------------------------

namespace ac3::internal::avx2 {

void apply_analysis_window(std::span<const double, 512> /*x*/,
                           std::span<double, 512> /*windowed*/) {
    std::unreachable();
}

}  // namespace ac3::internal::avx2

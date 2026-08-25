#include "mdct_avx2.hpp"

#include <cstdint>
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

void dct4_pre_twiddle(std::span<const double> /*u*/, std::span<const double> /*pre_re*/,
                      std::span<const double> /*pre_im*/,
                      std::span<const std::uint16_t> /*bitrev*/, std::span<double> /*z_re*/,
                      std::span<double> /*z_im*/) {
    std::unreachable();
}

void dct4_post_twiddle(std::span<const double> /*z_re*/, std::span<const double> /*z_im*/,
                       std::span<const double> /*post_re*/, std::span<const double> /*post_im*/,
                       double /*scale*/, std::span<double> /*out*/) {
    std::unreachable();
}

void imdct512_pre_twiddle(std::span<const double> /*coeffs*/, std::span<const double> /*cos1*/,
                          std::span<const double> /*sin1*/,
                          std::span<const std::uint16_t> /*bitrev*/, std::span<double> /*z_re*/,
                          std::span<double> /*z_im*/) {
    std::unreachable();
}

void imdct512_negate_copy(std::span<const double> /*z_re*/, std::span<const double> /*z_im*/,
                          std::span<double> /*t_re*/, std::span<double> /*t_im*/) {
    std::unreachable();
}

void imdct512_post_twiddle(std::span<const double> /*cos1*/, std::span<const double> /*sin1*/,
                           std::span<const double> /*t_re*/, std::span<const double> /*t_im*/,
                           std::span<double> /*y_re*/, std::span<double> /*y_im*/) {
    std::unreachable();
}

void imdct256_post_twiddle(std::span<const double> /*cos2*/, std::span<const double> /*sin2*/,
                           std::span<const double> /*t1_re*/, std::span<const double> /*t1_im*/,
                           std::span<const double> /*t2_re*/, std::span<const double> /*t2_im*/,
                           std::span<double> /*y1_re*/, std::span<double> /*y1_im*/,
                           std::span<double> /*y2_re*/, std::span<double> /*y2_im*/) {
    std::unreachable();
}

void imdct512_windowed_batch4(std::span<const double> /*spectra*/, std::size_t /*stride*/,
                              std::size_t /*group_start*/, std::span<const double> /*cos1*/,
                              std::span<const double> /*sin1*/,
                              const ac3::internal::FftTables<128>& /*fft*/,
                              std::span<double> /*pcm_out*/) {
    std::unreachable();
}

}  // namespace ac3::internal::avx2

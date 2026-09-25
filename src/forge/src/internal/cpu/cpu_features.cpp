#include "cpu_features.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <fmt/base.h>

#include "ac3/internal/cpu/hardware_avx2.hpp"

namespace ac3::internal::cpu {

namespace {

enum class ForcedTier : std::uint8_t { kAuto, kSse2, kAvx2 };

// AC3FORGE_SIMD_TIER, parsed once. Portable - no platform-specific code -
// which is why it lives here rather than in one of the directory-selected
// hardware_avx2.hpp headers alongside it.
ForcedTier forced_tier() {
    const char* const value = std::getenv("AC3FORGE_SIMD_TIER");
    if (value == nullptr || std::strcmp(value, "auto") == 0) {
        return ForcedTier::kAuto;
    }
    if (std::strcmp(value, "sse2") == 0) {
        return ForcedTier::kSse2;
    }
    if (std::strcmp(value, "avx2") == 0) {
        return ForcedTier::kAvx2;
    }
    fmt::print(stderr,
              "AC3FORGE_SIMD_TIER='{}' is not one of auto|sse2|avx2 - treating it as unset "
              "(auto)\n",
              value);
    return ForcedTier::kAuto;
}

}  // namespace

// The two diagnostics in this file go through fmt::print, which throws std::system_error when
// stderr cannot be written, so an escape from this noexcept function is std::terminate. That was
// already so before {fmt} was compiled into this library (ac3::fmt_private, cmake/Fmt.cmake);
// clang-tidy only sees the path now that fmt's bodies are in this translation unit. The abort()
// below chooses the same outcome for a forced tier this CPU cannot run.
// NOLINTNEXTLINE(bugprone-exception-escape)
bool has_avx2() noexcept {
    static const bool result = [] {
        const ForcedTier forced = forced_tier();
        if (forced == ForcedTier::kSse2) {
            return false;
        }
        const bool hardware_capable = cpuid_reports_avx2();
        if (forced == ForcedTier::kAvx2) {
            if (!hardware_capable) {
                // Never execute AVX2 on hardware that cannot run it - an
                // illegal-instruction fault is not an acceptable way to
                // report this. AC3FORGE_SIMD_TIER=avx2 exists specifically
                // to PROVE the AVX2 path runs somewhere, so silently
                // falling back here would defeat the one thing it is for.
                fmt::print(stderr,
                          "AC3FORGE_SIMD_TIER=avx2 was forced, but this CPU (or this build, "
                          "if AC3FORGE_AVX2=OFF or the target is not x86-64) cannot execute "
                          "AVX2 - refusing to run it rather than risk an illegal-instruction "
                          "fault\n");
                std::abort();
            }
            return true;
        }
        return hardware_capable;
    }();
    return result;
}

}  // namespace ac3::internal::cpu

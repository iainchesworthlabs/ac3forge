#pragma once

// ---------------------------------------------------------------------------
// The "no AVX2 tier at all" member of the CPU-probe seam. Selected whenever
// the target is not x86-64, or ICLFORGE_AVX2 is OFF (see
// src/ac3/CMakeLists.txt) - see cpu_features.hpp for what the seam is and
// why it exists.
//
// Unconditionally false: there is no AVX2-flagged code compiled into this
// binary at all (no forge_simd_avx2 object library was built), so there is
// nothing to probe hardware FOR - answering "does this CPU support AVX2"
// truthfully would still be meaningless when the answer to "is there
// anything to run" is already no. cpu_features.cpp's ICLFORGE_SIMD_TIER=avx2
// override treats this the same as genuinely incapable hardware: abort
// rather than claim a capability nothing can act on.
// ---------------------------------------------------------------------------

namespace iclforge::internal::cpu {

[[nodiscard]] inline bool cpuid_reports_avx2() noexcept { return false; }

}  // namespace iclforge::internal::cpu

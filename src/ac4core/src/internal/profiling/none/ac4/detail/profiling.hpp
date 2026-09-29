#pragma once

// Zone markers for the AC-4 kernels, no-op variant: the default, and the only
// one outside the minimum-footprint profile. AC4_ZONE_SCOPED_N() expands to
// nothing at all, so a kernel that carries a marker compiles to the object code
// it would have with none - the same promise ac3::forge's
// ac3/internal/profiling.hpp makes for its own AC3_ZONE_SCOPED_N(), and the
// same way of keeping it: CMake puts one of the two directories under
// src/internal/profiling/ on the include path (src/ac4core/CMakeLists.txt,
// which reads AC3FORGE_STAGE_TIMERS) and no source asks which it got.
//
// The stage_timers/ sibling is the other answer. planning/ac4.md, D14b, is
// what the markers are for: where the microseconds of one AC-4 frame go on a
// board.

#define AC4_ZONE_SCOPED_N(name)

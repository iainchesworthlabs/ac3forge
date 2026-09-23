#pragma once

#include "ac3/render/layout.hpp"

// The variant of tests/render/abi/speaker_abi.hpp compiled where the struct
// sizes below are known: the MSVC ABI, which clang-cl shares.
//
// A host-only regression guard for a fault a QEMU boot check found but a host
// build cannot see directly. Speaker::small and Speaker::realization were
// deliberately placed to land in Speaker's existing alignment padding rather
// than grow it (see the fields' own comment in layout.hpp) - kMaxSlots copies
// of a bigger Speaker, inside an OutputLayout that PlayerConfig holds by
// value, is what boot-looped the ESP32 example on a FreeRTOS stack overflow
// when kTextBytes grew instead.
//
// This cannot prove the same holds on the Xtensa/GCC target the component
// actually ships on, only on whichever ABI compiles this test. That is still
// worth having: it turns "someone reorders a field and the board boot-loops"
// into "the build fails", the same trade the kTextBytes comment records.
//
// Both variants ship this filename and tests/CMakeLists.txt puts the matching
// directory on the include path, so tests/render/test_layout.cpp includes it
// unconditionally - the numbers are an ABI fact, and a fact that only holds on
// some toolchains is exactly the kind of either/or this tree answers in CMake
// rather than in the preprocessor.

static_assert(sizeof(ac3::render::Speaker) == 32,
              "Speaker grew - see its field ordering comment in layout.hpp");
static_assert(sizeof(ac3::render::OutputLayout) == 616,
              "OutputLayout grew - kMaxSlots copies of this live on tight ESP32 stacks");

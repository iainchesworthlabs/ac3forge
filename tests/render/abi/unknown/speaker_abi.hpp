#pragma once

// The variant of tests/render/abi/speaker_abi.hpp compiled on every ABI whose
// Speaker/OutputLayout sizes this repository has not written down - which is
// every toolchain except MSVC and clang-cl.
//
// Deliberately empty of assertions. See the msvc/ copy for what the guard is
// protecting (Speaker::small and Speaker::realization living in existing
// padding, and the ESP32 stack overflow that made it matter) and why one ABI's
// numbers are worth asserting even though they prove nothing about the Xtensa
// target the component ships on. Writing a second set of numbers here would
// mean maintaining sizes for an ABI nobody has measured; asserting the MSVC
// ones everywhere would fail the GCC and Clang legs for no defect.
//
// Both variants ship this filename; tests/CMakeLists.txt puts the matching
// directory on the include path.

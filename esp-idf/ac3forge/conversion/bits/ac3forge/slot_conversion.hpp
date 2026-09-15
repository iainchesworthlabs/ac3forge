#pragma once

// The conversion ac3forge/interleave.hpp's interleaves use by default, for a
// part with no floating-point unit: BitConversion, which computes to_pcm16's
// and to_slot_24in32's results from the float's bits in integer arithmetic.
//
// One of two headers at this path. The component's CMakeLists.txt puts this
// directory on the include path when CONFIG_SOC_CPU_HAS_FPU is not set, which
// of the parts this component is built for means the ESP32-C3 and the
// ESP32-C6, and conversion/float/ otherwise.
// interleave.hpp says what the two compute and why they give the same integers.

namespace ac3forge {

struct BitConversion;
using SlotConversion = BitConversion;

}  // namespace ac3forge

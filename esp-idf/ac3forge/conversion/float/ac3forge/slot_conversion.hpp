#pragma once

// The conversion ac3forge/interleave.hpp's interleaves use by default, for a
// part with a floating-point unit: FloatConversion, which is to_pcm16 and
// to_slot_24in32 scaling, clipping and truncating in float.
//
// One of two headers at this path. The component's CMakeLists.txt puts this
// directory on the include path when CONFIG_SOC_CPU_HAS_FPU is set, which of
// the parts this component is built for is the ESP32-S3, and conversion/bits/
// otherwise. The host tests use this one and name BitConversion where they test
// the other. interleave.hpp says what the two compute and why a part without an
// FPU wants the other one.

namespace ac3forge {

struct FloatConversion;
using SlotConversion = FloatConversion;

}  // namespace ac3forge

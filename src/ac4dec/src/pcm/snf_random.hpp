#pragma once

#include <cstdint>

// The random number generator of ETSI TS 103 190-1 V1.4.1 clause 5.2.8.3
// (Pseudocode 54's state, Pseudocode 57's GetRandomNoiseValue()), as the
// audio spectral frontend's noise fill uses it: reset from the frame's
// sequence_counter by Pseudocode 24's ResetRandGenStateSnf() (clause 5.1.4.2).
//
// Pseudocode 57 updates two counters with `x = x++`, which C leaves undefined
// and C++17 makes a no-op; it is read as an increment, the only reading under
// which Pseudocode 24's closed form lands where stepping the generator from
// Pseudocode 55's reset state lands (src/ac4dec/ERRATA.md, "x = x++ in
// Pseudocode 57").

namespace ac4::detail {

struct RandGenState {
    // Pseudocode 54: every field is modulo 256.
    std::uint8_t offset_a = 0;     // uiOffsetA
    std::uint8_t offset_b = 0;     // uiOffsetB
    std::uint8_t state_idx = 1;    // uiStateIdx
    std::uint8_t current_idx = 0;  // uiCurrentIdx
};

// Pseudocode 24.
[[nodiscard]] RandGenState reset_rand_gen_state_snf(int sequence_counter) noexcept;

// Pseudocode 57: the sum of two entries of RANDOM_NOISE_TABLE, in float32 as
// the pseudocode declares it, then the state update.
[[nodiscard]] float get_random_noise_value(RandGenState& state) noexcept;

// Pseudocode 57's state update alone, which the tests step against
// Pseudocode 24's closed form.
void advance(RandGenState& state) noexcept;

}  // namespace ac4::detail

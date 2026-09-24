#include "pcm/snf_random.hpp"

#include "tables/noise_tables.hpp"

namespace ac4::detail {

RandGenState reset_rand_gen_state_snf(int sequence_counter) noexcept {
    // Pseudocode 24, in int as it is written; sequence_counter is 10 bits, and
    // `% 256` of a negative value is not something the syntax can produce.
    const int counter = sequence_counter < 0 ? 0 : sequence_counter;
    const int start = 255 * (counter % 256);
    RandGenState state;
    const int offset_a = start % 255;
    const int offset_b = (start / 255) % 256;
    state.offset_a = static_cast<std::uint8_t>(offset_a);
    state.offset_b = static_cast<std::uint8_t>(offset_b);
    int state_idx = (offset_a + 1) * ((offset_a + 2) + 2 * offset_b) / 2 + 255 * offset_b * (255 + offset_b) / 2;
    if (start % 130560 >= 65280) {
        state_idx += 128;
    }
    state.state_idx = static_cast<std::uint8_t>(state_idx % 256);
    const int current_idx = offset_a * (offset_a + 1) / 2 + offset_b * 32386;
    state.current_idx = static_cast<std::uint8_t>(current_idx % 256);
    return state;
}

void advance(RandGenState& state) noexcept {
    // uint8_t arithmetic promotes to int; each store truncates modulo 256,
    // which is Pseudocode 54's "implicit modulo 256".
    state.offset_a = static_cast<std::uint8_t>(state.offset_a + 1);
    state.state_idx = static_cast<std::uint8_t>(state.state_idx + 1);
    if (state.offset_a == 255) {
        state.current_idx = static_cast<std::uint8_t>(state.current_idx + 1);
        state.offset_b = static_cast<std::uint8_t>(state.offset_b + 1);
        state.offset_a = 0;
    }
    state.current_idx = static_cast<std::uint8_t>(state.current_idx + state.offset_a);
    state.state_idx = static_cast<std::uint8_t>(state.state_idx + state.offset_b + state.offset_a);
}

float get_random_noise_value(RandGenState& state) noexcept {
    const float first = tables::kRandomNoiseTable[state.current_idx];
    const float second = tables::kRandomNoiseTable[state.state_idx];
    const float result = first + second;
    advance(state);
    return result;
}

}  // namespace ac4::detail

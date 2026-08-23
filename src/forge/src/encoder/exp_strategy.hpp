#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

// Exponent-set change detection, shared by both encoders' §8.2.8 reuse-span
// planners (AC-3's encoder.cpp and E-AC-3's eac3_frame.cpp).
//
// The strategy that a chosen span earns is a spec rule and lives with the
// rest of the exponent pipeline (ac3::strategy_for_span, core/exponents.hpp).
// What follows is the other half of the plan - WHERE the spans end - and that
// is a judgement about cost rather than anything the standard states, which
// is why it sits here in the encoder's own headers.
//
// Internal to src/forge/src/encoder/ on purpose, the same way snr_search.hpp
// is: plumbing between the two encoder translation units, not library surface.

namespace ac3::internal {

// §8.2.8: "when the variation exceeds a threshold, new exponents will be
// sent".
//
// The threshold is a judgement about COST, so it is not one number. A full-
// bandwidth channel's set is 4 + 7*ngrps bits - about 590 at D15 over a
// 250-coefficient band - and spending that mid-frame has to buy back more
// than it costs, so it waits for the exponents to have really moved: a mean
// change above two steps, 12 dB per bin.
//
// The LFE's set is always two groups, 18 bits, thirty times cheaper. Holding
// it to the same bar means almost never refreshing it, and the frame's one
// set is then the per-bin minimum across six blocks - a scale chosen by the
// loudest of them. Any block quieter than that is quantized against the wrong
// scale for the sake of not spending 18 bits. So the LFE refreshes as soon as
// its exponents move at all, which is the trade its own cost argues for.
[[nodiscard]] inline bool needs_new_exponents(std::span<const std::uint8_t> current,
                                              std::span<const std::uint8_t> reference,
                                              bool is_lfe) {
    long long diff = 0;
    for (std::size_t i = 0; i < current.size(); ++i) {
        diff += std::abs(static_cast<int>(current[i]) - static_cast<int>(reference[i]));
    }
    return diff > (is_lfe ? 0 : 2 * static_cast<long long>(current.size()));
}

}  // namespace ac3::internal

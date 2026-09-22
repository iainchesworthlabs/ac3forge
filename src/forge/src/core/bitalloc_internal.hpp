#pragma once

#include <array>
#include <span>

#include "ac3/core/bitalloc.hpp"

// compute_bit_allocation, with its internal §7.2.2.5 masking curve exposed to
// the caller - the one quantity that routine derives and then discards, and
// the piece roadmap AP12's research trace export needs that exponents/bap
// alone do not carry. Internal to src/forge/src/ (decoder.cpp and
// eac3_decoder.cpp are its only two callers, feeding
// verify::StreamTrace::mask/Eac3StreamTrace::mask), matching the convention
// src/decoder/gain.hpp and src/encoder/snr_search.hpp already use for
// cross-translation-unit plumbing that is not library surface.
//
// A second entry point rather than a parameter added to the public, exported
// ac3::compute_bit_allocation: that function's signature is part of this
// library's ABI, and growing it would change every consumer's link
// requirement for a facility only the trace has any use for. See
// CONTRIBUTING.md's ABI-gate note and tools/ci/abi-allowlist/.

namespace ac3::internal {

// Same contract as ac3::compute_bit_allocation, plus `mask` - always written
// in full, on every path that function itself takes (including its two
// early-exit, all-zero cases), never left holding a previous call's values.
void compute_bit_allocation_traced(std::span<const std::uint8_t> exps, SampleRate sample_rate,
                                   const BitAllocCodes& codes, int csnroffst, int fsnroffst,
                                   std::span<std::uint8_t> bap, const BitAllocRegion& region,
                                   std::array<int, 50>& mask);

}  // namespace ac3::internal

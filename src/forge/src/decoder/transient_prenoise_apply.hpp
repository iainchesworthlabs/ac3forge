#pragma once

#include <span>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/transient_prenoise.hpp"

// The decoder's own way into ac3::apply_transient_prenoise: the same
// correction, with the synthesis buffer supplied by the caller instead of
// allocated per call, so a stream whose transients the decoder corrects a few
// times a second does not allocate for each of them.

namespace ac3::internal {

// The most §3.7.2's synthesis buffer holds: 2*TC1 + pnlen, pnlen at most two
// blocks less transprocloc's four-sample step.
inline constexpr int kTransientPrenoiseMaxSynthesis =
    2 * kTransientPrenoiseTC1 + 2 * kSamplesPerBlock - 4;  // 1020

// ac3::apply_transient_prenoise, same contract, with `synthesis` holding at
// least kTransientPrenoiseMaxSynthesis samples of scratch.
void apply_transient_prenoise(std::span<float> pcm, int transloc, int translen,
                              std::span<float> synthesis);

}  // namespace ac3::internal

#pragma once

#include <array>
#include <span>

#include "frame/metadata.hpp"

// Dialogue enhancement's parameters in the channel-independent method (ETSI TS
// 103 190-1 V1.4.1 clause 5.7.8.4), which raises channel m by g p m in each
// band: for p to raise the dialogue d in m by g d, p is the dialogue's share
// of the channel, <d, m> / <m, m>, per band of Table 173, on Table 209's
// scale.

namespace ac4::detail {

// Table 209's channel-independent value nearest `p`, as its index: 0 to 1.5
// in steps of 0.1, 1.75, 2, then 2.5 to 9 in steps of 0.5.
[[nodiscard]] int de_parameter_index(double p) noexcept;

// Table 209's value of an index.
[[nodiscard]] double de_parameter_value(int index) noexcept;

// One channel's parameters for a frame, from the long-block spectra of the
// channel and of the dialogue in it (Analysis::transform's lines,
// `frame_length` of them), band by band: Table 173's bands are QMF subbands,
// frame_length / 64 lines each. A band with no energy takes 0.
[[nodiscard]] std::array<int, kDeBands> de_parameters(std::span<const double> channel,
                                                      std::span<const double> dialogue,
                                                      int frame_length) noexcept;

}  // namespace ac4::detail

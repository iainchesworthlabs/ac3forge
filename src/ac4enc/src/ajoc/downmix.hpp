#pragma once

#include <array>
#include <span>
#include <vector>

// A-JOC's downmix from the encoder's side, which ETSI TS 103 190-2 V1.3.1
// leaves to it (p. 161): the groups of objects a computed downmix sums, the
// gains a static 5.X bed pans an object with, and the var_channel_element()
// track each downmix signal takes. src/ac4enc/ERRATA.md, "A-JOC's downmix",
// records the readings.

namespace iclforge::ac4::detail {

// The computed downmix's groups: for each object at `positions` (Annex F.2's
// X, Y and Z), the downmix signal it is summed into, of `signals`. The objects
// are taken in the order of their azimuth about the room's centre, from the
// back left through the front to the back right, and cut into `signals` runs
// as equal as can be, the first ones the longer; objects at one azimuth keep
// their order.
[[nodiscard]] std::vector<int> downmix_groups(std::span<const std::array<double, 3>> positions,
                                              int signals);

// A static 5.X bed's gains for an object at `position`, onto L, R, C, Ls and
// Rs: between the front and the back row by Y, and along each row by X,
// between L, C and R in front and Ls and Rs behind, each pair by the sine and
// cosine of a quarter turn, so that the gains' squares sum to 1. Z is left
// out.
[[nodiscard]] std::array<double, 5> static_downmix_gains(const std::array<double, 3>& position) noexcept;

// Pseudocode 14a from the writer's side: the full-band track of
// var_channel_element() (0 up, in the syntax's order, the LFE not counted)
// that A-JOC input `i` reads, of `m` full-band downmix signals.
[[nodiscard]] int ajoc_input_track(int i, int m) noexcept;

}  // namespace iclforge::ac4::detail

#pragma once

#include <array>
#include <span>
#include <vector>

#include "asf/coder.hpp"
#include "asf/stereo.hpp"

// The channel data elements of the 5.X and 7.X elements run backwards: ETSI
// TS 103 190-1 V1.4.1 clause 5.3.3's matrices for three_channel_data(),
// four_channel_data() and five_channel_data() (Tables 178 and 179 and clause
// 5.3.3.4) taken apart into the 2 x 2 steps they cascade, each with the
// parameter set (a, b, c, d) of one chparam_info(). A step's inverse is a
// channel pair's stereo processing (asf/stereo.hpp), so the encoder turns a
// unit's output channels into its tracks by undoing the decoder's steps from
// the last back to the first, choosing each step's parameters by the bits
// they save, as a pair's are chosen.
//
// Table 178, for each chel_matsel: step 1, with set 0, turns tracks p and q
// into X = a0 Ip + b0 Iq and Y = c0 Ip + d0 Iq. One of X and Y is an output as
// it stands; step 2, with set 1, takes the other with the third track r, as
// (X, Ir) where Y is the output and as (Ir, Y) where X is, and its two rows are
// the other two outputs. Each printed matrix is one such cascade, read here
// off the table's entries; test_ac4enc_multichannel.cpp holds all twelve to
// the printed table.
//
// Clause 5.3.3.4: set 0 turns (I0, I1) into (P0, P1) and set 1 (I2, I3) into
// (Q0, Q1); set 2 turns (P0, Q0) into (O0, O2) and set 3 (P1, Q1) into
// (O1, O3).
//
// Table 179: Table 178's matrix of the same chel_matsel on I0 to I2 gives T0
// to T2; set 2 turns (I3, I4) into (U0, U1); set 3 turns (T0, U0) into
// (O0, O3) and set 4 (T1, U1) into (O1, O4); O2 is T2.

namespace ac4::detail {

// A channel, or a track, as the rate loop codes it: its lines, grouped, and
// the noise each band may carry.
struct Channel {
    Grouped grouped{};
    std::vector<std::vector<double>> allowed{};
};

// One chel_matsel's cascade, as above.
struct ThreeChannelCascade {
    int p = 0;
    int q = 1;
    bool x_out = false;  // X is output `out` and step 2 takes (Ir, Y); else Y is, and step 2 takes (X, Ir)
    int out = 1;
    std::array<int, 2> step2_out{0, 2};  // the outputs of step 2's first and second rows
    int r = 2;
};

[[nodiscard]] const ThreeChannelCascade& three_channel_cascade(int chel_matsel);

// What a unit's matrix was made of.
struct UnitChoice {
    int chel_matsel = 0;
    std::vector<StereoChoice> sets{};  // the chparam_info()s in the syntax's order, set 0 first
    // The tracks' perceptual entropy and the chparam_info()s' and chel_matsel's
    // bits.
    double bits = 0.0;
};

// Each turns a unit's channels, given in the order of the matrix's outputs
// (O0, O1, ...), into its tracks (I0, I1, ...) in place. A step's parameters
// are chosen as choose_stereo() chooses a pair's, or are `forced`'s set of
// the same number where `forced` holds the unit's sets.
[[nodiscard]] UnitChoice undo_pair(std::array<Channel*, 2> unit, std::span<const StereoChoice> forced = {});
[[nodiscard]] UnitChoice undo_three(int chel_matsel, std::array<Channel*, 3> unit,
                                    std::span<const StereoChoice> forced = {});
[[nodiscard]] UnitChoice undo_four(std::array<Channel*, 4> unit, std::span<const StereoChoice> forced = {});
[[nodiscard]] UnitChoice undo_five(int chel_matsel, std::array<Channel*, 5> unit,
                                   std::span<const StereoChoice> forced = {});

}  // namespace ac4::detail

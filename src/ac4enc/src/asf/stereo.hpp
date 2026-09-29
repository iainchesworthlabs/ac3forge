#pragma once

#include <cstddef>
#include <vector>

#include "asf/coder.hpp"
#include "bit_writer.hpp"

// Stereo processing for a channel pair coded with one sf_info() (ETSI TS 103
// 190-1 V1.4.1 stereo_data() with b_enable_mdct_stereo_proc, Table 23, and
// chparam_info() and sap_data(), Tables 47 and 48). Clause 5.3.2's matrix
// turns the two tracks X0 and X1 back into L and R, per band:
//
//   left and right   L = X0,               R = X1
//   M/S              L = X0 + X1,          R = X0 - X1
//   prediction       L = (1 + a) X0 + X1,  R = (1 - a) X0 - X1
//
// with a = 0.1 alpha_q. So M/S sends M = (L + R)/2 and S = (L - R)/2, and the
// prediction sends M and the part of S that a M does not predict, S - a M,
// with a chosen per pair of bands to leave the least of S behind. A panned
// source is predicted whole: its S is a multiple of its M.
//
// A band's cost is its perceptual entropy: the bits a band needs grow with
// log2 of its energy over the noise it may carry. Noise in the tracks reaches
// the outputs through the matrix, so each track's allowance is the smaller of
// the two outputs' allowances over the gain to it. Both tracks' noise adds in
// each output, 3 dB over that allowance at most; halving it instead cost the
// race 1 dB of SNR on music at 192 kbps. Each frame takes the sap_mode whose
// bands and side information cost least.

namespace ac4::detail {

struct StereoChoice {
    int sap_mode = 0;                        // Table 47: 0 L/R, 1 M/S per band, 2 all M/S, 3 prediction
    std::vector<std::vector<bool>> ms_used;  // sap_mode 1: per group and band
    // sap_mode 3, per group and pair of bands (index sfb / 2): whether the pair
    // is predicted, and its alpha_q.
    bool sap_coeff_all = false;
    std::vector<std::vector<bool>> sap_used;
    std::vector<std::vector<int>> alpha_q;
};

// Chooses per band, turns the chosen bands of `left` and `right` into the
// tracks X0 and X1 in place, and changes the allowed noise of those bands to
// match.
[[nodiscard]] StereoChoice choose_stereo(Grouped& left, Grouped& right, std::vector<std::vector<double>>& allowed_left,
                                         std::vector<std::vector<double>>& allowed_right);

// What choose_stereo() does once it has chosen: the bands `choice` codes as
// M/S or predicted turned into the tracks in place, and their allowed noise
// with them.
void apply_stereo(Grouped& left, Grouped& right, std::vector<std::vector<double>>& allowed_left,
                  std::vector<std::vector<double>>& allowed_right, const StereoChoice& choice);

// A coupled pair of the immersive element in SCPL and ASPX_SCPL (ETSI TS 103
// 190-2 V1.3.1 clauses 5.2.3.2 and 5.3), `left` and `right` its two channels
// over sqrt 2 (Ls and Lb, say): simple coupling makes them of the pair's sum
// and difference, so every band is M/S, M = D'' and S = H'', and Table 20's
// chparam_info() predicts S from M per pair of bands as full SAP does, H' = S
// - a M. The pair is turned into M and S - a M in place, their allowed noise
// with them, and the chparam_info() returned: sap_mode 3 where a pair of bands
// is predicted, and 0, a of 0 throughout, where none is.
[[nodiscard]] StereoChoice choose_coupled(Grouped& left, Grouped& right,
                                          std::vector<std::vector<double>>& allowed_left,
                                          std::vector<std::vector<double>>& allowed_right);

// A track's cost as choose_stereo() weighs it: its bands' perceptual entropy,
// in bits.
[[nodiscard]] double perceptual_entropy(const Grouped& track, const std::vector<std::vector<double>>& allowed);

// chparam_info() and sap_data() for the choice, over the groups' max_sfb.
// delta_code_time is always 0: each alpha_q is sent against the pair below it.
void write_chparam_info(BitWriter& w, const StereoChoice& choice);

[[nodiscard]] std::size_t chparam_info_bits(const StereoChoice& choice);

}  // namespace ac4::detail

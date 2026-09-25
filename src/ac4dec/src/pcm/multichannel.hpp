#pragma once

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "pcm/stereo.hpp"
#include "syntax/asf.hpp"
#include "syntax/context.hpp"

// Multichannel processing, ETSI TS 103 190-1 V1.4.1 clause 5.3.3: the
// matrices that turn the tracks of three_channel_data(), four_channel_data()
// and five_channel_data() into their outputs, O = M I, built tile by tile from
// the element's chparam_info() parameters (clause 5.3.2, pcm/stereo.hpp):
//
//   three tracks   Table 178, by chel_matsel, from parameter sets 0 and 1;
//   four tracks    clause 5.3.3.4's matrix, from sets 0 to 3;
//   five tracks    Table 179, by chel_matsel, from sets 0 to 4.
//
// The tracks of one of these elements share one sf_info(), so a band of a
// window group is one run of lines in each, in bitstream order
// (pcm/asf_reconstruct.hpp).
//
// Each matrix is a cascade of 2 x 2 steps. Table 179 is written here as one:
// for every chel_matsel its matrix is Table 178's for the same chel_matsel on
// I0 to I2, whose rows T0 to T2 then meet I3 and I4 as
//
//   U0 = a2 I3 + b2 I4,   U1 = c2 I3 + d2 I4,
//   O0 = a3 T0 + b3 U0,   O3 = c3 T0 + d3 U0,
//   O1 = a4 T1 + b4 U1,   O4 = c4 T1 + d4 U1,   O2 = T2.
//
// tests/ac4dec/test_ac4dec_multichannel.cpp holds the table's 300 printed
// entries and checks every one against this.

namespace ac4::detail {

using Abcd = std::array<double, 4>;

template <std::size_t N>
using Matrix = std::array<std::array<double, N>, N>;

// Table 178. Nothing for chel_matsel 12 to 15, which the table does not define.
[[nodiscard]] std::optional<Matrix<3>> three_channel_matrix(int chel_matsel, const Abcd& p0, const Abcd& p1);

// Clause 5.3.3.4.
[[nodiscard]] Matrix<4> four_channel_matrix(std::span<const Abcd, 4> p);

// Table 179. Nothing for chel_matsel 12 to 15.
[[nodiscard]] std::optional<Matrix<5>> five_channel_matrix(int chel_matsel, std::span<const Abcd, 5> p);

// The matrix of a three, four or five track element, band by band, on the
// tracks' lines in bitstream order. `parameters` holds the element's
// chparam_info() parameters in order, two, four or five of them;
// `chel_matsel` is ignored for four tracks. Fails for a chel_matsel the
// tables do not define. Bands at or above a group's max_sfb hold no lines and
// are left as they are.
[[nodiscard]] ParseResult apply_channel_data(const SfInfo& info, const SfData& layout, int chel_matsel,
                                             std::span<const StereoParameters> parameters,
                                             std::span<std::vector<double>* const> tracks);

// One of Table 183's two 2 x 2 steps, which make the 7.X element's last two
// channels of the tracks of two different channel data elements: O0 = a I0 +
// b I1, O1 = c I0 + d I1, on the tracks' lines in window order (after
// ungrouping), with the parameters of a chparam_info() read under `base`'s
// sf_info(), the first input's. The immersive element's step 4 and Table 20
// (ETSI TS 103 190-2 V1.3.1 clause 5.2.3.2) are such steps too. The inputs
// must be transformed alike, window for window (src/ac4dec/ERRATA.md, "The 7.X
// element's additional channels"); it fails otherwise. `lengths` are each
// input's window lengths (window_lengths()).
[[nodiscard]] ParseResult apply_additional_pair(const SubstreamContext& ctx, const AsfPsyInfo& base,
                                                const StereoParameters& parameters,
                                                std::span<const int> base_lengths,
                                                std::span<const int> other_lengths, std::span<double> base_lines,
                                                std::span<double> other_lines);

}  // namespace ac4::detail

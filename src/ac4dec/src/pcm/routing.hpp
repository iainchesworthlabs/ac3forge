#pragma once

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "ac4dec/decoder.hpp"
#include "syntax/channel_elements.hpp"
#include "syntax/context.hpp"

// Where the tracks of a channel element go, ETSI TS 103 190-1 V1.4.1 clause
// 5.3.4: the single channel element's one track is C (5.3.3.1); a pair's
// stereo_data() makes L and R, and ASPX_ACPL_2's one track is L (5.3.4.1);
// the 3.0 element's L, R and C (5.3.4.2); the 5.X element's by Table 180,
// Table 181 in ASPX_ACPL_1 and 2, and 5.3.4.3.3 in ASPX_ACPL_3; the 7.X
// element's by Table 182, with Table 183's two last steps, and by Tables 184
// and 185 in the A-CPL modes (5.3.4.4.2 and 5.3.4.4.3); an LFE's mono_data(1)
// is the LFE, and the tables number the tracks after it (src/ac4dec/ERRATA.md,
// "The LFE's track is not numbered in Tables 180 and 182").
//
// The channels the A-CPL modes leave without a track (R of a pair in
// ASPX_ACPL_2, the surrounds of the 5.X element in ASPX_ACPL_2, all but L and
// R in ASPX_ACPL_3, the 7.X element's last pair in ASPX_ACPL_2) are 0 until
// A-CPL makes them in the QMF domain. ASPX_ACPL_1's two residuals are coded
// against the channels their A-CPL modules pair them with: 5.3.4.3.2's matrix
// in the 5.X element, (L, Ls) = P0 (A, s3) and (R, Rs) = P1 (B, s4), and the
// same step on Table 202's pairs in the 7.X element, each residual's
// chparam_info() read under the residual's own sf_info() (src/ac4dec/
// ERRATA.md, "ASPX_ACPL_1: the framing of the residuals").
//
// Table 182 names the 7.X element's outputs A to G before Table 183 makes
// channels of them. With Table 183's parameters at identity (a = d = 1, b =
// c = 0, as b_use_sap_add_ch unset makes them) A is L, B is R, C is C, D is Ls,
// E is Rs, and F and G are the channel mode's last pair - Lb and Rb, Lw and
// Rw, or Tfl and Tfr - in every one of the three modes. The route names the
// outputs by those channels, and Table 183's steps then mix the pairs it
// pairs: (Ls, Lb) and (Rs, Rb) in 3/4/0, (L, Lw) and (R, Rw) in 5/2/0, (L, Tfl)
// and (R, Tfr) in 3/2/2.

namespace ac4::detail {

// The channels decode() writes for a channel mode, in order: L, R, C, the LFE,
// Ls, Rs, then a 7.X mode's last pair. Empty for a mode no element here turns
// into PCM.
[[nodiscard]] std::span<const Speaker> speakers_of(int ch_mode) noexcept;

// One channel data element (or an LFE's mono_data), in syntax order.
struct DataElementRoute {
    int count = 1;                     // tracks: 1, 2, 3, 4 or 5
    int first_track = 0;               // its first, in ChannelElement::tracks
    std::array<Speaker, 5> outputs{};  // where O0, O1, ... go
    // Whether a matrix applies: a pair's b_enable_mdct_stereo_proc, and
    // always for three to five tracks.
    bool processed = false;
    int first_chparam = 0;             // its chparam_info()s, in ChannelElement::chparams
    int chel_matsel = 0;               // for three and five tracks
};

// One of Table 183's steps, or an ASPX_ACPL_1 residual's: (first, second) =
// P (first, second), in window order, P from the chparam_info() at `chparam`,
// read under the sf_info() of the channel `framing` names.
struct PairStep {
    Speaker first = Speaker::kLeft;
    Speaker second = Speaker::kRight;
    int chparam = 0;
    Speaker framing = Speaker::kLeft;
};

struct ElementRoute {
    std::vector<DataElementRoute> data;
    // The 7.X element's Table 183 steps, when b_use_sap_add_ch sends their
    // parameters (at identity they change nothing and are left out), and the
    // ASPX_ACPL_1 residuals' steps.
    std::vector<PairStep> steps;
    // The channels no track reaches in this codec mode.
    std::vector<Speaker> silent;
};

// The route of `element`, read under `ctx`. Fails when the element's parts do
// not add up to what its codec mode and coding_config name: that many tracks,
// chparam_info()s, stereo flags and chel_matsel values.
[[nodiscard]] ParseResult route_element(const SubstreamContext& ctx, const ChannelElement& element,
                                        ElementRoute& out);

// The A-SPX data of an element, by Part 1 Table 213: each aspx_data_1ch() or
// aspx_data_2ch() and the channels it carries, in syntax order, for the
// channel mode's element in `codec_mode`; empty in SIMPLE. `index` counts
// within aspx_1ch or aspx_2ch.
struct AspxUnit {
    bool pair = false;
    int index = 0;
    std::array<Speaker, 2> speakers{};
};
[[nodiscard]] std::vector<AspxUnit> aspx_units(int ch_mode, int codec_mode);

// Table 212: the channels companding_control() lists, in its order, for the
// channel mode's element in `codec_mode`. Empty where that mode sends none
// (SIMPLE, and the 7.X element's ASPX).
[[nodiscard]] std::vector<Speaker> companded_speakers(int ch_mode, int codec_mode);

}  // namespace ac4::detail

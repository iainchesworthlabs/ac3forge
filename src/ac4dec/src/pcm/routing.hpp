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
//
// The immersive element (ETSI TS 103 190-2 V1.3.1 clause 5.2.3) assigns its
// tracks to the intermediate signals A to K by Part 2 Table 19, whose track
// numbers are labels: each channel data element's outputs go where its row
// says, in the order the syntax reads the elements. Step 4's two steps then mix
// (D, F) and (E, G) where b_use_sap_add_ch sends their parameters, and Table
// 20's four predict H, I, J and K from D, E, F and G. None of those signals is
// a channel until S-CPL, A-CPL or A-JCC makes one of it; each is held in the
// channel it becomes, read from Tables 23, 8 and 25 together: A'' in L, B'' in
// R, C'' in C, D'' in Ls, E'' in Rs, F'' in Tfl, G'' in Tfr, H'' in Lb, I'' in
// Rb, J'' in Tbl and K'' in Tbr. Core decoding keeps A'' to G'' (Table 24), F''
// and G'' in the core's Tsl and Tsr, and reads H to K without decoding them.
// src/ac4dec/ERRATA.md, "The immersive element", records the readings.

namespace ac4::detail {

// The channels decode() writes for a channel mode in a decoding mode, in
// order: L, R, C, the LFE, Ls, Rs, then a 7.X mode's last pair, or the 7.X.4
// modes' Lb, Rb, Tfl, Tfr, Tbl and Tbr in full decoding and their core's Tsl
// and Tsr in core decoding (Part 2 clause 4.7). The Part 1 modes' are the same
// in both. Empty for a mode no element here turns into PCM.
[[nodiscard]] std::span<const Speaker> speakers_of(
    int ch_mode, DecodingMode decoding = DecodingMode::kFull) noexcept;

// Whether `ch_mode` is one of the 7.X.4 modes, whose element is the immersive
// element.
[[nodiscard]] bool is_immersive(int ch_mode) noexcept;

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
    // Tracks the decoding mode reads and does not decode: the immersive
    // element's H to K in core decoding. `outputs` is then unused.
    bool discarded = false;
};

// One of Table 183's steps, or an ASPX_ACPL_1 residual's: (first, second) =
// P (first, second), in window order, P from the chparam_info() at `chparam`,
// read under the sf_info() of the channel `framing` names. With `prediction`,
// Part 2 Table 20's: second += a' first, a' the chparam_info()'s prediction
// gain (stereo_parameters()' StereoUse::kPrediction), first unchanged.
struct PairStep {
    Speaker first = Speaker::kLeft;
    Speaker second = Speaker::kRight;
    int chparam = 0;
    Speaker framing = Speaker::kLeft;
    bool prediction = false;
};

struct ElementRoute {
    std::vector<DataElementRoute> data;
    // The 7.X element's Table 183 steps, when b_use_sap_add_ch sends their
    // parameters (at identity they change nothing and are left out), and the
    // ASPX_ACPL_1 residuals' steps; the immersive element's step 4 and Table
    // 20, in the order they apply.
    std::vector<PairStep> steps;
    // The channels no track reaches in this codec mode.
    std::vector<Speaker> silent;
};

// The route of `element`, read under `ctx` and decoded in `decoding`. Fails
// when the element's parts do not add up to what its codec mode and
// coding_config (or core_5ch_grouping) name: that many tracks,
// chparam_info()s, stereo flags and chel_matsel values.
[[nodiscard]] ParseResult route_element(const SubstreamContext& ctx, const ChannelElement& element,
                                        ElementRoute& out,
                                        DecodingMode decoding = DecodingMode::kFull);

// The A-SPX data of an element, by Part 1 Table 213 and Part 2 Table 8: each
// aspx_data_1ch() or aspx_data_2ch() and the channels it carries, in syntax
// order, for the channel mode's element in `codec_mode` (an immersive_mode
// value for the 7.X.4 modes); empty where the mode sends none. `index` counts
// within aspx_1ch or aspx_2ch. With `first_only`, a pair of which decoding
// takes the first channel alone (Table 8's square brackets, core decoding in
// ASPX_SCPL); `speakers[1]` then names the channel full decoding gives the
// second, which the decoder does not have.
struct AspxUnit {
    bool pair = false;
    int index = 0;
    std::array<Speaker, 2> speakers{};
    bool first_only = false;
};
[[nodiscard]] std::vector<AspxUnit> aspx_units(int ch_mode, int codec_mode,
                                               DecodingMode decoding = DecodingMode::kFull);

// Table 212: the channels companding_control() lists, in its order, for the
// channel mode's element in `codec_mode`. Empty where that mode sends none
// (SIMPLE, and the 7.X element's ASPX); for the immersive element L, R, C, Ls
// and Rs in ASPX_AJCC, the one mode that sends it (Part 2 clause 4.8.3.10.3).
[[nodiscard]] std::vector<Speaker> companded_speakers(int ch_mode, int codec_mode);

}  // namespace ac4::detail

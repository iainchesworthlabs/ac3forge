#pragma once

#include <span>
#include <vector>

#include "ac4dec/decoder.hpp"
#include "pcm/aspx.hpp"

// The immersive element's tools beside A-CPL and A-JCC, ETSI TS 103 190-2
// V1.3.1: simple coupling (S-CPL, clause 5.3), in the time domain between the
// IMDCT and QMF analysis, and the gains the QMF domain applies after A-SPX -
// full decoding's in ASPX_SCPL (clause 4.8.3.11.3, Table 10), core decoding's
// in ASPX_SCPL with the A-SPX post-processing (clauses 4.8.3.11.2 and 5.4), and
// the gain core decoding applies in place of A-CPL (clause 4.8.3.14).
//
// The channels hold the intermediate signals A'' to K'' as pcm/routing.hpp's
// header comment gives them until these tools make channels of them.

namespace ac4::detail {

// Clause 5.3.3 on one frame of the IMDCT's output, in place, for an immersive
// element in `codec_mode` (an immersive_mode value): time[c] is the channel
// speakers[c] names. In full decoding (Table 23) L, R and C are c_gain times A'',
// B'' and C'', and each coupled pair (Ls, Lb), (Rs, Rb), (Tfl, Tbl) and (Tfr,
// Tbr) is m_gain (D'' + H'', D'' - H''); in core decoding (Table 24) every
// channel but the LFE is c_gain times its signal. c_gain is 2 in SCPL and 1 in
// ASPX_SCPL, m_gain the square root of 2 and 1. Nothing in the other modes.
void apply_scpl(int codec_mode, DecodingMode decoding, std::span<const Speaker> speakers,
                std::span<std::vector<double>> time);

// The gains the immersive element's QMF domain applies to one channel after
// A-SPX, in its subbands below the sbx of the A-SPX data that carried it and
// from that sbx on.
struct BandGains {
    double low = 1.0;
    double high = 1.0;
};

// For a channel `speaker` of an immersive element in `codec_mode`:
//
//   full decoding, ASPX_SCPL     Table 10: 2 for L, R and C, the square root of
//                                2 for the coupled pairs;
//   core decoding, ASPX_SCPL     clause 4.8.3.11.2: 2 for every channel A-SPX
//                                makes, and in the subbands from sbx of Ls, Rs,
//                                Tsl and Tsr (Table 9) the post-processing's
//                                0.841395 (-1.5 dB) as well;
//   core decoding, ASPX_ACPL_1   clause 4.8.3.14: 2 for every channel but the
//   and ASPX_ACPL_2              LFE, in place of A-CPL;
//
// and 1 elsewhere: full decoding's A-CPL (pcm/acpl.hpp) and A-JCC apply their
// own.
[[nodiscard]] BandGains immersive_gains(int codec_mode, DecodingMode decoding,
                                        Speaker speaker) noexcept;

// `gains` on `num_ts` slots of `matrix`, subbands below `sbx` and from it.
void apply_band_gains(std::span<QmfValue> matrix, int num_ts, int sbx, BandGains gains) noexcept;

}  // namespace ac4::detail

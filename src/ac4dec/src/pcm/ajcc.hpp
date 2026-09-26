#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ac4dec/decoder.hpp"
#include "acpl/acpl.hpp"
#include "ajcc/ajcc.hpp"
#include "pcm/acpl.hpp"
#include "pcm/aspx.hpp"
#include "syntax/ajcc.hpp"

// A-JCC's QMF-domain step, ETSI TS 103 190-2 V1.3.1 clause 5.6, for the
// immersive element in ASPX_AJCC, after A-SPX (4.8.3.12): the five core
// channels, held in L, R, C, Ls and Rs as A'' to E'' (pcm/routing.hpp), made
// into the 7.X.4 layout's eleven channels in full decoding (5.6.3.5.2,
// Pseudocode 8) or the 5.X.2 core's seven in core decoding (5.6.3.5.3,
// Pseudocode 12). A frame's parameters are differentially decoded when the
// frame is read (5.6.3.2), so that a value outside its range refuses the frame
// before anything moves on, and applied d_ctrl frames later with the signal they
// belong to, as A-CPL's are (pcm/acpl.hpp). The core (ajcc/ajcc.hpp) holds the
// dequantisation and the modules' coefficients; A-CPL's core the
// decorrelators, the transient ducker and the interpolation.

namespace ac4::detail {

// ajcc_data()'s fourteen parameters, in syntax order: alpha1, alpha2, beta1,
// beta2, dry1 to dry4, wet1 to wet6. The left module takes alpha1, beta1, dry1,
// dry2 and wet1 to wet3; the right alpha2, beta2, dry3, dry4 and wet4 to wet6.
inline constexpr std::size_t kAjccParams = 14;

// One frame's A-JCC data, differentially decoded: each parameter's quantised
// values, [parameter][set][band].
struct AjccFrameValues {
    int core_mode = 0;  // ajcc_core_mode
    int num_bands = ajcc::kMaxParamBands;
    acpl::Quant quant_ab = acpl::Quant::kFine;  // ajcc_qm_ab
    acpl::Quant quant_dw = acpl::Quant::kFine;  // ajcc_qm_dw
    std::array<acpl::Framing, 2> framing{};     // the left and the right module's
    std::array<std::array<std::array<std::int8_t, ajcc::kMaxParamBands>, ajcc::kMaxParamSets>,
               kAjccParams>
        q{};
};

// Pseudocode 3's ajcc_SET_q_prev: each parameter's quantised values in the
// last parameter set decoded.
struct AjccQuantHistory {
    std::array<std::array<int, ajcc::kMaxParamBands>, kAjccParams> params{};
};

// Clause 5.6.3.2 for `data`, moving `history` on. Fails when a value leaves its
// range: alpha's and beta's A-CPL tables, dry's and wet's F0 codebook.
// `history` is then partly moved on, so the caller passes a copy and keeps it
// only when the frame is kept.
[[nodiscard]] ParseResult ajcc_values(const AjccData& data, AjccQuantHistory& history,
                                      AjccFrameValues& out);

// What A-JCC carries from frame to frame: its decorrelators with their
// transient duckers, the pre-modification's ajcc_core_mode_prev, and
// ajcc_param_prev of every coefficient its modules interpolate.
class AjccStage {
   public:
    AjccStage();

    // The first frame's state: silence in the decorrelators and duckers, every
    // ajcc_param_prev 0 (5.6.3.3), and ajcc_core_mode_prev to be the next
    // frame's ajcc_core_mode.
    void reset();

    // Applies one frame's values to `channels`, which name L, R, C, Ls and Rs
    // and the channels `decoding` makes of them.
    void apply(DecodingMode decoding, const AjccFrameValues& values, int num_ts,
               const AcplChannels& channels);

   private:
    // One module: `side` 0 left, 1 right; `x` its two inputs, `y` its
    // decorrelated ones (three in full decoding, two in core); `z` its
    // outputs, which it writes.
    void module(DecodingMode decoding, std::size_t side, const AjccFrameValues& values, int num_ts,
                std::array<const std::vector<QmfValue>*, 2> x,
                std::array<const std::vector<QmfValue>*, 3> y,
                std::array<std::vector<QmfValue>*, ajcc::kModule2Outputs> z);
    void decorrelate(std::size_t slot, const std::vector<QmfValue>& in, std::vector<QmfValue>& out,
                     int num_ts);

    // Pseudocode 8's decorrelators, D0, D2, D1, D0, D2 and D1, each its own
    // instance; core decoding takes the first two and the fourth and fifth.
    std::array<acpl::Decorrelator<double>, 6> decorrelators_;
    std::array<acpl::TransientDucker<double>, 6> duckers_{};
    ajcc::PreModification<double> pre_;
    // ajcc_param_prev and this frame's values of each module's coefficients,
    // [side][coefficient]: Pseudocode 11's 25 in full decoding, 14's 12 in core.
    std::array<std::array<acpl::ParamPrev, ajcc::kModule2Coefficients>, 2> prev_{};
    std::array<std::array<acpl::ParamSets, ajcc::kModule2Coefficients>, 2> coefficients_{};

    // Scratch, sized once: the five inputs times (2 + 1/sqrt 2) but C's, the
    // two pre-modified ones, a module's decorrelated inputs, and one
    // interpolated coefficient.
    std::array<std::vector<QmfValue>, 4> x_in_{};
    std::array<std::vector<QmfValue>, 2> w_in_{};
    std::array<std::vector<QmfValue>, 3> y_{};
    std::vector<double> interp_;
};

}  // namespace ac4::detail

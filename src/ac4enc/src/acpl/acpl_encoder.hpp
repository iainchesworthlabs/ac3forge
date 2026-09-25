#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

#include "acpl/acpl.hpp"
#include "acpl/acpl_syntax.hpp"
#include "dsp/qmf.hpp"
#include "frame/timing.hpp"

// The encoder's A-CPL: ETSI TS 103 190-1 V1.4.1 clause 5.7.7 run from the
// other side. The channels A-CPL rebuilds are analysed by the decoder's QMF
// bank on the decoder's slot axis (aspx/aspx_encoder.hpp): frame f's
// parameters apply to the slots of the A-SPX interval they share a control
// frame with, at frame_rate_index 13 QMF slots 32 (f + 1) - 6 to 32 (f + 1) +
// 25. Each frame's
// parameters are estimated per parameter band (Table 197) against the upmix of
// Pseudocodes 115 to 119, over 48 slots centred on the frame's last and each
// subband's own band (acpl_encoder.cpp, kBandCentreBin), quantised by Tables
// 203 to 208, and differentially coded against the values the decoder holds,
// along time outside I-frames where that takes fewer bits.
//
// A module rebuilds a pair (a, b) from the group g = a + b its upmix keeps:
// z0 = (1 + alpha) g / 2 + beta y / 2 and z1 = (1 - alpha) g / 2 - beta y / 2,
// with y the ducked decorrelation of the signal the decorrelator takes, which
// has that signal's energy. With d = (a - b) / 2, alpha is the least squares
// prediction of d from g, 2 Re<d, g> / <g, g>, and beta gives y's part the
// energy of what the prediction leaves, 2 sqrt(E_residual / E_decorrelator's
// input). The modes:
//
//   the channel pair (experimental): one module on (L, R), the coded channel
//     x0 = (L + R) / 2 (Pseudocode 115);
//   the 5.X element, ASPX_ACPL_1 and 2: two modules on (L, Ls / sqrt 2) and
//     (R, Rs / sqrt 2), the coded channels (L + Ls / sqrt 2) / 2 and its
//     mirror, which Pseudocode 117's upmix keeps exactly (z0 + z1 / sqrt 2 =
//     2 x0), and C coded as it is;
//   in ASPX_ACPL_1 (experimental), in either, the parameters from
//     acpl_qmf_band up, and below it the residuals of acpl_residuals();
//   the 5.X element, ASPX_ACPL_3: the Lo/Ro downmix over 1 + sqrt 2 coded as a
//     pair, so that Pseudocode 118's l and r are Lo and Ro. gamma5 and gamma6
//     predict C / sqrt 2 from Lo and Ro, and gamma1 to gamma4 follow from them
//     as DEE's streams have them: gamma1 + gamma5 = 1, gamma2 = -gamma6,
//     gamma3 = -gamma5 and gamma4 + gamma6 = 1, in the quantiser's steps. Each
//     pair's module then works on Lo less the predicted centre, and beta3
//     gives the centre the energy the prediction leaves out
//     (src/ac4enc/ERRATA.md, "ASPX_ACPL_3's gammas");
//   the immersive element, ASPX_ACPL_1 and 2 (ETSI TS 103 190-2 V1.3.1
//     clause 5.5.2, Pseudocode 2): four modules on the coupled pairs (Ls, Lb),
//     (Rs, Rb), (Tfl, Tbl) and (Tfr, Tbr), each as the channel pair's on the
//     pair over sqrt 2: the coded channel D'' = (Ls + Lb) / (2 sqrt 2), which
//     Pseudocode 2 doubles into the module and whose outputs it raises by
//     sqrt 2, so that Ls + Lb = 2 sqrt 2 D'' exactly, and ASPX_ACPL_1's
//     residual H'' = (Ls - Lb) / (2 sqrt 2), which below acpl_qmf_band makes
//     the pair as simple coupling does.

namespace ac4::detail {

// The QMF slots each estimate reads, and its DFT's bins (acpl_encoder.cpp).
inline constexpr int kAcplWindowSlots = 48;

enum class AcplLayout : std::uint8_t {
    kPair,       // the channel pair: L R
    kFiveX,      // the 5.X element's ASPX_ACPL_1 and 2: L R C Ls Rs
    kCoupling,   // the 5.X element's ASPX_ACPL_3: L R C Ls Rs
    kImmersive,  // the immersive element's ASPX_ACPL_1 and 2: Ls Lb Rs Rb Tfl Tbl Tfr Tbr
};

// The acpl_data_1ch() modules of a layout: 1, 2 or 4; none for kCoupling.
[[nodiscard]] std::size_t acpl_modules(AcplLayout layout) noexcept;

// A frame's A-CPL data, as the writer takes them.
struct AcplFrameFields {
    // The channel pair's one, the 5.X element's two, or the immersive
    // element's four.
    std::array<AcplData1chFields, 4> modules{};
    AcplData2chFields coupling{};  // ASPX_ACPL_3
};

class AcplEncoder {
   public:
    // `quant_mode` is acpl_quant_mode (acpl_quant_mode_0 and 1 alike);
    // `qmf_band` is ASPX_ACPL_1's acpl_qmf_band, 0 otherwise; `timing` the
    // frame grid.
    AcplEncoder(AcplLayout layout, int num_param_bands_id, int quant_mode, int qmf_band,
                const FrameTiming& timing = {});

    [[nodiscard]] AcplLayout layout() const noexcept { return layout_; }
    [[nodiscard]] AcplConfig1chFields config_1ch() const noexcept;
    [[nodiscard]] AcplConfig2chFields config_2ch() const noexcept;

    // The channels the layout rebuilds, in its order above.
    [[nodiscard]] std::size_t channels() const noexcept { return analyses_.size(); }

    // Analyses slot slots() of each channel, from the 64 samples of the
    // delayed input (full scale 1.0) from 64 slots() - d_pcm.
    void push_slot(std::span<const std::array<double, dsp::kQmfSubbands>> samples);
    [[nodiscard]] long long slots() const noexcept { return first_slot_ + static_cast<long long>(slots_.size()); }

    // The first slot frame f's parameters apply to, and the slot after the
    // last one propose() reads for it.
    [[nodiscard]] long long first_slot(long long frame) const noexcept;
    [[nodiscard]] long long slots_needed(long long frame) const noexcept;

    // Frame f's data from the slots its parameters apply to, which must have
    // been analysed, differentially coded against what the decoder holds.
    // Nothing moves on until commit().
    [[nodiscard]] AcplFrameFields propose(long long frame, bool iframe) const;

    // What a frame whose bits hold no more sends: the values the decoder
    // holds, again, which cost least.
    [[nodiscard]] AcplFrameFields held(bool iframe) const;

    // What a frame sends whose bits hold not even those: in an I-frame,
    // which codes its values whole, the values a stream starts from, 0 in
    // every band, which is what create() checks the rate holds; elsewhere the
    // held values.
    [[nodiscard]] AcplFrameFields least(bool iframe) const;

    // Moves what the decoder holds on to the values sent.
    void commit(const AcplFrameFields& sent);

    // Frees the slots frame f and later do not read.
    void drop_before_frame(long long frame);

   private:
    using Slot = std::array<std::complex<double>, dsp::kQmfSubbands>;
    // A channel's subbands over a frame's estimation window, each as its
    // DFT's bins (acpl_encoder.cpp, kBandCentreBin).
    using Spectrum = std::array<std::array<std::complex<double>, kAcplWindowSlots>, dsp::kQmfSubbands>;
    // A parameter's quantised values per band, [band].
    using Values = std::array<int, acpl::kMaxParamBands>;

    [[nodiscard]] const Slot& slot(std::size_t channel, long long index) const;
    // Each channel's Spectrum for the frame whose first A-CPL slot is `first`.
    [[nodiscard]] std::vector<Spectrum> spectra(long long first) const;
    // One parameter set sent as it costs least: along frequency, or along
    // time from `previous` outside I-frames.
    [[nodiscard]] AcplParamFields code(AcplKind kind, const Values& q, const Values& previous, bool iframe) const;
    // Every parameter set of the layout sent as `modules` or `coupling` has
    // it, against the values held.
    [[nodiscard]] AcplFrameFields sent_as(const std::array<std::array<Values, 2>, 4>& modules,
                                          const std::array<Values, 11>& coupling,
                                          bool iframe) const;

    AcplLayout layout_;
    FrameTiming timing_;
    int num_param_bands_id_ = 0;
    int num_bands_ = acpl::kMaxParamBands;
    int quant_mode_ = 0;
    int qmf_band_ = 0;
    int start_band_ = 0;
    std::vector<dsp::QmfAnalysis<double>> analyses_;
    std::deque<std::vector<Slot>> slots_;  // [slot][channel]
    long long first_slot_ = 0;
    // What the decoder holds for DIFF_TIME (Pseudocode 121's acpl_SET_q_prev):
    // the modules' alpha and beta, and acpl_data_2ch()'s eleven parameters in
    // Table 62's order.
    std::array<std::array<Values, 2>, 4> module_history_{};
    std::array<Values, 11> coupling_history_{};
};

// The channels the spectral frontend codes for a layout, from one sample of
// the input's channels in the layout's order: the channel pair's x0; the 5.X
// element's two downmixes and C; ASPX_ACPL_3's Lo and Ro over 1 + sqrt 2; or
// the immersive element's four sums, D'' to G''.
[[nodiscard]] std::vector<double> acpl_downmix(AcplLayout layout, std::span<const double> input);

// ASPX_ACPL_1's residuals, from one sample of the input's channels in the
// layout's order: the channel pair's (L - R) / 2, the 5.X element's
// (L - Ls / sqrt 2) / 2 and its mirror, or the immersive element's four
// differences, H'' to K''. Below acpl_qmf_band the decoder takes each module's
// output as the coded channel plus and minus its residual (Pseudocode 116),
// which gives the pair back as it was.
[[nodiscard]] std::vector<double> acpl_residuals(AcplLayout layout, std::span<const double> input);

}  // namespace ac4::detail

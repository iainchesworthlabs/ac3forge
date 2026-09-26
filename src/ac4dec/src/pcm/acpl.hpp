#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "ac4dec/decoder.hpp"
#include "acpl/acpl.hpp"
#include "pcm/aspx.hpp"
#include "syntax/channel_elements.hpp"
#include "syntax/context.hpp"

// The A-CPL codec modes' last QMF-domain step, ETSI TS 103 190-1 V1.4.1
// clause 5.7.7, after A-SPX (6.2.11, Table 214). A frame's parameters are
// differentially decoded and dequantised when the frame is read (5.7.7.7), so
// that a value outside its table refuses the frame before anything moves on,
// and are applied d_ctrl frames later, with the signal they belong to, by:
//
//   the channel pair element    one module on L and R (5.7.7.5, Pseudocodes
//                               115 and 116);
//   the 5.X element             two modules, on (L, Ls) and (R, Rs), in
//                               ASPX_ACPL_1 and 2 (5.7.7.6.1, Pseudocode 117);
//                               three, making all five channels of L and R,
//                               in ASPX_ACPL_3 (5.7.7.6.2, Pseudocodes 118
//                               and 119);
//   the 7.X element             two modules on the pairs Table 202 names by
//                               channel mode and add_ch_base (5.7.7.6.3,
//                               Pseudocode 120);
//   the immersive element       four modules, on (Ls, Lb), (Rs, Rb), (Tfl,
//                               Tbl) and (Tfr, Tbr), in full decoding (ETSI
//                               TS 103 190-2 V1.3.1 clause 5.5.2, Table 25
//                               and Pseudocode 2).
//
// The core (acpl/acpl.hpp) holds the decorrelators, the transient ducker and
// interpolation. src/ac4dec/ERRATA.md records the readings taken, under
// "A-CPL".

namespace ac4::detail {

// The most acpl_data_1ch() one element carries: the immersive element's four.
inline constexpr std::size_t kMaxAcplModules = 4;

// One acpl_data_1ch(), dequantised: the parameters of one module.
struct AcplModuleValues {
    acpl::Framing framing;
    int num_bands = acpl::kMaxParamBands;
    int qmf_band = 0;  // acpl_qmf_band: 0 in FULL mode
    acpl::ParamSets alpha{};
    acpl::ParamSets beta{};
};

// acpl_data_2ch(), dequantised.
struct AcplCouplingValues {
    acpl::Framing framing;
    int num_bands = acpl::kMaxParamBands;
    std::array<acpl::ParamSets, 2> alpha{};
    std::array<acpl::ParamSets, 2> beta{};
    acpl::ParamSets beta3{};
    std::array<acpl::ParamSets, 6> gamma{};
};

// A frame's A-CPL data as the QMF domain takes it: one, two or four modules'
// parameters, or acpl_data_2ch()'s.
struct AcplFrameValues {
    std::array<AcplModuleValues, kMaxAcplModules> modules{};
    std::size_t module_count = 0;
    std::optional<AcplCouplingValues> coupling;
};

// Pseudocode 121's acpl_SET_q_prev: each parameter's quantised values in the
// last parameter set decoded, which DIFF_TIME adds to.
struct AcplQuantHistory {
    // [acpl_data_1ch() in syntax order][acpl_alpha1, acpl_beta1][band].
    std::array<std::array<std::array<int, acpl::kMaxParamBands>, 2>, kMaxAcplModules> modules{};
    // acpl_data_2ch()'s eleven parameters in Table 62's order: alpha1,
    // alpha2, beta1, beta2, beta3, gamma1 to gamma6.
    std::array<std::array<int, acpl::kMaxParamBands>, 11> coupling{};
};

// 5.7.7.7 for the A-CPL data of `element`, whose codec mode uses A-CPL,
// moving `history` on. Fails when a value leaves its quantisation table, or
// the element lacks the data its mode carries; `history` is then partly moved
// on, so the caller passes a copy and keeps it only when the frame is kept.
[[nodiscard]] ParseResult acpl_values(const ChannelElement& element, AcplQuantHistory& history,
                                      AcplFrameValues& out);

// The QMF matrices A-CPL reads and writes, num_qmf_timeslots slots of 64
// subbands each, by the decoder's channels (speakers_of()).
struct AcplChannels {
    std::span<const Speaker> speakers;
    std::span<std::vector<QmfValue>* const> matrices;
};

// What A-CPL carries from frame to frame: the decorrelators with their
// transient duckers, and acpl_param_prev of every parameter its modules
// interpolate.
class AcplStage {
   public:
    AcplStage();

    // The state of the first frame: silence in the decorrelators and duckers,
    // and every acpl_param_prev 0 (5.7.7.3).
    void reset();

    // Applies one frame's parameters for an element of `kind` in `codec_mode`
    // (an immersive_mode value for the immersive element) under channel mode
    // `ch_mode` and, for the 7.X modes that send it, add_ch_base.
    void apply(int ch_mode, bool add_ch_base, ElementKind kind, int codec_mode, const AcplFrameValues& values,
               int num_ts, const AcplChannels& channels);

   private:
    // One interpolated parameter: its values in this frame and acpl_param_prev.
    struct Param {
        acpl::ParamSets values{};
        acpl::ParamPrev prev{};
    };

    // One module: `index` counts the element's acpl_data_1ch(), whose
    // acpl_param_prev it keeps, and `decorrelator` is one of decorrelators_.
    void module(const AcplModuleValues& values, int index, int decorrelator,
                std::span<const QmfValue> x0, std::span<const QmfValue> x1, std::span<QmfValue> z0,
                std::span<QmfValue> z1, int num_ts);
    void coupling(const AcplCouplingValues& values, std::span<const QmfValue> x0, std::span<const QmfValue> x1,
                  std::span<std::span<QmfValue>, 5> z, int num_ts);
    // Pseudocode 109 for `param` under `framing`, into `out`.
    void interpolate(const acpl::Framing& framing, int num_bands, const Param& param, int num_ts,
                     std::vector<double>& out) const;
    void decorrelate(int decorrelator, std::span<const QmfValue> in, std::span<QmfValue> out, int num_ts);

    // D0, D1 and D2, then the second instances of D0 and D1 the immersive
    // element's four modules take (Pseudocode 2): kDecorrelatorSlots.
    static constexpr std::size_t kDecorrelatorSlots = acpl::kDecorrelators + 2;
    std::array<acpl::Decorrelator<double>, kDecorrelatorSlots> decorrelators_;
    std::array<acpl::TransientDucker<double>, kDecorrelatorSlots> duckers_{};
    // acpl_param_prev: alpha and beta of each module, and acpl_data_2ch()'s
    // eleven parameters in AcplQuantHistory's order.
    std::array<std::array<acpl::ParamPrev, 2>, kMaxAcplModules> module_prev_{};
    std::array<acpl::ParamPrev, 11> coupling_prev_{};

    // Scratch, kept to save allocations per frame.
    std::array<std::vector<QmfValue>, 5> in_{};
    std::array<std::vector<QmfValue>, 3> transformed_{};
    std::array<std::vector<QmfValue>, kDecorrelatorSlots> decorrelated_{};
    std::vector<QmfValue> work_;
    std::array<std::vector<double>, 2> interp_{};
    std::vector<std::vector<double>> interp_scratch_;
};

}  // namespace ac4::detail

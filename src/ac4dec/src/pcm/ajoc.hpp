#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "ajoc/ajoc.hpp"
#include "pcm/aspx.hpp"
#include "syntax/ajoc.hpp"
#include "syntax/context.hpp"

// A-JOC's QMF-domain step, ETSI TS 103 190-2 V1.3.1 clause 5.7, for an A-JOC
// substream after A-SPX (4.8.3.13): the downmix, its inputs in QinAJOC's order
// (Pseudocode 14a, pcm/routing.hpp), made into the upmix objects in full
// decoding, or given dialogue enhancement in core decoding (5.8.2.4), which
// otherwise leaves the downmix as it is. A frame's parameters are
// differentially decoded and dequantised when the frame is read (5.7.3.2 and
// 5.7.3.3), so that a value outside its range refuses the frame before
// anything moves on, and applied d_ctrl frames later with the signal they
// belong to, as A-CPL's and A-JCC's are. The core (ajoc/ajoc.hpp) holds the
// interpolation, the decorrelators and the reconstruction.

namespace ac4::detail {

// Pseudocode 16's mtx_dry_q_prev and mtx_wet_q_prev per upmix object, with the
// quantisation and band count they were decoded at; `centre` for an object
// that was not present, whose values stand at the range's centre, which
// dequantises to 0 (src/ac4dec/ERRATA.md, "A-JOC's differential decoding
// across frames").
struct AjocQuantHistory {
    struct Object {
        bool valid = false;
        bool centre = false;
        int quant_select = 0;
        int num_bands = 0;
        std::array<std::array<int, ajoc::kMaxBands>, kMaxAjocDmxSignals> dry{};
        std::array<std::array<int, ajoc::kMaxBands>, kMaxAjocDecorr> wet{};
    };
    std::vector<Object> objects;
};

// One frame's A-JOC data, decoded and dequantised, and its dialogue
// enhancement: the upmix objects Pseudocode 28 makes dialogue, their downmix
// coefficients (Table 82, [dialogue object][downmix signal]) and G_max, (1 +
// de_max_gain) x 3 dB (Part 1 clause 4.3.14.3.2), where a configuration is in
// force.
struct AjocFrameValues {
    ajoc::FrameParameters params;
    bool de = false;
    std::vector<std::uint8_t> dialogue;
    std::vector<double> coeff;
    double gmax_db = 0.0;
};

// Clauses 5.7.3.2 and 5.7.3.3 for `data` and ajoc_dmx_de_data()'s `de`, moving
// `history` on. Fails where a value leaves its range, and where a DIFF_TIME
// value has no data point before it of the same quantisation and bands.
// `history` is then partly moved on, so the caller passes a copy and keeps it
// only when the frame is kept.
[[nodiscard]] ParseResult ajoc_values(const AjocData& data, const AjocDmxDeData& de,
                                      AjocQuantHistory& history, AjocFrameValues& out);

// What A-JOC carries from frame to frame: the core's reconstruction state,
// held on the heap for its decorrelators' history (seven of them), and the
// objects' matrices.
class AjocStage {
   public:
    AjocStage();

    void reset();

    // Full decoding: the upmix objects from `inputs` (QinAJOC's order) into
    // `objects`, sized here, with dialogue enhancement at G_DE `dialogue_db`
    // (clause 5.8.2.3).
    void reconstruct(const AjocFrameValues& values, double dialogue_db, int num_ts,
                     std::span<const std::vector<QmfValue>* const> inputs,
                     std::vector<std::vector<QmfValue>>& objects);

    // Core decoding: dialogue enhancement on `inputs` in place (clause
    // 5.8.2.4), where G_DE is above 0 dB and the stream names dialogue
    // objects; otherwise nothing.
    void enhance_core(const AjocFrameValues& values, double dialogue_db, int num_ts,
                      std::span<std::vector<QmfValue>* const> inputs);

   private:
    std::unique_ptr<ajoc::Reconstruction<double>> reconstruction_;
    std::vector<std::vector<QmfValue>*> outputs_;
};

}  // namespace ac4::detail

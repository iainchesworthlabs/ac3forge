#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <span>
#include <vector>

#include "iclforge/ac4core/ajoc/ajoc.hpp"
#include "ajoc/ajoc_syntax.hpp"
#include "iclforge/ac4core/dsp/qmf.hpp"
#include "frame/timing.hpp"

// The encoder's A-JOC: ETSI TS 103 190-2 V1.3.1 clause 5.7 run from the other
// side. The downmix signals, in A-JOC's input order, and the objects they
// rebuild are analysed by the decoder's QMF bank on the decoder's slot axis
// (aspx/aspx_encoder.hpp), and frame f's parameters apply to the slots of the
// A-SPX interval they share a control frame with, as A-CPL's do
// (acpl/acpl_encoder.hpp): at frame_rate_index 13, QMF slots 32 (f + 1) - 6
// on.
//
// Each frame weighs candidate parameters by running the decoder's own
// reconstruction (src/ac4core's ajoc::Reconstruction) from the state the
// frames sent leave it in, and keeps the one whose objects come closest to
// the objects given, within the bits the frame allows. Every candidate has one
// data point at the frame's first slot. Its dry matrix, per object and
// parameter band (Table 28), is the least squares fit of the object from the
// downmix: over the frame as the ramp takes the matrix there, linearly from
// the last frame's over 32 slots or in a step at the second slot; or over the
// 32 slots centred on the frame's end, ramped; and the last frame's again. The
// values are quantised to Tables 29 to 32's steps within their ranges and sent
// along frequency or, outside I-frames, along time where that takes fewer
// bits, each object sparse where that does, and not present where all its
// values are 0.
//
// With decorrelators, object o takes decorrelator o mod ajoc_num_decorr, whose
// wet coefficient in each band gives the object the energy its dry
// reconstruction leaves out: the decorrelator's output for that object,
// measured as the reconstruction gives it with a coefficient of 1, scaled to
// the missing energy.

namespace iclforge::ac4::detail {

struct AjocSetup {
    int num_dmx = 1;
    int num_umx = 1;
    int num_bands_code = 1;  // Table 78
    int quant_select = 0;    // 0 fine, 1 coarse
    int num_decorr = 0;      // 0 to 7
};

class AjocEncoder {
   public:
    AjocEncoder(const AjocSetup& setup, const FrameTiming& timing);

    [[nodiscard]] const AjocSetup& setup() const noexcept { return setup_; }

    // Analyses slot slots() of each downmix signal, in A-JOC's input order,
    // and of each object, from the 64 samples of the delayed signal from 64
    // slots() - d_pcm.
    void push_slot(std::span<const std::array<double, dsp::kQmfSubbands>> dmx,
                   std::span<const std::array<double, dsp::kQmfSubbands>> objects);
    [[nodiscard]] long long slots() const noexcept {
        return first_slot_ + static_cast<long long>(slots_.size());
    }

    // The first slot frame f's parameters apply to, and the slot after the
    // last one propose() reads for it.
    [[nodiscard]] long long first_slot(long long frame) const noexcept;
    [[nodiscard]] long long slots_needed(long long frame) const noexcept;

    // Frame f's ajoc(), from the slots it reads, which must have been
    // analysed: the candidate that leaves the least error in at most
    // `max_bits`, or the cheapest where none fits. Nothing moves on until
    // commit().
    [[nodiscard]] AjocFields propose(long long frame, bool iframe, std::size_t max_bits);

    // What a frame sends whose bits hold no more: every object not present,
    // which the decoder takes as silence.
    [[nodiscard]] AjocFields least(bool iframe) const;

    // Moves the decoder's state on by the frame sent: propose()'s, or with
    // `least` least()'s.
    void commit(bool least);

    // Frees the slots frame f and later do not read.
    void drop_before_frame(long long frame);

   private:
    using Slot = std::array<dsp::Complex<double>, dsp::kQmfSubbands>;
    using Reconstruction = ajoc::Reconstruction<double>;

    // A candidate's quantised values: per object, dry [ch][pb] and wet
    // [de][pb].
    struct Values {
        std::vector<std::vector<std::array<int, ajoc::kMaxBands>>> dry;
        std::vector<std::vector<std::array<int, ajoc::kMaxBands>>> wet;
        int ramp = 32;
    };
    [[nodiscard]] int bands() const noexcept;
    [[nodiscard]] int centre(bool wet) const noexcept;
    [[nodiscard]] double step_value(bool wet, int q) const noexcept;
    [[nodiscard]] int quantise(bool wet, double v) const noexcept;
    [[nodiscard]] Values held_values() const;
    [[nodiscard]] bool present(const Values& v, std::size_t o) const;
    [[nodiscard]] AjocFields fields_of(const Values& v, bool iframe) const;
    [[nodiscard]] ajoc::FrameParameters parameters_of(const Values& v) const;
    // The dry values of the least squares fit over slots [from, from + 32),
    // each slot t taking the matrix weight(t) of the way from the held one,
    // sent with a ramp of `ramp` slots.
    template <typename Weight>
    [[nodiscard]] Values fit(long long from, int ramp, const Weight& weight) const;
    // The frame's slots as the reconstruction takes them.
    void gather(long long first);
    // Runs `values` on a copy of the state from the frames sent: the objects'
    // error over the frame, and the copy and its output.
    [[nodiscard]] double run(const Values& values, Reconstruction& state,
                             std::vector<std::vector<dsp::Complex<double>>>& out);

    AjocSetup setup_;
    FrameTiming timing_;
    std::vector<dsp::QmfAnalysis<double>> dmx_analyses_;
    std::vector<dsp::QmfAnalysis<double>> object_analyses_;
    std::deque<std::vector<Slot>> slots_;  // [slot][downmix signals, then objects]
    long long first_slot_ = 0;
    Reconstruction state_;
    // What the decoder keeps between frames, its history for DIFF_TIME: the
    // last data point's values, the centre for an object not present then.
    Values held_;
    bool held_valid_ = false;
    // The frame being proposed: its downmix and objects over its slots, and
    // what propose() chose, with the state it leaves.
    std::vector<std::vector<dsp::Complex<double>>> x_;
    std::vector<std::vector<dsp::Complex<double>>> z_;
    Values proposed_;
    Reconstruction proposed_state_;
    bool proposed_iframe_ = true;
};

}  // namespace iclforge::ac4::detail

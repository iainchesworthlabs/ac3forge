#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "ac4dec/decoder.hpp"

// The intermediate spatial format renderer (ETSI TS 103 190-2 V1.3.1 clause
// 5.10.3): an ISF object's essence, with the gain its metadata sets, to the
// speakers of the output channel configuration, by the matrices of Annex
// A.2.1 (tables/isf_tables.hpp). Clause 4.8.3's object audio substream hands
// the tool each present object with its properties, so the object's gain
// (Annex F.5, -infinity for an inactive object) applies before the matrix
// (src/ac4dec/ERRATA.md, "The intermediate spatial format").

namespace ac4::detail {

// One ISF object for the renderer: its format (isf_config, Table 61), its
// place in the format's t = [M1..., U1..., L1..., Z] (clause 5.10.3.4), and its
// essence with its gain applied.
struct IsfInput {
    int config = 0;
    int index = 0;
    std::span<const float> samples;
};

// An object's gain as the renderer applies it (Annex F.5 and F.11): each
// update's from its sample, reached linearly over its ramp_duration from the
// gain at that sample, carried from frame to frame.
class IsfGain {
   public:
    // Applies the gain to `samples`, a frame of the object's essence, whose
    // updates within the frame are `updates`, in order.
    void apply(std::span<float> samples, std::span<const ObjectUpdate> updates) noexcept;

   private:
    double gain_ = 1.0;
    double target_ = 1.0;
    double step_ = 0.0;
    int left_ = 0;
};

// isf_config's format for an object count of Table 61 (4, 8, 10, 14, 15 or
// 30); -1 for another.
[[nodiscard]] int isf_config_of(int objects) noexcept;

// Renders `inputs`, each `length` samples, into `channels`, which carry
// `speakers`, by the matrix of the layout those speakers are (an LFE aside,
// which the tool leaves alone: clause 5.10.3.4's NOTE 1) - 2.X, 5.X, 7.X,
// 5.X.2, 5.X.4, 7.X.2 or 7.X.4, or a mono channel, which takes the 2.X
// layout's L + R as the stereo downmix's mono does. Where `speakers` is empty,
// the channels are `target`'s layout, which the call sets: 7.X.4 as coded,
// and a two-channel target the 2.X layout. False, with nothing rendered,
// where Annex A.2.1 has no matrix for the channels.
[[nodiscard]] bool render_isf(std::span<const IsfInput> inputs, DownmixTarget target,
                              std::size_t length, std::vector<std::vector<float>>& channels,
                              std::vector<Speaker>& speakers);

}  // namespace ac4::detail

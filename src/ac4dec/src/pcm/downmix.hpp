#pragma once

#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ac4dec/decoder.hpp"
#include "pcm/aspx.hpp"
#include "pcm/renderer.hpp"
#include "syntax/metadata.hpp"
#include "syntax/presentation.hpp"

// Rendering the decoded channels to fewer (ETSI TS 103 190-1 V1.4.1 clause
// 6.2.17), in the QMF domain after DRC, whose curve gain is the same in every
// channel and so passes through a downmix unchanged, and before synthesis, so
// only the channels that come out are synthesised.
//
// The downmixes cascade: a 7.X element's channels to 5.X by Table 219, which
// its channel mode and add_ch_base choose; 5.X (or 3.0) to two channels by
// Table 218 (217), Lo/Ro, Lt/Rt or Lt/Rt in its Pro Logic II form, with the
// stream's centre and surround mix gains (Tables 149 and 149a, -3 dB where it
// has sent none), the LFE at lfe_mixgain, and the Lo/Ro or Lt/Rt loudness
// correction; and two channels to one as L + R. A listener's choice of Lo/Ro or
// Lt/Rt overrides preferred_dmx_method, and Lt/Rt takes its Pro Logic II form
// where the stream prefers that. A mono stream comes out in stereo at 0.707 in
// each channel (6.2.17.6). The folds to 5.X take the loudness correction Part
// 2 clause 4.8.5.3 gives them.
//
// The immersive element takes Part 2's channel renderer (clause 5.10.2,
// pcm/renderer.hpp) instead of step 1: to the layout asked for, with the
// correction 4.8.5.3 gives that output, and to 5.X.0 on the way to two
// channels or one, the steps to which follow as for 5.X with the Lo/Ro or
// Lt/Rt correction alone, the core's in core decoding (src/ac4dec/ERRATA.md,
// "The renderer's two-channel output").
//
// The mix values, the custom downmix data and the loudness corrections
// persist from the frame that sends them until another does (6.2.17.0, Part
// 2 clause 4.8.5.3). The gains are in dB, their linear values 10^(dB/20)
// (src/ac4dec/ERRATA.md, "The downmix gains").

namespace ac4::detail {

// What a frame's metadata gives the downmix, where it sends them.
struct DownmixValues {
    std::optional<StereoDmxCoeff> coeff;
    std::optional<int> loro_loud_corr;
    std::optional<int> ltrt_loud_corr;
    std::optional<int> loud_corr_5x;  // Part 2's loud_corr_5_X
    // The rest of Part 2's loud_corr() (clause 6.2.9.1), for the immersive
    // element's outputs, and core decoding's.
    std::optional<int> loud_corr_5x2;
    std::optional<int> loud_corr_5x4;
    std::optional<int> loud_corr_7x;
    std::optional<int> loud_corr_7x2;
    std::optional<int> loud_corr_7x4;
    std::optional<int> loud_corr_core_5x2;
    std::optional<int> loud_corr_core_5x;
    std::optional<int> loud_corr_core_loro;
    std::optional<int> loud_corr_core_ltrt;
    // custom_dmx_data() where it sends custom downmix data (b_cdmx_data_present).
    std::optional<CustomDmxData> cdmx;
};

// The values from the presentation substream (bitstream version 2), or else
// the audio substream's basic_metadata().
[[nodiscard]] DownmixValues downmix_values(const PresentationSubstream* presentation,
                                           const Metadata& metadata);

// Tables 149 and 149a: a mix gain code's linear gain; a surround code the
// table reserves reads as the -3 dB of no code at all.
[[nodiscard]] double centre_mix_gain(int code) noexcept;
[[nodiscard]] double surround_mix_gain(int code) noexcept;

class DownmixStage {
   public:
    // The channels `speakers` names, in that order, to `target`'s layout; an
    // immersive element's, which `immersive` describes, by Part 2's renderer.
    void configure(std::span<const Speaker> speakers, bool add_ch_base, DownmixTarget target,
                   bool mix_lfe, const std::optional<ImmersiveLayout>& immersive = std::nullopt);

    // Whether the channels come out as coded.
    [[nodiscard]] bool passes_through() const noexcept { return pass_through_; }

    // The layout that comes out.
    [[nodiscard]] std::span<const Speaker> speakers() const noexcept { return out_speakers_; }

    // Back to the values no stream has sent: -3 dB mix gains, no LFE, no
    // loudness correction, Table 130's custom downmix parameters.
    void reset();

    // Takes this frame's values and writes out[o] = sum_c M[o][c] in[c] for
    // every QMF value, out resized to speakers().
    void process(const DownmixValues& values, std::span<std::vector<QmfValue>* const> in,
                 std::vector<std::vector<QmfValue>>& out);

    // The matrix in force, one row per channel out, one column per channel in.
    [[nodiscard]] const std::vector<std::vector<double>>& matrix() const noexcept {
        return matrix_;
    }

   private:
    // A combination of the input channels, one weight per channel.
    using Mix = std::vector<double>;

    void rebuild();
    // The immersive element's matrix, by render_matrix().
    void rebuild_immersive();
    // Step 2: Lo and Ro from the 5.X channels, each a combination of the
    // input channels, with the Lo/Ro or Lt/Rt correction given.
    [[nodiscard]] std::pair<Mix, Mix> two_channels(const Mix& l, const Mix& r, const Mix& c,
                                                   const Mix& lfe, const Mix& ls, const Mix& rs,
                                                   const std::optional<int>& loro_correction,
                                                   const std::optional<int>& ltrt_correction) const;
    // Lo and Ro into the matrix, or for mono their sum (6.2.17.2).
    void push_two(const Mix& lo, const Mix& ro);
    // The loudness correction in force for `output`.
    [[nodiscard]] std::optional<int> correction(LoudCorrOutput output) const noexcept;

    std::vector<Speaker> in_speakers_;
    std::vector<Speaker> out_speakers_;
    bool add_ch_base_ = false;
    DownmixTarget target_ = DownmixTarget::kAsCoded;
    bool mix_lfe_ = true;
    bool pass_through_ = true;
    std::optional<ImmersiveLayout> immersive_;
    RenderPlan plan_;  // the immersive element's
    // The values in force.
    std::optional<StereoDmxCoeff> coeff_;
    std::optional<int> loro_loud_corr_;
    std::optional<int> ltrt_loud_corr_;
    std::optional<int> loud_corr_5x_;
    std::optional<int> loud_corr_5x2_;
    std::optional<int> loud_corr_5x4_;
    std::optional<int> loud_corr_7x_;
    std::optional<int> loud_corr_7x2_;
    std::optional<int> loud_corr_7x4_;
    std::optional<int> loud_corr_core_5x2_;
    std::optional<int> loud_corr_core_5x_;
    std::optional<int> loud_corr_core_loro_;
    std::optional<int> loud_corr_core_ltrt_;
    std::optional<CustomDmxData> cdmx_;
    std::vector<std::vector<double>> matrix_;
};

}  // namespace ac4::detail

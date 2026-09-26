#pragma once

#include <array>
#include <span>
#include <vector>

#include "ac4dec/decoder.hpp"
#include "pcm/aspx.hpp"
#include "syntax/metadata.hpp"

// The dialogue enhancement tool of ETSI TS 103 190-1 V1.4.1 clause 5.7.8: in
// the QMF domain, before DRC, the dialogue in the front channels (L, R and C,
// Table 215) is raised by the gain the system asks for, capped by the stream's
// G_max, through a matrix per parameter band (Table 173) that clause 5.7.8.6
// interpolates slot by slot from the previous frame's.
//
// The four methods (Table 170): channel independent, each processed channel
// scaled by 1 + g p_i, or its Mid alone where de_ms_proc_flag is set;
// cross-channel, I + g r p^T over the processed channels; and the two hybrids
// (5.7.8.9), which split g between their parameters, (1 - alpha_c) g, and a
// dialogue waveform, alpha_c g, which the presentation's dialogue enhancement
// substream carries: one channel per processed channel with the channel
// independent method (one for the Mid with de_ms_proc_flag), one channel
// rendered by r with the cross-channel method. Without that substream they
// enhance by their parametric data alone, as the clause allows a
// low-complexity decoder to, at the whole gain (src/ac4dec/ERRATA.md,
// "Dialogue enhancement without its waveform").
//
// Subbands above Table 173's last band, 40, have no parameters and pass
// unchanged (ERRATA, "The subbands above dialogue enhancement's bands").

namespace ac4::detail {

inline constexpr int kDeFront = 3;  // L, R and C, the order clause 5.7.8.6 gives the 3x6 matrix

// One frame's dialogue enhancement, dequantised.
struct DeFrameValues {
    bool active = false;  // this frame carries parameters
    int method = 0;       // Table 170
    double max_gain_db = 0.0;
    // Which of L, R and C the parameters are for (de_channel_config, Table 171),
    // and each processed channel's parameters by band, in that order.
    std::array<bool, kDeFront> processed{};
    std::array<std::array<double, kDeNrBands>, kDeFront> p{};
    std::array<double, kDeFront> r{};  // the rendering vector, cross-channel
    bool ms = false;                   // de_ms_proc_flag
    double alpha_c = 0.0;              // de_signal_contribution / 31
};

// Tables 209 and 210: a parameter index's value, channel independent or
// cross-channel.
[[nodiscard]] double de_parameter(int index, bool cross_channel) noexcept;

// Table 172: a mixing coefficient index's value.
[[nodiscard]] double de_mix_coefficient(int index) noexcept;

// Clause 5.7.8.5: the rendering vector for one to three processed channels.
[[nodiscard]] std::array<double, kDeFront> de_rendering(int nr_channels, double coef1,
                                                        double coef2) noexcept;

// What a frame's dialog_enhancement() gives the tool.
[[nodiscard]] DeFrameValues de_frame_values(const DialogEnhancement& de);

class DeStage {
   public:
    // `speakers` names the channels in the order process() gets their
    // matrices; `slots` is num_qmf_timeslots.
    void configure(int slots, std::span<const Speaker> speakers);

    // The previous frame's matrices become the identity.
    void reset() noexcept;

    // Whether process() would change anything at this gain: parameters now or
    // a matrix left from the last frame to interpolate from.
    [[nodiscard]] bool active(double gain_db, const DeFrameValues& values) const noexcept;

    // Enhances `matrices` in place, `gain_db` being G_DE. `waveform` is the
    // dialogue enhancement substream's matrices, in its channels' order, for
    // the hybrid methods; empty where there is none.
    void process(double gain_db, const DeFrameValues& values,
                 std::span<std::vector<QmfValue>* const> matrices,
                 std::span<std::vector<QmfValue>* const> waveform = {});

   private:
    using Matrix = std::array<std::array<double, kDeFront>, kDeFront>;

    // Clause 5.7.8.6's 3x6 matrix per band: the parametric part, over L, R and
    // C, and the waveform part, over the waveform's channels.
    struct Frame {
        std::array<Matrix, kDeNrBands> h{};
        std::array<Matrix, kDeNrBands> w{};
    };

    [[nodiscard]] static Matrix identity() noexcept;
    [[nodiscard]] Frame frame_matrices(double gain_db, const DeFrameValues& values,
                                       std::size_t waveform_channels) const;

    int slots_ = 32;
    std::array<int, kDeFront> channel_{-1, -1, -1};  // each of L, R and C's matrix, -1 where absent
    Frame previous_{};
    bool previous_identity_ = true;
};

}  // namespace ac4::detail

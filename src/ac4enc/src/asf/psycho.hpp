#pragma once

#include <array>
#include <vector>

#include "asf/coder.hpp"
#include "asf/layout.hpp"

// The encoder's psychoacoustic model (planning/ac4.md, "Rate control and the
// psychoacoustic model"; decision 17): per scale factor band of each window
// group, the noise energy the band can carry unheard.
//
// Written for AC-4's bands from the published sources ac3::quality's model
// cites, not linked to it:
//
//   - the masking threshold from the band's energy, lowered by an offset
//     that runs from 5.5 dB for noise to 14.5 + z dB for a tone, z being the
//     band's Bark frequency, the tonality taken from the band's spectral
//     flatness (J. D. Johnston, "Transform coding of audio signals using
//     perceptual noise criteria", IEEE JSAC 6(2), 1988);
//   - spread to the bands around it at a fixed slope per Bark, falling
//     faster towards lower frequencies, keeping the largest contribution
//     rather than summing them, so that many narrow bands inside one critical
//     band do not add up to more masking than the critical band gives;
//   - never below a floor per line about as low as 16-bit PCM's own noise.
//
// There is no threshold in quiet. Terhardt's curve (E. Terhardt, "Calculating
// virtual pitch", Hearing Research 1, 1979), with a full-scale sine at 96 dB
// SPL, rises from 15 dB SPL at 11 kHz to 50 at 15 kHz, and removed the top
// octave of the speech and music that DEE's streams keep: with it the race's
// log-spectral distance at 192 kbps was 4.3 dB on music and 2.9 on speech,
// against DEE's 2.4 and 0.4, and without it 2.2 and 0.1 (planning/ac4.md, the
// encoder's ladder, item 5). ac3::quality's model makes the same curve
// optional for the same reason.

namespace ac4::detail {

class Psychoacoustics {
   public:
    Psychoacoustics(int sample_rate_hz, int frame_length);

    // Allowed noise energy per group and band, over all the band's lines in
    // all the group's windows.
    [[nodiscard]] std::vector<std::vector<double>> thresholds(const Grouped& grouped,
                                                              const FrameLayout& layout) const;

   private:
    [[nodiscard]] double full_scale_line_energy(int transform_length) const noexcept;

    int sample_rate_ = 48000;
    int frame_length_ = 2048;
    std::array<double, 5> full_scale_{};  // per halving of the frame length
};

// The scale factor per band at which the quantiser's noise, estimated as
// (4/27) g^(3/2) sum |x|^(1/2) over the band's lines (the power law's step
// spread over a uniform error), comes to the band's allowed noise.
[[nodiscard]] std::vector<std::vector<int>> scale_factors_for(const Grouped& grouped,
                                                              const std::vector<std::vector<double>>& allowed);

}  // namespace ac4::detail

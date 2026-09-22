#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

#include "ac4dec/decoder.hpp"
#include "bit_reader.hpp"

// What a substream's syntax needs from outside the substream, and the result
// type every syntax function returns.

namespace ac4::detail {

// A refusal or a failure, with the reason a report carries. `reason` is
// always a string literal.
struct SyntaxError {
    DecodeError error = DecodeError::kInvalidStream;
    std::string_view reason;
};

using ParseResult = std::expected<void, SyntaxError>;

[[nodiscard]] inline std::unexpected<SyntaxError> fail(DecodeError error, std::string_view reason) {
    return std::unexpected(SyntaxError{error, reason});
}

// After a read that could have run off the end: the substream was shorter
// than its syntax.
[[nodiscard]] inline ParseResult check(const BitReader& reader) {
    if (reader.overflow()) {
        return fail(DecodeError::kTruncated, "a syntax element runs past the end of the substream");
    }
    return {};
}

// Part 1 Tables 83 and 84: frame_len_base from the frame rate index and the
// sample rate, 0 when the pair is one neither table defines. Index 13 is the
// only one Table 84 gives 44.1 kHz, so every other index there is reserved and
// has no frame length to read a substream with - which is what ac4::
// samples_per_frame() says about the same pair. One helper for both the audio
// substreams and the presentation substream: deriving it twice let a 44.1 kHz
// frame be read with two different frame lengths at once.
[[nodiscard]] inline int frame_len_base(int frame_rate_index, int sample_rate_hz) noexcept {
    constexpr std::array<int, 14> kForExternal48 = {1920, 1920, 2048, 1536, 1536, 960, 960,
                                                    1024, 768,  768,  512,  384,  384, 2048};
    if (frame_rate_index < 0 || static_cast<std::size_t>(frame_rate_index) >= kForExternal48.size()) {
        return 0;
    }
    if (sample_rate_hz == 44100) {
        return frame_rate_index == 13 ? 2048 : 0;
    }
    return kForExternal48[static_cast<std::size_t>(frame_rate_index)];
}

// Channel modes as ch_mode numbers (Part 1 Table 88, Part 2 Table 56).
namespace ch_mode {
inline constexpr int kMono = 0;
inline constexpr int kStereo = 1;
inline constexpr int k3_0 = 2;
inline constexpr int k5_0 = 3;
inline constexpr int k5_1 = 4;
inline constexpr int k7_0_340 = 5;  // L C R Ls Rs Lb Rb
inline constexpr int k7_1_340 = 6;
inline constexpr int k7_0_520 = 7;  // L C R Lw Rw Ls Rs
inline constexpr int k7_1_520 = 8;
inline constexpr int k7_0_322 = 9;  // L C R Ls Rs Tfl Tfr
inline constexpr int k7_1_322 = 10;
inline constexpr int k7_0_4 = 11;
inline constexpr int k7_1_4 = 12;
inline constexpr int k9_0_4 = 13;
inline constexpr int k9_1_4 = 14;
inline constexpr int k22_2 = 15;
}  // namespace ch_mode

// Everything the table of contents says about one ac4_substream() that its
// syntax depends on. Filled by the decoder from ac4::Toc before the substream
// is read.
struct SubstreamContext {
    int bitstream_version = 2;
    int presentation_version = 1;
    int fs_index = 1;                 // Part 1 Table 82: 0 = 44.1 kHz, 1 = 48 kHz
    int frame_rate_index = 13;
    int frame_len_base = 2048;        // Part 1 Table 83, at the internal rate
    bool b_iframe = false;            // b_iframe (v0) or b_audio_ndot (v1) for this substream
    int sus_ver = 1;                  // Part 2 clause 6.2.1.6; 1 for bitstream_version 2
    int ch_mode = ch_mode::kStereo;
    // sf_multiplier: nullopt at 48 kHz: 0 for 96 kHz, 1 for 192 kHz (Table 89).
    std::optional<int> sf_multiplier;
    bool add_ch_base = false;         // Part 1 clause 4.3.3.7.6, for the 7.X modes that carry it
    bool b_associated = false;        // Part 1 clause 4.3.12.4.1, a parameter of extended_metadata
    bool b_dialog = false;            // Part 1 clause 4.3.12.4.2, likewise
    // Part 2 clause 6.2.2.2 passes it to metadata(): the b_alternative of the
    // ac4_presentation_substream_info() of the presentation this substream
    // was reached through (Part 2 clause 6.3.2.11.1).
    bool b_alternative = false;
    // Part 2 clause 6.2.1.8: which channels of an immersive or 7.X mode the
    // source populated. Carried for the renderer; the syntax does not use it.
    bool b_4_back_channels_present = true;
    bool b_centre_present = true;
    int top_channels_present = 3;

    [[nodiscard]] bool has_lfe() const noexcept {
        switch (ch_mode) {
            case ch_mode::k5_1:
            case ch_mode::k7_1_340:
            case ch_mode::k7_1_520:
            case ch_mode::k7_1_322:
            case ch_mode::k7_1_4:
            case ch_mode::k9_1_4:
            case ch_mode::k22_2:
                return true;
            default:
                return false;
        }
    }
};

}  // namespace ac4::detail

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ac4_objects_core.hpp"
#include "ac4enc/encoder.hpp"

// The AC-4 page's choices, Qt-free so that ac3tests can hold them to the two
// things they must agree on: the ac4::EncoderConfig the page encodes with and
// the `ac3cli ac4-encode` tokens it echoes, each spelled as ac3cli's parser
// reads it (apps/cli/support.cpp). A choice at the command's own default
// echoes nothing, so a plain encode's command line stays plain.
//
// What the page leaves to the command line: substreams and presentations,
// dialogue stems and hybrid dialogue enhancement, per-mode DRC profiles and
// transmitted gains, Lt/Rt's own mix levels, the downmix corrections, the LFE
// mix, I-frames at named frames or fragment boundaries, the syntax trace and
// every experimental= tool. The page encodes one source, in its own layout,
// as `ac4-encode` takes a WAV file; in object mode, the objects of its
// sources as `atmos-encode ... codec=ac4` takes them (the last section).

namespace ac3gui {

struct Ac4Choice {
    std::string_view token;  // what follows the key= in ac3cli's grammar
    std::string_view label;  // what the page shows
};

// Part 1 Table 83's frame rates at 48 kHz; index 13, the 2 048-sample frame,
// is the default and the only one at 44.1 kHz.
inline constexpr std::array<Ac4Choice, 14> kAc4FrameRates{{
    {"23.976", "23.976 fps"}, {"24", "24 fps"},         {"25", "25 fps"},
    {"29.97", "29.97 fps"},   {"30", "30 fps"},         {"47.95", "47.95 fps"},
    {"48", "48 fps"},         {"50", "50 fps"},         {"59.94", "59.94 fps"},
    {"60", "60 fps"},         {"100", "100 fps"},       {"119.88", "119.88 fps"},
    {"120", "120 fps"},       {"native", "Native (2 048-sample frames)"},
}};
inline constexpr std::size_t kAc4NativeFrameRate = 13;

inline constexpr std::array<Ac4Choice, 3> kAc4RateModes{{
    {"constant", "Constant"}, {"average", "Average"}, {"variable", "Variable"}}};

// Index 0 lets the encoder choose by the rate (ac4::CodecMode::kAuto).
inline constexpr std::array<Ac4Choice, 6> kAc4CodecModes{{
    {"auto", "Automatic (by bit rate)"},
    {"simple", "SIMPLE"},
    {"aspx", "ASPX"},
    {"aspx-acpl-1", "ASPX_ACPL_1"},
    {"aspx-acpl-2", "ASPX_ACPL_2"},
    {"aspx-acpl-3", "ASPX_ACPL_3"},
}};

// Part 1 Table 156.
inline constexpr std::array<Ac4Choice, 7> kAc4LoudnessPractices{{
    {"ebu-r128", "EBU R 128"},
    {"atsc-a85", "ATSC A/85"},
    {"arib-tr-b32", "ARIB TR-B32"},
    {"freetv-op59", "Free TV OP-59"},
    {"manual", "Manual"},
    {"consumer-leveller", "Consumer leveller"},
    {"not-indicated", "Not indicated"},
}};

// Part 1 Table 160's profiles, as drc= names them.
inline constexpr std::array<Ac4Choice, 5> kAc4DrcProfiles{{
    {"film-standard", "Film standard"},
    {"film-light", "Film light"},
    {"music-standard", "Music standard"},
    {"music-light", "Music light"},
    {"speech", "Speech"},
}};

// Part 1 Table 149 (cmixlev=) and Table 149a (surmixlev=); "off" is a gain of
// 0, -infinity dB.
inline constexpr std::array<Ac4Choice, 8> kAc4CentreLevels{{
    {"+3", "+3 dB"}, {"+1.5", "+1.5 dB"}, {"0", "0 dB"},   {"-1.5", "-1.5 dB"},
    {"-3", "-3 dB"}, {"-4.5", "-4.5 dB"}, {"-6", "-6 dB"}, {"off", "Off"},
}};
inline constexpr std::array<Ac4Choice, 6> kAc4SurroundLevels{{
    {"0", "0 dB"}, {"-1.5", "-1.5 dB"}, {"-3", "-3 dB"}, {"-4.5", "-4.5 dB"}, {"-6", "-6 dB"},
    {"off", "Off"},
}};

// Part 1 Table 150, as dmixmod= names it.
inline constexpr std::array<Ac4Choice, 4> kAc4PreferredDownmixes{{
    {"loro", "Lo/Ro"}, {"ltrt", "Lt/Rt"}, {"pl2", "Lt/Rt (Pro Logic II)"},
    {"none", "Not indicated"}}};

inline constexpr std::array<int, 4> kAc4DialogueMaxGains{3, 6, 9, 12};

// How an object stream is coded (ac4::ObjectCoding), as atmos-encode's coding=
// names it: index 0, A-JOC, is the default.
inline constexpr std::array<Ac4Choice, 2> kAc4ObjectCodings{{
    {"ajoc", "A-JOC (a downmix and the matrices that rebuild the objects)"},
    {"direct", "Direct-coded (each object a channel of its own)"},
}};

struct Ac4EncodeSettings {
    std::size_t frame_rate = kAc4NativeFrameRate;  // into kAc4FrameRates
    std::size_t rate_mode = 0;                     // into kAc4RateModes
    std::size_t codec_mode = 0;                    // into kAc4CodecModes
    // dialnorm=: 0 to 31.75 dB below full scale in steps of 0.25, or
    // measured from the programme.
    double dialnorm_db = 31.0;
    bool measure_dialnorm = false;
    std::optional<std::size_t> loudness;          // into kAc4LoudnessPractices; unset, none sent
    std::optional<std::size_t> drc;               // into kAc4DrcProfiles; unset, no DRC
    std::optional<std::size_t> centre_level;      // into kAc4CentreLevels; unset, the default
    std::optional<std::size_t> surround_level;    // into kAc4SurroundLevels
    std::optional<std::size_t> preferred_downmix; // into kAc4PreferredDownmixes
    // Dialogue enhancement over the channels that carry dialogue alone.
    bool dialogue_left = false;
    bool dialogue_right = false;
    bool dialogue_centre = false;
    bool dialogue_mid = false;  // dialogue-method=mid, L and R's Mid
    std::size_t dialogue_max_gain = 2;  // into kAc4DialogueMaxGains; 9 dB
    int iframe_interval = 24;
    bool crc = true;  // a raw stream's Annex G CRC; an MP4 sample has none
    // Object mode's one choice of its own, into kAc4ObjectCodings; what it shares
    // with the channels' encode is the dialnorm and the CRC.
    std::size_t object_coding = 0;
};

[[nodiscard]] bool ac4_downmix_named(const Ac4EncodeSettings& settings);
[[nodiscard]] bool ac4_dialogue_named(const Ac4EncodeSettings& settings);

// The trailing tokens of `ac3cli ac4-encode <in> <out> <kbps>` for these
// settings, the output an MP4 file where `mp4`.
[[nodiscard]] std::vector<std::string> ac4_cli_tokens(const Ac4EncodeSettings& settings, bool mp4);

// Why ac4-encode refuses these settings for a source of `channels` channels at
// `sample_rate_hz` before it reads the audio, in its words; nothing where it
// does not. The encoder's own refusals (ac4::Encoder::refusal_reason) come
// after this.
[[nodiscard]] std::optional<std::string> ac4_settings_refusal(const Ac4EncodeSettings& settings,
                                                              std::size_t channels,
                                                              int sample_rate_hz);

// The configuration ac4-encode builds from the same tokens, for a source of
// `channels` channels, before it measures anything: dialnorm is the page's
// value, and the loudness values dialnorm=auto and loudness= measure are the
// caller's to add (ac3::apps::measure_ac4_programme).
[[nodiscard]] ac4::EncoderConfig ac4_encoder_config(const Ac4EncodeSettings& settings,
                                                    int channels, int sample_rate_hz,
                                                    int bitrate_kbps);

// The practice loudness= names, where it names one.
[[nodiscard]] std::optional<ac4::LoudnessPractice> ac4_loudness_practice(
    const Ac4EncodeSettings& settings);

// --- Object mode ---------------------------------------------------------------
//
// Objects under the AC-4 codec are written by `ac3cli atmos-encode ... codec=ac4`
// (apps/common/ac4_objects_core.hpp), which takes fewer options than the channels'
// ac4-encode: an object stream is written at frame_rate_index 13 in a constant
// rate, and the loudness values, DRC, the stereo downmix and dialogue enhancement
// describe channels. What it takes is the coding (coding=), the dialnorm in whole
// dB (dialnorm=, 1 to 31, as atmos-encode reads it) and a raw stream's CRC
// (crc=). The other settings stay as they were, unread, for when the codec is
// channels again.

// The dialnorm object mode sends, in dB below full scale: the setting where it is
// a whole number from 1 to 31, nothing where it is measured, fractional or 0.
[[nodiscard]] std::optional<int> ac4_object_dialnorm(const Ac4EncodeSettings& settings);

// The trailing tokens of `ac3cli atmos-encode <in> <out> <kbps> ... codec=ac4`
// for these settings, without the codec=ac4 itself: coding=direct, a dialnorm
// off 31, and crc=off on a raw stream. Empty at every default.
[[nodiscard]] std::vector<std::string> ac4_object_cli_tokens(const Ac4EncodeSettings& settings,
                                                             bool mp4);

// Why atmos-encode refuses these settings for objects before it reads the audio,
// in its words; nothing where it does not.
[[nodiscard]] std::optional<std::string> ac4_object_settings_refusal(
    const Ac4EncodeSettings& settings);

// What E9's writer is given for these settings at `sample_rate_hz` and
// `bitrate_kbps`: the coding and the dialnorm, and the rest at the writer's own.
[[nodiscard]] ac3::apps::Ac4ObjectsParams ac4_objects_params(const Ac4EncodeSettings& settings,
                                                             std::uint32_t sample_rate_hz,
                                                             int bitrate_kbps);

}  // namespace ac3gui

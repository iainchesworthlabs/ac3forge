#include <catch2/catch_test_macros.hpp>

#include <string>

#include "eld_proc.hpp"

// The ALSA EDID/ELD backend's pure half (roadmap UX9), tested on a machine
// with no sound card and no /proc/asound at all - see
// test_alsa_device_names.cpp's own header comment for why this directory
// exists and how CMake gates it. sink_capabilities.cpp (finding which
// eld# file to read) needs real ALSA hardware and is not tested here;
// parse_eld_proc_text() (deciding what the text in that file means) does not,
// and is where a mistake would be silent - a sink that fell back to a
// probe would still work, but one this misread as accepting AC-3 when it
// does not would fail only on the receiver end, nowhere this project's own
// test suite would ever see it.

using ac3::alsa::parse_eld_proc_text;

namespace {

// Field text and ordering as confirmed against real /proc/asound/card*/eld#*
// output found in the wild - the exact whitespace (a run of tabs after each
// key) does not matter to split_eld_line, which splits on any whitespace run.
constexpr std::string_view kLpcmOnly =
    "monitor_present\t\t1\n"
    "eld_valid\t\t1\n"
    "sad_count\t\t1\n"
    "sad0_coding_type\t[0x1] LPCM\n"
    "sad0_channels\t\t2\n"
    "sad0_rates\t\t[0x1ee0] 32000 44100 48000 88200 96000 176400 192000\n"
    "sad0_bits\t\t[0xe0000] 16 20 24\n";

constexpr std::string_view kLpcmAc3Eac3 =
    "monitor_present\t\t1\n"
    "eld_valid\t\t1\n"
    "sad_count\t\t3\n"
    "sad0_coding_type\t[0x1] LPCM\n"
    "sad0_channels\t\t2\n"
    "sad0_rates\t\t[0x1e0] 32000 44100 48000\n"
    "sad0_bits\t\t[0xe0000] 16 20 24\n"
    "sad1_coding_type\t[0x2] AC-3\n"
    "sad1_channels\t\t6\n"
    "sad1_rates\t\t[0x0e0] 32000 44100 48000\n"
    "sad1_max_bitrate\t640\n"
    "sad2_coding_type\t[0xa] Dolby Digital+ (Dolby Digital Plus)\n"
    "sad2_channels\t\t8\n"
    "sad2_rates\t\t[0x0e0] 32000 44100 48000\n"
    "sad2_max_bitrate\t0\n";

constexpr std::string_view kAc3OnlyNoLpcm =
    "monitor_present\t\t1\n"
    "eld_valid\t\t1\n"
    "sad_count\t\t1\n"
    "sad0_coding_type\t[0x2] AC-3\n"
    "sad0_channels\t\t6\n"
    "sad0_rates\t\t[0x0e0] 32000 44100 48000\n"
    "sad0_max_bitrate\t640\n";

constexpr std::string_view kMonitorNotPresent =
    "monitor_present\t\t0\n"
    "eld_valid\t\t0\n";

constexpr std::string_view kEldInvalid =
    "monitor_present\t\t1\n"
    "eld_valid\t\t0\n"
    "sad_count\t\t0\n";

}  // namespace

TEST_CASE("an LPCM-only descriptor reports pcm true and no compressed format") {
    const auto caps = parse_eld_proc_text(kLpcmOnly);
    REQUIRE(caps.has_value());
    CHECK(caps->pcm);
    CHECK_FALSE(caps->ac3);
    CHECK_FALSE(caps->eac3);
    CHECK(caps->max_pcm_channels == 2);
    CHECK(caps->pcm_sample_rates_hz ==
          std::vector<std::uint32_t>{32000, 44100, 48000, 88200, 96000, 176400, 192000});
}

TEST_CASE("a descriptor with LPCM, AC-3 and E-AC-3 SADs reports all three") {
    const auto caps = parse_eld_proc_text(kLpcmAc3Eac3);
    REQUIRE(caps.has_value());
    CHECK(caps->pcm);
    CHECK(caps->ac3);
    CHECK(caps->eac3);
    // The compressed SADs' own channel counts (6, 8) must not inflate this -
    // only the LPCM SAD's own sad0_channels does, and only PCM playback ever
    // reads this field.
    CHECK(caps->max_pcm_channels == 2);
    CHECK(caps->pcm_sample_rates_hz == std::vector<std::uint32_t>{32000, 44100, 48000});
}

TEST_CASE("a compressed-only descriptor leaves the PCM fields at unknown") {
    const auto caps = parse_eld_proc_text(kAc3OnlyNoLpcm);
    REQUIRE(caps.has_value());
    CHECK_FALSE(caps->pcm);
    CHECK(caps->ac3);
    CHECK(caps->max_pcm_channels == 0);
    CHECK(caps->pcm_sample_rates_hz.empty());
}

TEST_CASE("monitor_present 0 reports no descriptor, not a parse failure") {
    const auto caps = parse_eld_proc_text(kMonitorNotPresent);
    REQUIRE_FALSE(caps.has_value());
    CHECK(caps.error() == ac3::audio::EdidError::kNoEdid);
}

TEST_CASE("eld_valid 0 reports no descriptor even when a monitor is present") {
    const auto caps = parse_eld_proc_text(kEldInvalid);
    REQUIRE_FALSE(caps.has_value());
    CHECK(caps.error() == ac3::audio::EdidError::kNoEdid);
}

TEST_CASE("text with neither monitor_present nor eld_valid is a parse failure") {
    const auto caps = parse_eld_proc_text("garbage that is not an eld proc file at all\n");
    REQUIRE_FALSE(caps.has_value());
    CHECK(caps.error() == ac3::audio::EdidError::kParseFailed);
}

TEST_CASE("empty text is a parse failure") {
    const auto caps = parse_eld_proc_text("");
    REQUIRE_FALSE(caps.has_value());
    CHECK(caps.error() == ac3::audio::EdidError::kParseFailed);
}

TEST_CASE("a final line with no trailing newline is still read") {
    const auto caps = parse_eld_proc_text(
        "monitor_present\t\t1\neld_valid\t\t1\nsad0_coding_type\t[0x1] LPCM");
    REQUIRE(caps.has_value());
    CHECK(caps->pcm);
}

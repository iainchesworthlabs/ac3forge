// The hardware self-report's arithmetic, tested on the host - see
// ac3forge/hardware_info.hpp's own header comment for why this can be, and
// sink_plan.hpp's test beside this one for the same discipline on a chip's
// I2S ceiling.

#include <catch2/catch_test_macros.hpp>

#include "ac3forge/hardware_info.hpp"

using ac3forge::describe_hardware;
using ac3forge::HardwareFacts;

namespace {

bool has(const std::vector<std::string>& lines, std::string_view needle) {
    for (const auto& line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("a part with an FPU and PSRAM reports both as capabilities, neither as a notice",
          "[io][hardware_info]") {
    HardwareFacts facts;
    facts.target = "esp32s3";
    facts.chip = "ESP32-S3";
    facts.revision_major = 0;
    facts.revision_minor = 2;
    facts.cores = 2;
    facts.fpu = true;
    facts.cpu_freq_mhz = 240;
    facts.psram_bytes = 8 * 1024 * 1024;
    facts.sink_max_slots = 16;

    const auto report = describe_hardware(facts);
    REQUIRE(has(report.capabilities, "2 cores"));
    REQUIRE(has(report.capabilities, "hardware floating-point unit"));
    REQUIRE(has(report.capabilities, "Running at 240 MHz"));
    REQUIRE(has(report.capabilities, "8 MiB of PSRAM"));
    REQUIRE(has(report.capabilities, "up to 16 slots"));
    REQUIRE_FALSE(has(report.notices, "floating point"));
    REQUIRE_FALSE(has(report.notices, "PSRAM"));
    REQUIRE_FALSE(has(report.notices, "built for"));
}

TEST_CASE("a part with no FPU and no PSRAM notices both, and never claims a capability it lacks",
          "[io][hardware_info]") {
    // The ESP32-C6's own shape: one core, no PSRAM at the die level, no FPU.
    HardwareFacts facts;
    facts.target = "esp32c6";
    facts.chip = "ESP32-C6";
    facts.revision_major = 0;
    facts.revision_minor = 2;
    facts.cores = 1;
    facts.fpu = false;
    facts.cpu_freq_mhz = 160;
    facts.psram_bytes = 0;
    facts.sink_max_slots = 8;

    const auto report = describe_hardware(facts);
    REQUIRE(has(report.capabilities, "fixed-point arithmetic"));
    REQUIRE(has(report.capabilities, "Running at 160 MHz"));
    REQUIRE_FALSE(has(report.capabilities, "PSRAM"));
    REQUIRE(has(report.notices, "No hardware floating point"));
    REQUIRE(has(report.notices, "No PSRAM in this build"));
}

TEST_CASE("a firmware built for one target running on another is noticed, case and hyphen insensitively",
          "[io][hardware_info]") {
    HardwareFacts mismatched;
    mismatched.target = "esp32c6";
    mismatched.chip = "ESP32-S3";
    REQUIRE(has(describe_hardware(mismatched).notices, "built for esp32c6"));

    HardwareFacts matched;
    matched.target = "esp32c6";
    matched.chip = "ESP32-C6";
    REQUIRE_FALSE(has(describe_hardware(matched).notices, "built for"));

    // Never claimed when either side is unknown: an owner with nothing to
    // say about the chip leaves both empty rather than this reading as a
    // mismatch.
    HardwareFacts unknown;
    unknown.target = "esp32c6";
    REQUIRE_FALSE(has(describe_hardware(unknown).notices, "built for"));
}

TEST_CASE("a revision notice fires only once the detected chip clears the threshold, and carries the caller's own cost",
          "[io][hardware_info]") {
    // The ESP32-P4's own shape (docs/platforms/bare-metal/esp32-p4.md): a
    // build with CONFIG_ESP32P4_SELECTS_REV_LESS_V3 accepts chip revision
    // v1.0 and up, but the notice is about v3.0 specifically (where
    // hal/i2s_ll.h's own clock-source choice changes), NOT about this
    // build's own lowered floor - a pre-production board (v1.3) is newer
    // than that floor but still below v3.0, so nothing is noticed.
    HardwareFacts pre_production;
    pre_production.revision_major = 1;
    pre_production.revision_minor = 3;
    pre_production.revision_notice_at = 300;
    pre_production.revision_floor_cost = "cost words";
    REQUIRE_FALSE(has(describe_hardware(pre_production).notices, "cost words"));

    // A v3.0+ chip under the SAME conservative build: it clears the
    // threshold the build's own accommodation was never guaranteed to meet,
    // which is the case worth a rebuild.
    HardwareFacts undersold;
    undersold.revision_major = 3;
    undersold.revision_minor = 0;
    undersold.revision_notice_at = 300;
    undersold.revision_floor_cost = "a build that required v3.0 or newer could use the faster clock";
    const auto report = describe_hardware(undersold);
    REQUIRE(has(report.notices, "detected chip is v3.0"));
    REQUIRE(has(report.notices, "v3.0 or newer"));
    REQUIRE(has(report.notices, "a build that required v3.0 or newer could use the faster clock"));

    // Comfortably past the threshold (v3.1): still fires, at-or-above, not
    // only exactly at it.
    HardwareFacts past;
    past.revision_major = 3;
    past.revision_minor = 1;
    past.revision_notice_at = 300;
    past.revision_floor_cost = "cost words";
    REQUIRE(has(describe_hardware(past).notices, "detected chip is v3.1"));

    // No threshold at all (every target but the ESP32-P4 today): nothing to
    // say, however old or new the detected revision reads.
    HardwareFacts no_threshold;
    no_threshold.revision_major = 3;
    no_threshold.revision_minor = 0;
    REQUIRE_FALSE(has(describe_hardware(no_threshold).notices, "or newer"));
}

TEST_CASE("a sink with nothing to say about its ceiling reports no slot count, not a limit of zero",
          "[io][hardware_info]") {
    HardwareFacts facts;
    facts.sink_max_slots = 0;
    REQUIRE_FALSE(has(describe_hardware(facts).capabilities, "slot"));
}

TEST_CASE("a sink ceiling names the slot width it was reached at, when the owner has one to give",
          "[io][hardware_info]") {
    HardwareFacts widthed;
    widthed.sink_max_slots = 32;
    widthed.sink_max_slots_bits = 16;
    REQUIRE(has(describe_hardware(widthed).capabilities, "up to 32 slots at 16-bit"));

    // No width to give (a capture/null sink, or any future one whose
    // ceiling is not a function of slot width) - the plain count stands,
    // never a guessed qualifier.
    HardwareFacts unwidthed;
    unwidthed.sink_max_slots = 8;
    unwidthed.sink_max_slots_bits = 0;
    const auto report = describe_hardware(unwidthed);
    REQUIRE(has(report.capabilities, "up to 8 slots"));
    REQUIRE_FALSE(has(report.capabilities, "up to 8 slots at"));
}

TEST_CASE("a hard revision limit fires only below its own threshold, the opposite direction from "
          "the notice above",
          "[io][hardware_info]") {
    // The ESP32-P4's own shape (esp32p4-hearth-sink-tdm-clock-ceiling): no
    // PLL clock source for I2S below v3.0, so TDM cannot open at any channel
    // count above 2 on such a chip, regardless of build - the notice is
    // unconditional on the detected revision alone.
    HardwareFacts below;
    below.revision_major = 1;
    below.revision_minor = 3;
    below.revision_hard_limit_below = 300;
    below.revision_hard_limit_cost = "only standard 1-2 channel I2S works on this chip";
    const auto report = describe_hardware(below);
    REQUIRE(has(report.notices, "detected chip is v1.3"));
    REQUIRE(has(report.notices, "below v3.0"));
    REQUIRE(has(report.notices, "only standard 1-2 channel I2S works on this chip"));

    // At or above the threshold: the limitation does not apply, nothing is
    // said.
    HardwareFacts at;
    at.revision_major = 3;
    at.revision_minor = 0;
    at.revision_hard_limit_below = 300;
    at.revision_hard_limit_cost = "cost words";
    REQUIRE_FALSE(has(describe_hardware(at).notices, "cost words"));

    // No threshold at all: nothing to say, however old the detected
    // revision reads.
    HardwareFacts no_threshold;
    no_threshold.revision_major = 1;
    no_threshold.revision_minor = 0;
    REQUIRE_FALSE(has(describe_hardware(no_threshold).notices, "below v"));
}

TEST_CASE("no clock reading claims no clock capability", "[io][hardware_info]") {
    HardwareFacts facts;
    facts.cpu_freq_mhz = 0;
    REQUIRE_FALSE(has(describe_hardware(facts).capabilities, "Running at"));
}

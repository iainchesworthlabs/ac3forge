#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// What this board is, in the user's own words: GET /hardware (control.hpp)
// reports it, so that a firmware misbehaving in the field says why from its
// own web page rather than needing a debugger and this project's memory of
// which chip does what - "I detect myself as an ESP32-P4, revision 1.3, no
// PSRAM fitted" is the whole point, not a guess from the model number alone.
//
// Free of ESP-IDF, like sink_plan.hpp beside it and for the same reason: the
// facts are a target's own, gathered once by control.cpp (already ESP-IDF-only)
// from esp_chip_info(), esp_psram and CONFIG_SOC_CPU_HAS_FPU; everything below
// is turning them into fixed English sentences, which is exactly the part
// worth being able to get wrong on a laptop rather than only on a board -
// tests/io/test_hardware_info.cpp exercises every chip this component has
// ever built for from synthetic facts, no board involved. Nothing here reads
// a Kconfig symbol or an ESP-IDF header by name, the same discipline
// sink_plan.hpp's own header comment explains for I2S_LL_SLOT_FRAME_BIT_MAX.

namespace ac3forge {

// One board's self-report. `target` is CONFIG_IDF_TARGET, verbatim
// ("esp32p4"): what this firmware was BUILT for. `chip` is esp_chip_info()'s
// model, named ("ESP32-P4"): what is ACTUALLY running it. The two usually
// agree - a firmware for one target refuses to boot on the wrong die - but
// they are gathered by two independent paths (one a compile-time string, the
// other a runtime register read), so describe_hardware checks them against
// each other rather than assuming a match: the one case this catches, a
// generic image flashed to the wrong board in a fleet, is exactly the kind of
// mistake nothing else here would notice.
struct HardwareFacts {
    std::string target;
    std::string chip;
    int revision_major = 0;
    int revision_minor = 0;
    int cores = 0;
    bool fpu = false;
    // CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, verbatim: what this build actually
    // runs the CPU at, not a peripheral's own clock (I2S's, say) and not a
    // ceiling the silicon could reach under a different build. 0 when a
    // caller has nothing to say (every real board sets this; only synthetic
    // facts in a test leave it at the default).
    int cpu_freq_mhz = 0;
    // 0 when PSRAM was never brought up: absent, or present but this build's
    // Kconfig never turned CONFIG_SPIRAM on. Either way, features that need
    // it are not available, which is the fact worth reporting; this struct
    // does not distinguish the two causes; a caller with something to say has
    // its own opportunity to say it (see kind of `notice`, below).
    std::size_t psram_bytes = 0;
    // The most slots this build's sink could ever be asked to carry - its
    // hardware ceiling, from the owner's own player::sink_max_slots() (see
    // ControlHandlers::sink_max_slots). 0 when the owner has nothing to say
    // (an owner with no sink at all), which is left out of the report rather
    // than shown as a limit of zero.
    int sink_max_slots = 0;
    // The chip revision (esp_chip_info_t's own MXX encoding, major * 100 +
    // minor) at or above which a build that assumed newer silicon than this
    // one does could have behaved differently - 0 when there is no such
    // threshold to speak of, which is every target but the ESP32-P4 today
    // (docs/platforms/bare-metal/esp32-p4.md, "The chip revision, and what
    // it blocks"). NOT this build's own accepted floor: a build that lowers
    // its floor to admit pre-production silicon still refuses anything
    // older than that floor outright (the bootloader itself, before this
    // code ever runs), so the one comparison worth making post-boot is
    // whether the detected chip clears the threshold a STRICTER build would
    // have required - the direction that can actually be observed here.
    // Paired with `revision_floor_cost`: what clearing it would have
    // bought, in the caller's own words, since this header does not know
    // what an I2S clock source is - the same reason sink_plan.hpp takes
    // frame_bit_max as a required parameter rather than a target-specific
    // default hidden inside it. Read together only when both are set.
    int revision_notice_at = 0;
    std::string revision_floor_cost;
};

struct HardwareReport {
    std::vector<std::string> capabilities;  // what this build can do
    std::vector<std::string> notices;       // limits, gaps and mismatches
};

namespace detail {

// A byte count as whole MiB where that is exact enough to read at a glance,
// otherwise the plain number - a `psram_bytes` of exactly 33,554,432 reads as
// "32 MiB"; anything odd (it should never be, but a report must not lie
// about a figure it cannot round cleanly) reads as itself.
[[nodiscard]] inline std::string describe_bytes(std::size_t bytes) {
    constexpr std::size_t kMiB = 1024 * 1024;
    if (bytes > 0 && bytes % kMiB == 0) {
        return std::to_string(bytes / kMiB) + " MiB";
    }
    return std::to_string(bytes) + " bytes";
}

// "1.3" from major 1, minor 3 - esp_chip_info_t's own MXX revision, split.
[[nodiscard]] inline std::string describe_revision(int major, int minor) {
    return std::to_string(major) + "." + std::to_string(minor);
}

// Upper case, hyphens and underscores dropped - "esp32p4" and "ESP32-P4"
// both become "ESP32P4", which is all `target` and `chip` need to agree on:
// neither name is free to change spelling on its own, only whether the two
// still name the same die.
[[nodiscard]] inline std::string canonical_chip_form(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        if (c == '-' || c == '_') {
            continue;
        }
        out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace detail

// Pure: every sentence comes only from `facts`, so a wrong one is a wrong
// INPUT (control.cpp's gathering) or a wrong RULE (here), never a
// board-only bug.
[[nodiscard]] inline HardwareReport describe_hardware(const HardwareFacts& facts) {
    HardwareReport report;

    if (!facts.target.empty() && !facts.chip.empty() &&
        detail::canonical_chip_form(facts.target) != detail::canonical_chip_form(facts.chip)) {
        report.notices.push_back("This firmware was built for " + facts.target +
                                 ", but the chip it is running on identifies itself as " +
                                 facts.chip + ".");
    }

    report.capabilities.push_back(
        std::to_string(facts.cores) + (facts.cores == 1 ? " core, " : " cores, ") +
        (facts.fpu ? "a hardware floating-point unit" : "fixed-point arithmetic (no floating-point unit)"));
    if (!facts.fpu) {
        report.notices.push_back(
            "No hardware floating point on this die: this build's decode arithmetic is fixed-point.");
    }

    if (facts.cpu_freq_mhz > 0) {
        report.capabilities.push_back("Running at " + std::to_string(facts.cpu_freq_mhz) + " MHz");
    }

    if (facts.psram_bytes > 0) {
        report.capabilities.push_back(detail::describe_bytes(facts.psram_bytes) + " of PSRAM");
    } else {
        report.notices.push_back(
            "No PSRAM in this build: a wide buffer ring, deep DMA queues or an object "
            "reconstruction buffer fall back to internal RAM, or may not fit at all.");
    }

    if (facts.sink_max_slots > 0) {
        report.capabilities.push_back("This sink's bus reaches up to " +
                                      std::to_string(facts.sink_max_slots) +
                                      (facts.sink_max_slots == 1 ? " slot" : " slots"));
    }

    if (facts.revision_notice_at != 0 && !facts.revision_floor_cost.empty()) {
        const int detected = facts.revision_major * 100 + facts.revision_minor;
        if (detected >= facts.revision_notice_at) {
            report.notices.push_back(
                "The detected chip is v" +
                detail::describe_revision(facts.revision_major, facts.revision_minor) + ", v" +
                detail::describe_revision(facts.revision_notice_at / 100, facts.revision_notice_at % 100) +
                " or newer. " + facts.revision_floor_cost);
        }
    }

    return report;
}

}  // namespace ac3forge

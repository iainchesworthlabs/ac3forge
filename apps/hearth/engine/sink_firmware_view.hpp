#pragma once

#include <string>
#include <string_view>

#include "sink_firmware.hpp"

// The Network page's Firmware tab (NetworkSinkFirmware.qml) as display
// strings, formatted here rather than in QML for network_view.hpp's reason:
// the page formats nothing itself, and these can be tested without Qt. The
// words are ota.py's where it has some, so the tool, the board's own page and
// this app say the same about one update.

namespace ac3::hearth {

struct FirmwarePanel {
    // The board answers GET /firmware now. While it does not - restarting,
    // gone, or firmware from before O1 - status_text says why, and the rows
    // below keep what it said last.
    bool answering = false;
    std::string status_text{};
    // Whether there is anything to show in the rows at all.
    bool reported = false;
    std::string running_text{};  // "v0.10.0-… · ota_0 · accepted · checked intact"
    std::string other_text{};
    // The two images' versions alone, for the sentences that ask first.
    std::string running_version{};
    std::string other_version{};
    // Whether the running image is the build this app is (its own git
    // describe), and the sentence that says so; empty when this app has no
    // version to compare with.
    bool same_build = false;
    std::string build_text{};
    std::string mode_text{};         // flash mode, or empty
    std::string trial_text{};        // the running image's trial, or empty
    std::string upload_text{};       // an update another client is sending, or empty
    std::string last_update_text{};  // how the last update ended, or empty
    std::string crash_text{};        // the last crash's core dump (O4), or empty

    // This computer's own update: under way, or how it ended.
    bool updating = false;
    // 0 to 1 while the image is being sent; -1 for a stage with no measure.
    double progress = -1;
    std::string progress_text{};
    // "" | "updated" | "rolledBack" | "refused" | "failed" | "silent" - QML's
    // own switch key, never translated.
    std::string outcome{};
    std::string outcome_text{};
    // The board's answer to the last Restart or Roll back.
    std::string action_text{};

    bool can_update = false;
    bool can_rollback = false;
    bool can_restart = false;

    std::string page_url{};      // the board's own page
    std::string log_url{};       // its recent console output (O4)
    std::string coredump_url{};  // its last crash's core dump, when it has one
};

// `app_version` is this app's own git describe (ac3::git_describe), empty
// when it was built without one.
[[nodiscard]] FirmwarePanel to_firmware_panel(const SinkFirmware::Snapshot& snapshot, std::string_view app_version);

// An image file chosen for a sink, as the dialog that asks first shows it.
struct FirmwareCandidate {
    std::string version{};
    // "ac3forge_hearth_sink v0.10.0-…, for an ESP32-S3, 1,480,768 bytes"
    std::string text{};
    // Why the sink would not take it, from what it last said; empty when it
    // would. The update checks again with what the board says then.
    std::string refusal{};
};

[[nodiscard]] FirmwareCandidate to_candidate(const FirmwareFile& file, const SinkFirmware::Snapshot& snapshot);

}  // namespace ac3::hearth

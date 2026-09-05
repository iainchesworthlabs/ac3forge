#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "default_device.hpp"
#include "engine.hpp"
#include "virtual_device.hpp"

// The diagnostics file (docs/crucible/troubleshooting.md, "Saving a
// diagnostics file"): a bounded ring of one-line notes that the engine, the
// window's Qt message handler and the controller write to, and a renderer
// that turns named facts, an engine snapshot and the ring into plain text.
//
// The rule this module exists to hold: the file never carries the signing
// key, the path to the key file, or the value of any environment variable.
// It is held twice. First structurally - the report is composed from named
// fields, and there is no field for the key bytes, the key path (whether
// chosen in Settings or given through AC3FORGE_SIGNING_KEY_FILE) or the
// inline AC3FORGE_SIGNING_KEY value; EngineStatus::signing (which names the
// key file for the Settings page) and AppStatus::image_path (a path inside
// the person's profile) are never read; the settings section is a fixed list
// of keys the caller whitelists, and any key under "signing/" is written as
// withheld whatever value arrived with it; the environment is never
// enumerated. Second, as a belt and braces for text that arrived through the
// message ring: the finished report is passed through scrub(), which replaces
// every spelling of the key path and of the inline key value.
//
// A future protected key store (docs/crucible/install.md promises one) must
// live under the "signing/" settings prefix so the renderer's rule covers it
// without a code change.
//
// No Qt, no platform header and no fmt here: the engine's threads write to
// the ring, the console runner can reuse the renderer, and ac3tests holds
// the rule on every CI leg.

namespace ac3::crucible {

// A bounded ring of recent one-line notes: what changed and what refused,
// never anything per frame. Written from the frame thread, the probe thread,
// the Qt message handler and the GUI thread, so every entry point takes the
// mutex and does nothing else that could block or log. Each note is stamped
// "+ssss.mmm " (seconds since the log was made, steady clock), has CR, LF and
// every other control character replaced by a space so one note is one line
// of the report, and is cut to kMaxLine bytes (at a UTF-8 boundary, marked
// " ...") when longer.
class DiagnosticLog {
public:
    static constexpr std::size_t kMaxLine = 512;
    static constexpr std::size_t kStampBytes = 10;  // "+ssss.mmm "
    static constexpr std::size_t kDefaultCapacity = 512;

    explicit DiagnosticLog(std::size_t capacity = kDefaultCapacity);
    DiagnosticLog(const DiagnosticLog&) = delete;
    DiagnosticLog& operator=(const DiagnosticLog&) = delete;

    void note(std::string_view line);
    // Oldest first.
    [[nodiscard]] std::vector<std::string> lines() const;
    // Notes that fell off the ring to make room.
    [[nodiscard]] std::uint64_t dropped() const;
    [[nodiscard]] std::size_t capacity() const { return capacity_; }
    [[nodiscard]] std::chrono::system_clock::time_point started_at() const { return wall_start_; }

private:
    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::vector<std::string> ring_;  // used as a deque: erase(begin()) when full is fine at 512
    std::uint64_t dropped_ = 0;
    std::chrono::steady_clock::time_point start_;
    std::chrono::system_clock::time_point wall_start_;
};

// The process-wide log the window's Qt message handler, the engine and the
// controller share. Allocated once and never destroyed, so a message emitted
// during static destruction cannot touch a dead object.
[[nodiscard]] DiagnosticLog& process_diagnostics();

// How the signing key was obtained, without saying where it is.
enum class KeySource : std::uint8_t { kNone, kFile, kEnvironmentFile, kEnvironmentInline };

struct SigningFacts {
    bool objects_enabled = false;
    KeySource source = KeySource::kNone;
    bool env_key_file_set = false;    // AC3FORGE_SIGNING_KEY_FILE present (its value is never read here)
    bool env_key_inline_set = false;  // AC3FORGE_SIGNING_KEY present (likewise)
};

// Everything the report is composed from, as named fields. There is no
// field for the key bytes, the key path, or any environment value, which is
// the first half of the rule; scrub() below is the second.
struct ReportFacts {
    std::string written_at;      // ISO 8601, formatted by the caller
    std::string log_started_at;  // likewise
    std::string version;         // ac3::version_details()
    std::vector<std::pair<std::string, std::string>> platform;  // name/value rows, in order
    SigningFacts signing;
    std::vector<RenderEndpoint> render_endpoints;  // DefaultDevice::endpoints()
    bool default_is_silent = false;                // ...and the default among them is the silent device
    std::string previous_default_name;
    bool moves_default = true;
    std::string default_message;
    std::string silent_device_name;
    bool silent_from_package = false;
    std::string silent_advice;
    SilentDeviceState silent;
    std::string driver_dir;
    std::string driver_message;
    bool foreground_available = false;
    std::string foreground_reason;
    // Whitelisted by the caller, in order. The renderer applies the rule a
    // second time: any key under "signing/" is rendered as withheld whatever
    // value arrived with it.
    std::vector<std::pair<std::string, std::string>> settings;
};

// Every spelling of a secret the report must not carry: the key path with
// native and forward separators and in its canonical form, the
// AC3FORGE_SIGNING_KEY_FILE value in the same forms, the AC3FORGE_SIGNING_KEY
// value. Empty strings are ignored.
struct Secrets {
    std::vector<std::string> strings;
};

// Replaces each secret, matched case-insensitively, with "<withheld>".
[[nodiscard]] std::string scrub(std::string text, const Secrets& secrets);

// The report. Reads named fields of `engine` only: never EngineStatus::signing
// (it names the key file) and never AppStatus::image_path; the application
// list carries name and description alone. Runs the finished text through
// scrub() before returning it.
[[nodiscard]] std::string render_report(const ReportFacts& facts, const EngineStatus& engine,
                                        const DiagnosticLog& log, const Secrets& secrets);

}  // namespace ac3::crucible

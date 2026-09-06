#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ac3gui's diagnostics file (docs/gui/accessibility.md, "Saving a diagnostics
// file"): a bounded ring of one-line notes the controller writes to, and a
// renderer that turns named facts and that ring into plain text.
//
// The rule this module exists to hold, taken from the Crucible's own
// (apps/crucible/engine/diagnostics.hpp) because it was written for exactly
// this and has held since 2026-09: the file never carries the signing key,
// the path to a key file, the value of any environment variable, or one byte
// of anything the person loaded.
//
// It is held twice. First structurally - the report is composed from named
// fields, and there is no field for key bytes, for a key path, for the inline
// AC3FORGE_SIGNING_KEY value, or for one sample of audio; the settings
// section is a fixed list the caller whitelists, and any key under "signing/"
// is written as withheld whatever value arrived with it; the environment is
// never enumerated, only asked whether the two named signing variables exist
// at all. Second, as belt and braces for text that arrived through the
// message ring: the finished report is passed through scrub(), which replaces
// every spelling of the key path and of the inline key value with
// "<withheld>".
//
// What the report DOES carry, said plainly because it is not nothing: the
// sources section names each source the way the window's own rail names it,
// by base name and shape rather than by the path it was opened from, and the
// settings section carries the output folder the person chose, because that
// is a setting they set. The ring is the one section whose content this
// module does not compose: every EncoderController::setStatus() text lands
// in it, and those name files by base name today ("Could not read
// stems.wav: ...", "Wrote 212 frames ... to take.ec3") but nothing here
// enforces that on a caller added later. So the file is worth reading before
// it is attached to anything - which is why it is written where the person
// chooses and sent nowhere.
//
// This is a second copy of the Crucible's ring and scrub rather than a shared
// one, because that header's ReportFacts is welded to the Crucible engine's
// own types (EngineStatus, RenderEndpoint, SilentDeviceState) and ac3gui has
// none of them. Lifting the ring and scrub into apps/common/ and leaving each
// window its own ReportFacts would remove the duplication; that is a change
// to a module the Crucible's tests already hold, and is worth doing on its
// own rather than inside an accessibility pass.
//
// No Qt here, deliberately, for the same reason the Crucible's is Qt-free:
// tests/gui/test_gui_diagnostics.cpp compiles this file straight into ac3tests,
// so the rule is checked on every CI leg including the ones that build no
// window at all. That is also why the file carries a gui_ prefix inside a
// directory already called gui: ac3tests compiles this module and the
// Crucible's into one target with both directories on its include path, and
// two headers named diagnostics.hpp would have a test resolve to whichever
// -I came first.

namespace ac3gui {

// A bounded ring of recent one-line notes: what was loaded, what a run did,
// what refused, never anything per frame or per meter tick. Written from the
// GUI thread and from the encode/capture workers, so every entry point takes
// the mutex and does nothing else that could block. Each note is stamped
// "+ssss.mmm " (seconds since the log was made, steady clock), has CR, LF and
// every other control character replaced by a space so one note is one line
// of the report, and is cut to kMaxLine bytes (at a UTF-8 boundary, marked
// " ...") when longer.
class MessageLog {
public:
    static constexpr std::size_t kMaxLine = 512;
    static constexpr std::size_t kStampBytes = 10;  // "+ssss.mmm "
    static constexpr std::size_t kDefaultCapacity = 256;

    explicit MessageLog(std::size_t capacity = kDefaultCapacity);
    MessageLog(const MessageLog&) = delete;
    MessageLog& operator=(const MessageLog&) = delete;

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
    std::vector<std::string> ring_;  // used as a deque: erase(begin()) when full is fine at 256
    std::uint64_t dropped_ = 0;
    std::chrono::steady_clock::time_point start_;
    std::chrono::system_clock::time_point wall_start_;
};

// The process-wide log the controller writes to. Allocated once and never
// destroyed, so a note emitted during static destruction cannot touch a dead
// object.
[[nodiscard]] MessageLog& process_diagnostics();

// One loaded source, as the report is allowed to describe it: what it is, not
// where it came from and not a byte of what is in it.
struct SourceFacts {
    std::string name;  // base name only - see the header comment
    int channels = 0;
    std::uint32_t rate_hz = 0;
    double seconds = 0.0;
    double offset_seconds = 0.0;
    std::string resample;  // empty when the source was taken at its own rate
};

// One entry of the run strip. `detail` is the failure text the window already
// shows in its own banner, never a path.
struct RunFacts {
    int id = 0;
    std::string status;
    std::string filename;  // base name, the same one the run strip draws
    std::string rate_text;
    std::string duration_text;
    std::string detail;
};

// Everything the report is composed from, as named fields. There is no field
// for key bytes, for a key path, for any environment value, or for sample
// data, which is the first half of the rule; scrub() below is the second.
struct ReportFacts {
    std::string written_at;      // ISO 8601, formatted by the caller
    std::string log_started_at;  // likewise
    std::string version;         // ac3::version_details()
    std::vector<std::pair<std::string, std::string>> platform;  // name/value rows, in order
    // Whether the two signing variables exist. Their values are never read.
    bool env_key_file_set = false;    // AC3FORGE_SIGNING_KEY_FILE
    bool env_key_inline_set = false;  // AC3FORGE_SIGNING_KEY
    std::vector<SourceFacts> sources;
    // The encode plan and the window's own state, as named rows in order.
    std::vector<std::pair<std::string, std::string>> plan;
    // Whitelisted by the caller, in order. The renderer applies the rule a
    // second time: any key under "signing/" is rendered as withheld whatever
    // value arrived with it.
    std::vector<std::pair<std::string, std::string>> settings;
    std::vector<RunFacts> runs;
    // The last refusals and failures, oldest first - what the window said
    // rather than a second judgement of it.
    std::vector<std::string> errors;
};

// Every spelling of a secret the report must not carry: the
// AC3FORGE_SIGNING_KEY_FILE value with native and forward separators and in
// its canonical form, and the AC3FORGE_SIGNING_KEY value. Empty strings are
// ignored.
struct Secrets {
    std::vector<std::string> strings;
};

// Replaces each secret, matched case-insensitively, with "<withheld>".
[[nodiscard]] std::string scrub(std::string text, const Secrets& secrets);

// The report. Runs the finished text through scrub() before returning it.
[[nodiscard]] std::string render_report(const ReportFacts& facts, const MessageLog& log,
                                        const Secrets& secrets);

}  // namespace ac3gui

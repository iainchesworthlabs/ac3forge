#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// The diagnostics ring (planning/hearth-reference-player.md, A3: "a
// diagnostics ring in Crucible's pattern", after
// apps/crucible/engine/diagnostics.hpp): a bounded ring of one-line notes that
// the player, the engine and the window write to, and the means of keeping
// paths out of it. diagnostics_report.hpp turns the ring into the file the
// Settings page saves.
//
// The page promises that pairing keys, codes and file paths are left out.
// A note the player or the engine composes names an item by its place in the
// queue and its title, never by its path. Text that arrives from elsewhere -
// what an item loader said about a file it could not read, which is free to
// quote the file's path - is scrubbed of that item's folder before it is
// noted, while the item is still in the queue to say what its path is.
//
// No Qt and no platform header: the engine's thread writes here, and ac3tests
// holds the rule on every CI leg.

namespace ac3::hearth {

// Recent one-line notes: what changed and what refused, never anything per
// block or per unit. Written from the engine thread and the window's, so
// every entry point takes the mutex, and does nothing under it that could
// block. Each note is stamped "+ssss.mmm " (seconds since the log was made,
// by the steady clock, with more digits after 9999 s), has CR, LF and every
// other control character replaced by a space so that one note is one line
// of the file, and is cut to kMaxLine bytes, at a UTF-8 boundary and marked
// " ...", when longer.
class DiagnosticLog {
public:
    static constexpr std::size_t kMaxLine = 512;
    static constexpr std::size_t kStampBytes = 10;  // "+ssss.mmm ", for the first 9999 s
    static constexpr std::size_t kDefaultCapacity = 512;

    explicit DiagnosticLog(std::size_t capacity = kDefaultCapacity);
    DiagnosticLog(const DiagnosticLog&) = delete;
    DiagnosticLog& operator=(const DiagnosticLog&) = delete;
    DiagnosticLog(DiagnosticLog&&) = delete;
    DiagnosticLog& operator=(DiagnosticLog&&) = delete;
    ~DiagnosticLog() = default;

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
    // A ring: once it is full, `head_` is the oldest line and the next note
    // replaces it.
    std::vector<std::string> ring_;
    std::size_t head_ = 0;
    std::uint64_t dropped_ = 0;
    std::chrono::steady_clock::time_point start_;
    std::chrono::system_clock::time_point wall_start_;
};

// The log the window's message handler, the engine and the window share.
// Made once and never destroyed, so a message written during static
// destruction cannot reach a destroyed log.
[[nodiscard]] DiagnosticLog& process_diagnostics();

// Text a note or the file must not carry, and what stands in its place.
struct Secrets {
    std::vector<std::string> strings;
};

inline constexpr std::string_view kWithheld = "<withheld>";

// Adds what keeps `path` out of a text: its folder and each folder above it,
// in both separators' spellings, so that "C:\Music\a.ec3" reads
// "<withheld>\a.ec3" - which item, and not where it lives. The top of a
// drive or of the file system says nothing about the person and is left, as
// withholding "/" or "C:" would mangle every other line; a bare name has no
// folder to withhold. The first folder of a relative path is a single word,
// and is withheld only where a separator follows it.
void withhold_path(Secrets& secrets, std::string_view path);

// Replaces each secret, matched without regard to ASCII case, with
// "<withheld>". Longer secrets are replaced first, so a folder goes whole
// before the folder above it is looked for.
[[nodiscard]] std::string scrub(std::string text, const Secrets& secrets);

}  // namespace ac3::hearth

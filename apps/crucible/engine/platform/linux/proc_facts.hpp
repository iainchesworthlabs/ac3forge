#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "process_tree.hpp"
#include "session_monitor.hpp"

// What the Linux SessionMonitor knows about a process, where it reads it, and
// the bookkeeping it does with it. session_monitor.cpp's own header comment is
// where the PipeWire half is explained - why the process id is on the Client
// and not on the stream node, and why an application is here only while it is
// playing; none of that is repeated here.
//
// Split out of that file so it can be tested at all. session_monitor.cpp
// includes pipewire_support.hpp, which includes <pipewire/pipewire.h>, so
// nothing in it compiles without the PipeWire development headers and nothing
// links without libpipewire - while none of what is below needs either. The
// same split process_tree.hpp already is, and the walk it holds is driven from
// here: a Catch2 case on any Linux machine exercises this, while the CI leg
// that installs PipeWire deliberately runs no tests at all
// (.github/workflows/_build.yml, the Crucible pass).
//
// The /proc readers take the directory to read from, defaulting to /proc.
// That is what lets a test hand them one it wrote itself - a comm holding a
// space, a stat line whose comm holds a ')', a pid with no exe link across a
// sandbox boundary - rather than depending on whichever processes happen to
// be running on the machine, which is the one thing a test cannot choose. It
// is the same reason process_tree.hpp injects its two readers.

namespace ac3::crucible {

inline constexpr std::string_view kProcRoot = "/proc";

// The parent from a /proc/<pid>/stat line. The second field there (comm, in
// parentheses) may itself hold spaces and parentheses - "(Web Content)",
// "(bash (deleted))" - so the parse starts after the LAST ')' and takes the
// second word from there: state, then ppid. Splitting on whitespace from the
// left instead reads a browser's comm as three fields and answers with a
// letter.
//
// nullopt where the line is not one: an empty read, which is what a process
// that went between the graph walk and the open gives, or a line with no
// ')' in it at all.
[[nodiscard]] inline std::optional<std::uint32_t> parse_ppid(std::string_view stat_line) {
    const auto paren = stat_line.rfind(')');
    if (paren == std::string_view::npos) {
        return std::nullopt;
    }
    std::istringstream rest{std::string{stat_line.substr(paren + 1)}};
    std::string state;
    std::uint32_t parent = 0;
    if (!(rest >> state >> parent)) {
        return std::nullopt;
    }
    return parent;
}

// /proc/<pid>/comm is the kernel's short name for the process, which is what
// Windows calls the image stem. Empty when the process has gone.
[[nodiscard]] inline std::string process_name(std::uint32_t pid,
                                              std::string_view proc_root = kProcRoot) {
    std::ifstream comm(std::string{proc_root} + "/" + std::to_string(pid) + "/comm");
    std::string name;
    std::getline(comm, name);
    return name;
}

// The executable behind a pid, for an icon. /proc/<pid>/exe is a symlink the
// owner can always read for their own processes - except across a sandbox
// boundary: a Flatpak or snap application's link may be unreadable or point
// inside its runtime, which is what the binary name from PipeWire is for.
[[nodiscard]] inline std::string process_exe(std::uint32_t pid,
                                             std::string_view proc_root = kProcRoot) {
    std::error_code ec;
    const auto target = std::filesystem::read_symlink(
        std::string{proc_root} + "/" + std::to_string(pid) + "/exe", ec);
    return ec ? std::string{} : target.string();
}

// The parent of a pid, from /proc/<pid>/stat. nullopt when the process has
// gone, which is parse_ppid's answer to the empty read.
[[nodiscard]] inline std::optional<std::uint32_t> ppid_of(std::uint32_t pid,
                                                          std::string_view proc_root = kProcRoot) {
    std::ifstream stat_file(std::string{proc_root} + "/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(stat_file, line);
    return parse_ppid(line);
}

[[nodiscard]] inline bool process_alive(std::uint32_t pid, std::string_view proc_root = kProcRoot) {
    std::error_code ec;
    return std::filesystem::exists(std::string{proc_root} + "/" + std::to_string(pid), ec);
}

// The three things a PipeWire stream says about the application behind it
// that /proc cannot answer for. Its own type rather than the PipeWire one, so
// that this header - and the tests over it - never see a PipeWire type;
// session_monitor.cpp fills one from each ac3::pipewire::OutputStreamNode.
struct StreamFacts {
    std::string binary;     // application.process.binary, or empty
    std::string icon_name;  // application.icon-name, or empty
    std::string app_id;     // the sandbox's app id, or empty
};

// Everything the monitor knows about one process.
struct ProcessFacts {
    std::string name;
    std::string exe;
    std::vector<std::uint32_t> tree;  // the pid and its same-executable ancestors
    std::string binary;
    std::string icon_name;
    std::string app_id;
};

// Read once per process and kept while it lives, for the reason the Windows
// monitor caches: reading /proc for every process on every refresh is the
// expensive part, and none of it changes. The identity from the stream is
// kept with it, so a kept application first seen silent still gets its icon
// once it plays.
class ProcessFactsCache {
public:
    explicit ProcessFactsCache(std::string proc_root = std::string{kProcRoot})
        : proc_root_(std::move(proc_root)) {}

    // `stream`: the stream this process was seen on, or null for a kept
    // process with none. On a hit the stream back-fills whatever is empty and
    // /proc is not read again - which is the point of the cache, and is also
    // why a process whose comm changes after it was first seen keeps the name
    // it was first seen under.
    const ProcessFacts& facts_for(std::uint32_t pid, const StreamFacts* stream) {
        if (const auto it = known_.find(pid); it != known_.end()) {
            if (stream != nullptr) {
                back_fill(it->second, *stream);
            }
            return it->second;
        }
        const std::string_view root{proc_root_};
        // Every member named, in declaration order: GCC's
        // -Werror=missing-field-initializers rejects a partial designated
        // initialiser, and the order must follow the struct.
        return known_
            .emplace(pid,
                     ProcessFacts{
                         .name = process_name(pid, root),
                         .exe = process_exe(pid, root),
                         .tree = same_image_ancestors(
                             pid, [root](std::uint32_t of) { return ppid_of(of, root); },
                             [root](std::uint32_t of) { return process_exe(of, root); }),
                         .binary = stream != nullptr ? stream->binary : std::string{},
                         .icon_name = stream != nullptr ? stream->icon_name : std::string{},
                         .app_id = stream != nullptr ? stream->app_id : std::string{}})
            .first->second;
    }

    // Drop the facts for processes that are no longer listed. `keep(pid)`
    // answers whether one still is; everything else goes, so a machine that
    // has been running for a day does not carry a fact for every process that
    // ever made a sound.
    template <class Keep>
    void forget_unless(Keep keep) {
        std::erase_if(known_, [&keep](const auto& entry) { return !keep(entry.first); });
    }

    [[nodiscard]] std::size_t size() const { return known_.size(); }

private:
    // Whatever this stream carries and the cached facts do not. Never the
    // other way round: a stream that reports nothing must not blank an icon
    // name an earlier stream from the same process gave.
    static void back_fill(ProcessFacts& facts, const StreamFacts& stream) {
        if (facts.binary.empty()) {
            facts.binary = stream.binary;
        }
        if (facts.icon_name.empty()) {
            facts.icon_name = stream.icon_name;
        }
        if (facts.app_id.empty()) {
            facts.app_id = stream.app_id;
        }
    }

    std::string proc_root_;
    std::unordered_map<std::uint32_t, ProcessFacts> known_;
};

// The executable's path, or, where /proc keeps it from us (a sandbox), the
// bare binary name PipeWire reports: a degenerate path whose basename is
// itself, which is all the icon lookup takes from it.
[[nodiscard]] inline std::string image_path_of(const ProcessFacts& facts) {
    return facts.exe.empty() ? facts.binary : facts.exe;
}

// The record an application with a stream gets. `application` is the stream's
// application.name, which is what a person reads when /proc has no name for
// the process - a sandboxed one, or one that went between the two reads.
[[nodiscard]] inline AppSession sounding_session(std::uint32_t pid, std::string_view application,
                                                 const ProcessFacts& facts) {
    AppSession app;
    app.app = pid;
    app.name = facts.name.empty() ? std::string{application} : facts.name;
    app.image_path = image_path_of(facts);
    app.description = std::string{application};
    app.icon_name = facts.icon_name;
    app.app_id = facts.app_id;
    app.active = true;
    // No portable way to ask, and none at all under Wayland; see
    // session_monitor.cpp's header comment for what is lost with it.
    app.has_window = true;
    app.packaged = false;
    app.has_session = true;
    // The stream's pid and its same-executable ancestors, for the engine's
    // full-screen match.
    app.session_pids = facts.tree;
    return app;
}

// The record an application the engine asked to keep gets: listed while its
// process lives even with no stream, so a placed application survives a
// silent spell instead of vanishing from the room. Not active and with no
// session, which is what greys it.
[[nodiscard]] inline AppSession kept_session(std::uint32_t pid, const ProcessFacts& facts) {
    AppSession app;
    app.app = pid;
    app.name = facts.name;
    app.image_path = image_path_of(facts);
    app.description = facts.name;
    app.icon_name = facts.icon_name;
    app.app_id = facts.app_id;
    app.active = false;
    app.has_window = true;
    app.has_session = false;
    // The same ancestor list a sounding application carries, so the
    // full-screen rule still matches one that has gone quiet.
    app.session_pids = facts.tree;
    return app;
}

// A second stream from one process: one entry, and the tap takes the process,
// not the stream. What the second stream carried and the first did not fills
// the entry already built, so an icon that arrives with the second shows on
// this refresh rather than the next one.
inline void fill_from_second_stream(AppSession& app, const ProcessFacts& facts) {
    if (app.icon_name.empty()) {
        app.icon_name = facts.icon_name;
    }
    if (app.app_id.empty()) {
        app.app_id = facts.app_id;
    }
    if (app.image_path.empty()) {
        app.image_path = image_path_of(facts);
    }
}

}  // namespace ac3::crucible

#include "session_monitor.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pipewire_support.hpp"
#include "platform_services.hpp"
#include "process_tree.hpp"

// The Linux SessionMonitor: who is playing sound, from the PipeWire graph
// (docs/crucible/promotion.md, Phase 4).
//
// This is where Linux and Windows differ most, and the difference is a
// simplification rather than a gap.
//
// Windows has to group audio sessions **by process tree**, because the
// session's process is usually not the application - a browser renders its
// audio from a utility process under the browser - so it walks parents until
// the image name changes. PipeWire needs none of that for grouping: the
// process that owns a stream is the one a person means, so an application
// here is one process id, the stream's, and that is the pid the tap targets
// (capture.cpp matches it exactly).
//
// The same walk does exist here, for matching only. The engine's full-screen
// rule compares the front window's pid with each application's
// session_pids, and a browser's window belongs to its main process while its
// audio comes from the utility process listed here; so session_pids carries
// the stream's pid and every same-executable ancestor of it
// (process_tree.hpp), and `app` stays the stream's pid. Nothing is grouped
// by root: two utility processes of one browser remain two entries, as
// before.
//
// Finding that process id is not where it looks, though. A
// Stream/Output/Audio node carries application.name, media.class and a
// client.id - and no pid at all. The process is on the **Client** object the
// client.id names, where the daemon records it from the socket credentials
// as pipewire.sec.pid. Reading application.process.id off the node, which is
// the obvious thing, matches nothing and yields an empty list for ever.
// ac3::pipewire::output_stream_nodes() does the join.
//
// Those credentials name the application only where the application talks to
// the daemon itself. An application using the PulseAudio API - which on a
// desktop is most of them - reaches it through pipewire-pulse, and its
// Client carries pipewire-pulse's pid, so believing the credentials there
// collapses every one of them into a single entry named after the relay and
// points the tap at a process that plays nothing. output_stream_nodes()
// handles it and says how; what arrives here is one pid per application,
// relayed or not.
//
// The icon's identity comes the same way. Neither application.icon-name nor
// application.process.binary is on the registry dictionary a listener is
// handed; both are on the node's info, and a Flatpak client's portal app id
// on its Client's, so output_stream_nodes() binds each stream and its client
// for their info and hands the three back beside the pid. They are cached
// here per process with the /proc facts, and back-filled when a later stream
// from the same process carries what the first did not.
//
// One more thing differs, and it is visible rather than internal: an
// application is here only while it is playing. Windows keeps an audio
// session for as long as the application holds the device open, so a paused
// media player stays in the list; PipeWire has no session, only a stream,
// and a player that is not playing has no node in the graph at all. So
// applications appear when they start making sound and leave when they stop,
// and there is nothing to be done about it from this side. The empty list
// says so (Room.qml) rather than leaving a person wondering.
//
// What is lost with it is Windows' `has_window` test, which asks the shell
// whether some process in the tree owns a visible top-level window. There is
// no portable way to ask that on Linux and no way at all under Wayland (see
// foreground.cpp for the same wall). Anything with an audio stream is
// therefore reported as an application: a background process that plays
// sound is rare on a desktop, and listing one is a smaller error than hiding
// a real application would be. The Behaviour setting that hides background
// applications simply has nothing to hide here.

namespace ac3::crucible {

namespace {

// /proc/<pid>/comm is the kernel's short name for the process, which is what
// Windows calls the image stem. Empty when the process has gone.
[[nodiscard]] std::string process_name(std::uint32_t pid) {
    std::ifstream comm("/proc/" + std::to_string(pid) + "/comm");
    std::string name;
    std::getline(comm, name);
    return name;
}

// The executable behind a pid, for an icon. /proc/<pid>/exe is a symlink the
// owner can always read for their own processes - except across a sandbox
// boundary: a Flatpak or snap application's link may be unreadable or point
// inside its runtime, which is what the binary name from PipeWire is for.
[[nodiscard]] std::string process_exe(std::uint32_t pid) {
    std::error_code ec;
    const auto target = std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/exe", ec);
    return ec ? std::string{} : target.string();
}

// The parent of a pid, from /proc/<pid>/stat. The second field there (comm,
// in parentheses) may itself hold spaces and parentheses, so the parse starts
// after the LAST ')' and takes the second word from there: state, then ppid.
// nullopt when the process has gone.
[[nodiscard]] std::optional<std::uint32_t> ppid_of(std::uint32_t pid) {
    std::ifstream stat_file("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(stat_file, line);
    const auto paren = line.rfind(')');
    if (paren == std::string::npos) {
        return std::nullopt;
    }
    std::istringstream rest(line.substr(paren + 1));
    std::string state;
    std::uint32_t parent = 0;
    if (!(rest >> state >> parent)) {
        return std::nullopt;
    }
    return parent;
}

[[nodiscard]] bool process_alive(std::uint32_t pid) {
    std::error_code ec;
    return std::filesystem::exists("/proc/" + std::to_string(pid), ec);
}

class LinuxSessionMonitor final : public SessionMonitor {
public:
    // See SessionMonitor::listing_rule, and this file's header comment for
    // why the answer here is the shorter one.
    [[nodiscard]] std::string listing_rule() const override {
        return "An application is listed while it is playing: PipeWire gives it a stream "
               "when it starts making sound and takes it away when it stops, so the list "
               "follows the sound rather than the windows. A placed application keeps its "
               "place while it runs, silent or not.";
    }

    std::vector<AppSession> refresh(const std::vector<std::uint32_t>& keep) override {
        std::unordered_map<std::uint32_t, AppSession> apps;

        // The pid comes from the Client that owns each stream, not from the
        // stream node - output_stream_nodes() does that join and says why.
        // With the identity: this is where an icon name comes from, and this
        // thread is its own, at twice a second.
        const auto streams = ac3::pipewire::output_stream_nodes(
            ac3::pipewire::StreamIdentityDepth::kWithInfo);
        const auto self = static_cast<std::uint32_t>(::getpid());
        for (const auto& stream : streams) {
            if (stream.pid == 0) {
                continue;  // the daemon could not attribute it; nothing to tap
            }
            if (stream.pid == self) {
                // Crucible's own streams - the output probes, the sinks - are
                // PipeWire streams like any other and were listed as an
                // application called "ac3forge probe" on the first Linux
                // screenshot. Windows never lists another instance of this
                // program; the same rule, by pid.
                continue;
            }
            auto& app = apps[stream.pid];
            if (app.app != 0) {
                // A second stream from the same application: one entry, and
                // the tap takes the process, not the stream. What this
                // stream carries and the first did not fills both the cache
                // and the entry already built, so an icon that arrives with
                // the second stream shows on this refresh, not the next.
                const Facts& more = facts_for(stream.pid, &stream);
                if (app.icon_name.empty()) {
                    app.icon_name = more.icon_name;
                }
                if (app.app_id.empty()) {
                    app.app_id = more.app_id;
                }
                if (app.image_path.empty()) {
                    app.image_path = more.exe.empty() ? more.binary : more.exe;
                }
                continue;
            }
            const Facts& facts = facts_for(stream.pid, &stream);
            app.app = stream.pid;
            app.name = facts.name.empty() ? stream.application : facts.name;
            // The executable's path, or, where /proc keeps it from us (a
            // sandbox), the bare binary name PipeWire reports: a degenerate
            // path whose basename is itself, which is all the icon lookup
            // takes from it.
            app.image_path = facts.exe.empty() ? facts.binary : facts.exe;
            app.description = stream.application;
            app.icon_name = facts.icon_name;
            app.app_id = facts.app_id;
            app.active = true;
            // No portable way to ask; see this file's header comment.
            app.has_window = true;
            app.packaged = false;
            app.has_session = true;
            // The stream's pid and its same-executable ancestors, for the
            // engine's full-screen match (this file's header comment).
            app.session_pids = facts.tree;
        }

        // Applications the engine asked to keep: listed while their process
        // lives even with no stream, so a placed application survives a
        // silent spell instead of vanishing from the room.
        for (const std::uint32_t pid : keep) {
            if (pid == 0 || apps.contains(pid) || !process_alive(pid)) {
                continue;
            }
            const Facts& facts = facts_for(pid, nullptr);
            AppSession app;
            app.app = pid;
            app.name = facts.name;
            app.image_path = facts.exe.empty() ? facts.binary : facts.exe;
            app.description = facts.name;
            app.icon_name = facts.icon_name;
            app.app_id = facts.app_id;
            app.active = false;
            app.has_window = true;
            app.has_session = false;
            // The same ancestor list a sounding application carries, so the
            // full-screen rule still matches one that has gone quiet.
            app.session_pids = facts.tree;
            apps.emplace(pid, std::move(app));
        }

        // Facts for processes that have gone.
        std::erase_if(facts_, [&apps](const auto& entry) {
            return !apps.contains(entry.first);
        });

        std::vector<AppSession> out;
        out.reserve(apps.size());
        for (auto& [pid, app] : apps) {
            out.push_back(std::move(app));
        }
        std::ranges::sort(out, {}, &AppSession::app);
        return out;
    }

private:
    // Read once per process and kept while it lives, for the reason the
    // Windows monitor caches: reading /proc for every process on every
    // refresh is the expensive part, and none of it changes. The identity
    // from the stream is kept with them, so a kept application that was
    // first seen silent still gets its icon once it plays.
    struct Facts {
        std::string name;
        std::string exe;
        std::vector<std::uint32_t> tree;  // the pid and its same-executable ancestors
        std::string binary;      // application.process.binary, or empty
        std::string icon_name;   // application.icon-name, or empty
        std::string app_id;      // the sandbox's app id, or empty
    };

    // `stream`: the stream this process was seen on, or null for a kept
    // process with none. On a hit a stream back-fills whatever is empty.
    const Facts& facts_for(std::uint32_t pid, const ac3::pipewire::OutputStreamNode* stream) {
        if (const auto it = facts_.find(pid); it != facts_.end()) {
            if (stream != nullptr) {
                Facts& facts = it->second;
                if (facts.binary.empty()) {
                    facts.binary = stream->binary;
                }
                if (facts.icon_name.empty()) {
                    facts.icon_name = stream->icon_name;
                }
                if (facts.app_id.empty()) {
                    facts.app_id = stream->app_id;
                }
            }
            return it->second;
        }
        // Every member named, in declaration order: GCC's
        // -Werror=missing-field-initializers rejects a partial designated
        // initialiser, and the order must follow the struct.
        return facts_
            .emplace(pid, Facts{.name = process_name(pid),
                                .exe = process_exe(pid),
                                .tree = same_image_ancestors(pid, ppid_of, process_exe),
                                .binary = stream != nullptr ? stream->binary : std::string{},
                                .icon_name = stream != nullptr ? stream->icon_name : std::string{},
                                .app_id = stream != nullptr ? stream->app_id : std::string{}})
            .first->second;
    }

    std::unordered_map<std::uint32_t, Facts> facts_;
};

}  // namespace

std::shared_ptr<SessionMonitor> platform_session_monitor() {
    return std::make_shared<LinuxSessionMonitor>();
}

}  // namespace ac3::crucible

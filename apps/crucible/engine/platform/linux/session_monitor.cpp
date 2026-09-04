#include "session_monitor.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "pipewire_support.hpp"
#include "platform_services.hpp"

// The Linux SessionMonitor: who is playing sound, from the PipeWire registry
// (docs/crucible/promotion.md, Phase 4).
//
// This is where Linux and Windows differ most, and the difference is a
// simplification rather than a gap.
//
// Windows has to group audio sessions **by process tree**, because the
// session's process is usually not the application - a browser renders its
// audio from a utility process under the browser - so it walks parents until
// the image name changes. PipeWire needs none of that: the process that owns
// a stream is the one a person means, so an application here is one process
// id and the tree walk has no counterpart.
//
// Finding that process id is not where it looks, though. A
// Stream/Output/Audio node carries application.name, media.class and a
// client.id - and no pid at all. The process is on the **Client** object the
// client.id names, where the daemon records it from the socket credentials
// as pipewire.sec.pid. Reading application.process.id off the node, which is
// the obvious thing, matches nothing and yields an empty list for ever.
// ac3::pipewire::output_stream_nodes() does the join.
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
// owner can always read for their own processes.
[[nodiscard]] std::string process_exe(std::uint32_t pid) {
    std::error_code ec;
    const auto target = std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/exe", ec);
    return ec ? std::string{} : target.string();
}

[[nodiscard]] bool process_alive(std::uint32_t pid) {
    std::error_code ec;
    return std::filesystem::exists("/proc/" + std::to_string(pid), ec);
}

class LinuxSessionMonitor final : public SessionMonitor {
public:
    std::vector<AppSession> refresh(const std::vector<std::uint32_t>& keep) override {
        std::unordered_map<std::uint32_t, AppSession> apps;

        // The pid comes from the Client that owns each stream, not from the
        // stream node - output_stream_nodes() does that join and says why.
        const auto streams = ac3::pipewire::output_stream_nodes();
        for (const auto& stream : streams) {
            if (stream.pid == 0) {
                continue;  // the daemon could not attribute it; nothing to tap
            }
            auto& app = apps[stream.pid];
            if (app.app != 0) {
                // A second stream from the same application: one entry, and
                // the tap takes the process, not the stream.
                continue;
            }
            const Facts& facts = facts_for(stream.pid);
            app.app = stream.pid;
            app.name = facts.name.empty() ? stream.application : facts.name;
            app.image_path = facts.exe;
            app.description = stream.application;
            app.active = true;
            // No portable way to ask; see this file's header comment.
            app.has_window = true;
            app.packaged = false;
            app.has_session = true;
            app.session_pids.push_back(stream.pid);
        }

        // Applications the engine asked to keep: listed while their process
        // lives even with no stream, so a placed application survives a
        // silent spell instead of vanishing from the room.
        for (const std::uint32_t pid : keep) {
            if (pid == 0 || apps.contains(pid) || !process_alive(pid)) {
                continue;
            }
            const Facts& facts = facts_for(pid);
            AppSession app;
            app.app = pid;
            app.name = facts.name;
            app.image_path = facts.exe;
            app.description = facts.name;
            app.active = false;
            app.has_window = true;
            app.has_session = false;
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
    // refresh is the expensive part, and none of it changes.
    struct Facts {
        std::string name;
        std::string exe;
    };

    const Facts& facts_for(std::uint32_t pid) {
        if (const auto it = facts_.find(pid); it != facts_.end()) {
            return it->second;
        }
        return facts_.emplace(pid, Facts{.name = process_name(pid), .exe = process_exe(pid)})
            .first->second;
    }

    std::unordered_map<std::uint32_t, Facts> facts_;
};

}  // namespace

std::shared_ptr<SessionMonitor> platform_session_monitor() {
    return std::make_shared<LinuxSessionMonitor>();
}

}  // namespace ac3::crucible

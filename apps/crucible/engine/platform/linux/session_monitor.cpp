#include "session_monitor.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pipewire_support.hpp"
#include "platform_services.hpp"
#include "proc_facts.hpp"

// The Linux SessionMonitor: who is playing sound, from the PipeWire graph
// (docs/crucible/promotion.md, Phase 4).
//
// What is left in this file is the PipeWire half. Everything that is not -
// the /proc readers, the per-process fact cache and its back-fill, and the
// two records a refresh builds - lives in proc_facts.hpp beside this file,
// because this translation unit cannot be compiled at all without the
// PipeWire headers and none of that needs them. That is what
// tests/crucible/platform/linux/test_session_facts.cpp drives; what is left
// here needs a running daemon and is checked by hand on hardware with
// tools/checks/crucible_platform_probe.cpp.
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
            // The three identity fields, out of the PipeWire type and into
            // the plain one the bookkeeping below is written against
            // (proc_facts.hpp): nothing past this line sees a PipeWire type,
            // which is what lets a test drive it.
            const StreamFacts identity{.binary = stream.binary,
                                       .icon_name = stream.icon_name,
                                       .app_id = stream.app_id};
            auto& app = apps[stream.pid];
            const ProcessFacts& facts = facts_.facts_for(stream.pid, &identity);
            if (app.app != 0) {
                // A second stream from the same application: one entry, and
                // the tap takes the process, not the stream. The facts_for()
                // call above has already filled the cache from this stream;
                // this fills the entry already built, so an icon that arrives
                // with the second stream shows on this refresh, not the next.
                fill_from_second_stream(app, facts);
                continue;
            }
            app = sounding_session(stream.pid, stream.application, facts);
        }

        // Applications the engine asked to keep: listed while their process
        // lives even with no stream, so a placed application survives a
        // silent spell instead of vanishing from the room.
        for (const std::uint32_t pid : keep) {
            if (pid == 0 || apps.contains(pid) || !process_alive(pid)) {
                continue;
            }
            apps.emplace(pid, kept_session(pid, facts_.facts_for(pid, nullptr)));
        }

        // Facts for processes that have gone.
        facts_.forget_unless([&apps](std::uint32_t pid) { return apps.contains(pid); });

        std::vector<AppSession> out;
        out.reserve(apps.size());
        for (auto& [pid, app] : apps) {
            out.push_back(std::move(app));
        }
        std::ranges::sort(out, {}, &AppSession::app);
        return out;
    }

private:
    // The /proc facts per process, the identity from the stream kept beside
    // them, and the eviction of both. proc_facts.hpp holds all three, and the
    // reasoning for each, because none of it needs PipeWire and all of it is
    // worth a test (tests/crucible/platform/linux/test_session_facts.cpp).
    ProcessFactsCache facts_;
};

}  // namespace

std::shared_ptr<SessionMonitor> platform_session_monitor() {
    return std::make_shared<LinuxSessionMonitor>();
}

}  // namespace ac3::crucible

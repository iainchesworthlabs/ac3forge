#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The Linux twin of the Windows session monitor's root_of()
// (engine/platform/windows/session_monitor.cpp): a pid and every ancestor
// that runs the same executable, nearest first.
//
// The engine's full-screen rule matches the window's process against each
// application's session_pids (engine.cpp, refresh_sessions). A browser's
// window belongs to its main process while its audio comes from a utility
// process underneath, so an application listed by its stream's pid needs
// its same-image ancestors listed with it, or the rule never sees the
// browser go full-screen. Stopping where the executable changes is what
// keeps a terminal that launched a player from being the player: a
// full-screen xterm whose child is pw-play is xterm, and pw-play stays where
// it was placed.
//
// Bounded at sixteen hops, as the Windows walk is: parent ids are recycled
// and a stale chain can loop. The readers are injected so the walk is
// tested against a tree built by hand rather than against /proc
// (tests/crucible/platform/linux/test_x11_foreground.cpp).

namespace ac3::crucible {

constexpr int kMaxAncestorHops = 16;

// `ppid_of(pid)`: the parent, or nullopt when the process has gone.
// `exe_of(pid)`: the executable's path, or empty when it cannot be read
// (gone, or another user's), which ends the walk.
template <class PpidOf, class ExeOf>
[[nodiscard]] std::vector<std::uint32_t> same_image_ancestors(std::uint32_t pid, PpidOf ppid_of,
                                                              ExeOf exe_of) {
    std::vector<std::uint32_t> out{pid};
    const std::string exe = exe_of(pid);
    if (exe.empty()) {
        return out;
    }
    std::uint32_t current = pid;
    for (int hops = 0; hops < kMaxAncestorHops; ++hops) {
        const std::optional<std::uint32_t> parent = ppid_of(current);
        if (!parent || *parent <= 1 || *parent == current || exe_of(*parent) != exe) {
            break;
        }
        out.push_back(*parent);
        current = *parent;
    }
    return out;
}

}  // namespace ac3::crucible

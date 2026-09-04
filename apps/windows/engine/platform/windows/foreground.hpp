#pragma once

#include <cstdint>
#include <optional>

// Which application is full-screen in the foreground, if any
// (docs/platforms/windows-demo.md, "Objects and the bed": that application
// is the bed whatever the user asked).
//
// The shell's own notion is asked first - SHQueryUserNotificationState says
// whether a full-screen application, a Direct3D exclusive one or
// presentation mode is up - and only then is the foreground window's
// process taken, so an ordinary maximised window never counts. Windows-only,
// this directory only.

namespace ac3::windemo {

// The foreground window's process id when the shell reports a full-screen
// state, else nullopt. The engine maps it to an application by process
// tree, since the window's process may not be the one with the audio
// session.
[[nodiscard]] std::optional<std::uint32_t> fullscreen_foreground_pid();

}  // namespace ac3::windemo

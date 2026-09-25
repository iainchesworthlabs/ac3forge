#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "ac3/render/layout.hpp"
#include "ac3forge/control.hpp"

// The board as a Sendspin player (planning/hearth-reference-player.md, B3), as
// a seam CMake resolves, like the source and the sink:
//
//   sendspin/player/  the component's Sendspin player
//                     (esp-idf/ac3forge/include/ac3forge/sendspin_host.hpp and
//                     burst_player.hpp), for a build with a network and
//                     CONFIG_AC3FORGE_SENDSPIN;
//   sendspin/none/    everything else, where each of these does nothing.
//
// A server connects to the board, pairs with it and plays to it: bursts over
// _ac3forge_player@v1 from ac3hearth, PCM over player@v1 from Music
// Assistant. The player owns the sink while a stream plays; a play from the
// control surface (POST /play, kept for debugging) owns it otherwise, and the
// two never run at once.
//
// sendspin_start() is app_main's. The rest may be called from any task: the
// control surface's, the console's, app_main. Until the player has started they
// find no player, and it has started only once its server has too.

namespace player {

// The port a server connects to, which the board's mDNS record advertises
// (planning/hearth-sendspin-extension.md, row T4: a player's port).
inline constexpr std::uint16_t kSendspinPort = 8928;

// Whether this build has a player at all.
[[nodiscard]] bool sendspin_built();

// Starts the player on a board that is on a network, playing streams that no
// server has given a layout onto `layout`. Nothing without either, or while
// the player runs. app_main calls it at boot, and again when a network joined
// after boot comes up.
void sendspin_start(const ac3::render::OutputLayout& layout);

// Whether this build has a player, running.
[[nodiscard]] bool sendspin_running();

// A Sendspin stream is playing.
[[nodiscard]] bool sendspin_playing();

// A play from the control surface is about to take the sink (true), or has
// given it back (false). While it has the sink the player tells its servers
// the board is not available, and a stream they send is not played.
void sendspin_set_external(bool external);

// The layout the control surface set, for streams no server sets one for. One
// set before the player has started, or while it starts, is kept for it.
[[nodiscard]] bool sendspin_set_layout(const ac3::render::OutputLayout& layout);

// The board's name, slot width or wiring changed: what it tells servers in
// its hello follows, and a server that was told otherwise connects again. A
// player that is starting takes the change up before it has started.
void sendspin_board_changed();

// The board is going away, into flash mode or through a restart
// (planning/esp32-ota.md): every server is told the board is restarting,
// the player stops writing to the sink, and the server stops listening. The
// player and its host stay in memory, since other tasks hold pointers to
// them; nothing starts them again, because the board restarts next.
void sendspin_leave();

// GET /status's "sendspin" object, or nothing without a player.
[[nodiscard]] std::optional<ac3forge::ControlSendspin> sendspin_status();

// POST /pairing's actions: "reset", "cancel" or "forget". False for anything
// else, or without a player.
[[nodiscard]] bool sendspin_pairing(std::string_view action);

// A line typed on the console. True when it was one of the player's
// commands: "pair reset", "pair cancel", "pair forget", "pair token",
// "sendspin".
bool sendspin_console(std::string_view line);

// Called by app_main about ten times a second: what the player reports to its
// server - levels, counters and what the decoder found - goes out from here.
void sendspin_poll();

}  // namespace player

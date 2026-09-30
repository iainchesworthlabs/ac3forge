// No Sendspin player: a build without a network, or without the component's
// player (CONFIG_ICLFORGE_SENDSPIN). See ../../sendspin.hpp.

#include "sendspin.hpp"

#include <optional>
#include <string_view>

namespace player {

bool sendspin_built() { return false; }

void sendspin_start(const iclforge::render::OutputLayout& /*layout*/) {}

bool sendspin_running() { return false; }

bool sendspin_playing() { return false; }

void sendspin_set_external(bool /*external*/) {}

bool sendspin_set_layout(const iclforge::render::OutputLayout& /*layout*/) { return true; }

void sendspin_board_changed() {}

void sendspin_leave() {}

std::optional<iclforge::ControlSendspin> sendspin_status() { return std::nullopt; }

bool sendspin_pairing(std::string_view /*action*/) { return false; }

std::optional<iclforge::ControlPairings> sendspin_pairings() { return std::nullopt; }

std::optional<bool> sendspin_forget_server(std::string_view /*server_id*/) { return std::nullopt; }

bool sendspin_console(std::string_view /*line*/) { return false; }

void sendspin_poll() {}

}  // namespace player

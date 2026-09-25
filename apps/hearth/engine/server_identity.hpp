#pragma once

#include <optional>
#include <string_view>

#include "ac3/sendspin/noise.hpp"
#include "settings_model.hpp"

// This computer's Sendspin server identity (planning/hearth-reference-player.md,
// A6), kept in the settings so that it is the same on every start. A sink binds
// each pairing record to the server identity that made it (connection.md, E8)
// and refuses that record's long-term PSK from any other identity, with no
// fallback to the Sentinel: an identity made afresh on each start would leave
// every pairing unusable after a restart, and each paired sink unreachable
// until it was paired again.
//
// The private key is kept under "identity/server" as 64 hex digits - beside the
// pairing records it belongs with (pairing_store.hpp), but not under
// "pairing/", which PairingStore rewrites whole on every change. The
// diagnostics file withholds everything under "identity/"
// (diagnostics_report.hpp).

namespace ac3::hearth {

inline constexpr std::string_view kServerIdentityKey = "identity/server";

// The identity `store` keeps; when it keeps none, or one that does not read as
// a key, a new one, written back. Nothing only when no key pair can be made at
// all. When the store will not take the new one, it is still this run's
// identity: pairing then lasts until the next start, as it did before the
// identity was kept.
//
// Not synchronised with anything else using `store`: call it before anything
// else can (NetworkController::start() calls it before its NetworkSinks, and so
// its PairingStore's own writes, exist).
[[nodiscard]] std::optional<sendspin::noise::KeyPair> load_or_make_server_identity(SettingsStore& store);

}  // namespace ac3::hearth

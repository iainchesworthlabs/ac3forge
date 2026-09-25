#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "freertos/FreeRTOS.h"

#include "ac3/sendspin/ac3forge_player.hpp"
#include "ac3/sendspin/chunks.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/player_session.hpp"
#include "ac3forge/sendspin_store.hpp"

// A Sendspin player on a board: the WebSocket a server connects to, a
// PlayerSession for each connection, admission between servers, pairing and
// the records it leaves (planning/hearth-reference-player.md, B3;
// planning/hearth-sendspin-extension.md). src/sendspin's player half does the
// protocol; this is what a board adds around it, and what the test sink's
// Sink class (apps/hearth/testsink/sink.cpp) is on a computer.
//
// ONE TASK RUNS EVERY SESSION. The host is an esp_http_server instance of its
// own, on the Sendspin port, and that server's task receives each WebSocket
// frame and hands it to its connection's session; the session's answer goes
// back on the same socket before the next frame is read. The sessions' timers
// run on that task too (httpd_queue_work from an esp_timer), and so does
// anything the owner asks of a session from another task. PlayerSession wants
// one caller at a time, and this task is that caller, so no lock is taken
// around a session. The Noise handshake's X25519 and SHA-256 run there, which
// is what SendspinHostConfig::stack_bytes is sized for.
//
// A server of its own rather than a route on the control surface's server
// (control.hpp): the page and the REST routes stay on port 80, where the page
// has always been, and a slow page request never holds up a burst.
//
// THE OWNER'S EVENTS (SendspinEvents) arrive on that task, inside the
// session's call: an implementation copies what it needs and returns. The
// methods below that say they are safe from any task queue their work onto the
// server's task.

namespace ac3forge {

class SendspinEvents {
   public:
    SendspinEvents() = default;
    virtual ~SendspinEvents() = default;
    SendspinEvents(const SendspinEvents&) = delete;
    SendspinEvents& operator=(const SendspinEvents&) = delete;

    // player@v1: a stream began or changed format in place; drop what is
    // buffered; the stream ended. `frame` is valid during the call; `local_time`
    // is the local time (esp_timer microseconds) its first sample plays at,
    // the output delay already taken off.
    virtual void on_stream_start(const ac3::sendspin::messages::PlayerStream& stream) = 0;
    virtual void on_stream_clear() = 0;
    virtual void on_stream_end() = 0;
    virtual void on_audio(std::span<const std::uint8_t> frame, std::int64_t server_time,
                          std::int64_t local_time) = 0;
    // A volume, mute or output delay command the player listed. The owner
    // applies it and reports the new state with set_player_state().
    virtual void on_player_command(const ac3::sendspin::messages::PlayerCommandMessage& command) = 0;

    // _ac3forge_player@v1, the same way.
    virtual void on_burst_stream_start(const ac3::sendspin::ac3forge::StreamStart& stream) = 0;
    virtual void on_burst_stream_clear() = 0;
    virtual void on_burst_stream_end() = 0;
    virtual void on_burst(const ac3::sendspin::BurstChunk& chunk, std::int64_t local_time) = 0;
    virtual void on_invalid_burst() = 0;
    virtual void on_ac3forge_command(const ac3::sendspin::ac3forge::CommandMessage& command) = 0;
    virtual void on_settings_refused(const ac3::sendspin::ac3forge::SettingsError& error) = 0;

    // A pairing attempt showed a dynamic code (digits), was held back for the
    // operator, or ended ("paired", or why not). For the console and the page;
    // the host already records each in status().
    virtual void on_pairing_code(std::string_view digits) = 0;
    virtual void on_pairing_held_back() = 0;
    virtual void on_pairing_ended(std::string_view outcome) = 0;
};

struct SendspinHostConfig {
    // The Sendspin port (planning/hearth-sendspin-extension.md, row T4),
    // which the board's mDNS record advertises.
    std::uint16_t port = 8928;
    // esp_http_server's control socket for this instance: each server on a
    // part needs its own.
    std::uint16_t control_port = 32769;
    // The server task: every session runs on it, the Noise handshake
    // included. SendspinStatus::stack_free says what a run left spare.
    std::size_t stack_bytes = 8192;
    // Below a burst player's decode task (BurstPlayerConfig::priority, 6),
    // which on a part with one core cannot wait for this one. A message this
    // task reads late is still dated by when it arrived, where the project
    // installs the arrival hook (ac3forge/tcp_arrivals.hpp).
    UBaseType_t priority = 5;
    BaseType_t core = tskNO_AFFINITY;
    // Connections at once: the one admitted for playback, a pairing
    // connection held beside it, and one arriving to displace either
    // (connection.md, Multiple servers). esp_http_server keeps three sockets of
    // its own beside these, which the part's lwIP socket count has to allow.
    std::size_t max_connections = 3;
    // What each connection's session is told, less its identity, which is
    // the store's.
    ac3::sendspin::PlayerConfig player;
};

// What the host is doing, for the console and the page. Fixed-size, so the
// control surface's task can take a copy without allocating.
struct SendspinStatus {
    bool running = false;
    std::uint16_t port = 0;
    std::uint8_t connections = 0;
    // This board's client_id: its identity's public key, base64url.
    std::array<char, 48> client_id{};
    std::uint8_t paired_servers = 0;

    // The admitted connection, when there is one.
    bool connected = false;
    std::array<char, 48> server_name{};
    // The first eight characters of its server_id.
    std::array<char, 12> server_id{};
    // "specification" or "aiosendspin 9.1.1": which forms the host speaks to it.
    const char* dialect = "";
    // The PSK its handshake matched: "long-term", "pairing" or "sentinel".
    const char* psk = "";
    // "playback", "pairing" or "none".
    const char* activity = "";
    // The playback role active on it: "_ac3forge_player@v1", "player@v1", or "".
    const char* role = "";
    bool clock_converged = false;
    std::int64_t clock_error_us = 0;
    // Clock bursts the filter took, and bursts it left out for their replies'
    // delay (ac3::sendspin::ClockSync::rejected()).
    std::uint32_t clock_updates = 0;
    std::uint32_t clock_rejected = 0;
    // The server holds a pairing this board no longer has
    // (planning/hearth-sendspin-extension.md, C4): remove the board from the
    // server before pairing it again.
    bool server_has_lost_pairing = false;

    // Pairing: the dynamic code while an attempt shows one, whether an
    // attempt waits for the operator, how many rounds have run since the last
    // verified server_kc (a code attempt waits at twenty), and how the last
    // attempt ended.
    std::array<char, 16> pairing_code{};
    bool pairing_held_back = false;
    std::uint32_t pairing_rounds = 0;
    std::array<char, 32> pairing_outcome{};

    // The least stack the server's task has had spare, in bytes.
    std::size_t stack_free = 0;
};

// One pairing record, for the console and the page.
struct SendspinPairing {
    SendspinStore::Key32 server_key{};
    // What the server's hello called it: empty for a record from a firmware
    // that did not keep names, until that server connects again.
    std::array<char, kServerNameBytes> name{};
    // A connection this record authenticated is open.
    bool connected = false;
    // The server that last played here, which may take the board back from a
    // holder that declares nothing (connection.md, Multiple servers).
    bool last_playback = false;
    // The board has used this record since it started, by a pairing or a
    // connection.
    bool seen = false;
};

// The records, the most recently used first. Fixed-size, like SendspinStatus.
struct SendspinPairings {
    std::array<SendspinPairing, SendspinStore::kRecordCapacity> servers{};
    std::size_t count = 0;
};

class SendspinHost final {
   public:
    SendspinHost();
    ~SendspinHost();
    SendspinHost(const SendspinHost&) = delete;
    SendspinHost& operator=(const SendspinHost&) = delete;

    // Loads the store, then starts the server. The events outlive the host.
    // False, having said why on the console, when it cannot.
    [[nodiscard]] bool start(SendspinHostConfig config, SendspinEvents& events);
    void stop();

    // The board is going away for an update or a restart
    // (planning/esp32-ota.md): every connection past its handshake is told
    // so with client/goodbye restart, and its server dials again once the
    // board is back. Then the server stops, as stop() stops it. Waits up to
    // `wait_ms` for the goodbyes to go out on the server's task. Safe from
    // any task but the server's own.
    void leave(std::uint32_t wait_ms = 500);

    // Safe from any task; queued onto the server's task, and a later call
    // replaces an earlier one that has not been applied yet.
    //
    // The player's own state, or the role's: volume, mute, delay, and for the
    // role its levels, counters and decoder report.
    void set_player_state(const ac3::sendspin::messages::PlayerState& state);
    void set_ac3forge_state(const ac3::sendspin::ac3forge::State& state);
    // Something outside Sendspin has the output (a play from the control
    // surface), or has given it back.
    void set_external_source(bool external);
    // The configuration new connections are given. A connection that has
    // said hello with another is told to go (client/goodbye restart), and its
    // server dials again.
    void set_player_config(const ac3::sendspin::PlayerConfig& player);
    // The operator's actions on the device (pairing.md): a reset of the
    // dynamic code's round limit, and cancelling the attempt in progress.
    void reset_pairing_rounds();
    void cancel_pairing();

    // Every pairing dropped and a new identity: the servers that knew this
    // board must pair it again. Open connections are closed. Safe from any
    // task whose stack is in internal RAM.
    [[nodiscard]] bool forget_pairings();
    // One server's pairing dropped, with its name, and the rest kept: its
    // connection closes with client/goodbye user_request, and it has to pair
    // again (its next handshake falls back to the Sentinel PSK, which tells it
    // so; connection.md). No longer the last-playback server either, if it
    // was. False when there is no record for it. Safe from any task whose
    // stack is in internal RAM.
    [[nodiscard]] bool forget_server(const SendspinStore::Key32& server_key);

    // The pairing records, with each server's name, which is read from NVS.
    // Safe from any task whose stack is in internal RAM.
    [[nodiscard]] SendspinPairings pairings() const;

    [[nodiscard]] SendspinStatus status() const;
    [[nodiscard]] const SendspinStore& store() const;

    // The admitted playback connection's clock, from a local time to the
    // server's: how a play time is reported against the group's timeline.
    // Nothing until that connection's clock has had an update. Safe from any
    // task.
    [[nodiscard]] std::optional<std::int64_t> server_time(std::int64_t local_us) const;
    // The other way: the local time a server time plays at, by the same clock.
    [[nodiscard]] std::optional<std::int64_t> local_time(std::int64_t server_us) const;

    struct Impl;

   private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3forge

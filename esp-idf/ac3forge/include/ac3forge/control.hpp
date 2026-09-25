#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ac3forge/player.hpp"

// The control surface: a few HTTP verbs over the player, for the ESP-IDF
// integrator whose controller is not Home Assistant. planning/esp32-player.md
// (Control) says why it is REST here and a media_player entity on ESPHome.
//
//   GET  /            a web page that shows what the player is doing and drives
//                     it through the routes below and nothing else, with its
//                     script at GET /ui.js. Both are sent from flash as the
//                     component embeds them (planning/esp32-device-ui.md).
//   GET  /api         the routes, as text
//   GET  /status      what is playing and how it is going, as JSON
//   GET  /hardware    what this board IS - the chip, its revision, whether it
//                     has an FPU and PSRAM, this sink's own ceiling, and the
//                     firmware running on it - as JSON, gathered once at
//                     startup rather than on every request: unlike /status,
//                     nothing in it changes while the board runs.
//                     planning/esp32-device-ui.md says why this is a route of
//                     its own rather than part of /status.
//   POST /play        body: the location to play - a URL for the HTTP source,
//                     a path for a file source. 202 when accepted (the owner
//                     opens it on its own task; /status says how that went),
//                     409 when the source cannot take a location, 400 when the
//                     body is empty.
//   POST /stop        200
//   POST /volume      body: a number 0.0 to 1.0. 200, or 409 if the sink has
//                     no volume to set.
//   GET  /layout      the output layout, as text: a name or a speaker list
//                     (ac3/render/layout.hpp)
//   PUT  /layout      body: a name or a speaker list. Takes effect at the next
//                     play. 200, 400 when it is not a layout, 409 when the
//                     sink's bus has fewer slots than it needs.
//   POST /pairing     body: reset, cancel or forget, for a board that is a
//                     Sendspin player. 200, 400 for another body, 409 with no
//                     player.
//
// And the board's own settings, which ControlHandlers lists: GET and PUT
// /name, /slot-width and /wiring, and PUT /network.
//
// And updates over the network, which Control hands to the owner's Firmware
// (firmware.hpp, planning/esp32-ota.md): GET and PUT /firmware, PUT
// /firmware/mode and /firmware/rollback, and POST /restart. In flash mode
// every route that would play something or change a setting answers 409.
//
// Every handler below runs on esp_http_server's task. Nothing here touches a
// Player: the callbacks hand the request to whichever task owns the player -
// a queue, in the streaming example - and answer from what that task
// publishes. The one exception is `stats`, which is a snapshot the Player
// already makes safe from any task.

namespace ac3forge {

class Firmware;

// GET /status's "sendspin" object, for a board that is a Sendspin player
// (sendspin_host.hpp, burst_player.hpp). Plain values, so that the control
// surface carries no part of the player: the owner fills it in.
struct ControlSendspin {
    // The server this board is admitted to, and how.
    std::string server;
    std::string server_id;
    std::string dialect;
    std::string psk;
    std::string activity;
    std::string role;
    bool clock_converged = false;
    long long clock_error_us = 0;
    // Clock bursts taken by the filter, and left out for their delay.
    unsigned long clock_updates = 0;
    unsigned long clock_rejected = 0;
    unsigned connections = 0;
    // This board's client_id, and the servers it is paired with.
    std::string client_id;
    unsigned paired = 0;
    // The dynamic pairing code while an attempt shows one; whether an
    // attempt waits for the operator; rounds since the last code that
    // matched; how the last attempt ended; and whether a server holds a
    // pairing this board has lost.
    std::string pairing_code;
    bool pairing_held = false;
    unsigned pairing_rounds = 0;
    std::string pairing_outcome;
    bool lost_pairing = false;
    // The stream: "bursts", "pcm" or "idle", and its counters.
    std::string stream;
    unsigned long long bursts = 0;
    unsigned long long underruns = 0;
    unsigned long long late = 0;
    unsigned long long dropped = 0;
    unsigned long long invalid = 0;
    unsigned resyncs = 0;
    // When frames played against when they should have, positive when late.
    long long error_us = 0;
    long long worst_error_us = 0;
    // The newest frame played with a known time: its place in the stream, and
    // when it played on the server's clock; with that, when the stream's first
    // frame played by the same measure, which two boards in a group compare.
    std::optional<unsigned long long> play_frame;
    std::optional<long long> play_server_us;
    std::optional<long long> origin_server_us;
    // Per output over the last 100 ms, in dB, and the stream's RMS scaled by a
    // million, as the console prints it.
    std::vector<float> peak_db;
    std::vector<float> rms_db;
    std::vector<unsigned long> stream_rms;
    unsigned burst_us = 0;
    unsigned worst_burst_us = 0;
    unsigned long decode_stack_free = 0;
    unsigned long server_stack_free = 0;
    long long settings_revision = 0;
    bool identifying = false;
};

// GET /status's "network" object: what the board is joined to, for a page
// that offers to move it to another network and for someone asking why its
// stream stutters. Plain values, which the owner fills in.
struct ControlNetwork {
    // "wifi", or "ethernet" for the MAC QEMU emulates.
    std::string kind;
    // The access point's SSID; empty on Ethernet and while the station is not
    // associated.
    std::string ssid;
    // The access point's signal as the station last heard it, in dBm, while
    // associated.
    std::optional<int> rssi_dbm;
    // The board's IPv4 address as text; empty while it holds none.
    std::string address;
};

struct ControlHandlers {
    // POST /play. False means refused (the source cannot take a location, or
    // the location was not acceptable); the reply says so.
    std::function<bool(std::string_view location)> play;
    // POST /stop.
    std::function<void()> stop;
    // POST /volume. False means this sink has nothing to set.
    std::function<bool(float volume)> set_volume;
    std::function<float()> volume;
    // GET and PUT /layout. set_layout returns false for text that is not a
    // layout or one the sink cannot carry; the owner applies it at its next
    // play, and `layout` reports whatever will play next.
    std::function<std::string()> layout;
    std::function<bool(std::string_view text)> set_layout;

    // GET and PUT /slot-width, in bits. How wide a slot the sink's bus
    // carries is a property of the DACs a board is wired to, so it is a
    // setting rather than a build choice; set_slot_bits returns false for a
    // width the sink does not have, and the owner applies it at its next
    // play. It moves sink_slots below with it - an I2S line carries 128 bits
    // a frame either way, so the same wiring reaches sixteen slots at 16 bits
    // and eight at 32 - which can leave a layout already set too wide to
    // play; the owner says so at the next play rather than here.
    std::function<int()> slot_bits;
    std::function<bool(int bits)> set_slot_bits;

    // GET and PUT /name: what the board calls itself, on the network and in a
    // server's list of sinks. Stored on the board (its settings), so it
    // survives a reflash.
    std::function<std::string()> name;
    std::function<bool(std::string_view text)> set_name;

    // GET and PUT /wiring: whether the second I2S line is connected to
    // anything. It moves sink_slots with it, the same way the slot width
    // does, because two lines carry twice one line's slots. An owner whose
    // sink can never have a second line leaves both empty, so that /status
    // has no wiring to offer a choice about.
    std::function<bool()> second_line;
    std::function<bool(bool wired)> set_second_line;

    // PUT /network: the access point the board joins, as an SSID and a
    // passphrase. Improv over the serial port is the other way in
    // (the board's own provisioning); this is the one the page offers to
    // someone already on the network who wants to move the board to another.
    // Takes effect at the next boot: the station is already associated.
    std::function<bool(std::string_view ssid, std::string_view password)> set_network;

    // GET /status draws on these. Any may be left empty; the field is then
    // omitted or reported as null.
    std::function<PlayerStats()> stats;
    std::function<std::optional<StreamInfo>()> stream;
    std::function<std::string()> location;  // what is, or was last, playing
    std::function<const char*()> source_name;
    std::function<const char*()> sink_name;
    // The slots the sink's bus has, and so the widest layout set_layout can
    // accept; reported beside the sink's name.
    std::function<int()> sink_slots;
    // GET /hardware's own slot count: this sink's ceiling at ANY setting it
    // takes, not just the one in force - player::sink_max_slots() beside
    // sink_open() and sink_slots() (main/audio_sink.hpp). Left empty reports
    // nothing, the same rule every optional handler here follows.
    std::function<int()> sink_max_slots;
    // The slot width sink_max_slots's figure was reached at (16 or 32) -
    // player::sink_max_slots_bit_width() beside it. Left empty (the default)
    // reports the slot count alone, with no width qualifier - the right
    // default for a sink whose ceiling is not a function of slot width at
    // all (a capture/null sink has no TDM frame to divide).
    std::function<int()> sink_max_slots_bits;
    // "playing", "stopped", "finished", "failed" - the owner knows.
    std::function<const char*()> state;

    // GET /status's "sendspin" object; null in the reply when this returns
    // nothing, and left out when the handler is empty.
    std::function<std::optional<ControlSendspin>()> sendspin;
    // POST /pairing: body "reset" (the dynamic code's round limit, the
    // operator's action pairing.md asks for), "cancel" (the attempt in
    // progress) or "forget" (every pairing, and a new identity). False for
    // anything else, or with no player.
    std::function<bool(std::string_view action)> pairing;

    // GET /status's "network" object; null in the reply when this returns
    // nothing (a build with no network), and left out when the handler is
    // empty. Called for every GET /status, on the server's task.
    std::function<std::optional<ControlNetwork>()> network;

    // The firmware routes (firmware.hpp), and flash mode's refusals. Null
    // leaves the routes answering that the board takes no updates. The owner
    // keeps it alive for as long as Control runs.
    Firmware* firmware = nullptr;
};

class Control {
   public:
    // The server task's stack, from internal RAM. Every callback above runs
    // on it, so it is sized for the owner's work as much as the server's: in
    // the streaming example PUT /layout parses the layout there, and under
    // QEMU on 2026-09-11 the task's deepest use was 4,596 bytes. At
    // esp_http_server's default of 4,096 that request overflowed the stack
    // without tripping its canary, and the part panicked later in FreeRTOS's
    // list code. An owner whose callbacks do more passes more.
    static constexpr std::size_t kDefaultStackBytes = 6144;

    Control() = default;
    ~Control();
    Control(const Control&) = delete;
    Control& operator=(const Control&) = delete;

    // Starts the server on `port`. False, having said why, if it could not.
    [[nodiscard]] bool start(const ControlHandlers& handlers, std::uint16_t port = 80,
                             std::size_t stack_bytes = kDefaultStackBytes);
    void stop();

   private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace ac3forge

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

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
//   POST /play        body: the location to play - a URL for the HTTP source,
//                     a path for a file source. 202 when accepted (the owner
//                     opens it on its own task; /status says how that went),
//                     409 when the source cannot take a location, 400 when the
//                     body is empty.
//   POST /stop        200
//   POST /volume      body: a number 0.0 to 1.0. 200, or 409 if the sink has
//                     no volume to set.
//   GET  /layout      the output layout, as text: a name or a speaker list
//                     (ac3forge/layout.hpp)
//   PUT  /layout      body: a name or a speaker list. Takes effect at the next
//                     play. 200, 400 when it is not a layout, 409 when the
//                     sink's bus has fewer slots than it needs.
//
// Every handler below runs on esp_http_server's task. Nothing here touches a
// Player: the callbacks hand the request to whichever task owns the player -
// a queue, in the streaming example - and answer from what that task
// publishes. The one exception is `stats`, which is a snapshot the Player
// already makes safe from any task.

namespace ac3forge {

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

    // GET /status draws on these. Any may be left empty; the field is then
    // omitted or reported as null.
    std::function<PlayerStats()> stats;
    std::function<std::optional<StreamInfo>()> stream;
    std::function<std::string()> location;  // what is, or was last, playing
    std::function<const char*()> source_name;
    std::function<const char*()> sink_name;
    // "playing", "stopped", "finished", "failed" - the owner knows.
    std::function<const char*()> state;
};

class Control {
   public:
    Control() = default;
    ~Control();
    Control(const Control&) = delete;
    Control& operator=(const Control&) = delete;

    // Starts the server on `port`. False, having said why, if it could not.
    [[nodiscard]] bool start(const ControlHandlers& handlers, std::uint16_t port = 80);
    void stop();

   private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace ac3forge

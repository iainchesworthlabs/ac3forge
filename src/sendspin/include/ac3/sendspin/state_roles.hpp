#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/json.hpp"

// The three roles whose state a server sends in server/state (messaging.md, server/state):
// metadata@v1, controller@v1 with its client/command, and color@v1 (roles/metadata/v1.md,
// roles/controller/v1.md, roles/color/v1.md), each object a struct with a writer that appends it as
// the value of a key the caller has written, and a reader. The messages that carry them are
// messages.hpp's.
//
// Where aiosendspin 9.1.1 reads these objects differently, the writer takes the dialect
// (planning/hearth-sendspin-extension.md, C36 and C37). Beside the objects, the arithmetic the
// roles require of a server: the controller's group volume and mute, and the colours' contrast.

namespace ac3::sendspin {

// --- metadata@v1 -------------------------------------------------------------------------------

namespace metadata {

inline constexpr std::string_view kRole = "metadata@v1";

struct Progress {
    std::int64_t track_progress_ms = 0;
    // 0 for an unknown or unlimited duration.
    std::int64_t track_duration_ms = 0;
    // The speed times 1,000: 1,000 normal, 0 paused.
    std::int32_t playback_speed = 1000;

    friend bool operator==(const Progress&, const Progress&) = default;
};

struct State {
    // Server clock time the state takes effect at, in microseconds; a future one schedules it.
    std::int64_t timestamp = 0;
    std::optional<std::string> title;
    std::optional<std::string> artist;
    std::optional<std::string> album_artist;
    std::optional<std::string> album;
    std::optional<std::string> artwork_url;
    std::optional<std::int32_t> year;
    std::optional<std::int32_t> track;
    // Absent: no position to report.
    std::optional<Progress> progress;

    friend bool operator==(const State&, const State&) = default;
};

// The specification's object carries the fields present and leaves the rest out, an absent
// progress clearing the position. An aiosendspin 9.1.1 client takes a field left out as
// unchanged, so to one every absent field goes out as null, and so does a year outside 1000 to
// 2040 or a track below 1, either of which would make it drop the message (C36).
void write_state(json::Writer& w, const State& state, Dialect dialect);
// Nothing when `timestamp` is missing or a present field has the wrong type or range; a null
// field reads as absent.
[[nodiscard]] std::optional<State> read_state(json::Value value);

// The track position at `now` on the server clock, by the role's calculation: the position at the
// timestamp advanced at the playback speed, clamped to the duration when there is one. Nothing
// without a progress.
[[nodiscard]] std::optional<std::int64_t> position_ms(const State& state, std::int64_t now);

}  // namespace metadata

// --- controller@v1 -----------------------------------------------------------------------------

namespace controller {

inline constexpr std::string_view kRole = "controller@v1";

enum class Command : std::uint8_t {
    kPlay,
    kPause,
    kStop,
    kNext,
    kPrevious,
    kVolume,
    kMute,
    kRepeatOff,
    kRepeatOne,
    kRepeatAll,
    kShuffle,
    kUnshuffle,
    kSwitch,
    kSeek,
    kSeekRelative,
};

enum class Repeat : std::uint8_t {
    kOff,
    kOne,
    kAll,
};

struct State {
    std::vector<Command> supported_commands;
    // The group's, as group_volume() and group_muted() compute them.
    std::int32_t volume = 100;
    bool muted = false;
    Repeat repeat = Repeat::kOff;
    bool shuffle = false;
    // Required when supported_commands has kSeek.
    std::optional<std::int64_t> seek_max_ms;

    friend bool operator==(const State&, const State&) = default;
};

void write_state(json::Writer& w, const State& state);
// Nothing when a required field is missing or out of range. A command this reader does not know
// is left out of supported_commands. aiosendspin 9.1.1 fills in a missing repeat and shuffle, and
// so does this reader.
[[nodiscard]] std::optional<State> read_state(json::Value value);

// The controller object of client/command.
struct CommandMessage {
    Command command = Command::kPlay;
    // kVolume: 0 to 100.
    std::int32_t volume = 0;
    // kMute.
    bool mute = false;
    // kSeek: 0 or more; the server checks it against seek_max_ms.
    std::int64_t position_ms = 0;
    // kSeekRelative: signed.
    std::int64_t offset_ms = 0;

    friend bool operator==(const CommandMessage&, const CommandMessage&) = default;
};

void write_command(json::Writer& w, const CommandMessage& command);
// Nothing for an unknown command, or one missing the value it needs or holding one out of range.
[[nodiscard]] std::optional<CommandMessage> read_command(json::Value value);

// One player of a group, as the group volume and mute see it.
struct Player {
    std::int32_t volume = 100;
    bool muted = false;
    // Whether the player lists the volume and mute commands.
    bool volume_supported = false;
    bool mute_supported = false;
};

// The mean volume of the players that support volume, rounded; 100 when none does.
[[nodiscard]] std::int32_t group_volume(std::span<const Player> players);
// True only when every player that supports mute is muted; false when none does.
[[nodiscard]] bool group_muted(std::span<const Player> players);
// Each player's volume after the group is set to `requested` (roles/controller/v1.md, Setting group
// volume): the difference from the group volume applied to every player that supports volume,
// what clamping at 0 or 100 loses shared among the rest until nothing is lost or every player is
// clamped, then rounded. A player without volume support keeps its volume.
[[nodiscard]] std::vector<std::int32_t> set_group_volume(std::span<const Player> players, std::int32_t requested);

}  // namespace controller

// --- color@v1 ----------------------------------------------------------------------------------

namespace color {

inline constexpr std::string_view kRole = "color@v1";

struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    friend bool operator==(const Rgb&, const Rgb&) = default;
};

struct State {
    // Server clock time the colours take effect at, in microseconds; a future one schedules them.
    std::int64_t timestamp = 0;
    std::optional<Rgb> background_dark;
    std::optional<Rgb> background_light;
    std::optional<Rgb> primary;
    std::optional<Rgb> accent;
    std::optional<Rgb> on_dark;
    std::optional<Rgb> on_light;

    friend bool operator==(const State&, const State&) = default;
};

// As metadata's: to an aiosendspin 9.1.1 client an absent colour goes out as null (C36).
void write_state(json::Writer& w, const State& state, Dialect dialect);
[[nodiscard]] std::optional<State> read_state(json::Value value);

// WCAG 2's relative luminance and contrast ratio, from 1 to 21.
[[nodiscard]] double relative_luminance(Rgb colour);
[[nodiscard]] double contrast_ratio(Rgb a, Rgb b);

// The contrast the role requires of the backgrounds and the colours on them.
inline constexpr double kMinimumContrast = 4.5;

// `state` with its backgrounds and foregrounds moved towards black or white, keeping their hue,
// until every pair the role names meets kMinimumContrast: background_dark against white and
// on_dark, background_light against black and on_light, on_dark against black, and on_light
// against white. primary and accent are left as they are.
[[nodiscard]] State with_contrast(const State& state);
// Whether `state` meets those pairs already.
[[nodiscard]] bool meets_contrast(const State& state);

}  // namespace color

}  // namespace ac3::sendspin

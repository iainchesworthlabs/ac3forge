#include "ac3/sendspin/state_roles.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/json.hpp"

namespace ac3::sendspin {

namespace {

template <class E>
struct Named {
    E value;
    std::string_view name;
};

template <class E, std::size_t N>
[[nodiscard]] std::optional<E> value_of(const std::array<Named<E>, N>& table, json::Value value) {
    for (const Named<E>& entry : table) {
        if (value.equals(entry.name)) {
            return entry.value;
        }
    }
    return std::nullopt;
}

template <class E, std::size_t N>
[[nodiscard]] std::string_view name_of(const std::array<Named<E>, N>& table, E value) {
    for (const Named<E>& entry : table) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return {};
}

constexpr std::int64_t kMaxInt32 = 2'147'483'647;

[[nodiscard]] std::optional<std::int32_t> read_int32(json::Value value, std::int64_t minimum, std::int64_t maximum) {
    const std::optional<std::int64_t> number = value.as_int();
    if (!number || *number < minimum || *number > maximum) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(*number);
}

// A field that may be absent or null, both of which read as absent: true with `out` filled, or
// with it left empty; false when present with another type.
[[nodiscard]] bool read_optional_string(json::Value value, std::optional<std::string>& out) {
    if (!value.exists() || value.is_null()) {
        return true;
    }
    out = value.as_string();
    return out.has_value();
}

[[nodiscard]] bool read_optional_int(json::Value value, std::int64_t minimum, std::int64_t maximum,
                                     std::optional<std::int32_t>& out) {
    if (!value.exists() || value.is_null()) {
        return true;
    }
    out = read_int32(value, minimum, maximum);
    return out.has_value();
}

}  // namespace

// --- metadata@v1 -------------------------------------------------------------------------------

namespace metadata {

namespace {

// aiosendspin 9.1.1 drops a message with a year or track outside these (C36).
constexpr std::int32_t kEarliestYear911 = 1000;
constexpr std::int32_t kLatestYear911 = 2040;

}  // namespace

void write_state(json::Writer& w, const State& state, Dialect dialect) {
    const bool spec = dialect == Dialect::kSpecification;
    w.begin_object().member("timestamp", state.timestamp);
    const auto text = [&](std::string_view key, const std::optional<std::string>& value) {
        if (value) {
            w.member(key, std::string_view{*value});
        } else if (!spec) {
            w.key(key).null();
        }
    };
    const auto number = [&](std::string_view key, const std::optional<std::int32_t>& value, bool valid_911) {
        if (value && (spec || valid_911)) {
            w.member(key, *value);
        } else if (!spec) {
            w.key(key).null();
        }
    };
    text("title", state.title);
    text("artist", state.artist);
    text("album_artist", state.album_artist);
    text("album", state.album);
    text("artwork_url", state.artwork_url);
    number("year", state.year, state.year && *state.year >= kEarliestYear911 && *state.year <= kLatestYear911);
    number("track", state.track, state.track && *state.track >= 1);
    if (state.progress) {
        w.key("progress")
            .begin_object()
            .member("track_progress", state.progress->track_progress_ms)
            .member("track_duration", state.progress->track_duration_ms)
            .member("playback_speed", state.progress->playback_speed)
            .end_object();
    } else if (!spec) {
        w.key("progress").null();
    }
    w.end_object();
}

std::optional<State> read_state(json::Value value) {
    const std::optional<std::int64_t> timestamp = value["timestamp"].as_int();
    if (!value.is_object() || !timestamp) {
        return std::nullopt;
    }
    State state;
    state.timestamp = *timestamp;
    if (!read_optional_string(value["title"], state.title) || !read_optional_string(value["artist"], state.artist) ||
        !read_optional_string(value["album_artist"], state.album_artist) ||
        !read_optional_string(value["album"], state.album) ||
        !read_optional_string(value["artwork_url"], state.artwork_url) ||
        !read_optional_int(value["year"], -kMaxInt32, kMaxInt32, state.year) ||
        !read_optional_int(value["track"], -kMaxInt32, kMaxInt32, state.track)) {
        return std::nullopt;
    }
    if (const json::Value progress = value["progress"]; progress.exists() && !progress.is_null()) {
        const std::optional<std::int64_t> position = progress["track_progress"].as_int();
        const std::optional<std::int64_t> duration = progress["track_duration"].as_int();
        const std::optional<std::int32_t> speed = read_int32(progress["playback_speed"], 0, kMaxInt32);
        if (!position || *position < 0 || !duration || *duration < 0 || !speed) {
            return std::nullopt;
        }
        state.progress = Progress{.track_progress_ms = *position, .track_duration_ms = *duration, .playback_speed = *speed};
    }
    return state;
}

std::optional<std::int64_t> position_ms(const State& state, std::int64_t now) {
    if (!state.progress) {
        return std::nullopt;
    }
    const Progress& progress = *state.progress;
    // In double, as the role's formula divides: a long interval at a high speed would overflow
    // 64-bit integers before the division.
    const double calculated = static_cast<double>(progress.track_progress_ms) +
                              (static_cast<double>(now - state.timestamp) *
                               static_cast<double>(progress.playback_speed) / 1'000'000.0);
    const double upper = progress.track_duration_ms != 0 ? static_cast<double>(progress.track_duration_ms) : 9.0e18;
    return static_cast<std::int64_t>(std::floor(std::clamp(calculated, 0.0, upper)));
}

}  // namespace metadata

// --- controller@v1 -----------------------------------------------------------------------------

namespace controller {

namespace {

constexpr std::array<Named<Command>, 15> kCommands{{
    {Command::kPlay, "play"},
    {Command::kPause, "pause"},
    {Command::kStop, "stop"},
    {Command::kNext, "next"},
    {Command::kPrevious, "previous"},
    {Command::kVolume, "volume"},
    {Command::kMute, "mute"},
    {Command::kRepeatOff, "repeat_off"},
    {Command::kRepeatOne, "repeat_one"},
    {Command::kRepeatAll, "repeat_all"},
    {Command::kShuffle, "shuffle"},
    {Command::kUnshuffle, "unshuffle"},
    {Command::kSwitch, "switch"},
    {Command::kSeek, "seek"},
    {Command::kSeekRelative, "seek_relative"},
}};

constexpr std::array<Named<Repeat>, 3> kRepeats{{
    {Repeat::kOff, "off"},
    {Repeat::kOne, "one"},
    {Repeat::kAll, "all"},
}};

}  // namespace

void write_state(json::Writer& w, const State& state) {
    w.begin_object().key("supported_commands").begin_array();
    for (const Command command : state.supported_commands) {
        w.string(name_of(kCommands, command));
    }
    w.end_array()
        .member("volume", state.volume)
        .member("muted", state.muted)
        .member("repeat", name_of(kRepeats, state.repeat))
        .member("shuffle", state.shuffle);
    if (state.seek_max_ms) {
        w.member("seek_max_ms", *state.seek_max_ms);
    }
    w.end_object();
}

std::optional<State> read_state(json::Value value) {
    const json::Value commands = value["supported_commands"];
    const std::optional<std::int32_t> volume = read_int32(value["volume"], 0, 100);
    const std::optional<bool> muted = value["muted"].as_bool();
    if (!value.is_object() || !commands.is_array() || !volume || !muted) {
        return std::nullopt;
    }
    State state;
    for (const json::Value element : commands.elements()) {
        if (!element.is_string()) {
            return std::nullopt;
        }
        if (const std::optional<Command> command = value_of(kCommands, element)) {
            state.supported_commands.push_back(*command);
        }
    }
    state.volume = *volume;
    state.muted = *muted;
    if (const json::Value repeat = value["repeat"]; repeat.exists()) {
        const std::optional<Repeat> mode = value_of(kRepeats, repeat);
        if (!mode) {
            return std::nullopt;
        }
        state.repeat = *mode;
    }
    if (const json::Value shuffle = value["shuffle"]; shuffle.exists()) {
        const std::optional<bool> on = shuffle.as_bool();
        if (!on) {
            return std::nullopt;
        }
        state.shuffle = *on;
    }
    if (const json::Value seek_max = value["seek_max_ms"]; seek_max.exists() && !seek_max.is_null()) {
        state.seek_max_ms = seek_max.as_int();
        if (!state.seek_max_ms || *state.seek_max_ms < 0) {
            return std::nullopt;
        }
    }
    return state;
}

void write_command(json::Writer& w, const CommandMessage& command) {
    w.begin_object().member("command", name_of(kCommands, command.command));
    switch (command.command) {
        case Command::kVolume:
            w.member("volume", command.volume);
            break;
        case Command::kMute:
            w.member("mute", command.mute);
            break;
        case Command::kSeek:
            w.member("position_ms", command.position_ms);
            break;
        case Command::kSeekRelative:
            w.member("offset_ms", command.offset_ms);
            break;
        default:
            break;
    }
    w.end_object();
}

std::optional<CommandMessage> read_command(json::Value value) {
    const std::optional<Command> which = value_of(kCommands, value["command"]);
    if (!value.is_object() || !which) {
        return std::nullopt;
    }
    CommandMessage command;
    command.command = *which;
    switch (*which) {
        case Command::kVolume: {
            const std::optional<std::int32_t> volume = read_int32(value["volume"], 0, 100);
            if (!volume) {
                return std::nullopt;
            }
            command.volume = *volume;
            break;
        }
        case Command::kMute: {
            const std::optional<bool> mute = value["mute"].as_bool();
            if (!mute) {
                return std::nullopt;
            }
            command.mute = *mute;
            break;
        }
        case Command::kSeek: {
            const std::optional<std::int64_t> position = value["position_ms"].as_int();
            if (!position || *position < 0) {
                return std::nullopt;
            }
            command.position_ms = *position;
            break;
        }
        case Command::kSeekRelative: {
            const std::optional<std::int64_t> offset = value["offset_ms"].as_int();
            if (!offset) {
                return std::nullopt;
            }
            command.offset_ms = *offset;
            break;
        }
        default:
            break;
    }
    return command;
}

std::int32_t group_volume(std::span<const Player> players) {
    std::int64_t sum = 0;
    std::int64_t count = 0;
    for (const Player& player : players) {
        if (player.volume_supported) {
            sum += player.volume;
            ++count;
        }
    }
    if (count == 0) {
        return 100;
    }
    return static_cast<std::int32_t>(std::lround(static_cast<double>(sum) / static_cast<double>(count)));
}

bool group_muted(std::span<const Player> players) {
    bool any = false;
    for (const Player& player : players) {
        if (player.mute_supported) {
            if (!player.muted) {
                return false;
            }
            any = true;
        }
    }
    return any;
}

std::vector<std::int32_t> set_group_volume(std::span<const Player> players, std::int32_t requested) {
    std::vector<std::int32_t> result;
    std::vector<double> proposed;
    std::vector<std::size_t> free;
    double sum = 0.0;
    for (std::size_t i = 0; i < players.size(); ++i) {
        result.push_back(players[i].volume);
        proposed.push_back(static_cast<double>(players[i].volume));
        if (players[i].volume_supported) {
            free.push_back(i);
            sum += static_cast<double>(players[i].volume);
        }
    }
    if (free.empty()) {
        return result;
    }
    const std::size_t supported = free.size();
    double delta = static_cast<double>(std::clamp<std::int32_t>(requested, 0, 100)) - (sum / static_cast<double>(supported));
    constexpr double kNothing = 1e-9;
    while (!free.empty() && std::fabs(delta) > kNothing) {
        double lost = 0.0;
        std::vector<std::size_t> still_free;
        for (const std::size_t i : free) {
            proposed[i] += delta;
            const double clamped = std::clamp(proposed[i], 0.0, 100.0);
            if (clamped != proposed[i]) {
                lost += proposed[i] - clamped;
                proposed[i] = clamped;
            } else {
                still_free.push_back(i);
            }
        }
        free = std::move(still_free);
        if (free.empty() || std::fabs(lost) <= kNothing) {
            break;
        }
        delta = lost / static_cast<double>(free.size());
    }
    for (std::size_t i = 0; i < players.size(); ++i) {
        if (players[i].volume_supported) {
            result[i] = static_cast<std::int32_t>(std::lround(proposed[i]));
        }
    }
    return result;
}

}  // namespace controller

// --- color@v1 ----------------------------------------------------------------------------------

namespace color {

namespace {

constexpr Rgb kBlack{.r = 0, .g = 0, .b = 0};
constexpr Rgb kWhite{.r = 255, .g = 255, .b = 255};

void write_rgb(json::Writer& w, std::string_view key, const std::optional<Rgb>& colour, bool spec) {
    if (colour) {
        w.key(key).begin_array().integer(colour->r).integer(colour->g).integer(colour->b).end_array();
    } else if (!spec) {
        w.key(key).null();
    }
}

[[nodiscard]] bool read_rgb(json::Value value, std::optional<Rgb>& out) {
    if (!value.exists() || value.is_null()) {
        return true;
    }
    if (!value.is_array() || value.size() != 3) {
        return false;
    }
    const std::optional<std::int32_t> r = read_int32(value.at(0), 0, 255);
    const std::optional<std::int32_t> g = read_int32(value.at(1), 0, 255);
    const std::optional<std::int32_t> b = read_int32(value.at(2), 0, 255);
    if (!r || !g || !b) {
        return false;
    }
    out = Rgb{.r = static_cast<std::uint8_t>(*r), .g = static_cast<std::uint8_t>(*g), .b = static_cast<std::uint8_t>(*b)};
    return true;
}

[[nodiscard]] double linear(std::uint8_t component) {
    const double c = static_cast<double>(component) / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

// `colour` moved `step` 255ths of the way to `target`, rounded.
[[nodiscard]] Rgb blend(Rgb colour, Rgb target, int step) {
    const auto mix = [&](std::uint8_t from, std::uint8_t to) {
        const double t = static_cast<double>(step) / 255.0;
        return static_cast<std::uint8_t>(
            std::lround(static_cast<double>(from) + ((static_cast<double>(to) - static_cast<double>(from)) * t)));
    };
    return Rgb{.r = mix(colour.r, target.r), .g = mix(colour.g, target.g), .b = mix(colour.b, target.b)};
}

// The nearest blend of `colour` towards `target` that `fits`; `target` itself fits whenever the
// role's pairs can be met at all.
template <class Fits>
[[nodiscard]] Rgb nearest(Rgb colour, Rgb target, const Fits& fits) {
    for (int step = 0; step <= 255; ++step) {
        const Rgb candidate = blend(colour, target, step);
        if (fits(candidate)) {
            return candidate;
        }
    }
    return target;
}

}  // namespace

void write_state(json::Writer& w, const State& state, Dialect dialect) {
    const bool spec = dialect == Dialect::kSpecification;
    w.begin_object().member("timestamp", state.timestamp);
    write_rgb(w, "background_dark", state.background_dark, spec);
    write_rgb(w, "background_light", state.background_light, spec);
    write_rgb(w, "primary", state.primary, spec);
    write_rgb(w, "accent", state.accent, spec);
    write_rgb(w, "on_dark", state.on_dark, spec);
    write_rgb(w, "on_light", state.on_light, spec);
    w.end_object();
}

std::optional<State> read_state(json::Value value) {
    const std::optional<std::int64_t> timestamp = value["timestamp"].as_int();
    if (!value.is_object() || !timestamp) {
        return std::nullopt;
    }
    State state;
    state.timestamp = *timestamp;
    if (!read_rgb(value["background_dark"], state.background_dark) ||
        !read_rgb(value["background_light"], state.background_light) || !read_rgb(value["primary"], state.primary) ||
        !read_rgb(value["accent"], state.accent) || !read_rgb(value["on_dark"], state.on_dark) ||
        !read_rgb(value["on_light"], state.on_light)) {
        return std::nullopt;
    }
    return state;
}

double relative_luminance(Rgb colour) {
    return (0.2126 * linear(colour.r)) + (0.7152 * linear(colour.g)) + (0.0722 * linear(colour.b));
}

double contrast_ratio(Rgb a, Rgb b) {
    const double la = relative_luminance(a);
    const double lb = relative_luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

State with_contrast(const State& state) {
    State out = state;
    const auto contrasts = [](Rgb a, Rgb b) { return contrast_ratio(a, b) >= kMinimumContrast; };
    // The foregrounds first, against the text colour they must also carry; then each background
    // against its text and its foreground.
    if (out.on_dark) {
        out.on_dark = nearest(*out.on_dark, kWhite, [&](Rgb c) { return contrasts(c, kBlack); });
    }
    if (out.on_light) {
        out.on_light = nearest(*out.on_light, kBlack, [&](Rgb c) { return contrasts(c, kWhite); });
    }
    if (out.background_dark) {
        out.background_dark = nearest(*out.background_dark, kBlack, [&](Rgb c) {
            return contrasts(c, kWhite) && (!out.on_dark || contrasts(c, *out.on_dark));
        });
    }
    if (out.background_light) {
        out.background_light = nearest(*out.background_light, kWhite, [&](Rgb c) {
            return contrasts(c, kBlack) && (!out.on_light || contrasts(c, *out.on_light));
        });
    }
    return out;
}

bool meets_contrast(const State& state) {
    const auto contrasts = [](Rgb a, Rgb b) { return contrast_ratio(a, b) >= kMinimumContrast; };
    if (state.on_dark && !contrasts(*state.on_dark, kBlack)) {
        return false;
    }
    if (state.on_light && !contrasts(*state.on_light, kWhite)) {
        return false;
    }
    if (state.background_dark &&
        (!contrasts(*state.background_dark, kWhite) || (state.on_dark && !contrasts(*state.background_dark, *state.on_dark)))) {
        return false;
    }
    if (state.background_light && (!contrasts(*state.background_light, kBlack) ||
                                   (state.on_light && !contrasts(*state.background_light, *state.on_light)))) {
        return false;
    }
    return true;
}

}  // namespace color

}  // namespace ac3::sendspin

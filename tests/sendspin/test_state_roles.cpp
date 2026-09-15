#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/json.hpp"
#include "ac3/sendspin/state_roles.hpp"

// metadata@v1, controller@v1 and color@v1's objects (roles/metadata/v1.md, roles/controller/v1.md,
// roles/color/v1.md) in the specification's form and aiosendspin 9.1.1's (C36, C37), with the
// controller's group volume and mute arithmetic and the colours' contrast.

namespace {

using ac3::sendspin::Dialect;
namespace json = ac3::sendspin::json;
namespace metadata = ac3::sendspin::metadata;
namespace controller = ac3::sendspin::controller;
namespace color = ac3::sendspin::color;

struct Parsed {
    std::string text;
    std::vector<json::Token> tokens;
    json::Document document;

    explicit Parsed(std::string source) : text(std::move(source)) {
        const bool parsed = static_cast<bool>(document.parse(text, tokens, 4096));
        CHECK(parsed);
    }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    Parsed(Parsed&&) = delete;
    Parsed& operator=(Parsed&&) = delete;
    ~Parsed() = default;

    [[nodiscard]] json::Value root() const { return document.root(); }
};

template <class T, class Write>
[[nodiscard]] std::string written(const T& value, Write write) {
    std::string out;
    json::Writer w(out);
    write(w, value);
    return out;
}

[[nodiscard]] metadata::State track() {
    metadata::State state;
    state.timestamp = 5000000;
    state.title = "So What";
    state.artist = "Miles Davis";
    state.album = "Kind of Blue";
    state.year = 1959;
    state.track = 1;
    state.progress = metadata::Progress{.track_progress_ms = 60000, .track_duration_ms = 562000, .playback_speed = 1000};
    return state;
}

}  // namespace

TEST_CASE("state roles: metadata in both dialects", "[sendspin][roles]") {
    const auto spec = [](const metadata::State& state) {
        return written(state, [](json::Writer& w, const metadata::State& s) { metadata::write_state(w, s, Dialect::kSpecification); });
    };
    const auto aio = [](const metadata::State& state) {
        return written(state, [](json::Writer& w, const metadata::State& s) { metadata::write_state(w, s, Dialect::kAiosendspin911); });
    };
    CHECK(spec(track()) == R"({"timestamp":5000000,"title":"So What","artist":"Miles Davis","album":"Kind of Blue",)"
                           R"("year":1959,"track":1,"progress":{"track_progress":60000,"track_duration":562000,"playback_speed":1000}})");
    // To a 9.1.1 client every absent field is null, since it takes a missing one as unchanged.
    CHECK(aio(track()) == R"({"timestamp":5000000,"title":"So What","artist":"Miles Davis","album_artist":null,)"
                          R"("album":"Kind of Blue","artwork_url":null,"year":1959,"track":1,)"
                          R"("progress":{"track_progress":60000,"track_duration":562000,"playback_speed":1000}})");
    metadata::State odd = track();
    odd.year = 812;
    odd.track = 0;
    odd.progress.reset();
    CHECK(spec(odd) == R"({"timestamp":5000000,"title":"So What","artist":"Miles Davis","album":"Kind of Blue","year":812,"track":0})");
    // A year or track 9.1.1 would drop the message for is cleared, as the progress is.
    CHECK(aio(odd) == R"({"timestamp":5000000,"title":"So What","artist":"Miles Davis","album_artist":null,)"
                      R"("album":"Kind of Blue","artwork_url":null,"year":null,"track":null,"progress":null})");

    const Parsed parsed(aio(track()));
    const std::optional<metadata::State> read = metadata::read_state(parsed.root());
    REQUIRE(read.has_value());
    CHECK(*read == track());

    const auto read_text = [](std::string text) { return metadata::read_state(Parsed(std::move(text)).root()); };
    CHECK_FALSE(read_text(R"({"title":"x"})"));
    CHECK_FALSE(read_text(R"({"timestamp":1,"title":7})"));
    CHECK_FALSE(read_text(R"({"timestamp":1,"progress":{"track_progress":-1,"track_duration":0,"playback_speed":1000}})"));
    CHECK_FALSE(read_text(R"({"timestamp":1,"progress":{"track_progress":0,"playback_speed":1000}})"));
    CHECK(read_text(R"({"timestamp":1,"future":[]})").has_value());
}

TEST_CASE("state roles: a metadata position from its progress", "[sendspin][roles]") {
    metadata::State state = track();
    CHECK(metadata::position_ms(state, 5000000) == 60000);
    CHECK(metadata::position_ms(state, 7500000) == 62500);
    CHECK(metadata::position_ms(state, 4000000) == 59000);
    // Clamped to the duration, and to 0.
    CHECK(metadata::position_ms(state, 5000000 + 600'000'000) == 562000);
    CHECK(metadata::position_ms(state, 5000000 - 61'000'000) == 0);
    // Paused, at one and a half times, and live with no duration.
    state.progress->playback_speed = 0;
    CHECK(metadata::position_ms(state, 9000000) == 60000);
    state.progress->playback_speed = 1500;
    CHECK(metadata::position_ms(state, 7000000) == 63000);
    state.progress->track_duration_ms = 0;
    CHECK(metadata::position_ms(state, 5000000 + 1'000'000'000) == 1560000);
    state.progress.reset();
    CHECK_FALSE(metadata::position_ms(state, 0).has_value());
}

TEST_CASE("state roles: controller state and commands", "[sendspin][roles]") {
    controller::State state;
    state.supported_commands = {controller::Command::kPlay, controller::Command::kPause, controller::Command::kVolume,
                                controller::Command::kSeek, controller::Command::kSeekRelative};
    state.volume = 42;
    state.muted = false;
    state.repeat = controller::Repeat::kAll;
    state.shuffle = true;
    state.seek_max_ms = 562000;
    const std::string text = written(state, controller::write_state);
    CHECK(text == R"({"supported_commands":["play","pause","volume","seek","seek_relative"],"volume":42,"muted":false,)"
                  R"("repeat":"all","shuffle":true,"seek_max_ms":562000})");
    const Parsed parsed(text);
    CHECK(controller::read_state(parsed.root()) == state);

    const auto read_state = [](std::string source) { return controller::read_state(Parsed(std::move(source)).root()); };
    // An aiosendspin 9.1.1 server of before the specification's repeat and shuffle, and a command
    // this reader does not know.
    const std::optional<controller::State> old = read_state(R"({"supported_commands":["play","rewind"],"volume":100,"muted":true})");
    REQUIRE(old.has_value());
    CHECK(old->supported_commands == std::vector<controller::Command>{controller::Command::kPlay});
    CHECK(old->repeat == controller::Repeat::kOff);
    CHECK_FALSE(old->shuffle);
    CHECK_FALSE(read_state(R"({"supported_commands":[],"volume":101,"muted":true})"));
    CHECK_FALSE(read_state(R"({"supported_commands":[],"volume":1,"muted":true,"repeat":"twice"})"));
    CHECK_FALSE(read_state(R"({"supported_commands":[],"volume":1})"));

    const auto round_trip = [](const controller::CommandMessage& command, const std::string& expected) {
        const std::string out = written(command, controller::write_command);
        CHECK(out == expected);
        const Parsed back(out);
        CHECK(controller::read_command(back.root()) == command);
    };
    controller::CommandMessage command;
    command.command = controller::Command::kNext;
    round_trip(command, R"({"command":"next"})");
    command = {};
    command.command = controller::Command::kVolume;
    command.volume = 70;
    round_trip(command, R"({"command":"volume","volume":70})");
    command = {};
    command.command = controller::Command::kMute;
    command.mute = true;
    round_trip(command, R"({"command":"mute","mute":true})");
    command = {};
    command.command = controller::Command::kSeek;
    command.position_ms = 120000;
    round_trip(command, R"({"command":"seek","position_ms":120000})");
    command = {};
    command.command = controller::Command::kSeekRelative;
    command.offset_ms = -15000;
    round_trip(command, R"({"command":"seek_relative","offset_ms":-15000})");

    const auto read_command = [](std::string source) { return controller::read_command(Parsed(std::move(source)).root()); };
    CHECK_FALSE(read_command(R"({"command":"volume"})"));
    CHECK_FALSE(read_command(R"({"command":"volume","volume":-1})"));
    CHECK_FALSE(read_command(R"({"command":"seek","position_ms":-1})"));
    CHECK_FALSE(read_command(R"({"command":"seek_relative"})"));
    CHECK_FALSE(read_command(R"({"command":"rewind"})"));
}

TEST_CASE("state roles: a group's volume and mute", "[sendspin][roles]") {
    using Player = controller::Player;
    const auto players = [](std::initializer_list<std::int32_t> volumes) {
        std::vector<Player> out;
        for (const std::int32_t volume : volumes) {
            out.push_back({.volume = volume, .muted = false, .volume_supported = true, .mute_supported = true});
        }
        return out;
    };
    CHECK(controller::group_volume(players({50, 50})) == 50);
    CHECK(controller::group_volume(players({33, 34})) == 34);
    CHECK(controller::set_group_volume(players({50, 50}), 100) == std::vector<std::int32_t>{100, 100});
    CHECK(controller::set_group_volume(players({90, 50}), 80) == std::vector<std::int32_t>{100, 60});
    // What clamping loses goes to the others: 95 + 7.5 stops at 100, and its 2.5 moves 57.5 to 60.
    CHECK(controller::set_group_volume(players({95, 50}), 80) == std::vector<std::int32_t>{100, 60});
    CHECK(controller::set_group_volume(players({10, 90}), 0) == std::vector<std::int32_t>{0, 0});
    CHECK(controller::set_group_volume(players({100, 100, 20}), 100) == std::vector<std::int32_t>{100, 100, 100});
    // Rounded at the end, not before.
    CHECK(controller::set_group_volume(players({33, 34}), 50) == std::vector<std::int32_t>{50, 51});

    // A player without volume support keeps its volume and does not count.
    std::vector<Player> mixed = players({40, 60});
    mixed.push_back({.volume = 5, .muted = true, .volume_supported = false, .mute_supported = false});
    CHECK(controller::group_volume(mixed) == 50);
    CHECK(controller::set_group_volume(mixed, 70) == std::vector<std::int32_t>{60, 80, 5});
    CHECK_FALSE(controller::group_muted(mixed));
    mixed[0].muted = true;
    mixed[1].muted = true;
    CHECK(controller::group_muted(mixed));

    const std::vector<Player> none{{.volume = 20, .muted = true, .volume_supported = false, .mute_supported = false}};
    CHECK(controller::group_volume(none) == 100);
    CHECK_FALSE(controller::group_muted(none));
    CHECK(controller::set_group_volume(none, 70) == std::vector<std::int32_t>{20});
}

TEST_CASE("state roles: colours in both dialects", "[sendspin][roles]") {
    color::State state;
    state.timestamp = 7;
    state.background_dark = color::Rgb{.r = 20, .g = 30, .b = 40};
    state.primary = color::Rgb{.r = 200, .g = 100, .b = 0};
    const auto spec = written(state, [](json::Writer& w, const color::State& s) { color::write_state(w, s, Dialect::kSpecification); });
    const auto aio = written(state, [](json::Writer& w, const color::State& s) { color::write_state(w, s, Dialect::kAiosendspin911); });
    CHECK(spec == R"({"timestamp":7,"background_dark":[20,30,40],"primary":[200,100,0]})");
    CHECK(aio == R"({"timestamp":7,"background_dark":[20,30,40],"background_light":null,"primary":[200,100,0],)"
                 R"("accent":null,"on_dark":null,"on_light":null})");
    const Parsed parsed(aio);
    CHECK(color::read_state(parsed.root()) == state);

    const auto read_text = [](std::string text) { return color::read_state(Parsed(std::move(text)).root()); };
    CHECK_FALSE(read_text(R"({"timestamp":7,"primary":[1,2]})"));
    CHECK_FALSE(read_text(R"({"timestamp":7,"primary":[256,0,0]})"));
    CHECK_FALSE(read_text(R"({"timestamp":7,"primary":"red"})"));
    CHECK_FALSE(read_text(R"({"primary":[1,2,3]})"));
}

TEST_CASE("state roles: colour contrast", "[sendspin][roles]") {
    constexpr color::Rgb kBlack{.r = 0, .g = 0, .b = 0};
    constexpr color::Rgb kWhite{.r = 255, .g = 255, .b = 255};
    CHECK(std::abs(color::contrast_ratio(kBlack, kWhite) - 21.0) < 1e-9);
    CHECK(std::abs(color::contrast_ratio(kWhite, kWhite) - 1.0) < 1e-9);
    // #767676 is the lightest grey that reaches 4.5:1 on white, and #777777 falls just short.
    CHECK(color::contrast_ratio(color::Rgb{.r = 0x77, .g = 0x77, .b = 0x77}, kWhite) < 4.5);
    CHECK(color::contrast_ratio(color::Rgb{.r = 0x76, .g = 0x76, .b = 0x76}, kWhite) >= 4.5);

    // Artwork's colours as extracted, which do not meet the pairs.
    color::State state;
    state.timestamp = 1;
    state.background_dark = color::Rgb{.r = 120, .g = 90, .b = 200};
    state.background_light = color::Rgb{.r = 150, .g = 140, .b = 60};
    state.on_dark = color::Rgb{.r = 230, .g = 220, .b = 250};
    state.on_light = color::Rgb{.r = 210, .g = 200, .b = 120};
    state.primary = color::Rgb{.r = 120, .g = 90, .b = 200};
    state.accent = color::Rgb{.r = 250, .g = 250, .b = 10};
    CHECK_FALSE(color::meets_contrast(state));
    const color::State fixed = color::with_contrast(state);
    CHECK(color::meets_contrast(fixed));
    CHECK(color::contrast_ratio(*fixed.background_dark, kWhite) >= color::kMinimumContrast);
    CHECK(color::contrast_ratio(*fixed.background_dark, *fixed.on_dark) >= color::kMinimumContrast);
    CHECK(color::contrast_ratio(*fixed.background_light, kBlack) >= color::kMinimumContrast);
    CHECK(color::contrast_ratio(*fixed.background_light, *fixed.on_light) >= color::kMinimumContrast);
    CHECK(color::contrast_ratio(*fixed.on_dark, kBlack) >= color::kMinimumContrast);
    CHECK(color::contrast_ratio(*fixed.on_light, kWhite) >= color::kMinimumContrast);
    // Darkened or lightened only as far as needed, and the colours without a rule untouched.
    CHECK(fixed.background_dark != color::Rgb{});
    CHECK(fixed.primary == state.primary);
    CHECK(fixed.accent == state.accent);
    CHECK(color::with_contrast(fixed) == fixed);
}

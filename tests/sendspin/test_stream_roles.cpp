#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ac3/sendspin/json.hpp"
#include "ac3/sendspin/stream_roles.hpp"

// artwork@v1, visualizer@v1 and source@v1's objects and binary messages (roles/artwork/v1.md,
// roles/visualizer/v1.md, roles/source/v1.md), byte for byte against the role files' layouts, and
// the messages a client must refuse.

namespace {

namespace json = ac3::sendspin::json;
namespace artwork = ac3::sendspin::artwork;
namespace visualizer = ac3::sendspin::visualizer;
namespace source = ac3::sendspin::source;

struct Parsed {
    std::string text;
    std::vector<json::Token> tokens;
    json::Document document;

    explicit Parsed(std::string input) : text(std::move(input)) {
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

}  // namespace

TEST_CASE("stream roles: artwork channels", "[sendspin][roles]") {
    artwork::Channels channels;
    channels.channels = {
        {.source = artwork::Source::kAlbum, .format = artwork::Format::kJpeg, .width = 480, .height = 480},
        {.source = artwork::Source::kNone, .format = artwork::Format::kJpeg, .width = 0, .height = 0},
        {.source = artwork::Source::kArtist, .format = artwork::Format::kPng, .width = 1280, .height = 720},
    };
    const std::string text = written(channels, artwork::write_channels);
    CHECK(text == R"({"channels":[{"source":"album","format":"jpeg","width":480,"height":480},{"source":"none"},)"
                  R"({"source":"artist","format":"png","width":1280,"height":720}]})");
    const Parsed parsed(text);
    CHECK(artwork::read_channels(parsed.root()) == channels);
    CHECK(channels.at(1).source == artwork::Source::kNone);
    CHECK(channels.at(3).source == artwork::Source::kNone);

    const auto read = [](std::string input) { return artwork::read_channels(Parsed(std::move(input)).root()); };
    CHECK(read(R"({"channels":[]})").has_value());
    CHECK_FALSE(read(R"({"channels":[{"source":"none"},{"source":"none"},{"source":"none"},{"source":"none"},{"source":"none"}]})"));
    CHECK_FALSE(read(R"({"channels":[{"source":"album","width":10,"height":10}]})"));
    CHECK_FALSE(read(R"({"channels":[{"source":"album","format":"jpeg","width":0,"height":10}]})"));
    CHECK_FALSE(read(R"({"channels":[{"source":"poster"}]})"));
    CHECK_FALSE(read(R"({})"));
}

TEST_CASE("stream roles: artwork messages", "[sendspin][roles]") {
    const auto announce = artwork::announce(2, 0x0102030405060708, 0x0A0B0C0D);
    CHECK(announce == std::array<std::uint8_t, 14>{10, 2, 1, 2, 3, 4, 5, 6, 7, 8, 0x0A, 0x0B, 0x0C, 0x0D});
    CHECK(artwork::cancel(3) == std::array<std::uint8_t, 2>{11, 1});
    const std::vector<std::uint8_t> image{0xFF, 0xD8, 0xFF, 0xE0};
    const std::optional<std::vector<std::uint8_t>> part = artwork::part(0, image);
    REQUIRE(part.has_value());
    CHECK(*part == std::vector<std::uint8_t>{8, 0, 0xFF, 0xD8, 0xFF, 0xE0});
    CHECK_FALSE(artwork::part(0, std::vector<std::uint8_t>(artwork::kMaxMessageBytes - 1)).has_value());
    CHECK(artwork::part(0, std::vector<std::uint8_t>(artwork::kMaxMessageBytes - 2)).has_value());

    const auto parsed_announce = artwork::parse_message(announce);
    REQUIRE(parsed_announce.has_value());
    CHECK(parsed_announce->kind == artwork::Kind::kAnnounce);
    CHECK(parsed_announce->channel == 2);
    CHECK(parsed_announce->timestamp == 0x0102030405060708);
    CHECK(parsed_announce->total_size == 0x0A0B0C0D);
    const auto parsed_part = artwork::parse_message(*part);
    REQUIRE(parsed_part.has_value());
    CHECK(parsed_part->kind == artwork::Kind::kPart);
    CHECK(parsed_part->data.size() == 4);
    const auto parsed_cancel = artwork::parse_message(artwork::cancel(1));
    REQUIRE(parsed_cancel.has_value());
    CHECK(parsed_cancel->kind == artwork::Kind::kCancel);
    CHECK(parsed_cancel->channel == 1);

    const auto error = [](std::vector<std::uint8_t> message) {
        const auto parsed = artwork::parse_message(message);
        REQUIRE_FALSE(parsed.has_value());
        return parsed.error();
    };
    CHECK(error({12, 0, 1}) == artwork::MessageError::kNotArtwork);
    CHECK(error({8}) == artwork::MessageError::kTooShort);
    CHECK(error(std::vector<std::uint8_t>(artwork::kMaxMessageBytes + 1, 8)) == artwork::MessageError::kTooLong);
    CHECK(error({8, 4}) == artwork::MessageError::kReservedFlags);
    CHECK(error({8, 3}) == artwork::MessageError::kReservedFlags);
    CHECK(error({8, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}) == artwork::MessageError::kBadAnnounce);
    CHECK(error({8, 1, 0}) == artwork::MessageError::kBadCancel);
}

TEST_CASE("stream roles: visualizer objects", "[sendspin][roles]") {
    CHECK(written(visualizer::Support{.buffer_capacity = 4096}, visualizer::write_support) == R"({"buffer_capacity":4096})");

    visualizer::State state;
    state.types = {visualizer::Type::kLoudness, visualizer::Type::kSpectrum, visualizer::Type::kBeat};
    state.rate_max = 30;
    state.spectrum = visualizer::Spectrum{.n_disp_bins = 16, .scale = visualizer::Scale::kMel, .f_min = 20, .f_max = 16000};
    const std::string text = written(state, visualizer::write_state);
    CHECK(text == R"({"types":["loudness","spectrum","beat"],"rate_max":30,)"
                  R"("spectrum":{"n_disp_bins":16,"scale":"mel","f_min":20,"f_max":16000}})");
    const Parsed parsed(text);
    CHECK(visualizer::read_state(parsed.root()) == state);

    const auto read_state = [](std::string input) { return visualizer::read_state(Parsed(std::move(input)).root()); };
    CHECK(read_state(R"({"types":[],"rate_max":1})").has_value());
    // spectrum without its configuration is the protocol error the role names.
    CHECK_FALSE(read_state(R"({"types":["spectrum"],"rate_max":30})"));
    CHECK_FALSE(read_state(R"({"types":["loudness"],"rate_max":0})"));
    CHECK_FALSE(read_state(R"({"types":["loudness"],"rate_max":30,"spectrum":{"n_disp_bins":8,"scale":"bark","f_min":0,"f_max":1}})"));
    const auto unknown = read_state(R"({"types":["pitch","peak"],"rate_max":10})");
    REQUIRE(unknown.has_value());
    CHECK(unknown->types == std::vector<visualizer::Type>{visualizer::Type::kPeak});

    // What the server streams: the types both have, at the lower rate, with its downbeats.
    const std::array<visualizer::Type, 3> available{visualizer::Type::kLoudness, visualizer::Type::kBeat, visualizer::Type::kPeak};
    const visualizer::StreamStart start = visualizer::derive(state, available, 20, true);
    CHECK(start.types == std::vector<visualizer::Type>{visualizer::Type::kLoudness, visualizer::Type::kBeat});
    CHECK(start.rate_max == 20);
    CHECK(start.tracks_downbeats == true);
    CHECK_FALSE(start.spectrum.has_value());
    const std::string start_text = written(start, visualizer::write_stream_start);
    CHECK(start_text == R"({"types":["loudness","beat"],"rate_max":20,"tracks_downbeats":true})");
    const Parsed start_parsed(start_text);
    CHECK(visualizer::read_stream_start(start_parsed.root()) == start);
    CHECK_FALSE(visualizer::read_stream_start(Parsed(R"({"types":["beat"],"rate_max":20})").root()));
}

TEST_CASE("stream roles: visualizer frames", "[sendspin][roles]") {
    const auto frame = [](visualizer::Type type) {
        visualizer::Frame f;
        f.type = type;
        f.timestamp = 0x0102030405060708;
        return f;
    };
    visualizer::Frame loudness = frame(visualizer::Type::kLoudness);
    loudness.value = 0xABCD;
    CHECK(visualizer::write_frame(loudness) == std::vector<std::uint8_t>{16, 1, 2, 3, 4, 5, 6, 7, 8, 0xAB, 0xCD});
    visualizer::Frame beat = frame(visualizer::Type::kBeat);
    beat.downbeat = true;
    CHECK(visualizer::write_frame(beat) == std::vector<std::uint8_t>{17, 1, 2, 3, 4, 5, 6, 7, 8, 1});
    visualizer::Frame f_peak = frame(visualizer::Type::kFPeak);
    f_peak.frequency = 440;
    f_peak.value = 30000;
    CHECK(visualizer::write_frame(f_peak) == std::vector<std::uint8_t>{18, 1, 2, 3, 4, 5, 6, 7, 8, 0x01, 0xB8, 0x75, 0x30});
    visualizer::Frame spectrum = frame(visualizer::Type::kSpectrum);
    spectrum.bins = {1, 0x0203, 0xFFFF};
    CHECK(visualizer::write_frame(spectrum) ==
          std::vector<std::uint8_t>{19, 1, 2, 3, 4, 5, 6, 7, 8, 0x00, 0x01, 0x02, 0x03, 0xFF, 0xFF});
    visualizer::Frame peak = frame(visualizer::Type::kPeak);
    peak.strength = 200;
    CHECK(visualizer::write_frame(peak) == std::vector<std::uint8_t>{20, 1, 2, 3, 4, 5, 6, 7, 8, 200});
    // No peak: the amplitude goes out as 0 whatever it held.
    visualizer::Frame silent = frame(visualizer::Type::kFPeak);
    silent.value = 500;
    CHECK(visualizer::write_frame(silent) == std::vector<std::uint8_t>{18, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0});

    for (const visualizer::Frame& each : {loudness, beat, f_peak, spectrum, peak}) {
        CHECK(visualizer::parse_frame(visualizer::write_frame(each), 3) == each);
    }
    // Reserved beat bits are ignored; a length of the wrong size, a spectrum of another bin count,
    // an amplitude without a peak and a reserved ID are not frames.
    CHECK(visualizer::parse_frame(std::vector<std::uint8_t>{17, 0, 0, 0, 0, 0, 0, 0, 0, 0xFE}, 0)->downbeat == false);
    CHECK(visualizer::parse_frame(std::vector<std::uint8_t>{18, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0).has_value());
    CHECK_FALSE(visualizer::parse_frame(std::vector<std::uint8_t>{18, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0xF4}, 0));
    CHECK_FALSE(visualizer::parse_frame(std::vector<std::uint8_t>{16, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 0));
    CHECK_FALSE(visualizer::parse_frame(visualizer::write_frame(spectrum), 4));
    CHECK_FALSE(visualizer::parse_frame(std::vector<std::uint8_t>{21, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 0));
    CHECK_FALSE(visualizer::parse_frame(std::vector<std::uint8_t>{16, 0, 0}, 0));

    CHECK(visualizer::scaled_level(-90.0) == 0);
    CHECK(visualizer::scaled_level(-60.0) == 0);
    CHECK(visualizer::scaled_level(-30.0) == 32768);
    CHECK(visualizer::scaled_level(0.0) == 65535);
    CHECK(visualizer::scaled_level(6.0) == 65535);
}

TEST_CASE("stream roles: source objects and chunks", "[sendspin][roles]") {
    CHECK(written(source::Support{.line_sense = true}, source::write_support) == R"({"features":{"line_sense":true}})");
    CHECK(source::read_support(Parsed(R"({})").root())->line_sense == false);
    CHECK_FALSE(source::read_support(Parsed(R"({"features":{"line_sense":"yes"}})").root()));

    CHECK(written(source::State{.signal = source::Signal::kAbsent}, source::write_state) == R"({"signal":"absent"})");
    CHECK(written(source::State{.signal = std::nullopt}, source::write_state) == R"({})");
    CHECK(source::read_state(Parsed(R"({"signal":"present"})").root())->signal == source::Signal::kPresent);
    CHECK_FALSE(source::read_state(Parsed(R"({"signal":"loud"})").root()));

    CHECK(written(source::Command::kStart, source::write_command) == R"({"command":"start"})");
    CHECK(source::read_command(Parsed(R"({"command":"stop"})").root()) == source::Command::kStop);
    CHECK_FALSE(source::read_command(Parsed(R"({"command":"pause"})").root()));

    const std::vector<std::uint8_t> frame{9, 8, 7};
    const std::vector<std::uint8_t> chunk = source::write_chunk(-2, frame);
    CHECK(chunk == std::vector<std::uint8_t>{12, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE, 9, 8, 7});
    const std::optional<source::Chunk> parsed = source::parse_chunk(chunk);
    REQUIRE(parsed.has_value());
    CHECK(parsed->timestamp == -2);
    CHECK(std::vector<std::uint8_t>(parsed->frame.begin(), parsed->frame.end()) == frame);
    CHECK(source::parse_chunk(source::write_chunk(1, {}))->frame.empty());
    CHECK_FALSE(source::parse_chunk(std::vector<std::uint8_t>{12, 0, 0}));
    CHECK_FALSE(source::parse_chunk(std::vector<std::uint8_t>{4, 0, 0, 0, 0, 0, 0, 0, 0}));
}

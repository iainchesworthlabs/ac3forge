#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/json.hpp"
#include "ac3/sendspin/messages.hpp"

// The core messages in both dialects. The specification's texts are checked byte for byte
// against what messaging.md and roles/player/v1.md define, aiosendspin 9.1.1's against the
// dataclasses in its models/core.py and models/player.py, and every reader against the
// shapes it must refuse and the ones it must let through.

namespace {

using ac3::sendspin::Dialect;
namespace m = ac3::sendspin::messages;
namespace json = ac3::sendspin::json;

// A parsed message whose payload stays valid while the Parsed lives.
struct Parsed {
    std::string text;
    std::vector<json::Token> tokens;
    json::Document document;
    std::optional<m::Envelope> envelope;

    explicit Parsed(std::string message) : text(std::move(message)) {
        if (document.parse(text, tokens, 4096)) {
            envelope = m::read_envelope(document);
        }
    }
    // The document points into `text` and `tokens`.
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    Parsed(Parsed&&) = delete;
    Parsed& operator=(Parsed&&) = delete;
    ~Parsed() = default;

    [[nodiscard]] json::Value payload() const {
        REQUIRE(envelope.has_value());
        return envelope->payload;
    }
};

}  // namespace

TEST_CASE("messages: the envelope", "[sendspin][messages]") {
    const Parsed ok(R"({"type":"server/hello","payload":{"name":"Hearth"}})");
    REQUIRE(ok.envelope.has_value());
    CHECK(ok.envelope->type == "server/hello");
    CHECK(ok.envelope->payload["name"].equals("Hearth"));

    CHECK_FALSE(Parsed(R"({"payload":{}})").envelope.has_value());
    CHECK_FALSE(Parsed(R"({"type":"server/hello"})").envelope.has_value());
    CHECK_FALSE(Parsed(R"({"type":"server/hello","payload":[]})").envelope.has_value());
    CHECK_FALSE(Parsed(R"({"type":7,"payload":{}})").envelope.has_value());
    CHECK_FALSE(Parsed(R"(["server/hello"])").envelope.has_value());
}

TEST_CASE("messages: server/hello", "[sendspin][messages]") {
    const std::string text = m::write_server_hello({.name = "Hearth", .languages = {"en-GB", "cy"}});
    CHECK(text == R"({"type":"server/hello","payload":{"name":"Hearth","languages":["en-GB","cy"]}})");
    const Parsed parsed(text);
    const auto hello = m::read_server_hello(parsed.payload());
    REQUIRE(hello.has_value());
    CHECK(hello->name == "Hearth");
    CHECK(hello->languages == std::vector<std::string>{"en-GB", "cy"});

    CHECK(m::write_server_hello({.name = "Hearth", .languages = {}}) ==
          R"({"type":"server/hello","payload":{"name":"Hearth"}})");
    CHECK_FALSE(m::read_server_hello(Parsed(R"({"type":"server/hello","payload":{}})").payload()));
    CHECK_FALSE(m::read_server_hello(
        Parsed(R"({"type":"server/hello","payload":{"name":"x","languages":"en"}})").payload()));
}

namespace {

m::ClientHello sample_hello() {
    m::ClientHello hello;
    hello.name = "Kitchen";
    hello.device_info.product_name = "Hearth sink";
    hello.supported_roles = {"_ac3forge_player@v1", "player@v1"};
    hello.player_support = m::PlayerSupport{
        .supported_formats = {{.codec = m::Codec::kFlac, .channels = 2, .sample_rate = 48000, .bit_depth = 24},
                              {.codec = m::Codec::kOpus, .channels = 2, .sample_rate = 48000, .bit_depth = 16}},
        .buffer_capacity = 1048576,
        .commands = {m::PlayerCommand::kVolume, m::PlayerCommand::kMute},
    };
    hello.pair_methods = {
        {.method = m::PairMethod::kPairingPsk, .locations = {m::SecretLocation::kDevice}, .out_channels = {},
         .formats = {}, .min_pin_length = 0},
        {.method = m::PairMethod::kDynamicCode, .locations = {}, .out_channels = {m::OutChannel::kDisplay},
         .formats = {m::CodeFormat::kDigits}, .min_pin_length = 6},
    };
    return hello;
}

}  // namespace

TEST_CASE("messages: client/hello in the specification's form", "[sendspin][messages]") {
    const std::string text = m::write_client_hello(sample_hello(), Dialect::kSpecification);
    CHECK(text ==
          R"({"type":"client/hello","payload":{"name":"Kitchen","device_info":{"product_name":"Hearth sink"},)"
          R"("supported_roles":["_ac3forge_player@v1","player@v1"],)"
          R"("player@v1_support":{"supported_formats":[{"codec":"flac","channels":2,"sample_rate":48000,"bit_depth":24},)"
          R"({"codec":"opus","channels":2,"sample_rate":48000,"bit_depth":16}],"buffer_capacity":1048576},)"
          R"("supported_pair_methods":{"pairing_psk":{"locations":["device"]},)"
          R"("dynamic_pairing_code":{"out_channels":["display"],"formats":["digits"]}},)"
          R"("unpaired_access":{"enabled":false}}})");

    const Parsed parsed(text);
    CHECK(m::client_hello_dialect(parsed.payload()) == Dialect::kSpecification);
    const auto hello = m::read_client_hello(parsed.payload(), Dialect::kSpecification);
    REQUIRE(hello.has_value());
    CHECK(hello->name == "Kitchen");
    CHECK(hello->device_info.product_name == "Hearth sink");
    CHECK(hello->supported_roles.size() == 2);
    REQUIRE(hello->player_support.has_value());
    CHECK(hello->player_support->supported_formats == sample_hello().player_support->supported_formats);
    CHECK(hello->player_support->buffer_capacity == 1048576);
    // The support object carries no commands in this dialect.
    CHECK(hello->player_support->commands.empty());
    REQUIRE(hello->pair_methods.size() == 2);
    CHECK(hello->pair_methods[0].method == m::PairMethod::kPairingPsk);
    CHECK(hello->pair_methods[0].locations == std::vector<m::SecretLocation>{m::SecretLocation::kDevice});
    CHECK(hello->pair_methods[1].method == m::PairMethod::kDynamicCode);
    CHECK(hello->pair_methods[1].formats == std::vector<m::CodeFormat>{m::CodeFormat::kDigits});
    CHECK_FALSE(hello->unpaired_access);
}

TEST_CASE("messages: client/hello in aiosendspin 9.1.1's form", "[sendspin][messages]") {
    m::ClientHello sample = sample_hello();
    sample.trusts_server = true;
    const std::string text = m::write_client_hello(sample, Dialect::kAiosendspin911);
    CHECK(text ==
          R"({"type":"client/hello","payload":{"name":"Kitchen","device_info":{"product_name":"Hearth sink"},)"
          R"("supported_roles":["_ac3forge_player@v1","player@v1"],)"
          R"("player@v1_support":{"supported_formats":[{"codec":"flac","channels":2,"sample_rate":48000,"bit_depth":24},)"
          R"({"codec":"opus","channels":2,"sample_rate":48000,"bit_depth":16}],"buffer_capacity":1048576,)"
          R"("supported_commands":["volume","mute"]},"trust_level":"user",)"
          R"("supported_pair_methods":[{"method":"pairing_psk","locations":["device"]},)"
          R"({"method":"dynamic_pin","out_channels":["display"],"min_pin_length":6}],)"
          R"("unpaired_access":{"enabled":false}}})");

    const Parsed parsed(text);
    CHECK(m::client_hello_dialect(parsed.payload()) == Dialect::kAiosendspin911);
    const auto hello = m::read_client_hello(parsed.payload(), Dialect::kAiosendspin911);
    REQUIRE(hello.has_value());
    CHECK(hello->trusts_server);
    REQUIRE(hello->player_support.has_value());
    CHECK(hello->player_support->commands ==
          std::vector<m::PlayerCommand>{m::PlayerCommand::kVolume, m::PlayerCommand::kMute});
    REQUIRE(hello->pair_methods.size() == 2);
    CHECK(hello->pair_methods[1].method == m::PairMethod::kDynamicCode);
    CHECK(hello->pair_methods[1].min_pin_length == 6);
    CHECK(hello->pair_methods[1].formats.empty());
}

TEST_CASE("messages: a client/hello as aiosendspin 9.1.1's client writes it", "[sendspin][messages]") {
    // models/core.py ClientHelloPayload with omit_none, as client/connection.py builds it:
    // no pairing_psk when that method is off, a dynamic_pin descriptor with its minimum.
    const Parsed parsed(
        R"({"type":"client/hello","payload":{"name":"Living room","supported_roles":["player@v1"],)"
        R"("trust_level":"none","player@v1_support":{"supported_formats":[{"codec":"pcm","channels":2,)"
        R"("sample_rate":44100,"bit_depth":16}],"buffer_capacity":2000000,"supported_commands":[]},)"
        R"("supported_pair_methods":[{"method":"dynamic_pin","min_pin_length":6}],)"
        R"("unpaired_access":{"enabled":true}}})");
    REQUIRE(m::client_hello_dialect(parsed.payload()) == Dialect::kAiosendspin911);
    const auto hello = m::read_client_hello(parsed.payload(), Dialect::kAiosendspin911);
    REQUIRE(hello.has_value());
    CHECK_FALSE(hello->trusts_server);
    CHECK(hello->unpaired_access);
    REQUIRE(hello->player_support.has_value());
    CHECK(hello->player_support->supported_formats.size() == 1);
    CHECK(hello->player_support->commands.empty());
    REQUIRE(hello->pair_methods.size() == 1);
    CHECK(hello->pair_methods[0].min_pin_length == 6);
    CHECK(hello->pair_methods[0].out_channels.empty());
}

TEST_CASE("messages: client/hello's pair methods follow pairing.md's rules for the unknown",
          "[sendspin][messages]") {
    const auto read = [](std::string_view methods) {
        const Parsed parsed(std::string(R"({"type":"client/hello","payload":{"name":"x","supported_roles":[],)"
                                         R"("unpaired_access":{"enabled":false},"supported_pair_methods":)") +
                            std::string(methods) + "}}");
        return m::read_client_hello(parsed.payload(), Dialect::kSpecification);
    };

    SECTION("an unknown method is ignored") {
        const auto hello = read(R"({"pairing_psk":{},"telepathy":{"strength":9}})");
        REQUIRE(hello.has_value());
        REQUIRE(hello->pair_methods.size() == 1);
        CHECK(hello->pair_methods[0].method == m::PairMethod::kPairingPsk);
    }
    SECTION("a dynamic code with no recognised format counts as unrecognised") {
        const auto hello = read(R"({"pairing_psk":{},"dynamic_pairing_code":{"out_channels":["display"],"formats":["hologram"]}})");
        REQUIRE(hello.has_value());
        CHECK(hello->pair_methods.size() == 1);
    }
    SECTION("unknown formats and channels are dropped from a usable descriptor") {
        const auto hello = read(
            R"({"dynamic_pairing_code":{"out_channels":["smell","speaker"],"formats":["qr_code","hologram"]}})");
        REQUIRE(hello.has_value());
        REQUIRE(hello->pair_methods.size() == 1);
        CHECK(hello->pair_methods[0].out_channels == std::vector<m::OutChannel>{m::OutChannel::kSpeaker});
        CHECK(hello->pair_methods[0].formats == std::vector<m::CodeFormat>{m::CodeFormat::kQrCode});
    }
    SECTION("a static code beside a dynamic one is disregarded") {
        const auto hello = read(
            R"({"static_pairing_code":{},"dynamic_pairing_code":{"out_channels":["display"],"formats":["digits"]}})");
        REQUIRE(hello.has_value());
        REQUIRE(hello->pair_methods.size() == 1);
        CHECK(hello->pair_methods[0].method == m::PairMethod::kDynamicCode);
    }
    SECTION("the array form is not the specification's") {
        CHECK_FALSE(read(R"([{"method":"pairing_psk"}])").has_value());
    }
}

TEST_CASE("messages: client/hello refusals", "[sendspin][messages]") {
    const auto read = [](std::string_view payload, Dialect dialect) {
        const Parsed parsed(std::string(R"({"type":"client/hello","payload":)") + std::string(payload) + "}");
        return m::read_client_hello(parsed.payload(), dialect);
    };
    const std::string_view base = R"("supported_pair_methods":{},"unpaired_access":{"enabled":false})";
    CHECK(read(std::string(R"({"name":"x","supported_roles":[],)") + std::string(base) + "}", Dialect::kSpecification));
    CHECK_FALSE(read(std::string(R"({"supported_roles":[],)") + std::string(base) + "}", Dialect::kSpecification));
    CHECK_FALSE(read(std::string(R"({"name":"x","supported_roles":"player@v1",)") + std::string(base) + "}",
                     Dialect::kSpecification));
    CHECK_FALSE(read(R"({"name":"x","supported_roles":[],"supported_pair_methods":{}})", Dialect::kSpecification));
    // A support object whose format lacks a field is malformed; one with an unknown codec
    // only loses that entry.
    CHECK_FALSE(read(std::string(R"({"name":"x","supported_roles":["player@v1"],"player@v1_support":)"
                                 R"({"supported_formats":[{"codec":"pcm","channels":2,"sample_rate":48000}],"buffer_capacity":1},)") +
                         std::string(base) + "}",
                     Dialect::kSpecification));
    const auto unknown_codec = read(
        std::string(R"({"name":"x","supported_roles":["player@v1"],"player@v1_support":)"
                    R"({"supported_formats":[{"codec":"mp3","channels":2,"sample_rate":48000,"bit_depth":16},)"
                    R"({"codec":"pcm","channels":2,"sample_rate":48000,"bit_depth":16}],"buffer_capacity":1},)") +
            std::string(base) + "}",
        Dialect::kSpecification);
    REQUIRE(unknown_codec.has_value());
    CHECK(unknown_codec->player_support->supported_formats.size() == 1);
    // aiosendspin 9.1.1 requires the support object's command list.
    CHECK_FALSE(read(R"({"name":"x","supported_roles":["player@v1"],"trust_level":"none","player@v1_support":)"
                     R"({"supported_formats":[],"buffer_capacity":1}})",
                     Dialect::kAiosendspin911));
}

TEST_CASE("messages: server/activate in both dialects", "[sendspin][messages]") {
    SECTION("playback with roles") {
        const m::Activate activate{.activities = {m::Activity::kPlayback},
                                   .active_roles = std::vector<std::string>{"player@v1"},
                                   .pairing = std::nullopt};
        const std::string text = m::write_activate(activate, Dialect::kSpecification);
        CHECK(text == R"({"type":"server/activate","payload":{"activities":["playback"],"active_roles":["player@v1"]}})");
        const auto read = m::read_activate(Parsed(text).payload(), Dialect::kSpecification);
        REQUIRE(read.has_value());
        CHECK(read->activities == std::vector<m::Activity>{m::Activity::kPlayback});
        CHECK(read->active_roles == std::vector<std::string>{"player@v1"});
        CHECK_FALSE(read->pairing.has_value());
    }
    SECTION("the specification's pairing object") {
        const m::Activate activate{
            .activities = {m::Activity::kPairing},
            .active_roles = std::vector<std::string>{},
            .pairing = m::PairingActivation{.method = m::PairMethod::kDynamicCode,
                                            .format = m::CodeFormat::kQrCode,
                                            .pin_length = 0,
                                            .languages = {}},
        };
        const std::string text = m::write_activate(activate, Dialect::kSpecification);
        CHECK(text ==
              R"({"type":"server/activate","payload":{"activities":["pairing"],"active_roles":[],)"
              R"("pairing":{"method":"dynamic_pairing_code","format":"qr_code"}}})");
        const auto read = m::read_activate(Parsed(text).payload(), Dialect::kSpecification);
        REQUIRE(read.has_value());
        REQUIRE(read->pairing.has_value());
        CHECK(read->pairing->method == m::PairMethod::kDynamicCode);
        CHECK(read->pairing->format == m::CodeFormat::kQrCode);
    }
    SECTION("aiosendspin 9.1.1's pairing object") {
        const m::Activate activate{
            .activities = {m::Activity::kPairing},
            .active_roles = std::nullopt,
            .pairing = m::PairingActivation{.method = m::PairMethod::kDynamicCode,
                                            .format = std::nullopt,
                                            .pin_length = 6,
                                            .languages = {"en"}},
        };
        const std::string text = m::write_activate(activate, Dialect::kAiosendspin911);
        CHECK(text ==
              R"({"type":"server/activate","payload":{"activities":["pairing"],)"
              R"("pairing":{"method":"dynamic_pin","pin_length":6,"languages":["en"]}}})");
        const auto read = m::read_activate(Parsed(text).payload(), Dialect::kAiosendspin911);
        REQUIRE(read.has_value());
        CHECK_FALSE(read->active_roles.has_value());
        REQUIRE(read->pairing.has_value());
        CHECK(read->pairing->method == m::PairMethod::kDynamicCode);
        CHECK(read->pairing->pin_length == 6);
        CHECK(read->pairing->languages == std::vector<std::string>{"en"});
        // The same method name read in the other dialect is not recognised.
        const auto other = m::read_activate(Parsed(text).payload(), Dialect::kSpecification);
        REQUIRE(other.has_value());
        CHECK_FALSE(other->pairing->method.has_value());
    }
    SECTION("what a client must be able to answer") {
        const auto read = [](std::string_view payload) {
            return m::read_activate(
                Parsed(std::string(R"({"type":"server/activate","payload":)") + std::string(payload) + "}").payload(),
                Dialect::kSpecification);
        };
        const auto management = read(R"({"activities":["management"]})");
        REQUIRE(management.has_value());
        CHECK(management->activities == std::vector<m::Activity>{m::Activity::kOther});
        CHECK_FALSE(read(R"({"activities":["playback","playback"]})").has_value());
        CHECK_FALSE(read(R"({"activities":["pairing"]})").has_value());
        CHECK_FALSE(read(R"({"activities":"playback"})").has_value());
        // A pairing object without the pairing activity is ignored.
        const auto ignored = read(R"({"activities":[],"pairing":{"method":"pairing_psk"}})");
        REQUIRE(ignored.has_value());
        CHECK_FALSE(ignored->pairing.has_value());
    }
}

TEST_CASE("messages: client/time and server/time", "[sendspin][messages]") {
    const std::string client = m::write_client_time({.client_transmitted = -5});
    CHECK(client == R"({"type":"client/time","payload":{"client_transmitted":-5}})");
    CHECK(m::read_client_time(Parsed(client).payload())->client_transmitted == -5);

    const std::string server = m::write_server_time(
        {.client_transmitted = 1, .server_received = 9007199254740993, .server_transmitted = 3});
    CHECK(server ==
          R"({"type":"server/time","payload":{"client_transmitted":1,"server_received":9007199254740993,"server_transmitted":3}})");
    const auto time = m::read_server_time(Parsed(server).payload());
    REQUIRE(time.has_value());
    CHECK(time->server_received == 9007199254740993);
    CHECK_FALSE(m::read_server_time(
        Parsed(R"({"type":"server/time","payload":{"client_transmitted":1,"server_received":2}})").payload()));
    CHECK_FALSE(m::read_client_time(Parsed(R"({"type":"client/time","payload":{"client_transmitted":1.5}})").payload()));
}

TEST_CASE("messages: client/state's player object in both dialects", "[sendspin][messages]") {
    const m::ClientState state{
        .available = true,
        .player = m::PlayerState{.volume = 40,
                                 .muted = false,
                                 .output_delay_ms = 120,
                                 .required_lead_time_ms = 45000,
                                 .min_buffer_ms = 250,
                                 .supported_commands = std::vector<m::PlayerCommand>{
                                     m::PlayerCommand::kVolume, m::PlayerCommand::kMute,
                                     m::PlayerCommand::kSetOutputDelay},
                                 .format = m::AudioFormat{.codec = m::Codec::kFlac,
                                                          .channels = 2,
                                                          .sample_rate = 48000,
                                                          .bit_depth = 24}},
    };

    SECTION("specification") {
        const std::string text = m::write_client_state(state, Dialect::kSpecification);
        CHECK(text ==
              R"({"type":"client/state","payload":{"available":true,"player":{"volume":40,"muted":false,)"
              R"("output_delay_ms":120,"required_lead_time_ms":45000,"min_buffer_ms":250,)"
              R"("supported_commands":["volume","mute","set_output_delay"],)"
              R"("format":{"codec":"flac","channels":2,"sample_rate":48000,"bit_depth":24}}}})");
        const auto read = m::read_client_state(Parsed(text).payload(), Dialect::kSpecification);
        REQUIRE(read.has_value());
        CHECK(read->available);
        REQUIRE(read->player.has_value());
        CHECK(read->player->output_delay_ms == 120);
        CHECK(read->player->required_lead_time_ms == 45000);
        CHECK(read->player->supported_commands->size() == 3);
        CHECK(read->player->format == state.player->format);
    }
    SECTION("aiosendspin 9.1.1") {
        // static_delay_ms, only set_static_delay among the commands, timing capped at
        // 30,000 ms, and no format (C29, C30, C32).
        const std::string text = m::write_client_state(state, Dialect::kAiosendspin911);
        CHECK(text ==
              R"({"type":"client/state","payload":{"available":true,"player":{"volume":40,"muted":false,)"
              R"("static_delay_ms":120,"required_lead_time_ms":30000,"min_buffer_ms":250,)"
              R"("supported_commands":["set_static_delay"]}}})");
        const auto read = m::read_client_state(Parsed(text).payload(), Dialect::kAiosendspin911);
        REQUIRE(read.has_value());
        REQUIRE(read->player.has_value());
        CHECK(read->player->output_delay_ms == 120);
        CHECK(read->player->supported_commands ==
              std::vector<m::PlayerCommand>{m::PlayerCommand::kSetOutputDelay});

        // An incremental 9.1.1 report: only what changed.
        const auto partial = m::read_client_state(
            Parsed(R"({"type":"client/state","payload":{"player":{"volume":55}}})").payload(),
            Dialect::kAiosendspin911);
        REQUIRE(partial.has_value());
        CHECK_FALSE(partial->available);
        CHECK(partial->player->volume == 55);
        CHECK_FALSE(partial->player->output_delay_ms.has_value());
        CHECK_FALSE(partial->player->supported_commands.has_value());
    }
    SECTION("refusals") {
        const auto read = [](std::string_view payload, Dialect dialect) {
            return m::read_client_state(
                Parsed(std::string(R"({"type":"client/state","payload":)") + std::string(payload) + "}").payload(),
                dialect);
        };
        CHECK(read(R"({"available":false})", Dialect::kSpecification).has_value());
        CHECK_FALSE(read(R"({})", Dialect::kSpecification).has_value());
        CHECK_FALSE(read(R"({"available":true,"player":{"volume":101,"output_delay_ms":0,"required_lead_time_ms":0,"min_buffer_ms":0,"supported_commands":[]}})",
                         Dialect::kSpecification));
        CHECK_FALSE(read(R"({"available":true,"player":{"output_delay_ms":5001,"required_lead_time_ms":0,"min_buffer_ms":0,"supported_commands":[]}})",
                         Dialect::kSpecification));
        CHECK_FALSE(read(R"({"available":true,"player":{"output_delay_ms":0,"min_buffer_ms":0,"supported_commands":[]}})",
                         Dialect::kSpecification));
        // 9.1.1's delay field is not the specification's.
        CHECK_FALSE(read(R"({"available":true,"player":{"static_delay_ms":0,"required_lead_time_ms":0,"min_buffer_ms":0,"supported_commands":[]}})",
                         Dialect::kSpecification));
    }
}

TEST_CASE("messages: server/command's player object in both dialects", "[sendspin][messages]") {
    const auto round_trip = [](const m::PlayerCommandMessage& player, Dialect dialect, std::string_view expected) {
        const std::string text = m::write_server_command({.player = player}, dialect);
        CHECK(text == expected);
        const auto read = m::read_server_command(Parsed(text).payload(), dialect);
        REQUIRE(read.has_value());
        REQUIRE(read->player.has_value());
        CHECK(read->player->command == player.command);
        return *read->player;
    };
    CHECK(round_trip({.command = m::PlayerCommand::kVolume, .volume = 70, .mute = false, .output_delay_ms = 0},
                     Dialect::kSpecification,
                     R"({"type":"server/command","payload":{"player":{"command":"volume","volume":70}}})")
              .volume == 70);
    CHECK(round_trip({.command = m::PlayerCommand::kMute, .volume = 0, .mute = true, .output_delay_ms = 0},
                     Dialect::kAiosendspin911,
                     R"({"type":"server/command","payload":{"player":{"command":"mute","mute":true}}})")
              .mute);
    CHECK(round_trip({.command = m::PlayerCommand::kSetOutputDelay, .volume = 0, .mute = false, .output_delay_ms = 80},
                     Dialect::kSpecification,
                     R"({"type":"server/command","payload":{"player":{"command":"set_output_delay","output_delay_ms":80}}})")
              .output_delay_ms == 80);
    CHECK(round_trip({.command = m::PlayerCommand::kSetOutputDelay, .volume = 0, .mute = false, .output_delay_ms = 80},
                     Dialect::kAiosendspin911,
                     R"({"type":"server/command","payload":{"player":{"command":"set_static_delay","static_delay_ms":80}}})")
              .output_delay_ms == 80);

    const auto read = [](std::string_view payload) {
        return m::read_server_command(
            Parsed(std::string(R"({"type":"server/command","payload":)") + std::string(payload) + "}").payload(),
            Dialect::kSpecification);
    };
    CHECK(read(R"({})").has_value());
    CHECK_FALSE(read(R"({"player":{"command":"volume"}})").has_value());
    CHECK_FALSE(read(R"({"player":{"command":"volume","volume":-1}})").has_value());
    CHECK_FALSE(read(R"({"player":{"command":"set_static_delay","static_delay_ms":1}})").has_value());
    CHECK_FALSE(read(R"({"player":{"command":"louder"}})").has_value());
}

TEST_CASE("messages: stream/start, stream/clear and stream/end", "[sendspin][messages]") {
    const std::vector<std::uint8_t> header{'f', 'L', 'a', 'C', 0x00, 0x00, 0x00, 0x22};
    const m::StreamStart start{
        .server_transmitted = 123456789,
        .player = m::PlayerStream{.format = {.codec = m::Codec::kFlac, .channels = 2, .sample_rate = 48000, .bit_depth = 16},
                                  .codec_header = header},
    };
    const std::string text = m::write_stream_start(start);
    CHECK(text ==
          R"({"type":"stream/start","payload":{"server_transmitted":123456789,"player":{"codec":"flac",)"
          R"("sample_rate":48000,"channels":2,"bit_depth":16,"codec_header":"ZkxhQwAAACI="}}})");
    const auto read = m::read_stream_start(Parsed(text).payload());
    REQUIRE(read.has_value());
    CHECK(read->server_transmitted == 123456789);
    REQUIRE(read->player.has_value());
    CHECK(read->player->codec_header == header);

    const auto start_read = [](std::string_view payload) {
        return m::read_stream_start(
            Parsed(std::string(R"({"type":"stream/start","payload":)") + std::string(payload) + "}").payload());
    };
    CHECK_FALSE(start_read(R"({"player":{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":16}})"));
    CHECK_FALSE(start_read(R"({"server_transmitted":1,"player":{"codec":"mp3","sample_rate":48000,"channels":2,"bit_depth":16}})"));
    CHECK_FALSE(start_read(R"({"server_transmitted":1,"player":{"codec":"flac","sample_rate":48000,"channels":2,"bit_depth":16,"codec_header":"ZkxhQw"}})"));
    CHECK(start_read(R"({"server_transmitted":1})").has_value());

    const std::string clear = m::write_stream_clear({.server_transmitted = 5, .roles = std::vector<std::string>{"player"}});
    CHECK(clear == R"({"type":"stream/clear","payload":{"server_transmitted":5,"roles":["player"]}})");
    CHECK(m::read_stream_clear(Parsed(clear).payload())->roles == std::vector<std::string>{"player"});
    CHECK_FALSE(m::read_stream_clear(Parsed(R"({"type":"stream/clear","payload":{}})").payload()));

    CHECK(m::write_stream_end({.roles = std::nullopt}) == R"({"type":"stream/end","payload":{}})");
    // aiosendspin 9.1.1 stamps server_transmitted on stream/end too (C38); it is ignored.
    const auto end = m::read_stream_end(
        Parsed(R"({"type":"stream/end","payload":{"server_transmitted":7,"roles":["player","_ac3forge_player"]}})")
            .payload());
    REQUIRE(end.has_value());
    CHECK(end->roles == std::vector<std::string>{"player", "_ac3forge_player"});
}

TEST_CASE("messages: group/update in both dialects", "[sendspin][messages]") {
    const std::string text = m::write_group_update(
        {.playback_state = m::PlaybackState::kPlaying, .group_id = "g1", .group_name = "Downstairs"});
    CHECK(text ==
          R"({"type":"group/update","payload":{"playback_state":"playing","group_id":"g1","group_name":"Downstairs"}})");
    CHECK(m::read_group_update(Parsed(text).payload(), Dialect::kSpecification).has_value());

    // Music Assistant never sets group_name, and merges updates (C15).
    const Parsed partial(R"({"type":"group/update","payload":{"group_id":"g1"}})");
    CHECK_FALSE(m::read_group_update(partial.payload(), Dialect::kSpecification).has_value());
    const auto read = m::read_group_update(partial.payload(), Dialect::kAiosendspin911);
    REQUIRE(read.has_value());
    CHECK(read->group_id == "g1");
    CHECK_FALSE(read->playback_state.has_value());
    CHECK_FALSE(read->group_name.has_value());
    CHECK_FALSE(m::read_group_update(Parsed(R"({"type":"group/update","payload":{"playback_state":"paused"}})").payload(),
                                     Dialect::kAiosendspin911));
}

TEST_CASE("messages: client/goodbye, client/leave and server/unpair", "[sendspin][messages]") {
    for (const m::GoodbyeReason reason :
         {m::GoodbyeReason::kAnotherServer, m::GoodbyeReason::kShutdown, m::GoodbyeReason::kRestart,
          m::GoodbyeReason::kUserRequest, m::GoodbyeReason::kUnauthorized, m::GoodbyeReason::kPairingRequired,
          m::GoodbyeReason::kConcurrentAttempt, m::GoodbyeReason::kUnpaired}) {
        const auto read = m::read_client_goodbye(Parsed(m::write_client_goodbye(reason)).payload());
        REQUIRE(read.has_value());
        CHECK(*read == reason);
    }
    CHECK(m::write_client_goodbye(m::GoodbyeReason::kConcurrentAttempt) ==
          R"({"type":"client/goodbye","payload":{"reason":"concurrent_attempt"}})");
    CHECK_FALSE(m::read_client_goodbye(Parsed(R"({"type":"client/goodbye","payload":{"reason":"bored"}})").payload()));
    CHECK(m::write_client_leave() == R"({"type":"client/leave","payload":{}})");
    CHECK(m::write_server_unpair() == R"({"type":"server/unpair","payload":{}})");
}

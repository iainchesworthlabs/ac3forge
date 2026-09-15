#include "ac3/sendspin/messages.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/sendspin/ac3forge_player.hpp"
#include "ac3/sendspin/base64.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/json.hpp"

namespace ac3::sendspin::messages {

namespace {

// --- Names on the wire ------------------------------------------------------------------

template <class E>
struct Named {
    E value;
    std::string_view name;
};

template <class E, std::size_t N>
[[nodiscard]] std::optional<E> find_value(const std::array<Named<E>, N>& table, json::Value value) {
    for (const Named<E>& entry : table) {
        if (value.equals(entry.name)) {
            return entry.value;
        }
    }
    return std::nullopt;
}

template <class E, std::size_t N>
[[nodiscard]] std::optional<E> find_value(const std::array<Named<E>, N>& table,
                                          std::string_view name) {
    for (const Named<E>& entry : table) {
        if (name == entry.name) {
            return entry.value;
        }
    }
    return std::nullopt;
}

template <class E, std::size_t N>
[[nodiscard]] std::string_view find_name(const std::array<Named<E>, N>& table, E value) {
    for (const Named<E>& entry : table) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return {};
}

constexpr std::array<Named<Codec>, 3> kCodecs{{
    {Codec::kOpus, "opus"},
    {Codec::kFlac, "flac"},
    {Codec::kPcm, "pcm"},
}};

constexpr std::array<Named<PlayerCommand>, 3> kPlayerCommands{{
    {PlayerCommand::kVolume, "volume"},
    {PlayerCommand::kMute, "mute"},
    {PlayerCommand::kSetOutputDelay, "set_output_delay"},
}};

constexpr std::array<Named<PlayerCommand>, 3> kPlayerCommands911{{
    {PlayerCommand::kVolume, "volume"},
    {PlayerCommand::kMute, "mute"},
    {PlayerCommand::kSetOutputDelay, "set_static_delay"},
}};

constexpr std::array<Named<PairMethod>, 3> kPairMethods{{
    {PairMethod::kPairingPsk, "pairing_psk"},
    {PairMethod::kDynamicCode, "dynamic_pairing_code"},
    {PairMethod::kStaticCode, "static_pairing_code"},
}};

constexpr std::array<Named<PairMethod>, 3> kPairMethods911{{
    {PairMethod::kPairingPsk, "pairing_psk"},
    {PairMethod::kDynamicCode, "dynamic_pin"},
    {PairMethod::kStaticCode, "static_pin"},
}};

constexpr std::array<Named<SecretLocation>, 3> kLocations{{
    {SecretLocation::kDevice, "device"},
    {SecretLocation::kLeaflet, "leaflet"},
    {SecretLocation::kOperator, "operator"},
}};

constexpr std::array<Named<OutChannel>, 2> kOutChannels{{
    {OutChannel::kDisplay, "display"},
    {OutChannel::kSpeaker, "speaker"},
}};

constexpr std::array<Named<CodeFormat>, 2> kCodeFormats{{
    {CodeFormat::kDigits, "digits"},
    {CodeFormat::kQrCode, "qr_code"},
}};

constexpr std::array<Named<Activity>, 2> kActivities{{
    {Activity::kPlayback, "playback"},
    {Activity::kPairing, "pairing"},
}};

constexpr std::array<Named<PlaybackState>, 2> kPlaybackStates{{
    {PlaybackState::kPlaying, "playing"},
    {PlaybackState::kStopped, "stopped"},
}};

constexpr std::array<Named<GoodbyeReason>, 8> kGoodbyeReasons{{
    {GoodbyeReason::kAnotherServer, "another_server"},
    {GoodbyeReason::kShutdown, "shutdown"},
    {GoodbyeReason::kRestart, "restart"},
    {GoodbyeReason::kUserRequest, "user_request"},
    {GoodbyeReason::kUnauthorized, "unauthorized"},
    {GoodbyeReason::kPairingRequired, "pairing_required"},
    {GoodbyeReason::kConcurrentAttempt, "concurrent_attempt"},
    {GoodbyeReason::kUnpaired, "unpaired"},
}};

[[nodiscard]] const std::array<Named<PlayerCommand>, 3>& player_commands(Dialect dialect) {
    return dialect == Dialect::kSpecification ? kPlayerCommands : kPlayerCommands911;
}

[[nodiscard]] const std::array<Named<PairMethod>, 3>& pair_methods(Dialect dialect) {
    return dialect == Dialect::kSpecification ? kPairMethods : kPairMethods911;
}

// aiosendspin 9.1.1 refuses these above 30,000 ms (C30).
constexpr std::int32_t kMaxTiming911 = 30000;
constexpr std::int32_t kMaxOutputDelay = 5000;
constexpr std::int64_t kMaxInt32 = std::numeric_limits<std::int32_t>::max();

// --- Writing ----------------------------------------------------------------------------

// {"type":"<type>","payload":{...}} with the payload's members written by `body`.
template <class Body>
[[nodiscard]] std::string envelope(std::string_view type, const Body& body) {
    std::string out;
    json::Writer w(out);
    w.begin_object().member("type", type).key("payload").begin_object();
    body(w);
    w.end_object().end_object();
    return out;
}

[[nodiscard]] std::string empty_envelope(std::string_view type) {
    return envelope(type, [](json::Writer&) {});
}

void write_strings(json::Writer& w, std::string_view name, const std::vector<std::string>& values) {
    w.key(name).begin_array();
    for (const std::string& value : values) {
        w.string(value);
    }
    w.end_array();
}

template <class E, std::size_t N>
void write_names(json::Writer& w, std::string_view name, const std::array<Named<E>, N>& table,
                 const std::vector<E>& values) {
    w.key(name).begin_array();
    for (const E value : values) {
        w.string(find_name(table, value));
    }
    w.end_array();
}

void write_format(json::Writer& w, const AudioFormat& format) {
    w.begin_object()
        .member("codec", find_name(kCodecs, format.codec))
        .member("channels", format.channels)
        .member("sample_rate", format.sample_rate)
        .member("bit_depth", format.bit_depth)
        .end_object();
}

// --- Reading ----------------------------------------------------------------------------

[[nodiscard]] std::optional<std::int32_t> read_int32(json::Value value, std::int64_t min,
                                                     std::int64_t max) {
    const std::optional<std::int64_t> number = value.as_int();
    if (!number || *number < min || *number > max) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(*number);
}

// A list of strings, or nothing when `value` is not an array of strings.
[[nodiscard]] std::optional<std::vector<std::string>> read_strings(json::Value value) {
    if (!value.is_array()) {
        return std::nullopt;
    }
    std::vector<std::string> out;
    out.reserve(value.size());
    for (const json::Value element : value.elements()) {
        std::optional<std::string> text = element.as_string();
        if (!text) {
            return std::nullopt;
        }
        out.push_back(std::move(*text));
    }
    return out;
}

// The recognised identifiers of a list of strings, in order and without repeats; nothing
// when `value` is not an array of strings.
template <class E, std::size_t N>
[[nodiscard]] std::optional<std::vector<E>> read_names(json::Value value,
                                                       const std::array<Named<E>, N>& table) {
    if (!value.is_array()) {
        return std::nullopt;
    }
    std::vector<E> out;
    for (const json::Value element : value.elements()) {
        if (!element.is_string()) {
            return std::nullopt;
        }
        const std::optional<E> known = find_value(table, element);
        if (known && std::find(out.begin(), out.end(), *known) == out.end()) {
            out.push_back(*known);
        }
    }
    return out;
}

// An audio format, or nothing when its codec is not recognised. Sets `malformed` when a
// field is missing or out of range.
[[nodiscard]] std::optional<AudioFormat> read_format(json::Value value, bool& malformed) {
    if (!value.is_object()) {
        malformed = true;
        return std::nullopt;
    }
    const json::Value codec_value = value["codec"];
    const std::optional<std::int32_t> channels = read_int32(value["channels"], 1, 255);
    const std::optional<std::int32_t> sample_rate = read_int32(value["sample_rate"], 1, 1000000);
    const std::optional<std::int32_t> bit_depth = read_int32(value["bit_depth"], 0, 64);
    if (!codec_value.is_string() || !channels || !sample_rate || !bit_depth) {
        malformed = true;
        return std::nullopt;
    }
    const std::optional<Codec> codec = find_value(kCodecs, codec_value);
    if (!codec) {
        return std::nullopt;
    }
    return AudioFormat{
        .codec = *codec,
        .channels = *channels,
        .sample_rate = *sample_rate,
        .bit_depth = *bit_depth,
    };
}

[[nodiscard]] std::unexpected<MessageError> malformed() {
    return std::unexpected(MessageError::kMalformed);
}

// --- client/hello's pieces --------------------------------------------------------------

void write_pair_method(json::Writer& w, const PairMethodDescriptor& descriptor, Dialect dialect) {
    const bool dynamic = descriptor.method == PairMethod::kDynamicCode;
    if (dynamic) {
        write_names(w, "out_channels", kOutChannels, descriptor.out_channels);
        if (dialect == Dialect::kSpecification) {
            write_names(w, "formats", kCodeFormats, descriptor.formats);
        } else {
            w.member("min_pin_length", descriptor.min_pin_length);
        }
    } else if (!descriptor.locations.empty()) {
        write_names(w, "locations", kLocations, descriptor.locations);
    }
}

// One descriptor's body; nothing when the reader cannot use it. The specification treats a
// dynamic-code descriptor left with no recognised format or channel as unrecognised.
[[nodiscard]] std::optional<PairMethodDescriptor> read_pair_method(PairMethod method,
                                                                   json::Value body,
                                                                   Dialect dialect) {
    if (!body.is_object()) {
        return std::nullopt;
    }
    PairMethodDescriptor descriptor;
    descriptor.method = method;
    if (method == PairMethod::kDynamicCode) {
        std::optional<std::vector<OutChannel>> channels = read_names(body["out_channels"], kOutChannels);
        if (dialect == Dialect::kSpecification) {
            std::optional<std::vector<CodeFormat>> formats = read_names(body["formats"], kCodeFormats);
            if (!channels || channels->empty() || !formats || formats->empty()) {
                return std::nullopt;
            }
            descriptor.formats = std::move(*formats);
        } else {
            const std::optional<std::int32_t> length = read_int32(body["min_pin_length"], 4, 12);
            if (!length) {
                return std::nullopt;
            }
            descriptor.min_pin_length = *length;
        }
        if (channels) {
            descriptor.out_channels = std::move(*channels);
        }
    } else if (const json::Value locations = body["locations"]; locations.exists()) {
        std::optional<std::vector<SecretLocation>> read = read_names(locations, kLocations);
        if (read) {
            descriptor.locations = std::move(*read);
        }
    }
    return descriptor;
}

[[nodiscard]] bool read_pair_methods(json::Value value, Dialect dialect,
                                     std::vector<PairMethodDescriptor>& out) {
    if (dialect == Dialect::kSpecification) {
        if (!value.is_object()) {
            return false;
        }
        for (const json::Member member : value.members()) {
            const std::optional<std::string_view> key = member.key.raw();
            const std::optional<PairMethod> method =
                key ? find_value(kPairMethods, *key) : std::nullopt;
            if (!method) {
                continue;
            }
            if (std::optional<PairMethodDescriptor> descriptor =
                    read_pair_method(*method, member.value, dialect)) {
                out.push_back(std::move(*descriptor));
            }
        }
    } else {
        if (!value.is_array()) {
            return false;
        }
        for (const json::Value element : value.elements()) {
            const std::optional<PairMethod> method = find_value(kPairMethods911, element["method"]);
            if (!method) {
                continue;
            }
            if (std::optional<PairMethodDescriptor> descriptor =
                    read_pair_method(*method, element, dialect)) {
                out.push_back(std::move(*descriptor));
            }
        }
    }
    // A static-code descriptor beside a dynamic one is disregarded (pairing.md,
    // client/hello pair-method descriptor).
    const bool dynamic = std::find_if(out.begin(), out.end(), [](const PairMethodDescriptor& d) {
                             return d.method == PairMethod::kDynamicCode;
                         }) != out.end();
    if (dynamic) {
        std::erase_if(out, [](const PairMethodDescriptor& d) {
            return d.method == PairMethod::kStaticCode;
        });
    }
    return true;
}

[[nodiscard]] bool read_player_support(json::Value value, Dialect dialect, PlayerSupport& out) {
    if (!value.is_object() || !value["supported_formats"].is_array()) {
        return false;
    }
    for (const json::Value element : value["supported_formats"].elements()) {
        bool bad = false;
        std::optional<AudioFormat> format = read_format(element, bad);
        if (bad) {
            return false;
        }
        if (format) {
            out.supported_formats.push_back(*format);
        }
    }
    const std::optional<std::int64_t> capacity = value["buffer_capacity"].as_int();
    if (!capacity || *capacity < 0) {
        return false;
    }
    out.buffer_capacity = static_cast<std::uint64_t>(*capacity);
    if (dialect == Dialect::kAiosendspin911) {
        std::optional<std::vector<PlayerCommand>> commands =
            read_names(value["supported_commands"], kPlayerCommands911);
        if (!commands) {
            return false;
        }
        std::erase(*commands, PlayerCommand::kSetOutputDelay);
        out.commands = std::move(*commands);
    }
    return true;
}

}  // namespace

// --- Envelope and shared values ---------------------------------------------------------

std::optional<Envelope> read_envelope(const json::Document& document) {
    const json::Value root = document.root();
    const std::optional<std::string_view> type = root["type"].raw();
    if (!root.is_object() || !type || !root["payload"].is_object()) {
        return std::nullopt;
    }
    return Envelope{.type = *type, .payload = root["payload"]};
}

std::string_view codec_name(Codec codec) {
    return find_name(kCodecs, codec);
}

std::optional<Codec> parse_codec(std::string_view name) {
    return find_value(kCodecs, name);
}

// --- server/hello and client/hello ------------------------------------------------------

std::string write_server_hello(const ServerHello& hello) {
    return envelope("server/hello", [&](json::Writer& w) {
        w.member("name", std::string_view{hello.name});
        if (!hello.languages.empty()) {
            write_strings(w, "languages", hello.languages);
        }
    });
}

std::expected<ServerHello, MessageError> read_server_hello(json::Value payload) {
    std::optional<std::string> name = payload["name"].as_string();
    if (!name) {
        return malformed();
    }
    ServerHello hello;
    hello.name = std::move(*name);
    if (const json::Value languages = payload["languages"]; languages.exists()) {
        std::optional<std::vector<std::string>> read = read_strings(languages);
        if (!read) {
            return malformed();
        }
        hello.languages = std::move(*read);
    }
    return hello;
}

std::string write_client_hello(const ClientHello& hello, Dialect dialect) {
    return envelope("client/hello", [&](json::Writer& w) {
        w.member("name", std::string_view{hello.name});
        const DeviceInfo& info = hello.device_info;
        if (!info.product_name.empty() || !info.manufacturer.empty() ||
            !info.software_version.empty() || !info.mac_address.empty()) {
            w.key("device_info").begin_object();
            if (!info.product_name.empty()) {
                w.member("product_name", std::string_view{info.product_name});
            }
            if (!info.manufacturer.empty()) {
                w.member("manufacturer", std::string_view{info.manufacturer});
            }
            if (!info.software_version.empty()) {
                w.member("software_version", std::string_view{info.software_version});
            }
            if (!info.mac_address.empty()) {
                w.member("mac_address", std::string_view{info.mac_address});
            }
            w.end_object();
        }
        write_strings(w, "supported_roles", hello.supported_roles);
        if (hello.player_support) {
            const PlayerSupport& support = *hello.player_support;
            w.key("player@v1_support").begin_object().key("supported_formats").begin_array();
            for (const AudioFormat& format : support.supported_formats) {
                write_format(w, format);
            }
            w.end_array().key("buffer_capacity").unsigned_integer(support.buffer_capacity);
            if (dialect == Dialect::kAiosendspin911) {
                w.key("supported_commands").begin_array();
                for (const PlayerCommand command : support.commands) {
                    if (command != PlayerCommand::kSetOutputDelay) {
                        w.string(find_name(kPlayerCommands911, command));
                    }
                }
                w.end_array();
            }
            w.end_object();
        }
        if (hello.ac3forge_support) {
            w.key(ac3forge::kSupportKey);
            ac3forge::write_support(w, *hello.ac3forge_support);
        }
        if (dialect == Dialect::kSpecification) {
            w.key("supported_pair_methods").begin_object();
            for (const PairMethodDescriptor& descriptor : hello.pair_methods) {
                w.key(find_name(kPairMethods, descriptor.method)).begin_object();
                write_pair_method(w, descriptor, dialect);
                w.end_object();
            }
            w.end_object();
        } else {
            w.member("trust_level", std::string_view{hello.trusts_server ? "user" : "none"});
            w.key("supported_pair_methods").begin_array();
            for (const PairMethodDescriptor& descriptor : hello.pair_methods) {
                w.begin_object().member("method", find_name(kPairMethods911, descriptor.method));
                write_pair_method(w, descriptor, dialect);
                w.end_object();
            }
            w.end_array();
        }
        w.key("unpaired_access").begin_object().member("enabled", hello.unpaired_access).end_object();
    });
}

Dialect client_hello_dialect(json::Value payload) {
    return payload["trust_level"].exists() ? Dialect::kAiosendspin911 : Dialect::kSpecification;
}

std::expected<ClientHello, MessageError> read_client_hello(json::Value payload, Dialect dialect) {
    ClientHello hello;
    std::optional<std::string> name = payload["name"].as_string();
    std::optional<std::vector<std::string>> roles = read_strings(payload["supported_roles"]);
    if (!name || !roles) {
        return malformed();
    }
    hello.name = std::move(*name);
    hello.supported_roles = std::move(*roles);

    if (const json::Value info = payload["device_info"]; info.is_object()) {
        const auto copy = [&](std::string_view key, std::string& out) {
            if (std::optional<std::string> text = info[key].as_string()) {
                out = std::move(*text);
            }
        };
        copy("product_name", hello.device_info.product_name);
        copy("manufacturer", hello.device_info.manufacturer);
        copy("software_version", hello.device_info.software_version);
        copy("mac_address", hello.device_info.mac_address);
    }

    if (const json::Value support = payload["player@v1_support"]; support.exists()) {
        PlayerSupport player;
        if (!read_player_support(support, dialect, player)) {
            return malformed();
        }
        hello.player_support = std::move(player);
    }
    if (const json::Value support = payload[ac3forge::kSupportKey]; support.exists()) {
        hello.ac3forge_support = ac3forge::read_support(support);
    }

    // Required in the specification; aiosendspin 9.1.1 leaves it out when it offers none.
    const json::Value methods = payload["supported_pair_methods"];
    if (methods.exists() || dialect == Dialect::kSpecification) {
        if (!read_pair_methods(methods, dialect, hello.pair_methods)) {
            return malformed();
        }
    }

    const json::Value unpaired = payload["unpaired_access"];
    if (unpaired.exists()) {
        const std::optional<bool> enabled = unpaired["enabled"].as_bool();
        if (!enabled) {
            return malformed();
        }
        hello.unpaired_access = *enabled;
    } else if (dialect == Dialect::kSpecification) {
        return malformed();
    }

    if (dialect == Dialect::kAiosendspin911) {
        hello.trusts_server = payload["trust_level"].equals("user");
    }
    return hello;
}

// --- server/activate --------------------------------------------------------------------

std::string write_activate(const Activate& activate, Dialect dialect) {
    return envelope("server/activate", [&](json::Writer& w) {
        w.key("activities").begin_array();
        for (const Activity activity : activate.activities) {
            if (activity != Activity::kOther) {
                w.string(find_name(kActivities, activity));
            }
        }
        w.end_array();
        if (activate.active_roles) {
            write_strings(w, "active_roles", *activate.active_roles);
        }
        if (activate.pairing && activate.pairing->method) {
            const PairingActivation& pairing = *activate.pairing;
            w.key("pairing").begin_object().member("method",
                                                    find_name(pair_methods(dialect), *pairing.method));
            if (*pairing.method == PairMethod::kDynamicCode) {
                if (dialect == Dialect::kSpecification) {
                    if (pairing.format) {
                        w.member("format", find_name(kCodeFormats, *pairing.format));
                    }
                } else {
                    w.member("pin_length", pairing.pin_length);
                    if (!pairing.languages.empty()) {
                        write_strings(w, "languages", pairing.languages);
                    }
                }
            }
            w.end_object();
        }
    });
}

std::expected<Activate, MessageError> read_activate(json::Value payload, Dialect dialect) {
    const json::Value activities = payload["activities"];
    if (!activities.is_array()) {
        return malformed();
    }
    Activate activate;
    std::vector<std::string_view> seen;
    for (const json::Value element : activities.elements()) {
        const std::optional<std::string_view> text = element.raw();
        if (!element.is_string() || !text ||
            std::find(seen.begin(), seen.end(), *text) != seen.end()) {
            return malformed();
        }
        seen.push_back(*text);
        activate.activities.push_back(find_value(kActivities, element).value_or(Activity::kOther));
    }

    if (const json::Value roles = payload["active_roles"]; roles.exists()) {
        std::optional<std::vector<std::string>> read = read_strings(roles);
        if (!read) {
            return malformed();
        }
        activate.active_roles = std::move(*read);
    }

    const bool pairing = std::find(activate.activities.begin(), activate.activities.end(),
                                   Activity::kPairing) != activate.activities.end();
    if (pairing) {
        const json::Value object = payload["pairing"];
        if (!object.is_object() || !object["method"].is_string()) {
            return malformed();
        }
        PairingActivation read;
        read.method = find_value(pair_methods(dialect), object["method"]);
        if (dialect == Dialect::kSpecification) {
            read.format = find_value(kCodeFormats, object["format"]);
        } else {
            read.pin_length = read_int32(object["pin_length"], 0, 64).value_or(0);
            if (std::optional<std::vector<std::string>> languages = read_strings(object["languages"])) {
                read.languages = std::move(*languages);
            }
        }
        activate.pairing = std::move(read);
    }
    return activate;
}

// --- client/time and server/time --------------------------------------------------------

std::string write_client_time(const ClientTime& time) {
    return envelope("client/time", [&](json::Writer& w) {
        w.member("client_transmitted", time.client_transmitted);
    });
}

std::expected<ClientTime, MessageError> read_client_time(json::Value payload) {
    const std::optional<std::int64_t> transmitted = payload["client_transmitted"].as_int();
    if (!transmitted) {
        return malformed();
    }
    return ClientTime{.client_transmitted = *transmitted};
}

std::string write_server_time(const ServerTime& time) {
    return envelope("server/time", [&](json::Writer& w) {
        w.member("client_transmitted", time.client_transmitted)
            .member("server_received", time.server_received)
            .member("server_transmitted", time.server_transmitted);
    });
}

std::expected<ServerTime, MessageError> read_server_time(json::Value payload) {
    const std::optional<std::int64_t> client = payload["client_transmitted"].as_int();
    const std::optional<std::int64_t> received = payload["server_received"].as_int();
    const std::optional<std::int64_t> transmitted = payload["server_transmitted"].as_int();
    if (!client || !received || !transmitted) {
        return malformed();
    }
    return ServerTime{
        .client_transmitted = *client,
        .server_received = *received,
        .server_transmitted = *transmitted,
    };
}

// --- client/state -----------------------------------------------------------------------

std::string write_client_state(const ClientState& state, Dialect dialect) {
    return envelope("client/state", [&](json::Writer& w) {
        w.member("available", state.available);
        if (state.player) {
            const PlayerState& player = *state.player;
            const bool spec = dialect == Dialect::kSpecification;
            const auto timing = [&](std::int32_t value) {
                return spec ? value : std::min(value, kMaxTiming911);
            };
            w.key("player").begin_object();
            if (player.volume) {
                w.member("volume", *player.volume);
            }
            if (player.muted) {
                w.member("muted", *player.muted);
            }
            w.member(spec ? "output_delay_ms" : "static_delay_ms", player.output_delay_ms.value_or(0))
                .member("required_lead_time_ms", timing(player.required_lead_time_ms.value_or(0)))
                .member("min_buffer_ms", timing(player.min_buffer_ms.value_or(0)));
            w.key("supported_commands").begin_array();
            if (player.supported_commands) {
                for (const PlayerCommand command : *player.supported_commands) {
                    if (spec || command == PlayerCommand::kSetOutputDelay) {
                        w.string(find_name(player_commands(dialect), command));
                    }
                }
            }
            w.end_array();
            if (spec && player.format) {
                w.key("format");
                write_format(w, *player.format);
            }
            w.end_object();
        }
        if (state.ac3forge) {
            w.key(ac3forge::kObjectKey);
            ac3forge::write_state(w, *state.ac3forge);
        }
    });
}

std::expected<ClientState, MessageError> read_client_state(json::Value payload, Dialect dialect) {
    const bool spec = dialect == Dialect::kSpecification;
    ClientState state;
    const std::optional<bool> available = payload["available"].as_bool();
    if (!available && spec) {
        return malformed();
    }
    state.available = available.value_or(false);

    if (const json::Value extension = payload[ac3forge::kObjectKey]; extension.exists()) {
        state.ac3forge = ac3forge::read_state(extension);
        if (!state.ac3forge) {
            return malformed();
        }
    }
    const json::Value object = payload["player"];
    if (!object.exists()) {
        return state;
    }
    if (!object.is_object()) {
        return malformed();
    }
    PlayerState player;
    if (const json::Value volume = object["volume"]; volume.exists()) {
        player.volume = read_int32(volume, 0, 100);
        if (!player.volume) {
            return malformed();
        }
    }
    if (const json::Value muted = object["muted"]; muted.exists()) {
        player.muted = muted.as_bool();
        if (!player.muted) {
            return malformed();
        }
    }
    // Each of the four must be valid where it is present, and in the specification's
    // dialect present.
    const auto optional_int = [&](std::string_view key, std::int64_t max,
                                  std::optional<std::int32_t>& out) {
        const json::Value value = object[key];
        if (!value.exists()) {
            return !spec;
        }
        out = read_int32(value, 0, max);
        return out.has_value();
    };
    if (!optional_int(spec ? "output_delay_ms" : "static_delay_ms", kMaxOutputDelay,
                      player.output_delay_ms) ||
        !optional_int("required_lead_time_ms", kMaxInt32, player.required_lead_time_ms) ||
        !optional_int("min_buffer_ms", kMaxInt32, player.min_buffer_ms)) {
        return malformed();
    }
    if (const json::Value commands = object["supported_commands"]; commands.exists()) {
        player.supported_commands = read_names(commands, player_commands(dialect));
        if (!player.supported_commands) {
            return malformed();
        }
    } else if (spec) {
        return malformed();
    }
    if (const json::Value format = object["format"]; spec && format.exists()) {
        bool bad = false;
        player.format = read_format(format, bad);
        if (bad) {
            return malformed();
        }
    }
    state.player = std::move(player);
    return state;
}

// --- server/command ---------------------------------------------------------------------

std::string write_server_command(const ServerCommand& command, Dialect dialect) {
    return envelope("server/command", [&](json::Writer& w) {
        if (command.player) {
            const PlayerCommandMessage& player = *command.player;
            w.key("player").begin_object().member("command",
                                                   find_name(player_commands(dialect), player.command));
            switch (player.command) {
                case PlayerCommand::kVolume:
                    w.member("volume", player.volume);
                    break;
                case PlayerCommand::kMute:
                    w.member("mute", player.mute);
                    break;
                case PlayerCommand::kSetOutputDelay:
                    w.member(dialect == Dialect::kSpecification ? "output_delay_ms" : "static_delay_ms",
                             player.output_delay_ms);
                    break;
            }
            w.end_object();
        }
        if (command.ac3forge) {
            w.key(ac3forge::kObjectKey);
            ac3forge::write_command(w, *command.ac3forge);
        }
    });
}

std::expected<ServerCommand, MessageError> read_server_command(json::Value payload, Dialect dialect) {
    ServerCommand command;
    if (const json::Value extension = payload[ac3forge::kObjectKey]; extension.exists()) {
        std::expected<ac3forge::CommandMessage, ac3forge::CommandFailure> read = ac3forge::read_command(extension);
        if (read) {
            command.ac3forge = std::move(*read);
        } else if (read.error().error == ac3forge::CommandError::kSettingsRefused) {
            command.ac3forge_refused = std::move(read.error().settings);
        } else {
            return malformed();
        }
    }
    const json::Value object = payload["player"];
    if (!object.exists()) {
        return command;
    }
    const std::optional<PlayerCommand> which = find_value(player_commands(dialect), object["command"]);
    if (!object.is_object() || !which) {
        return malformed();
    }
    PlayerCommandMessage player;
    player.command = *which;
    switch (*which) {
        case PlayerCommand::kVolume: {
            const std::optional<std::int32_t> volume = read_int32(object["volume"], 0, 100);
            if (!volume) {
                return malformed();
            }
            player.volume = *volume;
            break;
        }
        case PlayerCommand::kMute: {
            const std::optional<bool> mute = object["mute"].as_bool();
            if (!mute) {
                return malformed();
            }
            player.mute = *mute;
            break;
        }
        case PlayerCommand::kSetOutputDelay: {
            const std::optional<std::int32_t> delay = read_int32(
                object[dialect == Dialect::kSpecification ? "output_delay_ms" : "static_delay_ms"],
                0, kMaxOutputDelay);
            if (!delay) {
                return malformed();
            }
            player.output_delay_ms = *delay;
            break;
        }
    }
    command.player = player;
    return command;
}

// --- stream/start, stream/clear and stream/end ------------------------------------------

std::string write_stream_start(const StreamStart& start) {
    return envelope("stream/start", [&](json::Writer& w) {
        w.member("server_transmitted", start.server_transmitted);
        if (start.player) {
            const PlayerStream& player = *start.player;
            w.key("player").begin_object()
                .member("codec", codec_name(player.format.codec))
                .member("sample_rate", player.format.sample_rate)
                .member("channels", player.format.channels)
                .member("bit_depth", player.format.bit_depth);
            if (!player.codec_header.empty()) {
                w.member("codec_header", std::string_view{base64::encode(player.codec_header)});
            }
            w.end_object();
        }
        if (start.ac3forge) {
            w.key(ac3forge::kObjectKey);
            ac3forge::write_stream_start(w, *start.ac3forge);
        }
    });
}

std::expected<StreamStart, MessageError> read_stream_start(json::Value payload) {
    const std::optional<std::int64_t> transmitted = payload["server_transmitted"].as_int();
    if (!transmitted) {
        return malformed();
    }
    StreamStart start;
    start.server_transmitted = *transmitted;
    if (const json::Value extension = payload[ac3forge::kObjectKey]; extension.exists()) {
        start.ac3forge = ac3forge::read_stream_start(extension);
        if (!start.ac3forge) {
            return malformed();
        }
    }
    const json::Value object = payload["player"];
    if (!object.exists()) {
        return start;
    }
    bool bad = false;
    const std::optional<AudioFormat> format = read_format(object, bad);
    if (bad || !format) {
        return malformed();
    }
    PlayerStream player;
    player.format = *format;
    if (const json::Value header = object["codec_header"]; header.exists()) {
        const std::optional<std::string> text = header.as_string();
        std::optional<std::vector<std::uint8_t>> bytes =
            text ? base64::decode(*text) : std::nullopt;
        if (!bytes) {
            return malformed();
        }
        player.codec_header = std::move(*bytes);
    }
    start.player = std::move(player);
    return start;
}

std::string write_stream_clear(const StreamClear& clear) {
    return envelope("stream/clear", [&](json::Writer& w) {
        w.member("server_transmitted", clear.server_transmitted);
        if (clear.roles) {
            write_strings(w, "roles", *clear.roles);
        }
    });
}

std::expected<StreamClear, MessageError> read_stream_clear(json::Value payload) {
    const std::optional<std::int64_t> transmitted = payload["server_transmitted"].as_int();
    if (!transmitted) {
        return malformed();
    }
    StreamClear clear;
    clear.server_transmitted = *transmitted;
    if (const json::Value roles = payload["roles"]; roles.exists()) {
        clear.roles = read_strings(roles);
        if (!clear.roles) {
            return malformed();
        }
    }
    return clear;
}

std::string write_stream_end(const StreamEnd& end) {
    return envelope("stream/end", [&](json::Writer& w) {
        if (end.roles) {
            write_strings(w, "roles", *end.roles);
        }
    });
}

std::expected<StreamEnd, MessageError> read_stream_end(json::Value payload) {
    StreamEnd end;
    if (const json::Value roles = payload["roles"]; roles.exists()) {
        end.roles = read_strings(roles);
        if (!end.roles) {
            return malformed();
        }
    }
    return end;
}

// --- group/update -----------------------------------------------------------------------

std::string write_group_update(const GroupUpdate& update) {
    return envelope("group/update", [&](json::Writer& w) {
        if (update.playback_state) {
            w.member("playback_state", find_name(kPlaybackStates, *update.playback_state));
        }
        if (update.group_id) {
            w.member("group_id", std::string_view{*update.group_id});
        }
        if (update.group_name) {
            w.member("group_name", std::string_view{*update.group_name});
        }
    });
}

std::expected<GroupUpdate, MessageError> read_group_update(json::Value payload, Dialect dialect) {
    GroupUpdate update;
    const json::Value state = payload["playback_state"];
    if (state.exists()) {
        update.playback_state = find_value(kPlaybackStates, state);
        if (!update.playback_state) {
            return malformed();
        }
    }
    if (const json::Value id = payload["group_id"]; id.exists()) {
        update.group_id = id.as_string();
        if (!update.group_id) {
            return malformed();
        }
    }
    if (const json::Value name = payload["group_name"]; name.exists()) {
        update.group_name = name.as_string();
        if (!update.group_name) {
            return malformed();
        }
    }
    if (dialect == Dialect::kSpecification &&
        (!update.playback_state || !update.group_id || !update.group_name)) {
        return malformed();
    }
    return update;
}

// --- client/goodbye, client/leave, server/unpair ----------------------------------------

std::string write_client_goodbye(GoodbyeReason reason) {
    return envelope("client/goodbye", [&](json::Writer& w) {
        w.member("reason", find_name(kGoodbyeReasons, reason));
    });
}

std::expected<GoodbyeReason, MessageError> read_client_goodbye(json::Value payload) {
    const std::optional<GoodbyeReason> reason = find_value(kGoodbyeReasons, payload["reason"]);
    if (!reason) {
        return malformed();
    }
    return *reason;
}

std::string write_client_leave() {
    return empty_envelope("client/leave");
}

std::string write_server_unpair() {
    return empty_envelope("server/unpair");
}

}  // namespace ac3::sendspin::messages

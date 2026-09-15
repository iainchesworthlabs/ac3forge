#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/ac3forge_player.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/json.hpp"

// Sendspin's JSON messages after the Noise handshake (messaging.md, Core messages;
// roles/player/v1.md), each a struct with a writer and a reader, in both dialects where
// they differ (planning/hearth-sendspin-extension.md, Music Assistant and aiosendspin
// 9.1.1). The pairing messages have a header of their own, and so do the objects of
// `_ac3forge_player@v1` (ac3forge_player.hpp), which the messages here carry.
//
// Every message is {"type": "<type>", "payload": {...}}. A session parses the text into a
// json::Document, reads the type with read_envelope(), and hands the payload to that type's
// reader. Readers follow the specification's rules for what they do not know: unknown
// members are ignored, and so are the entries of a list whose identifier is not one they
// recognise. A required member that is missing or of the wrong type makes the message
// malformed, which the session treats as the protocol says for that message.
//
// Writers return the whole message text. Strings a peer sent are decoded copies, bounded by
// the message's own size.

namespace ac3::sendspin::messages {

enum class MessageError : std::uint8_t {
    kMalformed,
};

struct Envelope {
    // Points into the Document's text.
    std::string_view type;
    json::Value payload;
};

// The type and payload of a message, or nothing when the text is not an object with a
// string `type` and an object `payload`.
[[nodiscard]] std::optional<Envelope> read_envelope(const json::Document& document);

// --- Shared values --------------------------------------------------------------------

enum class Codec : std::uint8_t {
    kOpus,
    kFlac,
    kPcm,
};

[[nodiscard]] std::string_view codec_name(Codec codec);
[[nodiscard]] std::optional<Codec> parse_codec(std::string_view name);

struct AudioFormat {
    Codec codec = Codec::kPcm;
    std::int32_t channels = 2;
    std::int32_t sample_rate = 48000;
    // Meaningful for PCM and FLAC; the specification ignores it for Opus, and aiosendspin
    // 9.1.1 refuses anything but 16 there (C32).
    std::int32_t bit_depth = 16;

    friend bool operator==(const AudioFormat&, const AudioFormat&) = default;
};

enum class PlayerCommand : std::uint8_t {
    kVolume,
    kMute,
    // `set_output_delay`, which aiosendspin 9.1.1 calls `set_static_delay` (C29).
    kSetOutputDelay,
};

// --- server/hello and client/hello ------------------------------------------------------

struct ServerHello {
    std::string name;
    // BCP 47 tags in the operator's order; empty when not sent.
    std::vector<std::string> languages;
};

// The same in both dialects: a 9.1.1 client ignores `languages` (C9).
[[nodiscard]] std::string write_server_hello(const ServerHello& hello);
[[nodiscard]] std::expected<ServerHello, MessageError> read_server_hello(json::Value payload);

struct DeviceInfo {
    // Empty when not sent.
    std::string product_name;
    std::string manufacturer;
    std::string software_version;
    std::string mac_address;
};

struct PlayerSupport {
    // In the player's order of preference; entries with a codec the reader does not know
    // are left out.
    std::vector<AudioFormat> supported_formats;
    std::uint64_t buffer_capacity = 0;
    // aiosendspin 9.1.1 also lists `volume` and `mute` here, and Music Assistant sends
    // those commands only when they are listed (C28). Written and read in that dialect only.
    std::vector<PlayerCommand> commands;
};

enum class PairMethod : std::uint8_t {
    kPairingPsk,
    kDynamicCode,
    kStaticCode,
};

enum class SecretLocation : std::uint8_t {
    kDevice,
    kLeaflet,
    kOperator,
};

enum class OutChannel : std::uint8_t {
    kDisplay,
    kSpeaker,
};

enum class CodeFormat : std::uint8_t {
    kDigits,
    kQrCode,
};

struct PairMethodDescriptor {
    PairMethod method = PairMethod::kPairingPsk;
    // Pairing PSK and static code.
    std::vector<SecretLocation> locations;
    // Dynamic code.
    std::vector<OutChannel> out_channels;
    // Dynamic code, specification only.
    std::vector<CodeFormat> formats;
    // Dynamic code, aiosendspin 9.1.1 only (C20); 0 when not sent.
    std::int32_t min_pin_length = 0;
};

struct ClientHello {
    std::string name;
    DeviceInfo device_info;
    std::vector<std::string> supported_roles;
    std::optional<PlayerSupport> player_support;
    // Read as nothing when the object is absent or one ac3forge::read_support refuses: the
    // message stands, and a server does not activate the role.
    std::optional<ac3forge::Support> ac3forge_support;
    std::vector<PairMethodDescriptor> pair_methods;
    bool unpaired_access = false;
    // aiosendspin 9.1.1's `trust_level`: "user" on a long-term PSK connection, "none"
    // otherwise. Written and read in that dialect only.
    bool trusts_server = false;
};

// The specification writes `supported_pair_methods` as an object keyed by method;
// aiosendspin 9.1.1 as an array, and adds `trust_level` (C8).
[[nodiscard]] std::string write_client_hello(const ClientHello& hello, Dialect dialect);

// The dialect a client/hello payload is written in: aiosendspin 9.1.1 when it carries
// `trust_level`, which only 9.1.1 sends and a specification client does not.
[[nodiscard]] Dialect client_hello_dialect(json::Value payload);

// A reader following the specification's own rules for supported_pair_methods: an
// unrecognised method is ignored, a dynamic-code descriptor left with no recognised format
// or channel counts as unrecognised, and a static-code descriptor beside a dynamic one is
// dropped.
[[nodiscard]] std::expected<ClientHello, MessageError> read_client_hello(json::Value payload,
                                                                        Dialect dialect);

// --- server/activate --------------------------------------------------------------------

enum class Activity : std::uint8_t {
    kPlayback,
    kPairing,
    // Any other value, such as aiosendspin 9.1.1's `management` (C10). No allowed set holds
    // one, so a client answers it as an activation it does not admit.
    kOther,
};

struct PairingActivation {
    // Nothing when the method is not one the reader recognises, which a client answers
    // with pair/abort reason method_not_supported.
    std::optional<PairMethod> method;
    // Dynamic code, specification: the emission format; nothing when absent or not
    // recognised.
    std::optional<CodeFormat> format;
    // Dynamic code, aiosendspin 9.1.1: the code's length and the operator's languages
    // (C9, C20).
    std::int32_t pin_length = 0;
    std::vector<std::string> languages;
};

struct Activate {
    std::vector<Activity> activities;
    // Absent: the roles last declared persist.
    std::optional<std::vector<std::string>> active_roles;
    // Present exactly when `activities` holds kPairing.
    std::optional<PairingActivation> pairing;
};

// kOther activities and an unrecognised method are not written.
[[nodiscard]] std::string write_activate(const Activate& activate, Dialect dialect);
// Malformed when an activity is repeated or `pairing` is missing while `activities` holds
// kPairing. A `pairing` object without kPairing is ignored, as the specification says.
[[nodiscard]] std::expected<Activate, MessageError> read_activate(json::Value payload,
                                                                 Dialect dialect);

// --- client/time and server/time --------------------------------------------------------

struct ClientTime {
    std::int64_t client_transmitted = 0;
};

struct ServerTime {
    std::int64_t client_transmitted = 0;
    std::int64_t server_received = 0;
    std::int64_t server_transmitted = 0;
};

[[nodiscard]] std::string write_client_time(const ClientTime& time);
[[nodiscard]] std::expected<ClientTime, MessageError> read_client_time(json::Value payload);
[[nodiscard]] std::string write_server_time(const ServerTime& time);
[[nodiscard]] std::expected<ServerTime, MessageError> read_server_time(json::Value payload);

// --- client/state -----------------------------------------------------------------------

// The specification requires the four timing and command fields in every player object.
// aiosendspin 9.1.1 requires them only in the first and leaves out the ones that have not
// changed after it, so a reader in that dialect reports which were sent, and a writer writes
// a missing one as 0 or an empty list.
struct PlayerState {
    std::optional<std::int32_t> volume;
    std::optional<bool> muted;
    // aiosendspin 9.1.1's `static_delay_ms` (C29).
    std::optional<std::int32_t> output_delay_ms;
    std::optional<std::int32_t> required_lead_time_ms;
    std::optional<std::int32_t> min_buffer_ms;
    // In aiosendspin 9.1.1 only kSetOutputDelay can be listed here (C29); volume and mute
    // travel in the hello's support object instead.
    std::optional<std::vector<PlayerCommand>> supported_commands;
    // Specification only (C32).
    std::optional<AudioFormat> format;
};

struct ClientState {
    bool available = false;
    std::optional<PlayerState> player;
    std::optional<ac3forge::State> ac3forge;
};

// In aiosendspin 9.1.1's dialect, kVolume and kMute are left out of the player's
// `supported_commands`, and the timing fields are capped at 30,000 ms (C29, C30).
[[nodiscard]] std::string write_client_state(const ClientState& state, Dialect dialect);
// `available` is required in the specification and optional in aiosendspin 9.1.1, where a
// missing one reads as false.
[[nodiscard]] std::expected<ClientState, MessageError> read_client_state(json::Value payload,
                                                                        Dialect dialect);

// --- server/command ---------------------------------------------------------------------

struct PlayerCommandMessage {
    PlayerCommand command = PlayerCommand::kVolume;
    std::int32_t volume = 0;
    bool mute = false;
    std::int32_t output_delay_ms = 0;
};

struct ServerCommand {
    std::optional<PlayerCommandMessage> player;
    std::optional<ac3forge::CommandMessage> ac3forge;
    // Read only: a settings command ac3forge::read_command refused, with the revision it named,
    // for the sink's settings_error. The message is not malformed.
    std::optional<ac3forge::SettingsError> ac3forge_refused;
};

[[nodiscard]] std::string write_server_command(const ServerCommand& command, Dialect dialect);
// A player command whose value is missing, or out of its range, is malformed, and so is an
// `_ac3forge_player` command ac3forge::read_command finds malformed.
[[nodiscard]] std::expected<ServerCommand, MessageError> read_server_command(json::Value payload,
                                                                            Dialect dialect);

// --- stream/start, stream/clear and stream/end ------------------------------------------

struct PlayerStream {
    AudioFormat format;
    // Decoded from standard Base64; empty when absent.
    std::vector<std::uint8_t> codec_header;
};

struct StreamStart {
    std::int64_t server_transmitted = 0;
    std::optional<PlayerStream> player;
    std::optional<ac3forge::StreamStart> ac3forge;
};

// `roles` absent means every role the message covers.
struct StreamClear {
    std::int64_t server_transmitted = 0;
    std::optional<std::vector<std::string>> roles;
};

struct StreamEnd {
    std::optional<std::vector<std::string>> roles;
};

[[nodiscard]] std::string write_stream_start(const StreamStart& start);
[[nodiscard]] std::expected<StreamStart, MessageError> read_stream_start(json::Value payload);
[[nodiscard]] std::string write_stream_clear(const StreamClear& clear);
[[nodiscard]] std::expected<StreamClear, MessageError> read_stream_clear(json::Value payload);
[[nodiscard]] std::string write_stream_end(const StreamEnd& end);
[[nodiscard]] std::expected<StreamEnd, MessageError> read_stream_end(json::Value payload);

// --- group/update -----------------------------------------------------------------------

enum class PlaybackState : std::uint8_t {
    kPlaying,
    kStopped,
};

// The specification sends every field; aiosendspin 9.1.1 leaves out any it has not set,
// and a missing field keeps its earlier value (C15).
struct GroupUpdate {
    std::optional<PlaybackState> playback_state;
    std::optional<std::string> group_id;
    std::optional<std::string> group_name;
};

[[nodiscard]] std::string write_group_update(const GroupUpdate& update);
// In the specification's dialect every field is required; in 9.1.1's any may be missing.
// A playback_state other than playing or stopped (9.1.1 defines `paused`, which it does not
// send) is malformed.
[[nodiscard]] std::expected<GroupUpdate, MessageError> read_group_update(json::Value payload,
                                                                        Dialect dialect);

// --- client/goodbye, client/leave, server/unpair ----------------------------------------

enum class GoodbyeReason : std::uint8_t {
    kAnotherServer,
    kShutdown,
    kRestart,
    kUserRequest,
    kUnauthorized,
    kPairingRequired,
    kConcurrentAttempt,
    kUnpaired,
};

[[nodiscard]] std::string write_client_goodbye(GoodbyeReason reason);
[[nodiscard]] std::expected<GoodbyeReason, MessageError> read_client_goodbye(json::Value payload);

// No payload fields. aiosendspin 9.1.1 has no client/leave, and a player sends
// client/state with available false to it instead (C12).
[[nodiscard]] std::string write_client_leave();
[[nodiscard]] std::string write_server_unpair();

}  // namespace ac3::sendspin::messages

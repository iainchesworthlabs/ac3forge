#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/chunks.hpp"
#include "ac3/sendspin/json.hpp"

// _ac3forge_player@v1 (planning/hearth-sendspin-extension.md, The role _ac3forge_player@v1): the
// objects the role adds to client/hello, client/state, stream/start and server/command, each a
// struct with a writer that appends it as the value of a key the caller has written, and a reader.
// Its burst chunk, ID 192 (message_id::kAc3forgeBurst), is chunks.hpp's; the messages that carry
// these objects are messages.hpp's.
//
// Readers ignore unknown fields, as the specification says for every message (E14). A settings
// object is refused whole when a key it knows has a value out of range, and the reader then says
// which revision it refused and why, for the sink's settings_error.
//
// Nothing here depends on the codec library: the enumerations name what the page's text names, and
// a sink maps them onto ac3::forge and ac3::render.

namespace ac3::sendspin::ac3forge {

inline constexpr std::string_view kRole = "_ac3forge_player@v1";
inline constexpr std::string_view kSupportKey = "_ac3forge_player@v1_support";
// The key of the role's object in client/state, server/command and stream/start, and its name in
// the roles of stream/clear and stream/end.
inline constexpr std::string_view kObjectKey = "_ac3forge_player";

enum class DataType : std::uint8_t {
    kAc3,
    kEac3,
};

[[nodiscard]] std::string_view data_type_name(DataType type);
[[nodiscard]] BurstDataType burst_data_type(DataType type);

// --- The support object in client/hello --------------------------------------------------------

struct Outputs {
    // At the current setting.
    std::int32_t count = 0;
    std::int32_t bit_depth = 0;
    // The widths the sink's own page can set.
    std::vector<std::int32_t> bit_depths;
};

struct Management {
    bool routing = false;
    std::array<double, 2> trim_db{0.0, 0.0};
    double max_delay_ms = 0.0;
    std::array<double, 2> crossover_hz{0.0, 0.0};
    bool identify = false;
};

// The decoder keys of a settings command, as a sink lists them in decoder_settings.
inline constexpr std::array<std::string_view, 11> kDecoderSettingNames{
    "mode",
    "drc_cut",
    "drc_boost",
    "heavy_compression",
    "dialnorm",
    "downmix",
    "ltrt_phase_shift",
    "mix_lfe",
    "programme",
    "objects",
    "concealment",
};

struct Support {
    std::vector<DataType> data_types;
    std::vector<std::int32_t> sample_rates;
    Outputs outputs;
    std::int32_t layout_grammar = 1;
    Management management;
    std::vector<std::string> decoder_settings;
    std::uint64_t buffer_capacity = 0;
};

void write_support(json::Writer& w, const Support& support);
// Nothing when a required field is missing or of the wrong type, or data_types or sample_rates
// hold nothing this reader recognises: a server then does not activate the role.
[[nodiscard]] std::optional<Support> read_support(json::Value value);

// --- The state object in client/state ----------------------------------------------------------

enum class Command : std::uint8_t {
    kVolume,
    kMute,
    kSetOutputDelay,
    kSettings,
    kIdentify,
};

struct DecoderReport {
    DataType data_type = DataType::kEac3;
    // The coded audio coding mode, 0 to 7 (A/52, Table 5.8).
    std::int32_t acmod = 0;
    bool lfe = false;
    std::int32_t substreams = 0;
    // Objects the stream carries, and whether the sink placed them.
    std::int32_t objects = 0;
    bool objects_placed = false;
    double dialnorm = 0.0;

    friend bool operator==(const DecoderReport&, const DecoderReport&) = default;
};

// Levels below this, silence included, are written as it: JSON has no minus infinity.
inline constexpr double kSilenceDb = -120.0;

struct Level {
    std::int32_t output = 0;
    double peak_db = 0.0;
    double rms_db = 0.0;
};

struct Counters {
    std::uint64_t bursts_played = 0;
    std::uint64_t underruns = 0;
    std::uint64_t late_chunks = 0;
    std::uint64_t dropped_chunks = 0;
    std::uint64_t invalid_chunks = 0;
};

struct SettingsError {
    std::int64_t revision = 0;
    std::string why;
};

struct State {
    std::optional<std::int32_t> volume;
    std::optional<bool> muted;
    std::int32_t output_delay_ms = 0;
    std::int32_t required_lead_time_ms = 0;
    std::int32_t min_buffer_ms = 0;
    std::vector<Command> supported_commands;
    std::int64_t settings_revision = 0;
    std::optional<SettingsError> settings_error;
    std::optional<DecoderReport> decoder;
    std::optional<std::vector<Level>> levels;
    Counters counters;
    std::optional<std::string> why;
};

void write_state(json::Writer& w, const State& state);
[[nodiscard]] std::optional<State> read_state(json::Value value);

// --- The object in stream/start ----------------------------------------------------------------

struct StreamStart {
    DataType data_type = DataType::kEac3;
    std::int32_t sample_rate = 48000;
};

void write_stream_start(json::Writer& w, const StreamStart& start);
[[nodiscard]] std::optional<StreamStart> read_stream_start(json::Value value);

// --- The object in server/command --------------------------------------------------------------

enum class DecoderMode : std::uint8_t { kLine, kRf, kCustom };
enum class Downmix : std::uint8_t { kLoRo, kLtRt };
enum class ObjectsPolicy : std::uint8_t { kAuto, kAlways, kNever };
enum class Concealment : std::uint8_t { kNone, kRepeatFade, kMute };

// The decoder keys of a settings command; each absent key leaves the library's default.
struct DecoderSettings {
    std::optional<DecoderMode> mode;
    std::optional<double> drc_cut;
    std::optional<double> drc_boost;
    std::optional<bool> heavy_compression;
    std::optional<bool> dialnorm;
    std::optional<Downmix> downmix;
    std::optional<bool> ltrt_phase_shift;
    std::optional<bool> mix_lfe;
    // Absent, null (the stream's own choice), or a programme number.
    bool has_programme = false;
    std::optional<std::int32_t> programme;
    std::optional<ObjectsPolicy> objects;
    std::optional<Concealment> concealment;
};

// A settings command's object, which replaces the sink's settings whole.
struct Settings {
    std::int64_t revision = 0;
    std::optional<std::string> layout;
    std::optional<std::string> routing;
    std::optional<std::vector<double>> trim_db;
    std::optional<std::vector<double>> delay_ms;
    std::optional<double> crossover_hz;
    DecoderSettings decoder;
};

struct Identify {
    std::int32_t output = 0;
    double level_db = -30.0;
};

struct CommandMessage {
    Command command = Command::kVolume;
    std::int32_t volume = 0;
    bool mute = false;
    std::int32_t output_delay_ms = 0;
    Settings settings;
    // kIdentify: the tone to play, or nothing to stop it.
    std::optional<Identify> identify;
};

void write_command(json::Writer& w, const CommandMessage& command);

enum class CommandError : std::uint8_t {
    // The command, or a value it needs, is missing or of the wrong type.
    kMalformed,
    // A settings object with a known key out of range: settings_error names the revision.
    kSettingsRefused,
};

struct CommandFailure {
    CommandError error = CommandError::kMalformed;
    SettingsError settings;
};

[[nodiscard]] std::expected<CommandMessage, CommandFailure> read_command(json::Value value);

// What read_command cannot check without the sink's own support object: one trim and one delay
// per output, each inside management's range; a crossover inside its range; routing only when
// management offers it; and decoder keys only from decoder_settings. The reason for
// settings_error, or nothing when the settings fit. The layout and routing text are the sink's
// to parse, with ac3::render.
[[nodiscard]] std::optional<std::string> check_settings(const Settings& settings,
                                                        const Support& support);

}  // namespace ac3::sendspin::ac3forge

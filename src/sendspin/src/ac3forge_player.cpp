#include "ac3/sendspin/ac3forge_player.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/sendspin/chunks.hpp"
#include "ac3/sendspin/json.hpp"

namespace ac3::sendspin::ac3forge {

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

constexpr std::array<Named<DataType>, 2> kDataTypes{{
    {DataType::kAc3, "ac3"},
    {DataType::kEac3, "eac3"},
}};

constexpr std::array<Named<Command>, 5> kCommands{{
    {Command::kVolume, "volume"},
    {Command::kMute, "mute"},
    {Command::kSetOutputDelay, "set_output_delay"},
    {Command::kSettings, "settings"},
    {Command::kIdentify, "identify"},
}};

constexpr std::array<Named<DecoderMode>, 3> kModes{{
    {DecoderMode::kLine, "line"},
    {DecoderMode::kRf, "rf"},
    {DecoderMode::kCustom, "custom"},
}};

constexpr std::array<Named<Downmix>, 2> kDownmixes{{
    {Downmix::kLoRo, "loro"},
    {Downmix::kLtRt, "ltrt"},
}};

constexpr std::array<Named<ObjectsPolicy>, 3> kObjectsPolicies{{
    {ObjectsPolicy::kAuto, "auto"},
    {ObjectsPolicy::kAlways, "always"},
    {ObjectsPolicy::kNever, "never"},
}};

constexpr std::array<Named<Concealment>, 3> kConcealments{{
    {Concealment::kNone, "none"},
    {Concealment::kRepeatFade, "repeat_fade"},
    {Concealment::kMute, "mute"},
}};

constexpr std::int32_t kMaxOutputDelay = 5000;
constexpr std::int64_t kMaxSampleRate = 768000;
constexpr std::int64_t kMaxOutputs = 1024;
constexpr std::int64_t kMaxInt32 = 2'147'483'647;
constexpr double kIdentifyMinimumDb = -60.0;
constexpr double kIdentifyMaximumDb = -12.0;

[[nodiscard]] double level_on_wire(double db) {
    return std::isfinite(db) ? std::max(db, kSilenceDb) : kSilenceDb;
}

[[nodiscard]] std::optional<std::int32_t> read_int32(json::Value value, std::int64_t minimum, std::int64_t maximum) {
    const std::optional<std::int64_t> number = value.as_int();
    if (!number || *number < minimum || *number > maximum) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(*number);
}

[[nodiscard]] std::optional<std::uint64_t> read_uint64(json::Value value) {
    const std::optional<std::int64_t> number = value.as_int();
    if (!number || *number < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(*number);
}

// The largest magnitude a number in these objects reads at: past it json::Writer::number() cannot
// write the value back, and nothing the role describes comes near it.
constexpr double kMaxNumber = 1e9;

[[nodiscard]] std::optional<double> read_number(json::Value value) {
    const std::optional<double> number = value.as_double();
    if (!number || !std::isfinite(*number) || std::fabs(*number) > kMaxNumber) {
        return std::nullopt;
    }
    return number;
}

// [minimum, maximum], with minimum no more than maximum.
[[nodiscard]] std::optional<std::array<double, 2>> read_range(json::Value value) {
    if (!value.is_array() || value.size() != 2) {
        return std::nullopt;
    }
    const std::optional<double> low = read_number(value.at(0));
    const std::optional<double> high = read_number(value.at(1));
    if (!low || !high || *low > *high) {
        return std::nullopt;
    }
    return std::array<double, 2>{*low, *high};
}

[[nodiscard]] std::optional<std::vector<double>> read_numbers(json::Value value) {
    if (!value.is_array()) {
        return std::nullopt;
    }
    std::vector<double> numbers;
    for (const json::Value element : value.elements()) {
        const std::optional<double> number = read_number(element);
        if (!number) {
            return std::nullopt;
        }
        numbers.push_back(*number);
    }
    return numbers;
}

void write_numbers(json::Writer& w, std::string_view key, const std::vector<double>& numbers) {
    w.key(key).begin_array();
    for (const double number : numbers) {
        w.number(number);
    }
    w.end_array();
}

[[nodiscard]] std::optional<std::vector<Command>> read_commands(json::Value value) {
    if (!value.is_array()) {
        return std::nullopt;
    }
    std::vector<Command> commands;
    for (const json::Value element : value.elements()) {
        if (!element.is_string()) {
            return std::nullopt;
        }
        // A command this reader does not know is left out, as for player@v1.
        if (const std::optional<Command> command = value_of(kCommands, element)) {
            commands.push_back(*command);
        }
    }
    return commands;
}

}  // namespace

std::string_view data_type_name(DataType type) {
    return name_of(kDataTypes, type);
}

BurstDataType burst_data_type(DataType type) {
    return type == DataType::kAc3 ? BurstDataType::kAc3 : BurstDataType::kEac3;
}

// --- Support -----------------------------------------------------------------------------------

void write_support(json::Writer& w, const Support& support) {
    w.begin_object().key("data_types").begin_array();
    for (const DataType type : support.data_types) {
        w.string(data_type_name(type));
    }
    w.end_array().key("sample_rates").begin_array();
    for (const std::int32_t rate : support.sample_rates) {
        w.integer(rate);
    }
    w.end_array();
    w.key("outputs").begin_object().member("count", support.outputs.count).member("bit_depth", support.outputs.bit_depth);
    w.key("bit_depths").begin_array();
    for (const std::int32_t depth : support.outputs.bit_depths) {
        w.integer(depth);
    }
    w.end_array().end_object();
    w.member("layout_grammar", support.layout_grammar);
    const Management& management = support.management;
    w.key("management").begin_object().member("routing", management.routing);
    w.key("trim_db").begin_array().number(management.trim_db[0]).number(management.trim_db[1]).end_array();
    w.key("delay_ms").begin_array().number(0.0).number(management.max_delay_ms).end_array();
    w.key("crossover_hz").begin_array().number(management.crossover_hz[0]).number(management.crossover_hz[1]).end_array();
    w.member("identify", management.identify).end_object();
    w.key("decoder_settings").begin_array();
    for (const std::string& name : support.decoder_settings) {
        w.string(name);
    }
    w.end_array();
    w.key("buffer_capacity").unsigned_integer(support.buffer_capacity);
    w.end_object();
}

std::optional<Support> read_support(json::Value value) {
    if (!value.is_object()) {
        return std::nullopt;
    }
    Support support;
    const json::Value types = value["data_types"];
    const json::Value rates = value["sample_rates"];
    if (!types.is_array() || !rates.is_array()) {
        return std::nullopt;
    }
    for (const json::Value element : types.elements()) {
        if (!element.is_string()) {
            return std::nullopt;
        }
        if (const std::optional<DataType> type = value_of(kDataTypes, element)) {
            support.data_types.push_back(*type);
        }
    }
    for (const json::Value element : rates.elements()) {
        const std::optional<std::int32_t> rate = read_int32(element, 1, kMaxSampleRate);
        if (!rate) {
            return std::nullopt;
        }
        support.sample_rates.push_back(*rate);
    }
    if (support.data_types.empty() || support.sample_rates.empty()) {
        return std::nullopt;
    }

    const json::Value outputs = value["outputs"];
    const std::optional<std::int32_t> count = read_int32(outputs["count"], 0, kMaxOutputs);
    const std::optional<std::int32_t> depth = read_int32(outputs["bit_depth"], 0, 64);
    const json::Value depths = outputs["bit_depths"];
    if (!count || !depth || !depths.is_array()) {
        return std::nullopt;
    }
    support.outputs.count = *count;
    support.outputs.bit_depth = *depth;
    for (const json::Value element : depths.elements()) {
        const std::optional<std::int32_t> width = read_int32(element, 1, 64);
        if (!width) {
            return std::nullopt;
        }
        support.outputs.bit_depths.push_back(*width);
    }

    const std::optional<std::int32_t> grammar = read_int32(value["layout_grammar"], 1, 1'000'000);
    if (!grammar) {
        return std::nullopt;
    }
    support.layout_grammar = *grammar;

    const json::Value management = value["management"];
    const std::optional<bool> routing = management["routing"].as_bool();
    const std::optional<std::array<double, 2>> trim = read_range(management["trim_db"]);
    const std::optional<std::array<double, 2>> delay = read_range(management["delay_ms"]);
    const std::optional<std::array<double, 2>> crossover = read_range(management["crossover_hz"]);
    const std::optional<bool> identify = management["identify"].as_bool();
    if (!routing || !trim || !delay || !crossover || !identify) {
        return std::nullopt;
    }
    support.management = {.routing = *routing,
                          .trim_db = *trim,
                          .max_delay_ms = (*delay)[1],
                          .crossover_hz = *crossover,
                          .identify = *identify};

    const json::Value settings = value["decoder_settings"];
    const std::optional<std::uint64_t> capacity = read_uint64(value["buffer_capacity"]);
    if (!settings.is_array() || !capacity) {
        return std::nullopt;
    }
    for (const json::Value element : settings.elements()) {
        std::optional<std::string> name = element.as_string();
        if (!name) {
            return std::nullopt;
        }
        support.decoder_settings.push_back(std::move(*name));
    }
    support.buffer_capacity = *capacity;
    return support;
}

// --- State -------------------------------------------------------------------------------------

void write_state(json::Writer& w, const State& state) {
    w.begin_object();
    if (state.volume) {
        w.member("volume", *state.volume);
    }
    if (state.muted) {
        w.member("muted", *state.muted);
    }
    w.member("output_delay_ms", state.output_delay_ms)
        .member("required_lead_time_ms", state.required_lead_time_ms)
        .member("min_buffer_ms", state.min_buffer_ms);
    w.key("supported_commands").begin_array();
    for (const Command command : state.supported_commands) {
        w.string(name_of(kCommands, command));
    }
    w.end_array();
    w.member("settings_revision", state.settings_revision);
    if (state.settings_error) {
        w.key("settings_error")
            .begin_object()
            .member("revision", state.settings_error->revision)
            .member("why", std::string_view{state.settings_error->why})
            .end_object();
    }
    if (state.decoder) {
        const DecoderReport& decoder = *state.decoder;
        w.key("decoder")
            .begin_object()
            .member("data_type", data_type_name(decoder.data_type))
            .member("acmod", decoder.acmod)
            .member("lfe", decoder.lfe)
            .member("substreams", decoder.substreams)
            .member("objects", decoder.objects)
            .member("objects_placed", decoder.objects_placed);
        w.key("dialnorm").number(decoder.dialnorm, 1);
        w.end_object();
    }
    if (state.levels) {
        w.key("levels").begin_array();
        for (const Level& level : *state.levels) {
            w.begin_object().member("output", level.output);
            w.key("peak_db").number(level_on_wire(level.peak_db), 1);
            w.key("rms_db").number(level_on_wire(level.rms_db), 1).end_object();
        }
        w.end_array();
    }
    const Counters& counters = state.counters;
    w.key("counters").begin_object();
    w.key("bursts_played").unsigned_integer(counters.bursts_played);
    w.key("underruns").unsigned_integer(counters.underruns);
    w.key("late_chunks").unsigned_integer(counters.late_chunks);
    w.key("dropped_chunks").unsigned_integer(counters.dropped_chunks);
    w.key("invalid_chunks").unsigned_integer(counters.invalid_chunks);
    w.end_object();
    if (state.why) {
        w.member("why", std::string_view{*state.why});
    }
    w.end_object();
}

std::optional<State> read_state(json::Value value) {
    if (!value.is_object()) {
        return std::nullopt;
    }
    State state;
    if (const json::Value volume = value["volume"]; volume.exists()) {
        state.volume = read_int32(volume, 0, 100);
        if (!state.volume) {
            return std::nullopt;
        }
    }
    if (const json::Value muted = value["muted"]; muted.exists()) {
        state.muted = muted.as_bool();
        if (!state.muted) {
            return std::nullopt;
        }
    }
    const std::optional<std::int32_t> delay = read_int32(value["output_delay_ms"], 0, kMaxOutputDelay);
    const std::optional<std::int32_t> lead = read_int32(value["required_lead_time_ms"], 0, kMaxInt32);
    const std::optional<std::int32_t> buffer = read_int32(value["min_buffer_ms"], 0, kMaxInt32);
    std::optional<std::vector<Command>> commands = read_commands(value["supported_commands"]);
    const std::optional<std::int64_t> revision = value["settings_revision"].as_int();
    if (!delay || !lead || !buffer || !commands || !revision) {
        return std::nullopt;
    }
    state.output_delay_ms = *delay;
    state.required_lead_time_ms = *lead;
    state.min_buffer_ms = *buffer;
    state.supported_commands = std::move(*commands);
    state.settings_revision = *revision;

    if (const json::Value error = value["settings_error"]; error.exists()) {
        const std::optional<std::int64_t> refused = error["revision"].as_int();
        std::optional<std::string> why = error["why"].as_string();
        if (!refused || !why) {
            return std::nullopt;
        }
        state.settings_error = SettingsError{.revision = *refused, .why = std::move(*why)};
    }
    if (const json::Value decoder = value["decoder"]; decoder.exists()) {
        const std::optional<DataType> type = value_of(kDataTypes, decoder["data_type"]);
        const std::optional<std::int32_t> acmod = read_int32(decoder["acmod"], 0, 7);
        const std::optional<bool> lfe = decoder["lfe"].as_bool();
        const std::optional<std::int32_t> substreams = read_int32(decoder["substreams"], 0, 64);
        const std::optional<std::int32_t> objects = read_int32(decoder["objects"], 0, kMaxInt32);
        const std::optional<bool> placed = decoder["objects_placed"].as_bool();
        const std::optional<double> dialnorm = read_number(decoder["dialnorm"]);
        if (!type || !acmod || !lfe || !substreams || !objects || !placed || !dialnorm) {
            return std::nullopt;
        }
        state.decoder = DecoderReport{.data_type = *type,
                                      .acmod = *acmod,
                                      .lfe = *lfe,
                                      .substreams = *substreams,
                                      .objects = *objects,
                                      .objects_placed = *placed,
                                      .dialnorm = *dialnorm};
    }
    if (const json::Value levels = value["levels"]; levels.exists()) {
        if (!levels.is_array()) {
            return std::nullopt;
        }
        std::vector<Level> read;
        for (const json::Value element : levels.elements()) {
            const std::optional<std::int32_t> output = read_int32(element["output"], 0, kMaxOutputs - 1);
            const std::optional<double> peak = read_number(element["peak_db"]);
            const std::optional<double> rms = read_number(element["rms_db"]);
            if (!output || !peak || !rms) {
                return std::nullopt;
            }
            read.push_back({.output = *output, .peak_db = *peak, .rms_db = *rms});
        }
        state.levels = std::move(read);
    }
    const json::Value counters = value["counters"];
    const std::optional<std::uint64_t> played = read_uint64(counters["bursts_played"]);
    const std::optional<std::uint64_t> underruns = read_uint64(counters["underruns"]);
    const std::optional<std::uint64_t> late = read_uint64(counters["late_chunks"]);
    const std::optional<std::uint64_t> dropped = read_uint64(counters["dropped_chunks"]);
    const std::optional<std::uint64_t> invalid = read_uint64(counters["invalid_chunks"]);
    if (!played || !underruns || !late || !dropped || !invalid) {
        return std::nullopt;
    }
    state.counters = {.bursts_played = *played,
                      .underruns = *underruns,
                      .late_chunks = *late,
                      .dropped_chunks = *dropped,
                      .invalid_chunks = *invalid};
    if (const json::Value why = value["why"]; why.exists()) {
        state.why = why.as_string();
        if (!state.why) {
            return std::nullopt;
        }
    }
    return state;
}

// --- Stream start ------------------------------------------------------------------------------

void write_stream_start(json::Writer& w, const StreamStart& start) {
    w.begin_object().member("data_type", data_type_name(start.data_type)).member("sample_rate", start.sample_rate).end_object();
}

std::optional<StreamStart> read_stream_start(json::Value value) {
    const std::optional<DataType> type = value_of(kDataTypes, value["data_type"]);
    const std::optional<std::int32_t> rate = read_int32(value["sample_rate"], 1, kMaxSampleRate);
    if (!value.is_object() || !type || !rate) {
        return std::nullopt;
    }
    return StreamStart{.data_type = *type, .sample_rate = *rate};
}

// --- Commands ----------------------------------------------------------------------------------

void write_command(json::Writer& w, const CommandMessage& command) {
    w.begin_object().member("command", name_of(kCommands, command.command));
    switch (command.command) {
        case Command::kVolume:
            w.member("volume", command.volume);
            break;
        case Command::kMute:
            w.member("mute", command.mute);
            break;
        case Command::kSetOutputDelay:
            w.member("output_delay_ms", command.output_delay_ms);
            break;
        case Command::kSettings: {
            const Settings& settings = command.settings;
            w.key("settings").begin_object().member("revision", settings.revision);
            if (settings.layout) {
                w.member("layout", std::string_view{*settings.layout});
            }
            if (settings.routing) {
                w.member("routing", std::string_view{*settings.routing});
            }
            if (settings.trim_db) {
                write_numbers(w, "trim_db", *settings.trim_db);
            }
            if (settings.delay_ms) {
                write_numbers(w, "delay_ms", *settings.delay_ms);
            }
            if (settings.crossover_hz) {
                w.key("crossover_hz").number(*settings.crossover_hz, 1);
            }
            const DecoderSettings& decoder = settings.decoder;
            w.key("decoder").begin_object();
            if (decoder.mode) {
                w.member("mode", name_of(kModes, *decoder.mode));
            }
            if (decoder.drc_cut) {
                w.key("drc_cut").number(*decoder.drc_cut);
            }
            if (decoder.drc_boost) {
                w.key("drc_boost").number(*decoder.drc_boost);
            }
            if (decoder.heavy_compression) {
                w.member("heavy_compression", *decoder.heavy_compression);
            }
            if (decoder.dialnorm) {
                w.member("dialnorm", *decoder.dialnorm);
            }
            if (decoder.downmix) {
                w.member("downmix", name_of(kDownmixes, *decoder.downmix));
            }
            if (decoder.ltrt_phase_shift) {
                w.member("ltrt_phase_shift", *decoder.ltrt_phase_shift);
            }
            if (decoder.mix_lfe) {
                w.member("mix_lfe", *decoder.mix_lfe);
            }
            if (decoder.has_programme) {
                if (decoder.programme) {
                    w.member("programme", *decoder.programme);
                } else {
                    w.key("programme").null();
                }
            }
            if (decoder.objects) {
                w.member("objects", name_of(kObjectsPolicies, *decoder.objects));
            }
            if (decoder.concealment) {
                w.member("concealment", name_of(kConcealments, *decoder.concealment));
            }
            w.end_object().end_object();
            break;
        }
        case Command::kIdentify:
            if (command.identify) {
                w.key("identify").begin_object().member("output", command.identify->output);
                w.key("level_db").number(command.identify->level_db, 1).end_object();
            } else {
                w.key("identify").null();
            }
            break;
    }
    w.end_object();
}

namespace {

// A settings object: refused whole when a key it knows holds an invalid value.
[[nodiscard]] std::expected<Settings, CommandFailure> read_settings(json::Value value) {
    const std::optional<std::int64_t> revision = value["revision"].as_int();
    if (!value.is_object() || !revision) {
        return std::unexpected(CommandFailure{.error = CommandError::kMalformed, .settings = {}});
    }
    Settings settings;
    settings.revision = *revision;
    const auto refuse = [&](std::string why) {
        return std::unexpected(CommandFailure{
            .error = CommandError::kSettingsRefused,
            .settings = SettingsError{.revision = *revision, .why = std::move(why)}});
    };

    if (const json::Value layout = value["layout"]; layout.exists()) {
        settings.layout = layout.as_string();
        if (!settings.layout) {
            return refuse("layout is not a string");
        }
    }
    if (const json::Value routing = value["routing"]; routing.exists()) {
        settings.routing = routing.as_string();
        if (!settings.routing) {
            return refuse("routing is not a string");
        }
    }
    if (const json::Value trims = value["trim_db"]; trims.exists()) {
        settings.trim_db = read_numbers(trims);
        if (!settings.trim_db) {
            return refuse("trim_db is not a list of numbers");
        }
    }
    if (const json::Value delays = value["delay_ms"]; delays.exists()) {
        settings.delay_ms = read_numbers(delays);
        if (!settings.delay_ms || std::any_of(settings.delay_ms->begin(), settings.delay_ms->end(),
                                              [](double delay) { return delay < 0.0; })) {
            return refuse("delay_ms is not a list of numbers of 0 or more");
        }
    }
    if (const json::Value crossover = value["crossover_hz"]; crossover.exists()) {
        settings.crossover_hz = read_number(crossover);
        // Positive as the writer writes it, to a tenth of a hertz.
        if (!settings.crossover_hz || std::lround(*settings.crossover_hz * 10.0) <= 0) {
            return refuse("crossover_hz is not a positive number");
        }
    }

    const json::Value decoder = value["decoder"];
    if (decoder.exists() && !decoder.is_object()) {
        return refuse("decoder is not an object");
    }
    DecoderSettings& out = settings.decoder;
    const auto named = [&]<class E, std::size_t N>(std::string_view key, const std::array<Named<E>, N>& table,
                                                    std::optional<E>& target) {
        const json::Value field = decoder[key];
        if (!field.exists()) {
            return true;
        }
        target = value_of(table, field);
        return target.has_value();
    };
    const auto scale = [&](std::string_view key, std::optional<double>& target) {
        const json::Value field = decoder[key];
        if (!field.exists()) {
            return true;
        }
        target = read_number(field);
        return target && *target >= 0.0 && *target <= 1.0;
    };
    const auto flag = [&](std::string_view key, std::optional<bool>& target) {
        const json::Value field = decoder[key];
        if (!field.exists()) {
            return true;
        }
        target = field.as_bool();
        return target.has_value();
    };
    if (!named("mode", kModes, out.mode)) {
        return refuse("decoder.mode is not line, rf or custom");
    }
    if (!scale("drc_cut", out.drc_cut) || !scale("drc_boost", out.drc_boost)) {
        return refuse("decoder.drc_cut and drc_boost are from 0.0 to 1.0");
    }
    if (!flag("heavy_compression", out.heavy_compression) || !flag("dialnorm", out.dialnorm) ||
        !flag("ltrt_phase_shift", out.ltrt_phase_shift) || !flag("mix_lfe", out.mix_lfe)) {
        return refuse("a decoder switch is not true or false");
    }
    if (!named("downmix", kDownmixes, out.downmix)) {
        return refuse("decoder.downmix is not loro or ltrt");
    }
    if (const json::Value programme = decoder["programme"]; programme.exists()) {
        out.has_programme = true;
        if (!programme.is_null()) {
            out.programme = read_int32(programme, 0, 255);
            if (!out.programme) {
                return refuse("decoder.programme is not a programme number or null");
            }
        }
    }
    if (!named("objects", kObjectsPolicies, out.objects)) {
        return refuse("decoder.objects is not auto, always or never");
    }
    if (!named("concealment", kConcealments, out.concealment)) {
        return refuse("decoder.concealment is not none, repeat_fade or mute");
    }
    return settings;
}

}  // namespace

std::expected<CommandMessage, CommandFailure> read_command(json::Value value) {
    const auto malformed = [] { return std::unexpected(CommandFailure{.error = CommandError::kMalformed, .settings = {}}); };
    const std::optional<Command> which = value_of(kCommands, value["command"]);
    if (!value.is_object() || !which) {
        return malformed();
    }
    CommandMessage command;
    command.command = *which;
    switch (*which) {
        case Command::kVolume: {
            const std::optional<std::int32_t> volume = read_int32(value["volume"], 0, 100);
            if (!volume) {
                return malformed();
            }
            command.volume = *volume;
            break;
        }
        case Command::kMute: {
            const std::optional<bool> mute = value["mute"].as_bool();
            if (!mute) {
                return malformed();
            }
            command.mute = *mute;
            break;
        }
        case Command::kSetOutputDelay: {
            const std::optional<std::int32_t> delay = read_int32(value["output_delay_ms"], 0, kMaxOutputDelay);
            if (!delay) {
                return malformed();
            }
            command.output_delay_ms = *delay;
            break;
        }
        case Command::kSettings: {
            std::expected<Settings, CommandFailure> settings = read_settings(value["settings"]);
            if (!settings) {
                return std::unexpected(settings.error());
            }
            command.settings = std::move(*settings);
            break;
        }
        case Command::kIdentify: {
            const json::Value identify = value["identify"];
            if (!identify.exists()) {
                return malformed();
            }
            if (!identify.is_null()) {
                const std::optional<std::int32_t> output = read_int32(identify["output"], 0, kMaxOutputs - 1);
                const std::optional<double> level = read_number(identify["level_db"]);
                if (!output || !level || *level < kIdentifyMinimumDb || *level > kIdentifyMaximumDb) {
                    return malformed();
                }
                command.identify = Identify{.output = *output, .level_db = *level};
            }
            break;
        }
    }
    return command;
}

std::optional<std::string> check_settings(const Settings& settings, const Support& support) {
    const Management& management = support.management;
    const auto count = static_cast<std::size_t>(support.outputs.count);
    const auto inside = [](double value, const std::array<double, 2>& range) {
        return value >= range[0] && value <= range[1];
    };
    if (settings.routing && !management.routing) {
        return "this sink does not take routing";
    }
    if (settings.trim_db) {
        if (settings.trim_db->size() != count) {
            return "trim_db needs one value per output";
        }
        if (!std::all_of(settings.trim_db->begin(), settings.trim_db->end(),
                         [&](double trim) { return inside(trim, management.trim_db); })) {
            return "a trim_db value is outside management.trim_db";
        }
    }
    if (settings.delay_ms) {
        if (settings.delay_ms->size() != count) {
            return "delay_ms needs one value per output";
        }
        if (!std::all_of(settings.delay_ms->begin(), settings.delay_ms->end(),
                         [&](double delay) { return delay <= management.max_delay_ms; })) {
            return "a delay_ms value is outside management.delay_ms";
        }
    }
    if (settings.crossover_hz && !inside(*settings.crossover_hz, management.crossover_hz)) {
        return "crossover_hz is outside management.crossover_hz";
    }

    const DecoderSettings& decoder = settings.decoder;
    const std::array<bool, kDecoderSettingNames.size()> present{
        decoder.mode.has_value(),
        decoder.drc_cut.has_value(),
        decoder.drc_boost.has_value(),
        decoder.heavy_compression.has_value(),
        decoder.dialnorm.has_value(),
        decoder.downmix.has_value(),
        decoder.ltrt_phase_shift.has_value(),
        decoder.mix_lfe.has_value(),
        decoder.has_programme,
        decoder.objects.has_value(),
        decoder.concealment.has_value(),
    };
    for (std::size_t i = 0; i < present.size(); ++i) {
        if (present[i] && std::find(support.decoder_settings.begin(), support.decoder_settings.end(),
                                    kDecoderSettingNames[i]) == support.decoder_settings.end()) {
            return "decoder." + std::string{kDecoderSettingNames[i]} + " is not a setting this sink takes";
        }
    }
    return std::nullopt;
}

}  // namespace ac3::sendspin::ac3forge

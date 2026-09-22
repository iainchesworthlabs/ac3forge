#include "ac3/sendspin/stream_roles.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/sendspin/frames.hpp"
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

void put_be64(std::span<std::uint8_t> out, std::int64_t value) {
    auto bits = static_cast<std::uint64_t>(value);
    for (std::size_t i = 0; i < 8; ++i) {
        out[7 - i] = static_cast<std::uint8_t>(bits & 0xFFU);
        bits >>= 8U;
    }
}

[[nodiscard]] std::int64_t get_be64(std::span<const std::uint8_t> in) {
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        bits = (bits << 8U) | in[i];
    }
    return static_cast<std::int64_t>(bits);
}

void put_be16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

[[nodiscard]] std::uint16_t get_be16(std::span<const std::uint8_t> in) {
    return static_cast<std::uint16_t>((static_cast<unsigned>(in[0]) << 8U) | in[1]);
}

}  // namespace

// --- artwork@v1 --------------------------------------------------------------------------------

namespace artwork {

namespace {

constexpr std::array<Named<Source>, 3> kSources{{
    {Source::kAlbum, "album"},
    {Source::kArtist, "artist"},
    {Source::kNone, "none"},
}};

constexpr std::array<Named<Format>, 2> kFormats{{
    {Format::kJpeg, "jpeg"},
    {Format::kPng, "png"},
}};

constexpr std::uint8_t kCancelFlag = 1;
constexpr std::uint8_t kAnnounceFlag = 2;

}  // namespace

Channel Channels::at(std::size_t index) const {
    return index < channels.size() ? channels[index] : Channel{};
}

void write_channels(json::Writer& w, const Channels& channels) {
    w.begin_object().key("channels").begin_array();
    for (const Channel& channel : channels.channels) {
        w.begin_object().member("source", name_of(kSources, channel.source));
        if (channel.source != Source::kNone) {
            w.member("format", name_of(kFormats, channel.format))
                .member("width", channel.width)
                .member("height", channel.height);
        }
        w.end_object();
    }
    w.end_array().end_object();
}

std::optional<Channels> read_channels(json::Value value) {
    const json::Value list = value["channels"];
    if (!value.is_object() || !list.is_array() || list.size() > kMaxChannels) {
        return std::nullopt;
    }
    Channels channels;
    for (const json::Value element : list.elements()) {
        const std::optional<Source> source = value_of(kSources, element["source"]);
        if (!element.is_object() || !source) {
            return std::nullopt;
        }
        Channel channel;
        channel.source = *source;
        if (*source != Source::kNone) {
            const std::optional<Format> format = value_of(kFormats, element["format"]);
            const std::optional<std::int32_t> width = read_int32(element["width"], 1, kMaxInt32);
            const std::optional<std::int32_t> height = read_int32(element["height"], 1, kMaxInt32);
            if (!format || !width || !height) {
                return std::nullopt;
            }
            channel.format = *format;
            channel.width = *width;
            channel.height = *height;
        }
        channels.channels.push_back(channel);
    }
    return channels;
}

std::expected<Message, MessageError> parse_message(std::span<const std::uint8_t> message) {
    if (message.empty() || message[0] < message_id::kArtworkFirst || message[0] > message_id::kArtworkLast) {
        return std::unexpected(MessageError::kNotArtwork);
    }
    if (message.size() < 2) {
        return std::unexpected(MessageError::kTooShort);
    }
    if (message.size() > kMaxMessageBytes) {
        return std::unexpected(MessageError::kTooLong);
    }
    const std::uint8_t flags = message[1];
    if ((flags & 0xFCU) != 0 || flags == (kCancelFlag | kAnnounceFlag)) {
        return std::unexpected(MessageError::kReservedFlags);
    }
    Message parsed;
    parsed.channel = static_cast<std::size_t>(message[0] - message_id::kArtworkFirst);
    if ((flags & kAnnounceFlag) != 0) {
        if (message.size() != kAnnounceBytes) {
            return std::unexpected(MessageError::kBadAnnounce);
        }
        parsed.kind = Kind::kAnnounce;
        parsed.timestamp = get_be64(message.subspan(2, 8));
        parsed.total_size = (static_cast<std::uint32_t>(message[10]) << 24U) |
                            (static_cast<std::uint32_t>(message[11]) << 16U) |
                            (static_cast<std::uint32_t>(message[12]) << 8U) | message[13];
        return parsed;
    }
    if ((flags & kCancelFlag) != 0) {
        if (message.size() != 2) {
            return std::unexpected(MessageError::kBadCancel);
        }
        parsed.kind = Kind::kCancel;
        return parsed;
    }
    parsed.kind = Kind::kPart;
    parsed.data = message.subspan(2);
    return parsed;
}

std::array<std::uint8_t, kAnnounceBytes> announce(std::size_t channel, std::int64_t timestamp, std::uint32_t total_size) {
    std::array<std::uint8_t, kAnnounceBytes> out{};
    out[0] = static_cast<std::uint8_t>(message_id::kArtworkFirst + std::min(channel, kMaxChannels - 1));
    out[1] = kAnnounceFlag;
    put_be64(std::span<std::uint8_t>(out).subspan(2, 8), timestamp);
    out[10] = static_cast<std::uint8_t>(total_size >> 24U);
    out[11] = static_cast<std::uint8_t>((total_size >> 16U) & 0xFFU);
    out[12] = static_cast<std::uint8_t>((total_size >> 8U) & 0xFFU);
    out[13] = static_cast<std::uint8_t>(total_size & 0xFFU);
    return out;
}

std::array<std::uint8_t, 2> cancel(std::size_t channel) {
    return {static_cast<std::uint8_t>(message_id::kArtworkFirst + std::min(channel, kMaxChannels - 1)), kCancelFlag};
}

std::optional<std::vector<std::uint8_t>> part(std::size_t channel, std::span<const std::uint8_t> data) {
    // Checked without adding, so no size wraps past it.
    if (data.size() > kMaxMessageBytes - 2) {
        return std::nullopt;
    }
    const std::array<std::uint8_t, 2> header{
        static_cast<std::uint8_t>(message_id::kArtworkFirst + std::min(channel, kMaxChannels - 1)), 0};
    std::vector<std::uint8_t> out(header.size() + data.size());
    std::copy(header.begin(), header.end(), out.begin());
    std::copy(data.begin(), data.end(), out.begin() + static_cast<std::ptrdiff_t>(header.size()));
    return out;
}

}  // namespace artwork

// --- visualizer@v1 -----------------------------------------------------------------------------

namespace visualizer {

namespace {

constexpr std::array<Named<Type>, 5> kTypes{{
    {Type::kLoudness, "loudness"},
    {Type::kBeat, "beat"},
    {Type::kFPeak, "f_peak"},
    {Type::kSpectrum, "spectrum"},
    {Type::kPeak, "peak"},
}};

constexpr std::array<Named<Scale>, 3> kScales{{
    {Scale::kMel, "mel"},
    {Scale::kLog, "log"},
    {Scale::kLin, "lin"},
}};

[[nodiscard]] bool has(const std::vector<Type>& types, Type type) {
    return std::find(types.begin(), types.end(), type) != types.end();
}

void write_types(json::Writer& w, const std::vector<Type>& types) {
    w.key("types").begin_array();
    for (const Type type : types) {
        w.string(name_of(kTypes, type));
    }
    w.end_array();
}

[[nodiscard]] std::optional<std::vector<Type>> read_types(json::Value value) {
    if (!value.is_array()) {
        return std::nullopt;
    }
    std::vector<Type> types;
    for (const json::Value element : value.elements()) {
        if (!element.is_string()) {
            return std::nullopt;
        }
        if (const std::optional<Type> type = value_of(kTypes, element); type && !has(types, *type)) {
            types.push_back(*type);
        }
    }
    return types;
}

void write_spectrum(json::Writer& w, const Spectrum& spectrum) {
    w.key("spectrum")
        .begin_object()
        .member("n_disp_bins", spectrum.n_disp_bins)
        .member("scale", name_of(kScales, spectrum.scale))
        .member("f_min", spectrum.f_min)
        .member("f_max", spectrum.f_max)
        .end_object();
}

[[nodiscard]] std::optional<Spectrum> read_spectrum(json::Value value) {
    const std::optional<std::int32_t> bins = read_int32(value["n_disp_bins"], 1, kMaxInt32);
    const std::optional<Scale> scale = value_of(kScales, value["scale"]);
    const std::optional<std::int32_t> f_min = read_int32(value["f_min"], 0, kMaxInt32);
    const std::optional<std::int32_t> f_max = read_int32(value["f_max"], 0, kMaxInt32);
    if (!value.is_object() || !bins || !scale || !f_min || !f_max || *f_min > *f_max) {
        return std::nullopt;
    }
    return Spectrum{.n_disp_bins = *bins, .scale = *scale, .f_min = *f_min, .f_max = *f_max};
}

}  // namespace

std::uint8_t message_id(Type type) {
    switch (type) {
        case Type::kLoudness:
            return 16;
        case Type::kBeat:
            return 17;
        case Type::kFPeak:
            return 18;
        case Type::kSpectrum:
            return 19;
        case Type::kPeak:
            return 20;
    }
    return 16;
}

void write_support(json::Writer& w, const Support& support) {
    w.begin_object().key("buffer_capacity").unsigned_integer(support.buffer_capacity).end_object();
}

std::optional<Support> read_support(json::Value value) {
    const std::optional<std::int64_t> capacity = value["buffer_capacity"].as_int();
    if (!value.is_object() || !capacity || *capacity < 0) {
        return std::nullopt;
    }
    return Support{.buffer_capacity = static_cast<std::uint64_t>(*capacity)};
}

void write_state(json::Writer& w, const State& state) {
    w.begin_object();
    write_types(w, state.types);
    w.member("rate_max", state.rate_max);
    if (state.spectrum) {
        write_spectrum(w, *state.spectrum);
    }
    w.end_object();
}

std::optional<State> read_state(json::Value value) {
    std::optional<std::vector<Type>> types = read_types(value["types"]);
    const std::optional<std::int32_t> rate = read_int32(value["rate_max"], 1, kMaxInt32);
    if (!value.is_object() || !types || !rate) {
        return std::nullopt;
    }
    State state;
    state.types = std::move(*types);
    state.rate_max = *rate;
    if (const json::Value spectrum = value["spectrum"]; spectrum.exists()) {
        state.spectrum = read_spectrum(spectrum);
        if (!state.spectrum) {
            return std::nullopt;
        }
    }
    if (has(state.types, Type::kSpectrum) && !state.spectrum) {
        return std::nullopt;
    }
    return state;
}

void write_stream_start(json::Writer& w, const StreamStart& start) {
    w.begin_object();
    write_types(w, start.types);
    w.member("rate_max", start.rate_max);
    if (start.tracks_downbeats) {
        w.member("tracks_downbeats", *start.tracks_downbeats);
    }
    if (start.spectrum) {
        write_spectrum(w, *start.spectrum);
    }
    w.end_object();
}

std::optional<StreamStart> read_stream_start(json::Value value) {
    std::optional<std::vector<Type>> types = read_types(value["types"]);
    const std::optional<std::int32_t> rate = read_int32(value["rate_max"], 0, kMaxInt32);
    if (!value.is_object() || !types || !rate) {
        return std::nullopt;
    }
    StreamStart start;
    start.types = std::move(*types);
    start.rate_max = *rate;
    if (has(start.types, Type::kBeat)) {
        start.tracks_downbeats = value["tracks_downbeats"].as_bool();
        if (!start.tracks_downbeats) {
            return std::nullopt;
        }
    }
    if (has(start.types, Type::kSpectrum)) {
        start.spectrum = read_spectrum(value["spectrum"]);
        if (!start.spectrum) {
            return std::nullopt;
        }
    }
    return start;
}

StreamStart derive(const State& requested, std::span<const Type> available, std::int32_t rate_max,
                   bool tracks_downbeats) {
    StreamStart start;
    for (const Type type : requested.types) {
        if (std::find(available.begin(), available.end(), type) != available.end() &&
            (type != Type::kSpectrum || requested.spectrum)) {
            start.types.push_back(type);
        }
    }
    start.rate_max = std::min(requested.rate_max, rate_max);
    if (has(start.types, Type::kBeat)) {
        start.tracks_downbeats = tracks_downbeats;
    }
    if (has(start.types, Type::kSpectrum)) {
        start.spectrum = requested.spectrum;
    }
    return start;
}

std::vector<std::uint8_t> write_frame(const Frame& frame) {
    std::vector<std::uint8_t> out(9);
    out[0] = message_id(frame.type);
    put_be64(std::span<std::uint8_t>(out).subspan(1, 8), frame.timestamp);
    switch (frame.type) {
        case Type::kLoudness:
            put_be16(out, frame.value);
            break;
        case Type::kBeat:
            out.push_back(frame.downbeat ? 1 : 0);
            break;
        case Type::kFPeak:
            put_be16(out, frame.frequency);
            put_be16(out, frame.frequency == 0 ? std::uint16_t{0} : frame.value);
            break;
        case Type::kSpectrum:
            for (const std::uint16_t bin : frame.bins) {
                put_be16(out, bin);
            }
            break;
        case Type::kPeak:
            out.push_back(frame.strength);
            break;
    }
    return out;
}

std::optional<Frame> parse_frame(std::span<const std::uint8_t> message, std::size_t bins) {
    if (message.size() < 9) {
        return std::nullopt;
    }
    Frame frame;
    frame.timestamp = get_be64(message.subspan(1, 8));
    const std::span<const std::uint8_t> data = message.subspan(9);
    switch (message[0]) {
        case 16:
            if (data.size() != 2) {
                return std::nullopt;
            }
            frame.type = Type::kLoudness;
            frame.value = get_be16(data);
            return frame;
        case 17:
            if (data.size() != 1) {
                return std::nullopt;
            }
            frame.type = Type::kBeat;
            // Bits 1 to 7 are reserved, and a client ignores them.
            frame.downbeat = (data[0] & 1U) != 0;
            return frame;
        case 18:
            if (data.size() != 4) {
                return std::nullopt;
            }
            frame.type = Type::kFPeak;
            frame.frequency = get_be16(data.subspan(0, 2));
            frame.value = get_be16(data.subspan(2, 2));
            // No peak detected, and the amplitude MUST then be 0 too.
            if (frame.frequency == 0 && frame.value != 0) {
                return std::nullopt;
            }
            return frame;
        case 19:
            if (data.size() != bins * 2) {
                return std::nullopt;
            }
            frame.type = Type::kSpectrum;
            for (std::size_t i = 0; i < bins; ++i) {
                frame.bins.push_back(get_be16(data.subspan(i * 2, 2)));
            }
            return frame;
        case 20:
            if (data.size() != 1) {
                return std::nullopt;
            }
            frame.type = Type::kPeak;
            frame.strength = data[0];
            return frame;
        default:
            return std::nullopt;
    }
}

std::uint16_t scaled_level(double db) {
    if (!(db > -60.0)) {
        return 0;
    }
    if (db >= 0.0) {
        return 65535;
    }
    return static_cast<std::uint16_t>(std::lround((db + 60.0) / 60.0 * 65535.0));
}

}  // namespace visualizer

// --- source@v1 ---------------------------------------------------------------------------------

namespace source {

namespace {

constexpr std::array<Named<Signal>, 2> kSignals{{
    {Signal::kPresent, "present"},
    {Signal::kAbsent, "absent"},
}};

constexpr std::array<Named<Command>, 2> kCommands{{
    {Command::kStart, "start"},
    {Command::kStop, "stop"},
}};

}  // namespace

void write_support(json::Writer& w, const Support& support) {
    w.begin_object().key("features").begin_object().member("line_sense", support.line_sense).end_object().end_object();
}

std::optional<Support> read_support(json::Value value) {
    if (!value.is_object()) {
        return std::nullopt;
    }
    Support support;
    if (const json::Value line_sense = value["features"]["line_sense"]; line_sense.exists()) {
        const std::optional<bool> on = line_sense.as_bool();
        if (!on) {
            return std::nullopt;
        }
        support.line_sense = *on;
    }
    return support;
}

void write_state(json::Writer& w, const State& state) {
    w.begin_object();
    if (state.signal) {
        w.member("signal", name_of(kSignals, *state.signal));
    }
    w.end_object();
}

std::optional<State> read_state(json::Value value) {
    if (!value.is_object()) {
        return std::nullopt;
    }
    State state;
    if (const json::Value signal = value["signal"]; signal.exists()) {
        state.signal = value_of(kSignals, signal);
        if (!state.signal) {
            return std::nullopt;
        }
    }
    return state;
}

void write_command(json::Writer& w, Command command) {
    w.begin_object().member("command", name_of(kCommands, command)).end_object();
}

std::optional<Command> read_command(json::Value value) {
    return value_of(kCommands, value["command"]);
}

std::optional<Chunk> parse_chunk(std::span<const std::uint8_t> message) {
    if (message.size() < kChunkHeaderBytes || message[0] != message_id::kSourceFirst) {
        return std::nullopt;
    }
    return Chunk{.timestamp = get_be64(message.subspan(1, 8)), .frame = message.subspan(kChunkHeaderBytes)};
}

std::vector<std::uint8_t> write_chunk(std::int64_t timestamp, std::span<const std::uint8_t> frame) {
    std::vector<std::uint8_t> out(kChunkHeaderBytes + frame.size());
    out[0] = message_id::kSourceFirst;
    put_be64(std::span<std::uint8_t>(out).subspan(1, 8), timestamp);
    std::copy(frame.begin(), frame.end(), out.begin() + static_cast<std::ptrdiff_t>(kChunkHeaderBytes));
    return out;
}

}  // namespace source

}  // namespace ac3::sendspin

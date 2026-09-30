#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/json.hpp"

// The three roles besides player@v1 that carry binary messages: artwork@v1 and visualizer@v1 from
// a server, and source@v1 from a client (roles/artwork/v1.md, roles/visualizer/v1.md,
// roles/source/v1.md). Their JSON objects are structs with a writer that appends the object as the
// value of a key the caller has written, and a reader; their binary messages have writers and
// parsers that check what one message can say about itself. The messages that carry the objects
// are messages.hpp's.
//
// Hearth's server does not activate these roles for an aiosendspin 9.1.1 client, whose forms of
// them differ (planning/hearth-sendspin-extension.md, C33 to C35), so only the specification's are
// here.

namespace ac3::sendspin {

// --- artwork@v1 --------------------------------------------------------------------------------

namespace artwork {

inline constexpr std::string_view kRole = "artwork@v1";
inline constexpr std::size_t kMaxChannels = 4;
// The largest artwork message, so that one fits a Noise transport message unfragmented.
inline constexpr std::size_t kMaxMessageBytes = 65519;
inline constexpr std::size_t kAnnounceBytes = 14;

enum class Source : std::uint8_t {
    kAlbum,
    kArtist,
    kNone,
};

enum class Format : std::uint8_t {
    kJpeg,
    kPng,
};

struct Channel {
    Source source = Source::kNone;
    // Required unless the source is kNone.
    Format format = Format::kJpeg;
    std::int32_t width = 0;
    std::int32_t height = 0;

    friend bool operator==(const Channel&, const Channel&) = default;
};

// The channels, positional from channel 0: the client/state object a client declares, and the
// stream/start object a server answers with.
struct Channels {
    std::vector<Channel> channels;

    // The channel at `index`, kNone past the end of the array.
    [[nodiscard]] Channel at(std::size_t index) const;
    friend bool operator==(const Channels&, const Channels&) = default;
};

void write_channels(json::Writer& w, const Channels& channels);
// Nothing when `channels` is missing, holds more than four entries, or an entry that is not kNone
// lacks its format or a positive size.
[[nodiscard]] std::optional<Channels> read_channels(json::Value value);

enum class Kind : std::uint8_t {
    kAnnounce,
    kPart,
    kCancel,
};

struct Message {
    Kind kind = Kind::kPart;
    std::size_t channel = 0;
    // kAnnounce.
    std::int64_t timestamp = 0;
    std::uint32_t total_size = 0;
    // kPart: the next bytes of the image.
    std::span<const std::uint8_t> data;
};

enum class MessageError : std::uint8_t {
    kNotArtwork,     // an ID outside 8 to 11
    kTooShort,       // under two bytes
    kTooLong,        // over kMaxMessageBytes
    kReservedFlags,  // flag bits 2 to 7 set, or a cancel and an announce at once
    kBadAnnounce,    // an announce that is not 14 bytes
    kBadCancel,      // a cancel longer than two bytes
};

// One artwork message, as a client must refuse it (roles/artwork/v1.md, Malformed messages).
[[nodiscard]] std::expected<Message, MessageError> parse_message(std::span<const std::uint8_t> message);

[[nodiscard]] std::array<std::uint8_t, kAnnounceBytes> announce(std::size_t channel, std::int64_t timestamp,
                                                                 std::uint32_t total_size);
[[nodiscard]] std::array<std::uint8_t, 2> cancel(std::size_t channel);
// A part: the two header bytes, then `data`. Nothing when it would pass kMaxMessageBytes.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> part(std::size_t channel, std::span<const std::uint8_t> data);

}  // namespace artwork

// --- visualizer@v1 -----------------------------------------------------------------------------

namespace visualizer {

inline constexpr std::string_view kRole = "visualizer@v1";
inline constexpr std::string_view kSupportKey = "visualizer@v1_support";

enum class Type : std::uint8_t {
    kLoudness,
    kBeat,
    kFPeak,
    kSpectrum,
    kPeak,
};

// The message ID each type's frames carry: 16 to 20.
[[nodiscard]] std::uint8_t message_id(Type type);

struct Support {
    // Bytes of buffered visualizer messages, counting each message's ID, timestamp and data.
    std::uint64_t buffer_capacity = 0;
};

void write_support(json::Writer& w, const Support& support);
[[nodiscard]] std::optional<Support> read_support(json::Value value);

enum class Scale : std::uint8_t {
    kMel,
    kLog,
    kLin,
};

struct Spectrum {
    std::int32_t n_disp_bins = 0;
    Scale scale = Scale::kMel;
    std::int32_t f_min = 0;
    std::int32_t f_max = 0;

    friend bool operator==(const Spectrum&, const Spectrum&) = default;
};

// The client/state object.
struct State {
    std::vector<Type> types;
    std::int32_t rate_max = 0;
    // Required when `types` has kSpectrum.
    std::optional<Spectrum> spectrum;

    friend bool operator==(const State&, const State&) = default;
};

void write_state(json::Writer& w, const State& state);
// Nothing when a field is missing or out of range, or `types` has spectrum without a spectrum
// object, which the role calls a protocol error. A type this reader does not know is left out.
[[nodiscard]] std::optional<State> read_state(json::Value value);

// The stream/start object.
struct StreamStart {
    std::vector<Type> types;
    std::int32_t rate_max = 0;
    // Present exactly when `types` has kBeat.
    std::optional<bool> tracks_downbeats;
    // Present exactly when `types` has kSpectrum.
    std::optional<Spectrum> spectrum;

    friend bool operator==(const StreamStart&, const StreamStart&) = default;
};

void write_stream_start(json::Writer& w, const StreamStart& start);
[[nodiscard]] std::optional<StreamStart> read_stream_start(json::Value value);

// What a server streams for `requested`, of the types it can produce for the source: the types both
// have, at no more than the rate asked, with its beat tracker's downbeat support.
[[nodiscard]] StreamStart derive(const State& requested, std::span<const Type> available, std::int32_t rate_max,
                                 bool tracks_downbeats);

struct Frame {
    Type type = Type::kLoudness;
    std::int64_t timestamp = 0;
    // kLoudness, and the amplitude of kFPeak: 0 silence to 65,535 full scale.
    std::uint16_t value = 0;
    // kFPeak: the dominant frequency in Hz, 0 for none.
    std::uint16_t frequency = 0;
    // kBeat.
    bool downbeat = false;
    // kPeak.
    std::uint8_t strength = 0;
    // kSpectrum.
    std::vector<std::uint16_t> bins;

    friend bool operator==(const Frame&, const Frame&) = default;
};

// A frame's message: [ID][int64 timestamp][data], big-endian.
[[nodiscard]] std::vector<std::uint8_t> write_frame(const Frame& frame);
// Nothing for an ID outside 16 to 20, data of the wrong length for the type, or an f_peak amplitude
// without a frequency; a spectrum takes `bins` bins, its stream's n_disp_bins.
[[nodiscard]] std::optional<Frame> parse_frame(std::span<const std::uint8_t> message, std::size_t bins);

// A level in dB, -60 to 0, as the role's uint16 scale: -60 dB and below 0, 0 dB 65,535.
[[nodiscard]] std::uint16_t scaled_level(double db);

}  // namespace visualizer

// --- source@v1 ---------------------------------------------------------------------------------

namespace source {

inline constexpr std::string_view kRole = "source@v1";
inline constexpr std::string_view kSupportKey = "source@v1_support";
// [12][int64 timestamp][frame].
inline constexpr std::size_t kChunkHeaderBytes = 9;

struct Support {
    // The source reports `signal` in its state.
    bool line_sense = false;
};

void write_support(json::Writer& w, const Support& support);
[[nodiscard]] std::optional<Support> read_support(json::Value value);

enum class Signal : std::uint8_t {
    kPresent,
    kAbsent,
};

struct State {
    std::optional<Signal> signal;
};

void write_state(json::Writer& w, const State& state);
[[nodiscard]] std::optional<State> read_state(json::Value value);

enum class Command : std::uint8_t {
    kStart,
    kStop,
};

void write_command(json::Writer& w, Command command);
[[nodiscard]] std::optional<Command> read_command(json::Value value);

struct Chunk {
    // When the first sample was captured, on the server clock.
    std::int64_t timestamp = 0;
    std::span<const std::uint8_t> frame;
};

// A source audio chunk, ID 12. Nothing for another ID or a message shorter than its header.
[[nodiscard]] std::optional<Chunk> parse_chunk(std::span<const std::uint8_t> message);
[[nodiscard]] std::vector<std::uint8_t> write_chunk(std::int64_t timestamp, std::span<const std::uint8_t> frame);

}  // namespace source

}  // namespace ac3::sendspin

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "ac3/audio/monitor.hpp"
#include "ac3/render/layout.hpp"
#include "transport.hpp"

// Where a session's rendered audio goes (planning/hearth-reference-player.md,
// A3).
//
// The engine talks to this and never to a platform API. One implementation
// drives ac3::audio::PcmOutput (A2), which opens a local device at its own
// width and places each rendered slot by the device's speakers; ac3tests has
// another, a fake device with a clock of its own, which is what A3's exit
// needs - "a queue of mixed containers plays to a fake device gaplessly, with
// the expected sample count at every join" - and what makes the player's
// behaviour at a join checkable without a sound card or a stopwatch.
//
// A PCM sink only. A bitstream output takes packed bursts rather than
// rendered blocks (bitstream_sink.hpp), and a network group takes a stream;
// each has a seam of its own, rather than a single interface with a payload
// that means different things depending on the mode.

namespace ac3::hearth {

class PcmSink {
public:
    virtual ~PcmSink() = default;

    struct Format {
        std::uint32_t sample_rate = 0;
        // What the renderer produces, one block per slot. The sink decides
        // where each slot comes out; the engine never needs to know.
        render::OutputLayout layout{};
        // The endpoint the output decision chose, or empty for the sink's own.
        std::string endpoint_id{};
    };

    // Opens for `format`, and reports what it actually opened: the rate, and
    // the width - a local device's own channel count rather than the
    // layout's, which is the figure the transport's join decision compares
    // (OpenOutputFormat). The error is a sentence for the Output screen.
    [[nodiscard]] virtual std::expected<OpenOutputFormat, std::string> open(const Format& format) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool is_open() const = 0;

    // One rendered block: a span per slot of the layout it was opened for,
    // each at least `frames` long. False, having taken nothing, when the sink
    // is full - the caller is ahead of real time, and waits.
    virtual bool submit(std::span<const std::span<const float>> slots, std::size_t frames) = 0;

    // Where the device has got to, from its own clock (A2's MonitorPosition),
    // or nothing while closed.
    [[nodiscard]] virtual std::optional<audio::MonitorPosition> position() const = 0;

    // Drops what has not been played, and counts from zero again - a seek, a
    // skip. Returns once done.
    virtual void flush() = 0;

    // Stops the device without closing it, and starts it again. False when
    // nothing is open or the platform refused.
    virtual bool pause() = 0;
    virtual bool resume() = 0;
};

// The real one: a local render endpoint through ac3::audio::PcmOutput.
// `device_id` empty selects the endpoint the enumeration marks default, as
// PcmOutput's own start() describes.
[[nodiscard]] std::unique_ptr<PcmSink> make_device_sink(std::string device_id,
                                                        bool low_latency = false);

}  // namespace ac3::hearth

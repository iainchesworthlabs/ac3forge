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
#include "ac3/render/routing.hpp"
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

    // The Speakers page (planning/hearth-reference-player.md, A5): where each
    // rendered slot comes out, and what the device is. Non-pure with inert
    // defaults - a sink built before these existed (a test fake, mainly)
    // keeps compiling and simply does not support them, rather than every
    // implementer needing a change the day these were added. The real one,
    // ac3::hearth::make_device_sink's DeviceSink, forwards to
    // ac3::audio::PcmOutput, which already carries a routing patch - see
    // that header's own comment for why trim and delay are deliberately NOT
    // here too: PcmSink::submit() takes the RENDERED (slot-ordered) blocks,
    // and Player applies TrimDelay to those before they ever reach here, so
    // one pass covers whichever sink is open.

    // Replaces the routing patch between the renderer's slots and this
    // sink's outputs, between blocks. False, changing nothing, when nothing
    // is open, when `routing`'s own output count does not match what is
    // open, or when this sink does not support routing control.
    virtual bool set_routing(const render::Routing& routing) {
        static_cast<void>(routing);
        return false;
    }
    // The routing patch in effect, or a default (every slot unassigned) when
    // nothing is open or this sink does not support routing control.
    [[nodiscard]] virtual render::Routing routing() const { return render::Routing{}; }
    // The open device's own name - "Onkyo receiver", "Realtek" - for a
    // settings page; empty where there is none to show. Never used to decide
    // anything, only to say what is set up.
    [[nodiscard]] virtual std::string device_name() const { return {}; }
    // The open device's own speaker mask, 0 where it has none
    // (ac3::audio::speakers.hpp) - what speaker_routing() built the default
    // patch from, and what a settings page reads to label the routing grid's
    // columns by speaker rather than by bare output number.
    [[nodiscard]] virtual std::uint32_t speaker_mask() const { return 0; }
};

// The real one: a local render endpoint through ac3::audio::PcmOutput.
// `device_id` empty selects the endpoint the enumeration marks default, as
// PcmOutput's own start() describes.
[[nodiscard]] std::unique_ptr<PcmSink> make_device_sink(std::string device_id,
                                                        bool low_latency = false);

}  // namespace ac3::hearth

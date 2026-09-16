#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "ac3/audio/monitor.hpp"
#include "ac3/audio/passthrough.hpp"
#include "transport.hpp"

// Where a bitstreamed item goes (planning/hearth-reference-player.md, A3:
// "Passthrough, reusing PassthroughSink").
//
// The PCM sink's counterpart for the other kind of payload: IEC 61937
// bursts, each one AC-3 frame or six E-AC-3 blocks' worth of access units,
// already packed. The engine packs them (ac3::iec61937::wrap_frame and
// Eac3BurstPacker) and talks to this, never to a platform API. One
// implementation drives ac3::audio::PassthroughSink; ac3tests has a fake with
// a clock of its own, as it has for the PCM sink.
//
// Every figure a sink reports is in the CONTENT's frames - 1536 to a burst
// in either format - as PassthroughSink::position() counts them, so the
// player's timeline is the same arithmetic whichever sink it is feeding.

namespace ac3::hearth {

class BitstreamSink {
public:
    virtual ~BitstreamSink() = default;

    struct Format {
        audio::BitstreamFormat format = audio::BitstreamFormat::kAc3;
        // The content's rate; E-AC-3's link runs at four times it.
        std::uint32_t sample_rate = 0;
        // The endpoint the output decision chose, or empty for the sink's own.
        std::string endpoint_id{};
    };

    // Opens for `format`, and reports what it opened: the mode, the rate and
    // the stream on the link. The error is a sentence for the Output screen.
    [[nodiscard]] virtual std::expected<OpenOutputFormat, std::string> open(const Format& format) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool is_open() const = 0;

    // One burst, of the length the open format's bursts have. False, having
    // taken nothing, when the sink is full - the caller is ahead of real
    // time, and waits.
    virtual bool submit(std::span<const std::byte> burst) = 0;

    // Where the device has got to, in content frames, or nothing while
    // closed.
    [[nodiscard]] virtual std::optional<audio::MonitorPosition> position() const = 0;

    // Drops what has not been played, and counts from zero again. Returns
    // once done.
    virtual void flush() = 0;

    // Stops the link without closing it, and starts it again. A receiver
    // loses its lock while the link is stopped. False when nothing is open or
    // the platform refused.
    virtual bool pause() = 0;
    virtual bool resume() = 0;
};

// The real one: a local endpoint's exclusive IEC 61937 output through
// ac3::audio::PassthroughSink. `device_id` empty lets the sink choose the
// first output that will take the format, as PassthroughSink::start() does.
[[nodiscard]] std::unique_ptr<BitstreamSink> make_passthrough_sink(std::string device_id = {});

}  // namespace ac3::hearth

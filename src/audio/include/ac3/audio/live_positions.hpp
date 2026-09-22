#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

#include "ac3/oba/scene.hpp"

// A live object-position source over OSC (roadmap UX4): a UDP listener on
// its own thread, feeding an ac3::oba::SceneCursor once per encoder frame.
// This is the socket-and-thread half; the OSC 1.0 wire form itself
// (ac3::oba::parse_osc_packet/apply, src/forge) is pure and portable, and
// lives in the distributed library instead - see that header's own comment
// for why the split falls where it does.
//
// The seam both front ends share: `ac3cli live mode=atmos positions=osc:
// <port>` (apps/cli/commands/live_audio.cpp) and the GUI's live room
// (apps/gui/encoder_controller.cpp) each construct one of these and call
// drain_into() once per encode frame, exactly the way the GUI's live room
// already drains its own mutex-guarded manual-placement snapshot every
// frame - this generalises that established pattern rather than inventing a
// new one.

namespace ac3::audio {

enum class PositionSourceError : std::uint8_t {
    kBadAddress,      // bind_address did not parse as an IPv4 dotted-quad
    kSocketFailed,    // the OS socket layer itself failed
    kBindFailed,      // the OS refused the bind (port in use, permission, ...)
    kAlreadyRunning,  // start() called on an instance that is already listening
};

[[nodiscard]] std::string_view describe(PositionSourceError error);

// Counters for a status line - see apps/cli/commands/live_audio.cpp's
// `positions:` line and the GUI's liveOscDatagrams/Updates/Dropped
// properties for what reads these. Never a reason to stop listening: a
// malformed or unaddressed datagram is exactly what these count, not a
// fault the session needs to know about any other way.
struct PositionSourceStats {
    std::uint64_t datagrams = 0;          // UDP datagrams received
    std::uint64_t packets_rejected = 0;   // ac3::oba::OscParseStats::packets_rejected, summed
    std::uint64_t messages_dropped = 0;   // ac3::oba::OscParseStats::messages_dropped, summed
    std::uint64_t updates_applied = 0;    // SceneCursor::push calls this source has made
};

// Owns a UDP socket and a receiver thread. Objects are addressed 0-based, up
// to `objects - 1` (the count the session was built with - the same fixed
// slot budget `live mode=atmos` resolves once at session start, per
// resolve_object_slots' own comment on why that count cannot change
// mid-session); an OSC message naming an index outside that range is
// dropped the same as any other unusable message, counted in
// messages_dropped.
class LivePositionSource {
public:
    explicit LivePositionSource(std::size_t objects);
    ~LivePositionSource();
    LivePositionSource(const LivePositionSource&) = delete;
    LivePositionSource& operator=(const LivePositionSource&) = delete;
    LivePositionSource(LivePositionSource&&) = delete;
    LivePositionSource& operator=(LivePositionSource&&) = delete;

    // Binds and starts the receiver thread. bind_address is a dotted-quad
    // IPv4 literal ("127.0.0.1", "0.0.0.0") - never a hostname, so this
    // never blocks on DNS. port == 0 asks the OS for an ephemeral port;
    // local_port() reads back what it actually got, which is how a
    // hermetic test binds without racing a fixed port number.
    [[nodiscard]] std::expected<void, PositionSourceError> start(std::string_view bind_address,
                                                                  std::uint16_t port);

    // Joins the receiver thread and closes the socket. Safe to call whether
    // or not start() succeeded; the destructor calls this too.
    void stop();

    [[nodiscard]] bool running() const;
    [[nodiscard]] std::uint16_t local_port() const;
    [[nodiscard]] PositionSourceStats stats() const;

    // Once per encode frame, before sampling `cursor` - the sampling
    // boundary this whole type exists to define. Merges whatever this
    // source has received since the last call onto `cursor`'s objects and
    // pushes the result; an object with a pending gain/lfe-only update and
    // no position yet is left pending rather than applied (see
    // ac3::oba::apply's own comment) and is retried on the next call, not
    // dropped. Allocates nothing: the pending-update slots are sized once,
    // at construction, to `objects`.
    void drain_into(ac3::oba::SceneCursor& cursor, double time_s);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3::audio

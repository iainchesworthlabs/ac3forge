#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "ac3/audio/monitor.hpp"
#include "ac3/audio/passthrough.hpp"
#include "ac3/render/layout.hpp"
#include "transport.hpp"

// Forward-declared, not included: player.hpp includes this header
// unconditionally (PlayerOutputs::group), and player.cpp/player.hpp must
// stay buildable without src/sendspin at all (AC3FORGE_SENDSPIN_CORE_ONLY,
// fuzz/run.sh) - the same reason output_decision.hpp carries a group by
// plain std::string rather than a sendspin type, and pcm_sink.hpp/
// bitstream_sink.hpp name no backend. GroupResolver only names
// std::shared_ptr<Group>, never constructs or dereferences one, so the
// incomplete type is enough here; network_group_sink.cpp includes the real
// header where a Group is actually used.
namespace ac3::sendspin {
class Group;
}  // namespace ac3::sendspin

// Where a network group's programme goes (planning/hearth-reference-player.md,
// A6: "a group of two test sinks and the reference Python player plays one
// programme").
//
// A group needs BOTH forms from the one decode at once: rendered PCM for a
// member playing player@v1 (ac3::sendspin::Group::push()), and the item's own
// coded units, packed into IEC 61937 bursts, for a member playing
// _ac3forge_player@v1 (Group::push_burst()) - a mixed group takes both from
// the same session together (tests/hearth/test_group.cpp's own proof).
// Neither PcmSink nor BitstreamSink fits alone - pcm_sink.hpp's own comment
// says why: "a network group takes a stream... a seam of its own" - so this
// is its own interface rather than a third mode bent into either.
//
// push_burst() takes one whole burst at an absolute programme frame
// (ac3::sendspin::Group::Burst::frame), not a running byte stream, so a
// caller does not have to keep the PCM and the bursts in lock-step - Player
// paces each independently (player.cpp), and this interface mirrors that:
// submit_pcm() and submit_burst() are unrelated calls, each with its own
// backpressure.

namespace ac3::hearth {

class NetworkGroupSink {
public:
    virtual ~NetworkGroupSink() = default;

    struct Format {
        std::uint32_t sample_rate = 0;
        // What the renderer produces, for a member playing player@v1.
        render::OutputLayout layout{};
        // The item's own coded form, for a member playing
        // _ac3forge_player@v1; unset when the item carries nothing IEC 61937
        // can wrap - submit_burst() is then never called, and the group
        // plays to player@v1 members only.
        std::optional<audio::BitstreamFormat> stream{};
    };

    // Opens `group_name` - resolved against whatever this sink was built
    // with (make_group_sink()'s `resolve`), not a fixed group chosen once at
    // construction, since which group the user has selected can change
    // without the player being rebuilt. The error is a sentence for the
    // Output screen.
    [[nodiscard]] virtual std::expected<OpenOutputFormat, std::string> open(const std::string& group_name,
                                                                            const Format& format) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool is_open() const = 0;

    // One rendered block, planar like PcmSink::submit() - converted to the
    // group's own interleaved form internally. Returns the frames actually
    // taken, which can be fewer than offered (Group::push()'s own
    // backpressure) or 0 while a member's player holds enough; the caller
    // retries what was not taken.
    [[nodiscard]] virtual std::size_t submit_pcm(std::span<const std::span<const float>> slots,
                                                 std::size_t frames) = 0;
    // One burst: `pc`/`pd` as ac3::iec61937 writes them, `payload` the
    // elementary-stream bytes they describe (not the IEC 61937 carrier
    // bytes - a group's members are not S/PDIF, so there is nothing to
    // word-swizzle or zero-pad here), and `frame` the programme frame that
    // is its first decoded sample (Group::Burst::frame). False, taking
    // nothing, on the same terms as submit_pcm(); the caller retries.
    [[nodiscard]] virtual bool submit_burst(std::uint16_t pc, std::uint16_t pd,
                                            std::span<const std::byte> payload, std::int64_t frame) = 0;

    // Where the group has got to, from what has been taken so far. There is
    // no clock to read back over the wire - a member's own player buffers
    // what it is sent, on its own timeline, past where this can see - so
    // "taken" is treated as "heard", the same idealisation a fire-and-forget
    // sink with no telemetry has to make. Shaped like PcmSink::position()'s
    // own MonitorPosition so Player's timeline arithmetic does not need to
    // know the difference.
    [[nodiscard]] virtual std::optional<audio::MonitorPosition> position() const = 0;

    // Counts from zero again, matching submitted_since_open_'s own reset at
    // a seek (player.cpp). It cannot recall bytes already sent over the
    // wire, so a seek during network playback is not click-free the way a
    // local device's flush() is: a member briefly finishes what it was
    // already sent before the new position's audio arrives.
    virtual void flush() = 0;

    // A group has no wire-level pause: with nothing left to send, a
    // member's own player simply runs out and goes quiet, so these are
    // trivial - the transport's own state already stops fill()/drain() from
    // being called (Player::pump()) while paused.
    virtual bool pause() = 0;
    virtual bool resume() = 0;
};

// The real one: resolves `group_name` through `resolve` at each open() -
// which ac3::sendspin::Group backs a name, and whether that has changed
// since the last open, is NetworkController's own business (the
// HearthController<->NetworkController coupling, still to land - see
// planning/hearth-reference-player.md#a6-network-outputs-in-the-application),
// not this sink's.
using GroupResolver = std::function<std::shared_ptr<sendspin::Group>(const std::string& group_name)>;
[[nodiscard]] std::unique_ptr<NetworkGroupSink> make_group_sink(GroupResolver resolve);

}  // namespace ac3::hearth

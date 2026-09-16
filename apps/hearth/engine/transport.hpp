#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "output_decision.hpp"
#include "queue.hpp"

// The transport state machine (planning/hearth-reference-player.md, A3): play,
// pause, stop, next, previous and seek over the queue, and what the rest of
// the engine has to DO about each of them.
//
// Pure in the same way the output decision is, and for the same reason: it
// owns no thread, no device and no decoder. A command returns the new state
// plus the one action the caller should carry out, so every transition -
// including the ones that only happen at the end of a track, or when the
// queue changes underneath a playing item - is a case in
// tests/hearth/test_transport.cpp rather than something only reachable with a
// sound card and a stopwatch.
//
// Gapless lives here too, as the decision of whether the next item can join
// the output that is already open (the plan's Gapless playback section): the
// same rate and the same rendered width continue, and anything else reopens
// and says so. What that costs in samples at the join is the session's
// business - a raw elementary stream carries no encoder-delay field, so up to
// a frame of padding plus the 256-sample transform delay remains - and the
// note this returns is where the app gets the sentence to show.

namespace ac3::hearth {

enum class TransportState : std::uint8_t {
    kStopped,
    kPlaying,
    kPaused,
};

[[nodiscard]] std::string_view describe(TransportState state);

// What playback does when an item cannot be played (the Settings page's "An
// item fails"): move on to the next item that can, or stop at that one.
enum class FailurePolicy : std::uint8_t {
    kSkip,
    kStop,
};

[[nodiscard]] std::string_view describe(FailurePolicy policy);

// What the engine should do as a result of a command. One action per command:
// the state machine never asks for two things at once, which is what keeps
// the caller's side a switch rather than a script.
enum class TransportAction : std::uint8_t {
    // Nothing to do: a command that changed nothing (pause while paused), or
    // one the queue could not satisfy (next at the end).
    kNone,
    // Open the output for the current item and start it. The caller decides
    // the output with choose_output() and opens what it names.
    kStartItem,
    // The output is open and holds the finishing item's format: start the
    // next item into it without closing (gapless).
    kJoinItem,
    // Close the output and open it again for the next item, because the
    // format changed or gapless is off. `note` says which.
    kReopenForItem,
    kPauseOutput,
    kResumeOutput,
    // Stop and close. The queue keeps its current item, so play starts there
    // again rather than at the front.
    kStopOutput,
    // Seek within the current item, to `seek_to`.
    kSeekItem,
};

[[nodiscard]] std::string_view describe(TransportAction action);

struct TransportOutcome {
    TransportState state = TransportState::kStopped;
    TransportAction action = TransportAction::kNone;
    // The item the action is about, or Queue::kNone when it is about no item
    // (a pause, a stop).
    std::size_t item = Queue::kNone;
    // Where kSeekItem should land.
    std::chrono::milliseconds seek_to{0};
    // Empty unless there is something to say: why the output has to reopen,
    // why a command did nothing, why an item was skipped. Shown, and logged.
    std::string note{};
};

// What the output is carrying right now, as far as the transport needs to
// know: enough to decide whether the next item can join it.
struct OpenOutputFormat {
    std::uint32_t sample_rate = 0;
    // The rendered width the output was opened at, which for a local output
    // is the device's own channel count rather than the item's, and for a
    // bitstream the link's two.
    std::uint16_t channels = 0;
    OutputMode mode = OutputMode::kNone;
    // What a bitstream output carries on its link; nothing for a local one.
    std::optional<audio::BitstreamFormat> stream = std::nullopt;
};

class Transport {
public:
    // `queue` outlives this object; the transport does not own it, because
    // the queue is also the application's own list and is edited from
    // outside while playback runs.
    explicit Transport(Queue& queue) : queue_(&queue) {}

    [[nodiscard]] TransportState state() const { return state_; }
    [[nodiscard]] bool gapless() const { return gapless_; }
    void set_gapless(bool on) { gapless_ = on; }
    [[nodiscard]] bool repeat() const { return repeat_; }
    void set_repeat(bool on) { repeat_ = on; }
    [[nodiscard]] FailurePolicy on_failure() const { return on_failure_; }
    void set_on_failure(FailurePolicy policy) { on_failure_ = policy; }

    // What the output currently holds. The caller sets this when it opens or
    // reopens an output, and clears it when it closes one; the transport
    // reads it to decide whether a join is possible.
    void set_open_format(const OpenOutputFormat& format) { open_ = format; }
    void clear_open_format() { open_ = OpenOutputFormat{}; }
    [[nodiscard]] const OpenOutputFormat& open_format() const { return open_; }

    // The commands. Each returns what the caller should do.
    TransportOutcome play();
    TransportOutcome pause();
    TransportOutcome stop();
    TransportOutcome next();
    TransportOutcome previous();
    TransportOutcome seek(std::chrono::milliseconds to);

    // The current item has played to its end. This is the one event the
    // transport is told about rather than asked for, and it is where gapless
    // is decided. `next_mode` is the output the next item would be played
    // through, as the output decision has it; a join needs it to be the mode
    // already open. Unset, the next item is taken to want the open mode.
    TransportOutcome item_finished(std::optional<OutputMode> next_mode = std::nullopt);

    // `item`, which the caller has just marked unplayable, would not open.
    // Under kSkip this is item_finished(next_mode): playback moves on to the
    // next item that can play. Under kStop playback stops, with `item`
    // current so that the queue shows where and why; the caller lets what is
    // already submitted play out first when the item was the next one rather
    // than the one being started.
    TransportOutcome item_failed(std::size_t item,
                                 std::optional<OutputMode> next_mode = std::nullopt);

    // The queue changed under a playing item: the item that was playing is
    // gone. Restarts at whatever the queue now calls current, or stops when
    // the queue has emptied.
    TransportOutcome current_item_removed();

private:
    // The action that starts `item`, given what the output already holds and
    // the mode the item would be played through.
    [[nodiscard]] TransportOutcome start_or_join(std::size_t item, bool joining,
                                                 std::optional<OutputMode> mode = std::nullopt);

    Queue* queue_;
    TransportState state_ = TransportState::kStopped;
    bool gapless_ = true;
    bool repeat_ = false;
    FailurePolicy on_failure_ = FailurePolicy::kSkip;
    OpenOutputFormat open_;
};

}  // namespace ac3::hearth

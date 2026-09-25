#include "transport.hpp"

#include <fmt/format.h>

#include <utility>

// See transport.hpp. Every branch here is a case in
// tests/hearth/test_transport.cpp; the comments say what a person would
// expect of the transport rather than restating the code.

namespace ac3::hearth {

namespace {

// Every return below goes through this rather than a braced initialiser that
// names some of TransportOutcome's fields: a partial designated initialiser
// is a -Wmissing-designated-field-initializers error under this project's
// warning set (clang; MSVC accepts it), and naming all five fields at
// thirteen return sites reads worse than one factory does.
[[nodiscard]] TransportOutcome outcome(TransportState state, TransportAction action,
                                        std::size_t item = Queue::kNone, std::string note = {},
                                        std::chrono::milliseconds seek_to =
                                            std::chrono::milliseconds{0}) {
    return TransportOutcome{.state = state,
                            .action = action,
                            .item = item,
                            .seek_to = seek_to,
                            .note = std::move(note)};
}


// Whether an item's own format matches what the output is already carrying.
// The item's rate has to match; its channel count does NOT, because a local
// output is opened at the DEVICE's width and the renderer puts whatever the
// item carries onto that (A2's PcmOutput). What must not change under an
// open output is the width the output itself was opened at, and that follows
// from the device and the room's layout rather than from the item.
//
// A bitstream output is the stricter case: the sink was handed a format and a
// carrier rate, so a different rate, a different stream format - E-AC-3's
// link runs four times as fast as AC-3's - or a different mode entirely
// means a new stream to the sink.
[[nodiscard]] bool same_stream(const OpenOutputFormat& open, const ItemFacts& next,
                                OutputMode mode) {
    if (open.mode == OutputMode::kNone || open.mode != mode) {
        return false;
    }
    if (next.sample_rate == 0 || open.sample_rate == 0) {
        // Nothing probed this item yet, so nothing can be promised about the
        // join; reopening is the answer that cannot be wrong.
        return false;
    }
    if (open.sample_rate != next.sample_rate) {
        return false;
    }
    switch (mode) {
        case OutputMode::kBitstream:
            return next.stream.has_value() && next.stream == open.stream;
        case OutputMode::kBitstreamAsAc3:
            return next.stream == audio::BitstreamFormat::kEac3;
        case OutputMode::kLocalPcm:
        case OutputMode::kNetworkGroup:
        case OutputMode::kNone: return true;
    }
    return true;
}

[[nodiscard]] std::string_view stream_name(std::optional<audio::BitstreamFormat> stream) {
    if (!stream) {
        return "not a bitstream";
    }
    return audio::format_name(*stream);
}

// Why an item that did not join reopens the output, for the status line.
[[nodiscard]] std::string reopen_note(const OpenOutputFormat& open, const QueueItem& next,
                                      OutputMode mode) {
    if (open.mode != OutputMode::kNone && mode != open.mode) {
        return fmt::format("\"{}\" plays as {}, and the output is open for {}, so it reopens - "
                           "there is a gap.",
                           next.title, describe(mode), describe(open.mode));
    }
    if (open.sample_rate != 0 && next.facts.sample_rate != 0 &&
        open.sample_rate != next.facts.sample_rate) {
        return fmt::format(
            "\"{}\" is {} Hz and the output is open at {} Hz, so it reopens - there is a gap.",
            next.title, next.facts.sample_rate, open.sample_rate);
    }
    if (mode == OutputMode::kBitstream && open.stream && next.facts.stream != open.stream) {
        return fmt::format(
            "\"{}\" is {} and the output is carrying {}, so it reopens - there is a gap.",
            next.title, stream_name(next.facts.stream), stream_name(open.stream));
    }
    return fmt::format("The output reopens for \"{}\", so there is a gap.", next.title);
}

}  // namespace

std::string_view describe(TransportState state) {
    switch (state) {
        case TransportState::kStopped: return "stopped";
        case TransportState::kPlaying: return "playing";
        case TransportState::kPaused: return "paused";
    }
    return "unknown transport state";
}

std::string_view describe(FailurePolicy policy) {
    switch (policy) {
        case FailurePolicy::kSkip: return "skip to the next";
        case FailurePolicy::kStop: return "stop";
    }
    return "unknown failure policy";
}

std::string_view describe(TransportAction action) {
    switch (action) {
        case TransportAction::kNone: return "nothing";
        case TransportAction::kStartItem: return "start the item";
        case TransportAction::kJoinItem: return "join the item to the open output";
        case TransportAction::kReopenForItem: return "reopen the output for the item";
        case TransportAction::kPauseOutput: return "pause the output";
        case TransportAction::kResumeOutput: return "resume the output";
        case TransportAction::kStopOutput: return "stop the output";
        case TransportAction::kSeekItem: return "seek";
    }
    return "unknown transport action";
}

TransportOutcome Transport::start_or_join(std::size_t item, bool joining,
                                          std::optional<OutputMode> mode) {
    const QueueItem* const entry = item < queue_->size() ? &queue_->items()[item] : nullptr;
    if (entry == nullptr) {
        state_ = TransportState::kStopped;
        return outcome(state_, TransportAction::kStopOutput, Queue::kNone,
                       "Nothing left in the queue to play.");
    }
    if (!entry->playable()) {
        return outcome(state_, TransportAction::kNone, item,
                       fmt::format("\"{}\" cannot be played here: {}", entry->title,
                                   entry->facts.unplayable_because));
    }

    queue_->set_current(item);
    state_ = TransportState::kPlaying;

    if (!joining) {
        return outcome(state_, TransportAction::kStartItem, item);
    }
    if (!gapless_) {
        return outcome(state_, TransportAction::kReopenForItem, item,
                       "Gapless is off, so the output stops and starts again between items.");
    }
    // A join is only ever a continuation of what is playing: the mode the
    // output is in, unless the caller has said the item wants another.
    const OutputMode wanted = mode.value_or(open_.mode);
    if (same_stream(open_, entry->facts, wanted)) {
        return outcome(state_, TransportAction::kJoinItem, item);
    }
    return outcome(state_, TransportAction::kReopenForItem, item,
                   reopen_note(open_, *entry, wanted));
}

TransportOutcome Transport::play() {
    if (state_ == TransportState::kPaused) {
        state_ = TransportState::kPlaying;
        return outcome(state_, TransportAction::kResumeOutput, queue_->current_index());
    }
    if (state_ == TransportState::kPlaying) {
        return outcome(state_, TransportAction::kNone, queue_->current_index());
    }
    if (queue_->empty()) {
        return outcome(state_, TransportAction::kNone, Queue::kNone,
                       "The queue is empty. Add a file or a folder to play something.");
    }
    // Stopped: start where the queue says, which is where a stop left it
    // rather than the front.
    std::size_t item = queue_->current_index();
    if (item == Queue::kNone) {
        item = queue_->next_index(repeat_);
    }
    return start_or_join(item, /*joining=*/false);
}

TransportOutcome Transport::pause() {
    if (state_ != TransportState::kPlaying) {
        return outcome(state_, TransportAction::kNone, queue_->current_index());
    }
    state_ = TransportState::kPaused;
    return outcome(state_, TransportAction::kPauseOutput, queue_->current_index());
}

TransportOutcome Transport::stop() {
    const bool was_running = state_ != TransportState::kStopped;
    state_ = TransportState::kStopped;
    return outcome(state_,
                   was_running ? TransportAction::kStopOutput : TransportAction::kNone,
                   queue_->current_index());
}

TransportOutcome Transport::next() {
    const std::size_t item = queue_->next_index(repeat_);
    if (item == Queue::kNone) {
        if (state_ == TransportState::kStopped) {
            return outcome(state_, TransportAction::kNone, Queue::kNone,
                           "Nothing after this in the queue.");
        }
        state_ = TransportState::kStopped;
        return outcome(state_, TransportAction::kStopOutput, Queue::kNone,
                       "That was the last item in the queue.");
    }
    // Skipping forward by hand is not a gapless join: the current item is
    // being abandoned part-way, so whatever is queued for it has to go. The
    // caller's flush is what makes the skip immediate.
    const bool was_stopped = state_ == TransportState::kStopped;
    auto result = start_or_join(item, /*joining=*/false);
    if (!was_stopped && result.action == TransportAction::kStartItem) {
        result.action = TransportAction::kReopenForItem;
    }
    return result;
}

TransportOutcome Transport::previous() {
    const std::size_t item = queue_->previous_index(repeat_);
    if (item == Queue::kNone) {
        // At the front, "previous" restarts the current item, which is what
        // every other player does.
        if (queue_->current_index() == Queue::kNone) {
            return outcome(state_, TransportAction::kNone, Queue::kNone,
                           "Nothing before this in the queue.");
        }
        return seek(std::chrono::milliseconds{0});
    }
    const bool was_stopped = state_ == TransportState::kStopped;
    auto result = start_or_join(item, /*joining=*/false);
    if (!was_stopped && result.action == TransportAction::kStartItem) {
        result.action = TransportAction::kReopenForItem;
    }
    return result;
}

TransportOutcome Transport::seek(std::chrono::milliseconds to) {
    if (queue_->current_index() == Queue::kNone) {
        return outcome(state_, TransportAction::kNone, Queue::kNone,
                       "Nothing is playing to seek within.");
    }
    if (to < std::chrono::milliseconds{0}) {
        to = std::chrono::milliseconds{0};
    }
    // Seeking while stopped or paused is legal and does not start playback:
    // the position moves and the state stands.
    return outcome(state_, TransportAction::kSeekItem, queue_->current_index(), {}, to);
}

TransportOutcome Transport::item_finished(std::optional<OutputMode> next_mode) {
    if (state_ == TransportState::kStopped) {
        return outcome(state_, TransportAction::kNone);
    }
    const std::size_t item = queue_->next_index(repeat_);
    if (item == Queue::kNone) {
        state_ = TransportState::kStopped;
        return outcome(state_, TransportAction::kStopOutput, Queue::kNone,
                       "The queue has finished.");
    }
    return start_or_join(item, /*joining=*/true, next_mode);
}

bool Transport::would_join(std::optional<OutputMode> next_mode) const {
    if (state_ == TransportState::kStopped || !gapless_) {
        return false;
    }
    // The same answers start_or_join() gives, read rather than acted on; the
    // queue names only an item that can be played.
    const std::size_t item = queue_->next_index(repeat_);
    if (item >= queue_->size()) {
        return false;
    }
    return same_stream(open_, queue_->items()[item].facts, next_mode.value_or(open_.mode));
}

TransportOutcome Transport::item_failed(std::size_t item, std::optional<OutputMode> next_mode) {
    if (on_failure_ == FailurePolicy::kSkip) {
        return item_finished(next_mode);
    }
    // Stopped at the item, whatever was playing before it: the person asked
    // to be shown the failure rather than have it passed over.
    const bool was_running = state_ != TransportState::kStopped;
    state_ = TransportState::kStopped;
    const QueueItem* const entry = item < queue_->size() ? &queue_->items()[item] : nullptr;
    if (entry == nullptr) {
        return outcome(state_,
                       was_running ? TransportAction::kStopOutput : TransportAction::kNone,
                       Queue::kNone, "Nothing left in the queue to play.");
    }
    queue_->set_current(item);
    return outcome(state_, was_running ? TransportAction::kStopOutput : TransportAction::kNone,
                   item,
                   fmt::format("\"{}\" cannot be played here, so playback stops: {}",
                               entry->title, entry->facts.unplayable_because));
}

TransportOutcome Transport::current_item_removed() {
    if (queue_->empty()) {
        const bool was_running = state_ != TransportState::kStopped;
        state_ = TransportState::kStopped;
        return outcome(state_,
                       was_running ? TransportAction::kStopOutput
                                   : TransportAction::kNone,
                       Queue::kNone, "The queue was emptied.");
    }
    if (state_ == TransportState::kStopped) {
        return outcome(state_, TransportAction::kNone, queue_->current_index());
    }
    // Something else is current now (Queue::remove leaves the next item
    // there): play that, through a reopen rather than a join, since the item
    // that was playing stopped mid-stream.
    auto result = start_or_join(queue_->current_index(), /*joining=*/false);
    if (result.action == TransportAction::kStartItem) {
        result.action = TransportAction::kReopenForItem;
        result.note = "The item that was playing was removed from the queue.";
    }
    return result;
}

}  // namespace ac3::hearth

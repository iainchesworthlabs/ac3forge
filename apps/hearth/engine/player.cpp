#include "player.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <span>
#include <utility>

// See player.hpp. The transport decides and this carries it out; every
// judgement about WHAT should happen is transport.cpp's, and this file is
// about doing it without losing or duplicating a frame.

namespace ac3::hearth {

namespace {

// The pending ring's first size: more blocks than one pump at the default
// budget plus one access unit's worth, so it rarely has to grow.
constexpr std::size_t kInitialPendingBlocks = 32;

}  // namespace

Player::Player(std::unique_ptr<PcmSink> sink, ItemLoader loader, const render::OutputLayout& layout,
               const DecoderSettings& settings)
    : sink_(std::move(sink)), loader_(std::move(loader)), layout_(layout), settings_(settings) {}

void Player::set_decoder_settings(const DecoderSettings& settings) {
    if (settings == settings_) {
        return;
    }
    if (settings.programme != settings_.programme) {
        // A prepared session holds the old programme's units.
        prepared_.reset();
        prepared_index_ = Queue::kNone;
    }
    settings_ = settings;
    if (!session_ || !decoder_) {
        // Nothing is being decoded: the next item to start builds its decoder
        // with these.
        decoder_.reset();
        return;
    }
    const StreamDecoder::BlockFn deliver =
        [this](std::span<const std::span<const float>> rendered, std::size_t n) {
            take_block(rendered, n);
        };
    session_->hand_over(*decoder_, deliver);
    build_decoder(decoder_rate_);
}

void Player::build_decoder(std::uint32_t rate) {
    decoder_.emplace(layout_, rate, settings_);
    decoder_rate_ = rate;
}

TransportOutcome Player::play() {
    TransportOutcome outcome = transport_.play();
    perform(outcome, nullptr);
    return outcome;
}

TransportOutcome Player::pause() {
    TransportOutcome outcome = transport_.pause();
    perform(outcome, nullptr);
    return outcome;
}

TransportOutcome Player::stop() {
    TransportOutcome outcome = transport_.stop();
    perform(outcome, nullptr);
    return outcome;
}

TransportOutcome Player::next() {
    TransportOutcome outcome = transport_.next();
    perform(outcome, nullptr);
    return outcome;
}

TransportOutcome Player::previous() {
    TransportOutcome outcome = transport_.previous();
    perform(outcome, nullptr);
    return outcome;
}

TransportOutcome Player::seek(std::chrono::milliseconds to) {
    TransportOutcome outcome = transport_.seek(to);
    perform(outcome, nullptr);
    return outcome;
}

void Player::perform(const TransportOutcome& outcome, PumpReport* report) {
    if (report != nullptr && !outcome.note.empty()) {
        report->note = outcome.note;
    }
    switch (outcome.action) {
        case TransportAction::kNone:
        case TransportAction::kJoinItem:  // item_ended() swaps the session in itself
            break;
        case TransportAction::kStartItem:
        case TransportAction::kReopenForItem: {
            // A command, or a reopen whose old audio has already been heard:
            // either way the output starts afresh for this item.
            after_drain_.reset();
            close_output();
            const OpenFailure failure = open_output_for(outcome.item, report);
            if (failure == OpenFailure::kItem && outcome.item < queue_.size()) {
                // The item could not be played. It is marked so the transport
                // skips it from now on, and playback moves past it - which
                // ends, since a queue of nothing playable has no next item.
                ItemFacts facts = queue_.items()[outcome.item].facts;
                facts.unplayable_because = last_error_;
                queue_.set_facts(outcome.item, std::move(facts));
                perform(transport_.item_finished(), report);
            } else if (failure == OpenFailure::kOutput) {
                // The device would not open. Nothing in the queue is at
                // fault, so playback stops and the reason is kept.
                perform(transport_.stop(), report);
                if (report != nullptr) {
                    report->note = last_error_;
                }
            }
            break;
        }
        case TransportAction::kPauseOutput:
            if (sink_->is_open()) {
                sink_->pause();
            }
            break;
        case TransportAction::kResumeOutput:
            if (sink_->is_open()) {
                sink_->resume();
            }
            break;
        case TransportAction::kStopOutput:
            // A seek kept for the next start survives a stop: stopping and
            // then choosing where to start is the ordinary way to use one.
            after_drain_.reset();
            close_output();
            session_.reset();
            prepared_.reset();
            prepared_index_ = Queue::kNone;
            if (report != nullptr) {
                report->stopped = true;
            }
            break;
        case TransportAction::kSeekItem:
            if (session_ && decoder_) {
                session_->seek(outcome.seek_to, *decoder_);
                // What was decoded and submitted for the old position must
                // not be heard after the new one; the sink's counts restart
                // with the flush, and so do ours.
                clear_pending();
                if (sink_->is_open()) {
                    sink_->flush();
                }
                submitted_since_open_ = 0;
            } else if (outcome.item != Queue::kNone) {
                seek_on_start_ = SeekOnStart{.item = outcome.item, .to = outcome.seek_to};
            }
            break;
    }
}

bool Player::start_session(std::size_t item) {
    if (item >= queue_.size()) {
        prepared_.reset();
        prepared_index_ = Queue::kNone;
        last_error_ = "That item is no longer in the queue.";
        return false;
    }
    // The prepared session is only this item's if the queue has not been
    // edited under it since: an index names a place in the list, and the
    // path is what says the same file is still there.
    const bool prepared_here = prepared_ && prepared_index_ == item &&
                               prepared_path_ == queue_.items()[item].path;
    if (prepared_here) {
        session_ = std::move(prepared_);
        prepared_.reset();
        prepared_index_ = Queue::kNone;
        return true;
    }
    prepared_.reset();
    prepared_index_ = Queue::kNone;
    auto opened = Session::open(queue_.items()[item].path, loader_, settings_.programme);
    if (!opened) {
        last_error_ = std::move(opened.error());
        return false;
    }
    queue_.set_facts(item, opened->facts());
    session_ = std::move(*opened);
    return true;
}

void Player::apply_seek_on_start(std::size_t item) {
    // A seek made on this item lands before its first unit is decoded; one
    // made on any other item is stale now that this one is starting.
    if (seek_on_start_ && seek_on_start_->item == item && session_ && decoder_) {
        session_->seek(seek_on_start_->to, *decoder_);
    }
    seek_on_start_.reset();
}

Player::OpenFailure Player::open_output_for(std::size_t item, PumpReport* report) {
    if (!start_session(item)) {
        if (report != nullptr) {
            report->note = last_error_;
        }
        return OpenFailure::kItem;
    }
    const std::uint32_t rate = session_->facts().sample_rate;
    if (!decoder_ || decoder_rate_ != rate) {
        build_decoder(rate);
    } else {
        decoder_->reset();
    }
    const auto opened = sink_->open(PcmSink::Format{.sample_rate = rate, .layout = layout_});
    if (!opened) {
        last_error_ = opened.error();
        session_.reset();
        if (report != nullptr) {
            report->note = last_error_;
        }
        return OpenFailure::kOutput;
    }
    ++opens_;
    submitted_since_open_ = 0;
    transport_.set_open_format(*opened);
    clear_pending();
    apply_seek_on_start(item);
    history_.push_back(PlayedItem{.queue_index = item,
                                  .title = queue_.items()[item].title,
                                  .first_frame = 0,
                                  .frames = 0,
                                  .expected_frames = session_->total_samples(),
                                  .output_opens = opens_});
    if (report != nullptr) {
        report->item_started = true;
        report->output_reopened = opens_ > 1;
        if (!session_->facts().note.empty()) {
            report->note = session_->facts().note;
        }
    }
    return OpenFailure::kNone;
}

void Player::close_output() {
    if (sink_->is_open()) {
        sink_->close();
    }
    transport_.clear_open_format();
    clear_pending();
    drain_target_.reset();
    submitted_since_open_ = 0;
}

Player::Pending& Player::push_block() {
    if (pending_count_ == pending_.size()) {
        // Full: turn the ring so its oldest block comes first, then grow it
        // at the end. The blocks keep their buffers through both.
        std::rotate(pending_.begin(),
                    std::next(pending_.begin(), static_cast<std::ptrdiff_t>(pending_head_)),
                    pending_.end());
        pending_head_ = 0;
        pending_.resize(pending_.empty() ? kInitialPendingBlocks : pending_.size() * 2);
    }
    Pending& block = pending_[(pending_head_ + pending_count_) % pending_.size()];
    ++pending_count_;
    return block;
}

void Player::clear_pending() {
    pending_head_ = 0;
    pending_count_ = 0;
    pending_frames_ = 0;
}

void Player::take_block(std::span<const std::span<const float>> rendered, std::size_t n) {
    if (n == 0 || history_.empty()) {
        return;
    }
    const std::size_t slots = layout_.slots();
    Pending& block = push_block();
    block.frames = n;
    block.record = history_.size() - 1;
    // A reused buffer is as large as the largest block it has held, so this
    // only allocates while the ring is new.
    block.samples.resize(slots * n);
    for (std::size_t slot = 0; slot < slots; ++slot) {
        const auto out = std::next(block.samples.begin(), static_cast<std::ptrdiff_t>(slot * n));
        if (slot < rendered.size()) {
            std::copy_n(rendered[slot].begin(), n, out);
        } else {
            std::fill_n(out, n, 0.0F);
        }
    }
    pending_frames_ += n;
}

void Player::fill(std::size_t frames) {
    if (!session_ || !decoder_ || history_.empty()) {
        return;
    }
    // Built once per call, capturing one pointer, so the std::function holds
    // it without allocating.
    const StreamDecoder::BlockFn deliver =
        [this](std::span<const std::span<const float>> rendered, std::size_t n) {
            take_block(rendered, n);
        };
    while (pending_frames_ < frames && !session_->finished()) {
        const auto got = session_->render(*decoder_, deliver, frames - pending_frames_);
        if (!got) {
            // One undecodable unit: say so and carry on with the next. The
            // session has already stepped past it.
            last_error_ = got.error();
        }
    }
}

std::size_t Player::drain(std::size_t budget) {
    const std::size_t slots = layout_.slots();
    std::array<std::span<const float>, render::OutputLayout::kMaxSlots> views{};
    std::size_t submitted = 0;
    while (pending_count_ != 0 && submitted < budget) {
        const Pending& block = pending_[pending_head_];
        for (std::size_t slot = 0; slot < slots; ++slot) {
            views[slot] = std::span<const float>(block.samples).subspan(slot * block.frames,
                                                                         block.frames);
        }
        if (!sink_->submit(std::span<const std::span<const float>>(views.data(), slots),
                           block.frames)) {
            break;
        }
        if (block.record < history_.size()) {
            PlayedItem& played = history_[block.record];
            if (played.frames == 0) {
                played.first_frame = submitted_since_open_;
            }
            played.frames += block.frames;
        }
        submitted += block.frames;
        submitted_since_open_ += block.frames;
        pending_frames_ -= block.frames;
        pending_head_ = (pending_head_ + 1) % pending_.size();
        --pending_count_;
    }
    return submitted;
}

bool Player::played_out() {
    if (pending_count_ != 0) {
        return false;
    }
    const auto position = sink_->position();
    if (!position) {
        // Closed, or a sink with no clock to wait on.
        return true;
    }
    if (!drain_target_) {
        // Taken once, when the last block has gone in: the device-clock
        // frame by which the last frame submitted will have been heard -
        // everything the clock has run through, everything still held, and
        // the output path's own delay. Not "as many frames played as were
        // submitted": the clock also runs through the silence an underrun
        // inserts, so that test would close the output early by however much
        // silence there had been. The two counts behind the reading are taken
        // without a lock, so it can be short by up to one device period
        // (ac3/audio/playback_counter.hpp).
        drain_target_ =
            position->frames_played + position->frames_queued + position->latency_frames;
    }
    return position->frames_played >= *drain_target_;
}

void Player::item_ended(PumpReport& report) {
    // Open the next item before asking the transport, so its join decision
    // sees the item's real rate rather than "not probed yet", which would
    // force a reopen on every item the player had not read ahead of time.
    // An item that will not open is marked, which takes it out of
    // next_index()'s answer, and the one after it is tried - so a run of
    // unreadable items between two good ones still ends in a join. Each pass
    // marks one more item, so this ends.
    for (;;) {
        const std::size_t next = queue_.next_index(transport_.repeat());
        if (next == Queue::kNone) {
            break;
        }
        const std::string path = queue_.items()[next].path;
        if (prepared_ && prepared_index_ == next && prepared_path_ == path) {
            break;
        }
        prepared_.reset();
        prepared_index_ = Queue::kNone;
        auto opened = Session::open(path, loader_, settings_.programme);
        if (opened) {
            queue_.set_facts(next, opened->facts());
            prepared_ = std::move(*opened);
            prepared_index_ = next;
            prepared_path_ = path;
            break;
        }
        ItemFacts facts = queue_.items()[next].facts;
        facts.unplayable_because = opened.error();
        queue_.set_facts(next, std::move(facts));
        report.note = std::move(opened.error());
    }

    const TransportOutcome outcome = transport_.item_finished();
    if (!outcome.note.empty()) {
        report.note = outcome.note;
    }
    switch (outcome.action) {
        case TransportAction::kJoinItem: {
            // The next item follows into the open output. The decoder was
            // reset by the finished session's last render(); the output, and
            // everything already queued for it, carries on.
            const std::uint32_t rate = transport_.open_format().sample_rate;
            if (!start_session(outcome.item)) {
                // Prepared above, so this is a queue edited in between and
                // an item that no longer opens. Marked, and the transport is
                // asked again from it - each pass marks one more item, so
                // this ends, in a join, a reopen or a stop.
                if (outcome.item < queue_.size()) {
                    ItemFacts facts = queue_.items()[outcome.item].facts;
                    facts.unplayable_because = last_error_;
                    queue_.set_facts(outcome.item, std::move(facts));
                }
                session_.reset();
                report.note = last_error_;
                item_ended(report);
                return;
            }
            if (!decoder_ || decoder_rate_ != rate) {
                build_decoder(rate);
            }
            apply_seek_on_start(outcome.item);
            history_.push_back(PlayedItem{.queue_index = outcome.item,
                                          .title = queue_.items()[outcome.item].title,
                                          .first_frame = 0,
                                          .frames = 0,
                                          .expected_frames = session_->total_samples(),
                                          .output_opens = opens_});
            report.item_started = true;
            if (!session_->facts().note.empty()) {
                report.note = session_->facts().note;
            }
            break;
        }
        case TransportAction::kReopenForItem:
        case TransportAction::kStopOutput:
            // What has already been submitted plays out first; pump() carries
            // the decision out once the sink's clock has passed it.
            after_drain_ = outcome;
            drain_target_.reset();
            session_.reset();
            break;
        default:
            session_.reset();
            break;
    }
}

PumpReport Player::pump(std::size_t budget) {
    PumpReport report;
    if (after_drain_) {
        report.frames_submitted += drain(budget);
        if (played_out()) {
            const TransportOutcome outcome = *after_drain_;
            after_drain_.reset();
            perform(outcome, &report);
        }
        return report;
    }
    if (transport_.state() != TransportState::kPlaying || !session_) {
        return report;
    }
    fill(budget);
    report.frames_submitted += drain(budget);
    if (session_ && session_->finished()) {
        item_ended(report);
        if (session_ && report.item_started) {
            // A join: the next item's first blocks go in behind the last
            // item's tail straight away, so the sink never waits on a gap
            // the output does not have.
            fill(budget);
            report.frames_submitted += drain(budget > report.frames_submitted
                                                 ? budget - report.frames_submitted
                                                 : 0);
        }
    }
    return report;
}

}  // namespace ac3::hearth

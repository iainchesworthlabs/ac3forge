#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ac3/render/layout.hpp"
#include "decoder_settings.hpp"
#include "pcm_sink.hpp"
#include "queue.hpp"
#include "session.hpp"
#include "stream_decoder.hpp"
#include "transport.hpp"

// The player (planning/hearth-reference-player.md, A3): the queue, the
// transport, one session at a time, and a PCM sink, put together.
//
// The transport decides; this carries the decision out. Each command is
// forwarded to the transport, and the one action it returns is performed on
// the sink and the sessions. Audio moves only in pump(), which the
// application calls from its engine thread whenever the sink has room and a
// test calls in a loop against a fake device - so nothing here owns a thread,
// and a whole queue can be played deterministically, one pump at a time.
//
// Gapless, as the plan's section defines it: when the current item has
// delivered its last frame, the next item is opened straight away - before
// the transport is told the item finished, so the transport's join decision
// sees the next item's real rate rather than "not probed yet" - and if the
// transport says join, the next item's first frame follows the last one's
// with nothing between them and nothing reopened. If it says reopen, the
// player waits for everything already submitted to be heard (by the sink's
// own clock), then closes and opens the output at the new format. The record
// of where each item began in the output's timeline and how many frames it
// delivered is kept, which is what A3's exit checks at every join.

namespace ac3::hearth {

// One item's playback, as the output saw it.
struct PlayedItem {
    std::size_t queue_index = Queue::kNone;
    std::string title{};
    // Where in the output's timeline the item's first frame went: frames
    // submitted since the output was last opened.
    std::uint64_t first_frame = 0;
    // What it delivered, and what its access units code. Equal for an item
    // that played to its end from its start.
    std::uint64_t frames = 0;
    std::uint64_t expected_frames = 0;
    // How many times the output had been opened when this item started: the
    // same figure for two items means the second joined the first.
    std::uint32_t output_opens = 0;
};

// Where the item being heard has got to.
struct PlayPosition {
    // The queue index of the item the device is playing now, or Queue::kNone.
    std::size_t item = Queue::kNone;
    // How much of it has been heard, by the device's clock less its output
    // path's delay, and how long it is.
    std::chrono::milliseconds heard{0};
    std::chrono::milliseconds duration{0};
};

// What one pump() did.
struct PumpReport {
    std::size_t frames_submitted = 0;
    bool item_started = false;
    bool output_reopened = false;
    // The queue ran out and the output was closed.
    bool stopped = false;
    // Anything the transport or a session had to say, for the status line.
    std::string note{};
};

class Player {
public:
    // `layout` is what every item is rendered onto; the sink places its slots.
    Player(std::unique_ptr<PcmSink> sink, ItemLoader loader, const render::OutputLayout& layout,
           const DecoderSettings& settings = {});

    // The transport holds the queue's address, so a player stays where it
    // was made.
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;
    Player(Player&&) = delete;
    Player& operator=(Player&&) = delete;
    ~Player() = default;

    // The queue, for a caller that edits it before playback starts or reads
    // it. Edits while playing go through the functions below, which keep the
    // transport and what is already decoded in step with the list.
    [[nodiscard]] Queue& queue() { return queue_; }
    [[nodiscard]] const Queue& queue() const { return queue_; }

    // Queue edits, kept consistent with playback. Removing the item that is
    // playing restarts at whatever the queue then calls current, or stops
    // with nothing left; a reopen still waiting for the old item to be heard
    // follows its item to wherever an edit moved it; and the history's queue
    // indices follow their items too (kNone once an item is removed).
    void add(QueueItem item);
    void insert(std::size_t index, QueueItem item);
    void remove(std::size_t index);
    bool move(std::size_t from, std::size_t to);
    void clear();
    // Plays `index` from its start, as choosing it in the list does.
    TransportOutcome play_item(std::size_t index);

    // Where the item the device is playing has got to. Follows the device's
    // own clock across joins and seeks.
    [[nodiscard]] PlayPosition position() const;
    [[nodiscard]] const Transport& transport() const { return transport_; }
    void set_gapless(bool on) { transport_.set_gapless(on); }
    void set_repeat(bool on) { transport_.set_repeat(on); }

    // What every item is decoded with. A change reaches the playing item at
    // its next unit: the audio already decoded plays out as it was, and a new
    // decoder, primed with the unit before, carries on - nothing lost,
    // nothing repeated, nothing to hear at the change beyond the change
    // itself. A different programme takes effect when an item next starts,
    // since it is a different list of units.
    void set_decoder_settings(const DecoderSettings& settings);
    [[nodiscard]] const DecoderSettings& decoder_settings() const { return settings_; }

    TransportOutcome play();
    TransportOutcome pause();
    TransportOutcome stop();
    TransportOutcome next();
    TransportOutcome previous();
    // With nothing open - stopped, or waiting for one item to be heard out
    // before the next one's output opens - the position is kept for the item
    // the transport names and applied when that item starts, as the
    // transport promises; a different item starting drops it.
    TransportOutcome seek(std::chrono::milliseconds to);

    // Moves rendered audio into the sink until it will take no more or
    // nothing more is ready, and moves on to the next item when the current
    // one has delivered everything. `budget` bounds the frames one call
    // submits, so a caller on a real-time thread keeps its own cadence.
    PumpReport pump(std::size_t budget = 4800);

    // Whether pump() has anything to do: an output is open, or a reopen or a
    // stop is waiting for the audio already submitted to be heard.
    [[nodiscard]] bool active() const { return after_drain_.has_value() || sink_->is_open(); }

    [[nodiscard]] const std::vector<PlayedItem>& history() const { return history_; }
    [[nodiscard]] std::uint32_t output_opens() const { return opens_; }
    [[nodiscard]] const std::string& last_error() const { return last_error_; }

private:
    // One rendered block waiting for room in the sink.
    struct Pending {
        std::vector<float> samples{};  // planar: slot 0's frames, then slot 1's, ...
        std::size_t frames = 0;
        // The history_ entry these frames belong to. At a join the old
        // item's tail and the new item's head sit in the queue together, and
        // each block counts towards its own item.
        std::size_t record = 0;
    };

    // Why an item could not be started: the item itself, which is then
    // skipped, or the output, which stops playback - an item is not
    // unplayable because the device refused to open.
    enum class OpenFailure : std::uint8_t { kNone, kItem, kOutput };

    // A seek made while nothing was open, and the item it was made on.
    struct SeekOnStart {
        std::size_t item = Queue::kNone;
        std::chrono::milliseconds to{0};
    };

    // From output frame `output_start` on - counted since the output was
    // last opened or flushed - the output plays history_[record]'s item from
    // its own frame `item_start`. What position() reads the clock against.
    struct Segment {
        std::size_t record = 0;
        std::uint64_t output_start = 0;
        std::uint64_t item_start = 0;
    };

    // Carries out what the transport decided.
    void perform(const TransportOutcome& outcome, PumpReport* report);
    OpenFailure open_output_for(std::size_t item, PumpReport* report);
    bool start_session(std::size_t item);
    void apply_seek_on_start(std::size_t item);
    void close_output();

    // The pending blocks, oldest first, as a ring whose blocks are never
    // freed: each keeps its buffer for the next block to reuse, so steady
    // playback allocates nothing per block.
    Pending& push_block();
    void clear_pending();

    // Takes one rendered block into the pending ring, for the current item.
    void take_block(std::span<const std::span<const float>> rendered, std::size_t n);
    // Builds the decoder for `rate` from the current settings.
    void build_decoder(std::uint32_t rate);
    // Decodes into the pending blocks until they hold at least `frames`.
    void fill(std::size_t frames);
    // Submits pending blocks while the sink takes them.
    std::size_t drain(std::size_t budget);
    // Whether everything submitted since the output opened has been heard.
    [[nodiscard]] bool played_out();
    // The current item has delivered everything: decide what comes next.
    void item_ended(PumpReport& report);
    // After a queue edit: a waiting reopen goes to the item now current, and
    // a prepared session, keyed by index, is dropped.
    void after_edit();
    // Moves the history's queue indices the way an edit moved the items.
    void remap_history(const std::function<std::size_t(std::size_t)>& moved);

    std::unique_ptr<PcmSink> sink_;
    ItemLoader loader_;
    render::OutputLayout layout_;
    DecoderSettings settings_;
    Queue queue_;
    Transport transport_{queue_};

    std::optional<Session> session_;
    std::optional<StreamDecoder> decoder_;
    // The rate decoder_ was built for: its renderer's small-speaker
    // crossover depends on it, so a new rate means a new decoder.
    std::uint32_t decoder_rate_ = 0;

    std::vector<Pending> pending_;
    std::size_t pending_head_ = 0;
    std::size_t pending_count_ = 0;
    std::size_t pending_frames_ = 0;

    // The next item, opened ahead of the join decision, and where it was in
    // the queue - the index and the path both, since the list can be edited
    // while a reopen waits for the old item to be heard.
    std::optional<Session> prepared_;
    std::size_t prepared_index_ = Queue::kNone;
    std::string prepared_path_;

    // A reopen or a stop waiting for the audio already submitted to play,
    // and the device-clock frame by which it will have (played_out()).
    std::optional<TransportOutcome> after_drain_;
    std::optional<std::uint64_t> drain_target_;

    std::optional<SeekOnStart> seek_on_start_;

    std::uint64_t submitted_since_open_ = 0;
    std::uint32_t opens_ = 0;
    std::vector<PlayedItem> history_;
    std::vector<Segment> segments_;
    std::string last_error_;
};

}  // namespace ac3::hearth

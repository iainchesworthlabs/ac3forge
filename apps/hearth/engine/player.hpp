#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/iec61937/iec61937.hpp"
#include "ac3/render/identify.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/render/routing.hpp"
#include "ac3/render/trim_delay.hpp"
#include "ac3_transcoder.hpp"
#include "bitstream_sink.hpp"
#include "decoder_settings.hpp"
#include "diagnostic_log.hpp"
#include "output_decision.hpp"
#include "pcm_sink.hpp"
#include "play_meters.hpp"
#include "queue.hpp"
#include "session.hpp"
#include "stream_decoder.hpp"
#include "transport.hpp"
#include "unit_reports.hpp"

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
// transport would join it, the next item's first frame follows the last
// one's with nothing between them and nothing reopened. Otherwise - another
// format, or nothing next at all - the player waits for everything already
// submitted to be heard (by the sink's own clock) before telling the
// transport, so until the item's tail has been heard it is still the item
// playing: a pause or a seek there is the item's, and an item added to the
// queue meanwhile can still join. Then a reopen closes and opens the output
// at the new format, and a stop closes it. The record of where each item
// began in the output's timeline and how many frames it delivered is kept,
// which is what A3's exit checks at every join.
//
// Given a diagnostics ring (diagnostic_log.hpp), the player notes what it did
// and could not do: each output opened and closed, each item started, joined
// or refused, and units that would not decode - the first with its reason,
// the rest as a count once the item is done with, so a damaged file writes
// two lines rather than one per unit.
//
// Each item plays the way the output decision says (output_decision.hpp),
// asked when the item starts and again before the next one joins. A
// bitstream output is sent the item's access units, packed into IEC 61937
// bursts, from the same units the decoder is given: the decode still runs,
// for the meters and the unit reports, which are released by the
// bitstream sink's clock as they are by a PCM sink's. Only whole units can be
// sent, so a bitstreamed item plays every sample of the units its part of
// the stream touches (Session::play_whole_units()), and the decoder settings
// reach the meters but not the receiver, which decodes with its own.
//
// An item transcoded to AC-3 for a receiver that takes nothing newer is
// decoded onto 5.1 with neutral settings instead, and those blocks are what
// the meters measure and what Ac3Transcoder encodes. The link carries the
// encoder's frames, whose output runs its 256-sample delay behind the decode,
// so everything on the link is placed that much later; the decode can cut,
// so the item plays exactly its part and a join is seamless through the one
// encoder. What the encoder still holds when the output plays out or reopens
// is padded out and sent first.

namespace ac3::hearth {

// Each item's output, from its facts and the output the player holds open
// while asking (OutputSelector::choose()).
using OutputChooser = std::function<OutputChoice(const ItemFacts& item, const HeldOutput& held)>;

// What a player plays through.
struct PlayerOutputs {
    // The local PCM output; with none, nothing is decoded to a device.
    std::unique_ptr<PcmSink> pcm{};
    // The passthrough output; with none, nothing is bitstreamed.
    std::unique_ptr<BitstreamSink> bitstream{};
    // Unset, every item is decoded to `pcm`.
    OutputChooser choose{};
};

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
    // `diagnostics`, when given, outlives the player.
    Player(std::unique_ptr<PcmSink> sink, ItemLoader loader, const render::OutputLayout& layout,
           const DecoderSettings& settings = {}, DiagnosticLog* diagnostics = nullptr);
    // With a choice of outputs for each item.
    Player(PlayerOutputs outputs, ItemLoader loader, const render::OutputLayout& layout,
           const DecoderSettings& settings = {}, DiagnosticLog* diagnostics = nullptr);

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
    // Stops whatever plays and makes `index` current without starting it, so
    // that the next play() starts there (and a seek made now is kept for
    // that start). False, changing nothing, for an index out of range.
    bool select(std::size_t index);

    // Where the item the device is playing has got to. Follows the device's
    // own clock across joins and seeks.
    [[nodiscard]] PlayPosition position() const;

    // The newest meter snapshot the device's clock has reached since the last
    // call, copied into `latest`; false when there is none. The meters run on
    // the output's slots; see play_meters.hpp for what starts again when.
    [[nodiscard]] bool meters(MeterSnapshot& latest);
    // The report of the newest unit the device's clock has reached since the
    // last call, copied into `latest`; false when there is none
    // (unit_reports.hpp).
    [[nodiscard]] bool unit_report(UnitReport& latest);
    [[nodiscard]] const Transport& transport() const { return transport_; }
    void set_gapless(bool on) { transport_.set_gapless(on); }
    void set_repeat(bool on) { transport_.set_repeat(on); }
    // Whether an item that cannot be played is passed over, or stops
    // playback at that item once what was submitted before it has played.
    void set_on_failure(FailurePolicy policy) { transport_.set_on_failure(policy); }

    // What every item is decoded with. A change reaches the playing item at
    // its next unit: the audio already decoded plays out as it was, and a new
    // decoder, primed with the unit before, carries on - nothing lost,
    // nothing repeated, nothing to hear at the change beyond the change
    // itself. A different programme takes effect when an item next starts,
    // since it is a different list of units.
    void set_decoder_settings(const DecoderSettings& settings);
    [[nodiscard]] const DecoderSettings& decoder_settings() const { return settings_; }
    // What every item is rendered onto. Fixed for this player's lifetime -
    // there is no set_layout() yet; changing the layout while playing needs
    // the output reopened at the new width, which A5's Speakers page defers
    // (planning/hearth-reference-player.md).
    [[nodiscard]] const render::OutputLayout& layout() const { return layout_; }
    // Why the decoder settings are not what the listener hears, or empty
    // when they are: a bitstream is decoded by the receiver.
    [[nodiscard]] std::string_view settings_note() const;

    // The speaker setup (planning/hearth-reference-player.md, A5's Speakers
    // page): the per-slot trim and delay applied to the renderer's slots
    // before they reach the open sink, local or not. Routing itself lives on
    // the sink (PcmSink::set_routing()/routing()): ac3::audio::PcmOutput
    // already carries a patch and builds a sensible default from the
    // device's own speaker mask, so there is nothing for the player to add
    // there. Trim and delay are the player's own instead, so one TrimDelay
    // covers whichever sink is open.
    //
    // Every setting is keyed by the render layout's OWN slot (0 is always
    // this Player's first coded-channel slot), not the sink's output - the
    // routing patch is what may send slot 0 to output 5. Settings survive a
    // rate change: a new decoder at a new rate keeps them, converted to that
    // rate's sample counts. False, changing nothing: a slot at or past
    // layout().slots(), a trim TrimDelay::set_trim_db() itself refuses, a
    // delay past kMaxDelayMs, or a frequency
    // render::LayoutRenderer::set_crossover_hz() itself refuses.
    //
    // 40 ms is room-scale: about 14 metres of path-length difference at the
    // speed of sound, well past what a domestic room's largest speaker
    // asymmetry needs, with headroom over the figures an AVR's own manual
    // typically quotes (Onkyo and Denon both stop their distance setting
    // well under that). TrimDelay's storage is outputs * max_delay_samples
    // floats, rebuilt only on a rate change, so the cost of headroom here is
    // a few hundred kilobytes at 96 kHz, not a per-block one.
    static constexpr double kMaxDelayMs = 40.0;

    bool set_trim_db(std::size_t slot, double db);
    bool set_delay_ms(std::size_t slot, double ms);
    [[nodiscard]] double trim_db(std::size_t slot) const;
    [[nodiscard]] double delay_ms(std::size_t slot) const;
    bool set_crossover_hz(double hz);
    [[nodiscard]] double crossover_hz() const { return crossover_hz_; }

    // The identify tone (planning/hearth-reference-player.md, A5's Speakers
    // page): pink noise on one render layout slot at a time, in place of
    // whatever the item playing would put there - take_block() overwrites
    // the block entirely (render::IdentifyTone::fill()'s own behaviour:
    // silence on every other slot too), ahead of trim_delay_, which does
    // not apply to it - the level here is the identify tone's own, not the
    // room's speaker trim. A slot of Speaker::Kind::kLfe gets the low band
    // (30-80 Hz) automatically, never the full band, the way a subwoofer
    // feed's own test tone needs. The level is a setting, like crossover_hz,
    // and survives a rate change (reapplied to the rebuilt generator by
    // reconfigure_identify()); which slot is sounding it is a bare index
    // with nothing rate-dependent to rebuild, so it needs no such handling.
    // False, changing nothing:
    // identify_start() for a slot at or past layout().slots(),
    // set_identify_level_db() for a level outside render::IdentifyTone's own
    // [kMinLevelDb, kMaxLevelDb].
    bool identify_start(std::size_t slot);
    void identify_stop();
    // Queue::kNone while nothing is sounding the tone.
    [[nodiscard]] std::size_t identify_slot() const { return identify_slot_; }
    bool set_identify_level_db(double db);
    [[nodiscard]] double identify_level_db() const { return identify_level_db_; }

    // The transport bar's master volume - not the per-speaker trim above,
    // which matches speakers to each other: this is the one overall
    // listening level every artboard's footer shows, applied in take_block()
    // after render and after trim/delay, the last thing done to a block
    // before it joins the pending ring. Local PCM only, the same scope trim
    // and delay already have: a bitstream or a transcode is decoded by the
    // receiver, which has its own volume, so neither reaches this gain
    // (take_block()'s own early returns for both apply here too). 0 dB is
    // unity - what a fresh player already sends - so there is no headroom to
    // add above it, only to attenuate.
    static constexpr double kMinVolumeDb = -60.0;
    static constexpr double kMaxVolumeDb = 0.0;
    bool set_volume_db(double db);
    [[nodiscard]] double volume_db() const { return volume_db_; }

    // The routing patch, and what the local device is: PcmSink's own
    // (pcm_sink.hpp), forwarded - sink_ is this player's PCM sink whether or
    // not it is the output currently open, and is null for a player given no
    // PCM sink at all (PlayerOutputs::pcm unset), which these all answer as
    // "nothing to route or say".
    bool set_routing(const render::Routing& routing) { return sink_ && sink_->set_routing(routing); }
    [[nodiscard]] render::Routing routing() const { return sink_ ? sink_->routing() : render::Routing{}; }
    [[nodiscard]] std::string device_name() const {
        return sink_ ? sink_->device_name() : std::string{};
    }
    [[nodiscard]] std::uint32_t speaker_mask() const { return sink_ ? sink_->speaker_mask() : 0U; }

    // The output decision the open output, or the last one, was made by.
    [[nodiscard]] const OutputChoice& output_choice() const { return choice_; }

    // The outputs, or the choices about them, have changed: the item playing
    // is decided again, and if the answer is another mode or endpoint it
    // moves there, from the position being heard - paused still, if it was.
    // An item already playing out its last units finishes where it is, and
    // an item joined behind one still being heard is decided again once the
    // join has been heard (pump() does it), so the end of the one before is
    // not cut. Returns what changed, for the status line, or nothing.
    std::string refollow();

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
    //
    // An output whose device has gone away - the sink closed itself, as
    // ac3::audio's sinks do when their device is unplugged - stops playback
    // here, with the output closed, the report marked stopped, and the reason
    // in last_error(). Nothing submitted to it will be heard, so nothing
    // waits for it: neither the item, nor a reopen or stop waiting for the
    // audio to play out.
    PumpReport pump(std::size_t budget = 4800);

    // Whether pump() has anything to do: an output is open, or a reopen or a
    // stop is waiting for the audio already submitted to be heard - or an
    // output this player opened has closed by itself, which pump() finds.
    [[nodiscard]] bool active() const {
        return after_drain_.has_value() || output_open() || output_lost();
    }

    [[nodiscard]] const std::vector<PlayedItem>& history() const { return history_; }
    [[nodiscard]] std::uint32_t output_opens() const { return opens_; }
    [[nodiscard]] const std::string& last_error() const { return last_error_; }

private:
    // Content frames of one history_ entry.
    struct Span {
        std::size_t record = 0;
        std::uint64_t frames = 0;
    };

    // One rendered block, or one burst, waiting for room in the sink.
    struct Pending {
        std::vector<float> samples{};  // planar: slot 0's frames, then slot 1's, ...
        // For a bitstream output, the burst instead, of `frames` content
        // frames: what the units packed into it code, and whose they are - a
        // burst at a join holds units of both items.
        std::vector<std::byte> burst{};
        std::vector<Span> spans{};
        std::size_t frames = 0;
        // The history_ entry these frames belong to. At a join the old
        // item's tail and the new item's head sit in the queue together, and
        // each block counts towards its own item as it is submitted.
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
    // `unsent` is how many frames the item has since packed and not sent,
    // each of which moves everything after it that much earlier on a
    // bitstream's link.
    struct Segment {
        std::size_t record = 0;
        std::uint64_t output_start = 0;
        std::uint64_t item_start = 0;
        std::uint64_t unsent = 0;
    };

    // Carries out what the transport decided.
    void perform(const TransportOutcome& outcome, PumpReport* report);
    OpenFailure open_output_for(std::size_t item, PumpReport* report);
    // What an item that would not start leads to: the item marked and the
    // failure policy applied, or playback stopped for an output's fault.
    void open_failed(std::size_t item, OpenFailure failure, PumpReport* report);
    // Opens the output `choice_` names for the current session, or says why
    // it could not: the item's fault (kItem) or the output's (kOutput).
    OpenFailure open_chosen(std::size_t item, PumpReport* report);
    bool start_session(std::size_t item);
    void apply_seek_on_start(std::size_t item);
    void close_output();

    // The open output, whichever sink it is.
    [[nodiscard]] bool bitstreaming() const {
        return mode_ == OutputMode::kBitstream || mode_ == OutputMode::kBitstreamAsAc3;
    }
    [[nodiscard]] bool output_open() const;
    // An output this player opened, and has not closed, that is not open: its
    // sink closed itself when the device went away.
    [[nodiscard]] bool output_lost() const { return mode_ != OutputMode::kNone && !output_open(); }
    [[nodiscard]] std::optional<audio::MonitorPosition> output_position() const;
    [[nodiscard]] std::uint64_t heard_frames() const;
    // Where the next frame the decode delivers goes in the output's
    // timeline: everything submitted, queued, and - for a bitstream - packed
    // into a burst not yet complete, or for a transcode taken by the encoder
    // and heard its delay later.
    [[nodiscard]] std::uint64_t timeline_end() const;
    // Where the first sample decoded after an open or a flush is heard: a
    // transcode's delay in, or straight away.
    [[nodiscard]] std::uint64_t link_start() const;
    // For a bitstream, whose units are packed before they are decoded: where
    // the last frame the decode delivered sits on the link, from the
    // session's own position in its stream.
    [[nodiscard]] std::uint64_t decoded_end() const;
    // The output a session's item would be played through, as the decision
    // has it, asked while the open output is held.
    [[nodiscard]] OutputChoice decide(const Session& session) const;
    [[nodiscard]] HeldOutput held_output() const;
    // Why the prepared item cannot join the open output although the
    // transport would join it, or empty: another endpoint, or units the
    // packer cannot make whole bursts of with the ones it holds.
    [[nodiscard]] std::string join_blocked(const OutputChoice& next, std::string_view title) const;

    // A unit the session sent, into the packer and, once it completes one,
    // the pending ring as a burst.
    void send_unit(std::span<const std::byte> unit, std::uint32_t samples);
    // Forgets what the packer holds: a flush, or a new output.
    void reset_packer();
    // A transcode's whole frames into the pending ring as bursts, or with
    // `last`, everything it holds, padded out.
    void encode_transcoded(bool last);
    // A transcode that failed stops playback; true when it did.
    bool stop_for_transcode(PumpReport& report);
    // So does an output that was lost (output_lost()); true when it did.
    bool stop_for_lost_output(PumpReport& report);

    // The pending blocks, oldest first, as a ring whose blocks are never
    // freed: each keeps its buffer for the next block to reuse, so steady
    // playback allocates nothing per block.
    Pending& push_block();
    void clear_pending();

    // Takes one rendered block into the pending ring, for the current item.
    void take_block(std::span<const std::span<const float>> rendered, std::size_t n);
    // Takes the report of the unit whose last `frames` frames were just
    // taken.
    void take_report(const UnitReport& report, std::size_t frames);
    // Builds the decoder for `rate` from the current settings, or a
    // transcode's from its own.
    void build_decoder(std::uint32_t rate, bool transcode);
    // Whether the decoder there is is the one build_decoder() would build.
    [[nodiscard]] bool decoder_fits(std::uint32_t rate, bool transcode) const;
    // Decodes into the pending blocks until they hold at least `frames`.
    void fill(std::size_t frames);
    // Submits pending blocks while the sink takes them.
    std::size_t drain(std::size_t budget);
    // Whether everything submitted since the output opened has been heard.
    [[nodiscard]] bool played_out();
    // Opens the item after the current one ahead of its join decision,
    // marking any that will not open on the way. Returns the one that would
    // not, when an item that fails is to stop playback, else Queue::kNone.
    std::size_t prepare_next(PumpReport& report);
    // Whether the item remembered as failing is still what the queue plays
    // next, ahead of anything that can be played.
    [[nodiscard]] bool failed_next_stands() const;
    void mark_unplayable(std::size_t item, std::string why);
    // How the prepared item would be played, decided once for it.
    [[nodiscard]] const OutputChoice& prepared_decision();
    void drop_prepared();
    // Whether the next item would join the output now: the transport would,
    // and neither the endpoint nor the packer stands in the way.
    bool next_joins(PumpReport& report);
    // The current item has delivered everything: decide what comes next.
    // `heard` says its tail has been heard already.
    void item_ended(PumpReport& report, bool heard);
    // A reopen or a stop that waits for what has been submitted to play out;
    // pump() carries it out once the sink's clock has passed it, which with
    // `heard` it already has.
    void play_out_then(const TransportOutcome& outcome, PumpReport& report, bool heard);
    // After a queue edit: a waiting reopen goes to the item now current, and
    // a prepared session, keyed by index, is dropped.
    void after_edit();
    // Moves the history's queue indices the way an edit moved the items.
    void remap_history(const std::function<std::size_t(std::size_t)>& moved);

    // The diagnostics notes, when there is a ring to write to. A line about
    // an item names it by its place and title (describe_item()), with the
    // item's folder withheld, since what a loader says can quote its path.
    void note(std::string_view line) const;
    // `line` with the folders of queue item `index` withheld.
    void note_withheld(std::size_t index, std::string_view line) const;
    void note_item(std::size_t index, std::string_view title, std::string_view what) const;
    [[nodiscard]] std::string_view title_of(std::size_t index) const;
    // The item session_ plays has started from an open, or joined.
    void note_started(std::size_t item, bool joined) const;
    // A unit of the item being decoded would not decode.
    void note_unit_error(const std::string& reason);
    // Notes how many more units of that item would not decode, if any.
    void settle_unit_errors();

    std::unique_ptr<PcmSink> sink_;
    std::unique_ptr<BitstreamSink> bitstream_;
    OutputChooser choose_;
    // The open output's mode, kNone while closed, and the decision behind it.
    OutputMode mode_ = OutputMode::kNone;
    OutputChoice choice_;
    // A decision to take again once a join has been heard (refollow()).
    bool refollow_pending_ = false;
    // A bitstream output's packing: E-AC-3 units wait here until they make
    // six blocks. `packed_frames_` is how many content frames they code, and
    // `packed_spans_` whose they are.
    std::optional<iec61937::Eac3BurstPacker> packer_;
    std::uint64_t packed_frames_ = 0;
    std::vector<Span> packed_spans_;
    // A transcoding output's encoder, and why it failed, if it did.
    std::optional<Ac3Transcoder> transcoder_;
    std::optional<std::string> transcode_error_;
    ItemLoader loader_;
    render::OutputLayout layout_;
    DecoderSettings settings_;

    // The speaker setup: source-of-truth settings (kept in real units, which
    // survive a rate change unlike the sample counts render::TrimDelay
    // itself holds) and the processor built_decoder() reconfigures onto
    // whenever decoder_rate_ changes. Sized to render::OutputLayout::kMaxSlots
    // regardless of layout_.slots(), the way render::TrimDelay's own arrays
    // are sized to kMaxOutputs, so a slot index never needs bounds-checking
    // against two different limits.
    std::array<double, render::OutputLayout::kMaxSlots> trim_db_{};
    std::array<double, render::OutputLayout::kMaxSlots> delay_ms_{};
    double crossover_hz_ = render::LayoutRenderer::kDefaultCrossoverHz;
    render::TrimDelay trim_delay_;
    std::vector<float> trim_delay_storage_;
    // The rate trim_delay_ is configured for, kept apart from decoder_rate_:
    // build_decoder() runs for a transcode's decoder too, which trim and
    // delay do not apply to (transcoded audio is not rendered to speakers),
    // so trim_delay_ is reconfigured only when the LOCAL decoder's rate
    // actually changes.
    std::uint32_t trim_delay_rate_ = 0;
    // Reconfigures trim_delay_ for `rate` if it is not already, then
    // reapplies trim_db_/delay_ms_ converted to that rate's sample counts.
    void reconfigure_trim_delay(std::uint32_t rate);

    // The identify tone: source-of-truth level (survives a rate change) and
    // the generator build_decoder() reconfigures whenever the LOCAL
    // decoder's rate changes - render::IdentifyTone takes its sample rate
    // at construction, with no setter, so a rate change rebuilds it rather
    // than adjusting it in place, the same reason decoder_ itself is
    // rebuilt rather than retuned. identify_slot_ is Queue::kNone while
    // nothing is being identified; a bare index with no rate-dependent
    // state of its own, so unlike the level it needs no rebuild logic to
    // survive one - take_block() just reads it fresh every block.
    double identify_level_db_ = render::IdentifyTone::kDefaultLevelDb;
    render::IdentifyTone identify_tone_;
    std::uint32_t identify_rate_ = 0;
    std::size_t identify_slot_ = Queue::kNone;
    // Reconfigures identify_tone_ for `rate` if it is not already, then
    // reapplies identify_level_db_.
    void reconfigure_identify(std::uint32_t rate);

    // The transport bar's master volume: volume_db_ is the source of truth,
    // volume_gain_ the linear factor take_block() applies, recomputed
    // whenever set_volume_db() changes it - the same split trim_delay_'s own
    // gain array keeps, and for the same reason (a block's hot loop multiplies
    // floats; it does not call std::pow).
    double volume_db_ = 0.0;
    float volume_gain_ = 1.0F;

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
    std::optional<OutputChoice> prepared_choice_;
    // The next item, when it would not open and an item that fails stops
    // playback, and why: the transport hears of it once the current item has
    // ended, and the item is marked then.
    std::size_t failed_next_ = Queue::kNone;
    std::string failed_next_why_;

    // A reopen or a stop waiting for the audio already submitted to play,
    // and the device-clock frame by which it will have (played_out()).
    std::optional<TransportOutcome> after_drain_;
    std::optional<std::uint64_t> drain_target_;
    // Set while the current item, decoded to its end, waits for its tail to
    // be heard: an item that would join it now has come late.
    bool tail_waiting_ = false;

    std::optional<SeekOnStart> seek_on_start_;

    std::uint64_t submitted_since_open_ = 0;
    std::uint32_t opens_ = 0;
    std::vector<PlayedItem> history_;
    std::vector<Segment> segments_;
    // Built for the output's layout and rate; the history entry whose blocks
    // were metered last, so a new item restarts the programme measurements.
    std::optional<PlayMeters> meters_;
    // Whether meters_ was built for a transcode's layout.
    bool meters_transcoding_ = false;
    std::size_t metered_record_ = Queue::kNone;
    // Each unit's report, stamped like the meters' snapshots.
    UnitReports reports_;
    std::string last_error_;

    DiagnosticLog* diagnostics_ = nullptr;
    // The history entry whose first undecodable unit has been noted, and how
    // many more of its units have failed since.
    std::size_t unit_error_record_ = Queue::kNone;
    std::uint64_t unit_errors_more_ = 0;
};

}  // namespace ac3::hearth

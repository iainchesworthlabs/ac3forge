#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ac3/render/identify.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/render/render.hpp"
#include "ac3/render/routing.hpp"
#include "bitstream_sink.hpp"
#include "decoder_settings.hpp"
#include "diagnostic_log.hpp"
#include "output_selector.hpp"
#include "pcm_sink.hpp"
#include "play_meters.hpp"
#include "player.hpp"
#include "queue.hpp"
#include "session.hpp"
#include "transport.hpp"

// The engine thread (planning/hearth-reference-player.md, A3): a Player on a
// thread of its own, driven by commands from any other thread, reporting what
// it did as snapshots any other thread can read.
//
// The Player is single-threaded by design - every judgement in it assumes one
// caller - so once the engine has started, its thread is the only one that
// touches it. A command is queued and returns at once; the engine thread
// carries commands out in the order they were queued, between pumps. The
// window posts commands and reads status(); nothing it does waits on the
// audio, and nothing the audio does waits on the window.
//
// While an output is open the engine pumps once per EngineTiming::period and
// waits on its command queue in between, so a command is carried out within
// a period; a device sink's own buffer spans many periods. With nothing open
// it sleeps until a command arrives.
//
// After every batch of commands, and after every pump that started an item,
// reopened or closed the output or had something to say, the engine publishes
// a snapshot - the queue, the transport, the settings, the history - and calls
// the change callback on its own thread. The play position moves with every
// pump and is kept apart, so a position slider does not copy the list a
// thousand times a second.
//
// Given a diagnostics ring (diagnostic_log.hpp), the engine notes each
// command as its thread carries it out, and anything the transport said
// about it; the player notes what playback did in between.

namespace ac3::hearth {

// A local output and a passthrough output, and where the endpoints each item
// is decided against are read from: device_endpoints() for this machine's
// own. The engine decides every item through an OutputSelector of its own.
struct EngineOutputs {
    std::unique_ptr<PcmSink> pcm{};
    std::unique_ptr<BitstreamSink> bitstream{};
    EndpointSource endpoints{};
};

struct EngineTiming {
    // How long the engine waits between pumps while an output is open.
    std::chrono::milliseconds period{5};
    // The frames one pump may submit.
    std::size_t budget = 4800;
};

struct EngineStatus {
    // One more for every publication, so a reader can tell a new snapshot
    // from one it has already acted on.
    std::uint64_t generation = 0;
    TransportState state = TransportState::kStopped;
    std::vector<QueueItem> queue{};
    std::size_t current = Queue::kNone;
    bool gapless = true;
    bool repeat = false;
    FailurePolicy on_failure = FailurePolicy::kSkip;
    DecoderSettings settings{};
    // Why the settings are not what is heard, or empty: a bitstream is
    // decoded by the receiver (Player::settings_note()).
    std::string settings_note{};
    // What the output is open at; all zero while it is closed. And why that
    // output: the output decision's reason for the item that opened it or
    // last joined it.
    OpenOutputFormat output{};
    std::string output_reason{};
    // The Output screen's choices, for an engine that decides its outputs.
    OutputPreferences output_preferences{};
    std::uint32_t output_opens = 0;
    std::vector<PlayedItem> history{};
    // The latest thing a command, the transport or an item had to say, and
    // the player's last error.
    std::string note{};
    std::string error{};

    // The speaker setup (planning/hearth-reference-player.md, A5's Speakers
    // page; Player::set_trim_db() and friends). trim_db/delay_ms are one
    // entry per render layout slot (index i is slot i, not necessarily
    // output i - the routing patch below may send it elsewhere), always
    // sized to the layout this engine was built with. routing, device_name
    // and speaker_mask are PcmSink's own (pcm_sink.hpp): a default-
    // constructed Routing, an empty name and a zero mask where there is no
    // PCM sink or nothing is open.
    std::vector<double> trim_db{};
    std::vector<double> delay_ms{};
    double crossover_hz = render::LayoutRenderer::kDefaultCrossoverHz;
    render::Routing routing{};
    std::string device_name{};
    std::uint32_t speaker_mask = 0;
    // The identify tone (Player::identify_start() and friends): the level
    // every session plays at, and the render layout slot currently sounding
    // it, or Queue::kNone while none is - the same sentinel and the same
    // reason as PlayPosition::item.
    double identify_level_db = render::IdentifyTone::kDefaultLevelDb;
    std::size_t identify_slot = Queue::kNone;
    // What every item is rendered onto (Player::layout()) - fixed for this
    // engine's lifetime. A settings page reads each slot's own name
    // (OutputLayout::slot_name()) to label the routing grid and the
    // trim/delay table by speaker rather than by bare slot number.
    render::OutputLayout layout{};
};

class Engine {
public:
    // `diagnostics`, when given, outlives the engine.
    Engine(std::unique_ptr<PcmSink> sink, ItemLoader loader, const render::OutputLayout& layout,
           const DecoderSettings& settings = {}, const EngineTiming& timing = {},
           DiagnosticLog* diagnostics = nullptr);
    // With a choice of outputs for each item (Player's PlayerOutputs). The
    // chooser runs on the engine thread.
    Engine(PlayerOutputs outputs, ItemLoader loader, const render::OutputLayout& layout,
           const DecoderSettings& settings = {}, const EngineTiming& timing = {},
           DiagnosticLog* diagnostics = nullptr);
    // Deciding each item's output itself, from `outputs.endpoints`.
    Engine(EngineOutputs outputs, ItemLoader loader, const render::OutputLayout& layout,
           const DecoderSettings& settings = {}, const EngineTiming& timing = {},
           DiagnosticLog* diagnostics = nullptr);
    // Stops the thread, and with it whatever is playing.
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    // Commands. Each returns at once; the engine thread carries them out in
    // the order they were made.
    void play();
    void pause();
    void stop();
    void next();
    void previous();
    void seek(std::chrono::milliseconds to);
    void add(std::vector<QueueItem> items);
    void insert(std::size_t index, QueueItem item);
    void remove(std::size_t index);
    void move(std::size_t from, std::size_t to);
    void clear();
    void play_item(std::size_t index);
    void set_decoder_settings(const DecoderSettings& settings);
    // The speaker setup (EngineStatus's own fields say what is in effect).
    // Each posts and returns at once, like every other command; a slot out
    // of range or a value out of bounds is refused on the engine thread and
    // reaches the caller as EngineStatus::note, the way a refused transport
    // command already does - the trim/delay/crossover/routing fields simply
    // do not change, which is the caller's own sign that a set was refused.
    void set_trim_db(std::size_t slot, double db);
    void set_delay_ms(std::size_t slot, double ms);
    void set_crossover_hz(double hz);
    // Refused when this engine has no PCM sink (PlayerOutputs::pcm unset) -
    // there is nothing to route.
    void set_routing(const render::Routing& routing);
    // The identify tone: a level that persists like crossover_hz, and a
    // slot that plays until identify_stop() or another identify_start()
    // moves it there instead. Refused, with a note, for an out-of-range
    // slot or level, leaving EngineStatus unchanged - the same rule as
    // every other speaker-setup command above.
    void set_identify_level_db(double db);
    void identify_start(std::size_t slot);
    void identify_stop();
    void set_gapless(bool on);
    void set_repeat(bool on);
    void set_on_failure(FailurePolicy policy);
    // Replaces the queue with `items`, stopping whatever plays, and makes
    // `current` the item a play starts, `position` into it: the queue a
    // window brings back at start (settings_model.hpp). Nothing plays until
    // asked to.
    void restore(std::vector<QueueItem> items, std::size_t current,
                 std::chrono::milliseconds position);
    // The Output screen's choices. The item playing is decided again, and
    // moves if the answer changed (Player::refollow()). Refused, with a
    // note, by an engine given no endpoints to decide from.
    void set_output_preferences(OutputPreferences preferences);
    // The machine's outputs have changed - ac3::audio::RenderDeviceWatch's
    // callback calls this: the endpoints are read again, for the item
    // playing now and for every item after it.
    void refresh_outputs();

    // Waits until every command made before the call has been carried out
    // and its effect published - for a test, or a caller that has to read
    // back what its own command did. Never from the change callback, which
    // runs on the engine thread this would be waiting for.
    void sync();

    [[nodiscard]] EngineStatus status() const;
    [[nodiscard]] PlayPosition position() const;
    // The newest meter snapshot the device has played up to, or nothing while
    // no output is open. Kept apart from status() for the position's reason.
    [[nodiscard]] std::optional<MeterSnapshot> meters() const;
    // The report of the unit the device is playing, or nothing while no
    // output is open.
    [[nodiscard]] std::optional<UnitReport> unit_report() const;

    // Called on the engine thread after each publication, with the snapshot
    // just published. It should hand the news to its own thread and return.
    void on_change(std::function<void(const EngineStatus&)> callback);

private:
    // A command runs against the player and returns anything it had to say.
    using Command = std::function<std::string(Player&)>;

    // What every constructor ends with: the first status, and the thread.
    void start(const render::OutputLayout& layout, const DecoderSettings& settings);

    void post(Command command);
    void run(const std::stop_token& stop);
    // Snapshots the player and makes it the status; `carried` is how many
    // commands had been carried out by then, for sync().
    void publish(const std::string& note, std::uint64_t carried);
    // A line for the diagnostics ring, if there is one; and what the
    // transport said about a command, noted with the folders of the item it
    // is about withheld, and returned as the command's result.
    void note(std::string_view line) const;
    [[nodiscard]] std::string transport_said(const TransportOutcome& outcome) const;

    EngineTiming timing_;
    DiagnosticLog* diagnostics_ = nullptr;
    // The engine thread's alone once the thread has started, as the player
    // is: it decides each item for the player, when the engine was given
    // endpoints to decide from.
    std::unique_ptr<OutputSelector> selector_;
    Player player_;

    mutable std::mutex mutex_;
    // The engine thread waits on `wake_` for commands, and sync() on
    // `published_cv_` for their effect.
    std::condition_variable_any wake_;
    std::condition_variable_any published_cv_;
    std::deque<Command> commands_;
    std::uint64_t posted_ = 0;
    std::uint64_t published_ = 0;
    EngineStatus status_;
    PlayPosition position_;
    MeterSnapshot meters_;
    bool has_meters_ = false;
    UnitReport report_;
    bool has_report_ = false;
    std::function<void(const EngineStatus&)> on_change_;
    // The engine thread's own copies, filled by the player and copied into
    // meters_ and report_ under the lock, all keeping their storage.
    MeterSnapshot meter_scratch_;
    UnitReport report_scratch_;

    // Last, so it starts once everything above exists and stops before any
    // of it goes.
    std::jthread thread_;
};

}  // namespace ac3::hearth

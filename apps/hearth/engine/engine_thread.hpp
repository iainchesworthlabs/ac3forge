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

#include "ac3/render/layout.hpp"
#include "decoder_settings.hpp"
#include "diagnostic_log.hpp"
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
    DecoderSettings settings{};
    // What the output is open at; all zero while it is closed.
    OpenOutputFormat output{};
    std::uint32_t output_opens = 0;
    std::vector<PlayedItem> history{};
    // The latest thing a command, the transport or an item had to say, and
    // the player's last error.
    std::string note{};
    std::string error{};
};

class Engine {
public:
    // `diagnostics`, when given, outlives the engine.
    Engine(std::unique_ptr<PcmSink> sink, ItemLoader loader, const render::OutputLayout& layout,
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
    void set_gapless(bool on);
    void set_repeat(bool on);

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
    // The engine thread's alone once the thread has started.
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

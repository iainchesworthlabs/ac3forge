#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine_thread.hpp"
#include "queue.hpp"
#include "transport.hpp"

// The engine's settings (planning/hearth-reference-player.md, A3: "the
// settings model"): what the Settings page's Playback and Network cards hold,
// the queue kept for the next start, and the keys they are kept under.
//
// Keeping them is the window's: it stores its settings through QSettings
// (A5). This model reads and writes them through SettingsStore, which the
// window implements over QSettings and a test over a map. So what each value
// means, what it defaults to and what a damaged value becomes are decided
// here, once, and tested on every leg.
//
// The keys. Values are text, as QSettings writes them to a file:
//   playback/gapless       true or false
//   playback/resumeQueue   true or false
//   playback/onFailure     skip or stop
//   network/name           how sinks and players show this computer
//   network/discover       true or false
//   queue/...              the queue kept for the next start
//   speakers/...           the speaker setup kept for the next start
//   pairing/...            the pairing records (pairing_store.hpp)
// Lists use QSettings' own array layout ("queue/size", then "queue/1/path"
// and so on, counted from 1), so the window can read them with its array
// functions as well. The diagnostics file withholds everything under
// "queue/" and "pairing/" (diagnostics_report.hpp) - "speakers/" holds
// nothing a person typed (numbers and a layout/routing description), so it
// is not withheld.

namespace ac3::hearth {

class SettingsStore {
public:
    SettingsStore() = default;
    virtual ~SettingsStore() = default;
    SettingsStore(const SettingsStore&) = delete;
    SettingsStore& operator=(const SettingsStore&) = delete;
    SettingsStore(SettingsStore&&) = delete;
    SettingsStore& operator=(SettingsStore&&) = delete;

    [[nodiscard]] virtual std::optional<std::string> value(std::string_view key) const = 0;
    virtual void set_value(std::string_view key, std::string_view value) = 0;
    // Removes every key under `group`: "queue" removes "queue/size" and
    // "queue/1/path", and leaves "queued" alone.
    virtual void remove_group(std::string_view group) = 0;
    // Writes what has been set to wherever the store keeps it. False when
    // that failed: what was kept before is what a later start reads.
    [[nodiscard]] virtual bool sync() = 0;
};

// A store in memory, for tests and for a window with nowhere to keep its
// settings. What sync() last wrote is kept apart, as a file would be.
class MemorySettingsStore final : public SettingsStore {
public:
    using Values = std::map<std::string, std::string, std::less<>>;

    // Opens with `values`, as a store reading them from a file does.
    explicit MemorySettingsStore(Values values = {});

    [[nodiscard]] std::optional<std::string> value(std::string_view key) const override;
    void set_value(std::string_view key, std::string_view value) override;
    void remove_group(std::string_view group) override;
    [[nodiscard]] bool sync() override;

    // Whether sync() fails, as a full disk or a read-only folder makes it.
    void set_sync_fails(bool fails) { sync_fails_ = fails; }
    [[nodiscard]] const Values& values() const { return values_; }
    // The values as last written: what a store opened afresh would read.
    [[nodiscard]] const Values& synced() const { return synced_; }

private:
    Values values_;
    Values synced_;
    bool sync_fails_ = false;
};

struct PlaybackSettings {
    // Keeps the output open from one item to the next where the two can
    // join (transport.hpp).
    bool gapless = true;
    // On the next start, the queue comes back at the item and the position
    // it had reached when the window closed.
    bool resume_queue = true;
    FailurePolicy on_failure = FailurePolicy::kSkip;

    friend bool operator==(const PlaybackSettings&, const PlaybackSettings&) = default;
};

struct NetworkSettings {
    // How sinks and players show this computer: the name in server/hello and
    // the mDNS instance's.
    std::string name{};
    // Whether to look for Sendspin players over mDNS. Off, the Network page
    // lists only the players already paired.
    bool discover = true;

    friend bool operator==(const NetworkSettings&, const NetworkSettings&) = default;
};

struct EngineSettings {
    PlaybackSettings playback{};
    NetworkSettings network{};

    friend bool operator==(const EngineSettings&, const EngineSettings&) = default;
};

// A name as the network carries it: control characters dropped, spaces
// trimmed from both ends, and cut at a UTF-8 boundary to the 63 bytes of the
// DNS label the mDNS instance goes in. Empty when nothing is left.
[[nodiscard]] std::string network_name(std::string_view typed);

// "Hearth on <host>", as network_name() keeps it: the name until the person
// gives one.
[[nodiscard]] std::string default_network_name(std::string_view host);

// The settings in `store`. A value that is missing, or that does not read as
// one of its values, is the default; a name that network_name() leaves empty
// is default_network_name(host).
[[nodiscard]] EngineSettings load_settings(const SettingsStore& store, std::string_view host);
void save_settings(const EngineSettings& settings, SettingsStore& store);

// The settings as the diagnostics file lists them, key and value, in order.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> settings_rows(
    const EngineSettings& settings);

// The queue kept for the next start.
struct SavedQueue {
    // Each item's path and title. What a probe found is not kept: the next
    // start finds it again.
    std::vector<QueueItem> items{};
    std::size_t current = Queue::kNone;
    // How far into the current item playback had got.
    std::chrono::milliseconds position{0};
};

// What to keep of a snapshot and the play position: the item being heard,
// and how far into it, or the queue's current item from its start when
// nothing is being heard.
[[nodiscard]] SavedQueue saved_queue(const EngineStatus& status, const PlayPosition& position);

// Replaces what the store holds under "queue" with `saved`.
void save_queue(const SavedQueue& saved, SettingsStore& store);

// The queue in `store`, as far as it reads. An item with no path is left
// out, and the current item's place follows; a count, a current item or a
// position that does not read is none.
[[nodiscard]] SavedQueue load_queue(const SettingsStore& store);

// The speaker setup kept for the next start (the Speakers page: layout,
// trim, delay, crossover and routing). One setup today, not one per output
// device, despite FirstRunDialog.qml's own promise of "a setup for each
// output": which device is about to open is not known until the engine (and
// its PcmSink) already has, so keying this earlier would mean probing
// outputs before there is an engine to open one for - a cost issue #885
// this exists for does not ask for. HearthController::start()'s own comment
// says the same.
struct SavedSpeakerSetup {
    // OutputLayout::text(): a name ("7.1.4") or the list form, with any
    // ':small'/realization suffix already folded in - OutputLayout::parse()
    // restores both from this one string (layout.hpp's own header comment),
    // so nothing here needs a separate key for heights or per-speaker size.
    // Empty when nothing has been saved.
    std::string layout{};
    // One entry per layout slot; always the same length as each other, and
    // that length is how many slots were saved.
    std::vector<double> trim_db{};
    std::vector<double> delay_ms{};
    double crossover_hz = ac3::render::LayoutRenderer::kDefaultCrossoverHz;
    // Routing::format()'s text form, and the device output count it was
    // captured against - Routing::parse() needs both to rebuild the same
    // patch. Empty when nothing has been saved, or when nothing was open to
    // capture a patch from.
    std::string routing{};
    std::size_t routing_outputs = 0;
};

// The speaker setup in `status`, ready to save.
[[nodiscard]] SavedSpeakerSetup saved_speaker_setup(const EngineStatus& status);

// Replaces what the store holds under "speakers" with `saved`.
void save_speaker_setup(const SavedSpeakerSetup& saved, SettingsStore& store);

// The speaker setup in `store`, as far as it reads: layout is empty and
// every other field keeps SavedSpeakerSetup{}'s own default for whatever
// individually does not read - the same "as far as it can" reading
// load_queue() gives a damaged queue.
[[nodiscard]] SavedSpeakerSetup load_speaker_setup(const SettingsStore& store);

}  // namespace ac3::hearth

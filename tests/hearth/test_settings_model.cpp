#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "ac3/render/layout.hpp"
#include "ac3/render/routing.hpp"
#include "engine_thread.hpp"
#include "queue.hpp"
#include "settings_model.hpp"
#include "transport.hpp"

// The engine's settings model (apps/hearth/engine/settings_model.hpp): what
// each Playback and Network setting reads as, with its default for anything
// missing or damaged; the network name as the network carries it; the queue
// kept for the next start; and the speaker setup kept for the next start
// (issue #885) - all as QSettings' array layout holds them.

using namespace std::chrono_literals;

using ac3::hearth::EngineSettings;
using ac3::hearth::EngineStatus;
using ac3::hearth::FailurePolicy;
using ac3::hearth::MemorySettingsStore;
using ac3::hearth::PlayPosition;
using ac3::hearth::Queue;
using ac3::hearth::QueueItem;
using ac3::hearth::SavedQueue;
using ac3::hearth::SavedSpeakerSetup;

namespace {

QueueItem queued(const std::string& path, const std::string& title) {
    QueueItem item;
    item.path = path;
    item.title = title;
    return item;
}

}  // namespace

TEST_CASE("settings: a store removes a group whole, and keeps what it last wrote",
          "[hearth][settings-model]") {
    MemorySettingsStore store{{{"queue", "x"}, {"queued", "y"}}};
    store.set_value("queue/size", "2");
    store.set_value("queue/1/path", "a");
    store.set_value("network/name", "n");
    CHECK(store.value("queue/1/path") == std::optional<std::string>{"a"});
    CHECK_FALSE(store.value("queue/2/path").has_value());

    store.remove_group("queue");
    CHECK_FALSE(store.value("queue").has_value());
    CHECK_FALSE(store.value("queue/size").has_value());
    CHECK_FALSE(store.value("queue/1/path").has_value());
    CHECK(store.value("queued") == std::optional<std::string>{"y"});
    CHECK(store.value("network/name") == std::optional<std::string>{"n"});

    // What was opened with is what was last written, until a sync.
    CHECK(store.synced().contains("queue"));
    REQUIRE(store.sync());
    CHECK_FALSE(store.synced().contains("queue"));
    CHECK(store.synced().contains("network/name"));
    store.set_sync_fails(true);
    store.set_value("network/name", "m");
    CHECK_FALSE(store.sync());
    CHECK(store.synced().at("network/name") == "n");
    CHECK(store.value("network/name") == std::optional<std::string>{"m"});
}

TEST_CASE("settings: what is missing or cannot be read is the default", "[hearth][settings-model]") {
    const MemorySettingsStore empty;
    const EngineSettings defaults = ac3::hearth::load_settings(empty, "DESKTOP-7KQ2");
    CHECK(defaults.playback.gapless);
    CHECK(defaults.playback.resume_queue);
    CHECK(defaults.playback.on_failure == FailurePolicy::kSkip);
    CHECK(defaults.network.name == "Hearth on DESKTOP-7KQ2");
    CHECK(defaults.network.discover);

    // Each a value a lenient reader would take for "off": every switch here
    // is on by default, so only a reading that differs from the default shows.
    const MemorySettingsStore damaged{{{"playback/gapless", "no"},
                                       {"playback/resumeQueue", "0"},
                                       {"playback/onFailure", "Stop"},
                                       {"network/name", " \t\n "},
                                       {"network/discover", "False"}}};
    CHECK(ac3::hearth::load_settings(damaged, "DESKTOP-7KQ2") == defaults);

    const MemorySettingsStore set{{{"playback/gapless", "false"},
                                   {"playback/resumeQueue", "false"},
                                   {"playback/onFailure", "stop"},
                                   {"network/name", "  Living room  "},
                                   {"network/discover", "false"}}};
    const EngineSettings read = ac3::hearth::load_settings(set, "DESKTOP-7KQ2");
    CHECK_FALSE(read.playback.gapless);
    CHECK_FALSE(read.playback.resume_queue);
    CHECK(read.playback.on_failure == FailurePolicy::kStop);
    CHECK(read.network.name == "Living room");
    CHECK_FALSE(read.network.discover);
}

TEST_CASE("settings: what is saved reads back, and is what the diagnostics file lists",
          "[hearth][settings-model]") {
    EngineSettings settings;
    settings.playback.gapless = false;
    settings.playback.on_failure = FailurePolicy::kStop;
    settings.network.name = "Study";
    settings.network.discover = false;
    MemorySettingsStore store;
    ac3::hearth::save_settings(settings, store);
    CHECK(ac3::hearth::load_settings(store, "elsewhere") == settings);

    const std::vector<std::pair<std::string, std::string>> rows{
        {"playback/gapless", "false"},    {"playback/resumeQueue", "true"},
        {"playback/onFailure", "stop"},   {"network/name", "Study"},
        {"network/discover", "false"},
    };
    CHECK(ac3::hearth::settings_rows(settings) == rows);
    for (const auto& [key, value] : rows) {
        INFO(key);
        CHECK(store.value(key) == std::optional<std::string>{value});
    }
}

TEST_CASE("settings: a network name is one DNS label's worth of text", "[hearth][settings-model]") {
    CHECK(ac3::hearth::network_name("  Kitchen\r\n speaker \x7F ") == "Kitchen speaker");
    CHECK(ac3::hearth::network_name("\x01\x02").empty());
    CHECK(ac3::hearth::network_name(std::string(70, 'a')) == std::string(63, 'a'));
    // A two-byte character that would end past the limit is left out whole,
    // and a space the cut leaves at the end goes too.
    std::string accented(61, 'b');
    accented += " \xC3\xA9";
    CHECK(ac3::hearth::network_name(accented) == std::string(61, 'b'));
    std::string fits(61, 'c');
    fits += "\xC3\xA9";
    CHECK(ac3::hearth::network_name(fits) == fits);

    CHECK(ac3::hearth::default_network_name("DESKTOP-7KQ2") == "Hearth on DESKTOP-7KQ2");
    CHECK(ac3::hearth::default_network_name(" \n") == "Hearth");
    const std::string long_host(80, 'h');
    CHECK(ac3::hearth::default_network_name(long_host) == "Hearth on " + std::string(53, 'h'));
}

TEST_CASE("settings: the queue kept for the next start is the item being heard, and how far in",
          "[hearth][settings-model]") {
    EngineStatus status;
    status.queue = {queued("C:/Music/one.ec3", "One"), queued("C:/Music/two.ec3", "Two"),
                    queued("/srv/three.ac3", "Three")};
    status.queue[1].facts.sample_rate = 48000;
    status.queue[1].facts.unplayable_because = "not kept";
    status.current = 2;

    // Heard: the item and the position the device has reached, which can
    // differ from the queue's current item while a reopen waits.
    const SavedQueue heard = ac3::hearth::saved_queue(
        status, PlayPosition{.item = 1, .heard = 83456ms, .duration = 200000ms});
    REQUIRE(heard.items.size() == 3);
    CHECK(heard.items[1].path == "C:/Music/two.ec3");
    CHECK(heard.items[1].title == "Two");
    CHECK(heard.items[1].facts.sample_rate == 0);
    CHECK(heard.items[1].facts.unplayable_because.empty());
    CHECK(heard.current == 1);
    CHECK(heard.position == 83456ms);

    // Nothing heard: the current item, from its start.
    const SavedQueue stopped = ac3::hearth::saved_queue(status, PlayPosition{});
    CHECK(stopped.current == 2);
    CHECK(stopped.position == 0ms);

    MemorySettingsStore store;
    ac3::hearth::save_queue(heard, store);
    CHECK(store.value("queue/size") == std::optional<std::string>{"3"});
    CHECK(store.value("queue/1/path") == std::optional<std::string>{"C:/Music/one.ec3"});
    CHECK(store.value("queue/3/title") == std::optional<std::string>{"Three"});
    CHECK(store.value("queue/current") == std::optional<std::string>{"2"});
    CHECK(store.value("queue/positionMs") == std::optional<std::string>{"83456"});

    const SavedQueue read = ac3::hearth::load_queue(store);
    REQUIRE(read.items.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(read.items[i].path == heard.items[i].path);
        CHECK(read.items[i].title == heard.items[i].title);
    }
    CHECK(read.current == 1);
    CHECK(read.position == 83456ms);

    // A shorter queue replaces the group rather than overlaying it.
    ac3::hearth::save_queue(SavedQueue{.items = {queued("/one", "One")}}, store);
    CHECK_FALSE(store.value("queue/2/path").has_value());
    CHECK_FALSE(store.value("queue/current").has_value());
    const SavedQueue shorter = ac3::hearth::load_queue(store);
    CHECK(shorter.items.size() == 1);
    CHECK(shorter.current == Queue::kNone);
    CHECK(shorter.position == 0ms);
}

TEST_CASE("settings: a damaged saved queue reads as far as it can", "[hearth][settings-model]") {
    // An item with no path is left out, and the current item's place follows;
    // an item with no title shows its file's name.
    const MemorySettingsStore gaps{{{"queue/size", "4"},
                                    {"queue/1/path", "/a.ec3"},
                                    {"queue/2/title", "No path"},
                                    {"queue/3/path", "C:\\Music\\c.ec3"},
                                    {"queue/4/path", "d.ec3"},
                                    {"queue/current", "3"},
                                    {"queue/positionMs", "1500"}}};
    const SavedQueue read = ac3::hearth::load_queue(gaps);
    REQUIRE(read.items.size() == 3);
    CHECK(read.items[0].title == "a.ec3");
    CHECK(read.items[1].title == "c.ec3");
    CHECK(read.items[2].title == "d.ec3");
    CHECK(read.current == 1);
    CHECK(read.position == 1500ms);

    // A current item that was left out, or past the end, is none, and takes
    // the position with it.
    const MemorySettingsStore gone{{{"queue/size", "2"},
                                    {"queue/1/path", "/a"},
                                    {"queue/current", "2"},
                                    {"queue/positionMs", "1500"}}};
    CHECK(ac3::hearth::load_queue(gone).current == Queue::kNone);
    CHECK(ac3::hearth::load_queue(gone).position == 0ms);

    // Counts and numbers that do not read.
    for (const char* size : {"", "-1", "two", "3 ", "0x3"}) {
        INFO(size);
        const MemorySettingsStore bad{{{"queue/size", size}, {"queue/1/path", "/a"}}};
        CHECK(ac3::hearth::load_queue(bad).items.empty());
    }
    const MemorySettingsStore bad_numbers{{{"queue/size", "1"},
                                           {"queue/1/path", "/a"},
                                           {"queue/current", "one"},
                                           {"queue/positionMs", "99999999999999999999"}}};
    const SavedQueue numbers = ac3::hearth::load_queue(bad_numbers);
    CHECK(numbers.items.size() == 1);
    CHECK(numbers.current == Queue::kNone);
    CHECK(numbers.position == 0ms);
    const MemorySettingsStore huge_position{{{"queue/size", "1"},
                                             {"queue/1/path", "/a"},
                                             {"queue/current", "1"},
                                             {"queue/positionMs", "18446744073709551615"}}};
    CHECK(ac3::hearth::load_queue(huge_position).position == 0ms);

    // A count far past anything written stops at the limit rather than
    // walking for ever, and what is there still reads.
    const MemorySettingsStore huge{{{"queue/size", "18446744073709551615"},
                                    {"queue/1/path", "/a"}}};
    CHECK(ac3::hearth::load_queue(huge).items.size() == 1);
    CHECK(ac3::hearth::load_queue(MemorySettingsStore{}).items.empty());
}

TEST_CASE("settings: the speaker setup kept for the next start round-trips through the store",
          "[hearth][settings-model]") {
    EngineStatus status;
    status.layout = *ac3::render::OutputLayout::parse("5.1");
    status.trim_db = {1.5, -2.0, 0.0, 3.25, -6.0, 0.0};
    status.delay_ms = {0.0, 5.0, 10.0, 0.0, 2.5, 0.0};
    status.crossover_hz = 100.0;
    status.routing = *ac3::render::Routing::identity(6, 8);

    const SavedSpeakerSetup saved = ac3::hearth::saved_speaker_setup(status);
    CHECK(saved.layout == "5.1");
    CHECK(saved.trim_db == status.trim_db);
    CHECK(saved.delay_ms == status.delay_ms);
    CHECK(saved.crossover_hz == 100.0);
    CHECK(saved.routing_outputs == 8);  // identity(6, 8): 6 channels onto 8 device outputs

    MemorySettingsStore store;
    ac3::hearth::save_speaker_setup(saved, store);
    CHECK(store.value("speakers/layout") == std::optional<std::string>{"5.1"});
    CHECK(store.value("speakers/slots") == std::optional<std::string>{"6"});
    CHECK(store.value("speakers/routing").has_value());
    CHECK(store.value("speakers/routingOutputs") == std::optional<std::string>{"8"});

    // What is saved reads back exactly, doubles included - text_of()/
    // double_of()'s own round trip, not any particular on-disk spelling.
    const SavedSpeakerSetup read = ac3::hearth::load_speaker_setup(store);
    CHECK(read.layout == saved.layout);
    CHECK(read.trim_db == saved.trim_db);
    CHECK(read.delay_ms == saved.delay_ms);
    CHECK(read.crossover_hz == saved.crossover_hz);
    CHECK(read.routing == saved.routing);
    CHECK(read.routing_outputs == saved.routing_outputs);

    // Nothing open to capture a routing patch from: format() gives "", and
    // that empty text is not saved as if it were a real (if degenerate)
    // patch - load_speaker_setup() then has nothing to try to apply.
    EngineStatus closed;
    closed.layout = *ac3::render::OutputLayout::parse("2.0");
    const SavedSpeakerSetup no_routing = ac3::hearth::saved_speaker_setup(closed);
    CHECK(no_routing.routing.empty());
    MemorySettingsStore store2;
    ac3::hearth::save_speaker_setup(no_routing, store2);
    CHECK_FALSE(store2.value("speakers/routing").has_value());
    CHECK_FALSE(store2.value("speakers/routingOutputs").has_value());

    // A shorter setup replaces the group rather than overlaying it.
    ac3::hearth::save_speaker_setup(
        SavedSpeakerSetup{.layout = "2.0", .trim_db = {0.0, 0.0}, .delay_ms = {0.0, 0.0}}, store);
    CHECK_FALSE(store.value("speakers/trimDb/3").has_value());
    CHECK_FALSE(store.value("speakers/routing").has_value());
    const SavedSpeakerSetup shorter = ac3::hearth::load_speaker_setup(store);
    CHECK(shorter.trim_db.size() == 2);
}

TEST_CASE("settings: a damaged saved speaker setup reads as far as it can", "[hearth][settings-model]") {
    const SavedSpeakerSetup defaults = ac3::hearth::load_speaker_setup(MemorySettingsStore{});
    CHECK(defaults.layout.empty());
    CHECK(defaults.trim_db.empty());
    CHECK(defaults.delay_ms.empty());
    CHECK(defaults.crossover_hz == ac3::render::LayoutRenderer::kDefaultCrossoverHz);
    CHECK(defaults.routing.empty());
    CHECK(defaults.routing_outputs == 0);

    // A missing entry within the claimed slot count reads as 0 (no
    // adjustment) rather than stopping the whole read, the same as a queue
    // item with no title falling back rather than failing the whole queue;
    // a crossoverHz or routingOutputs that does not read keeps the struct's
    // own default instead.
    const MemorySettingsStore gaps{{{"speakers/layout", "5.1"},
                                    {"speakers/slots", "3"},
                                    {"speakers/trimDb/1", "2.5"},
                                    {"speakers/trimDb/3", "-1.5"},
                                    {"speakers/delayMs/2", "4"},
                                    {"speakers/crossoverHz", "not a number"},
                                    {"speakers/routing", "0,1"},
                                    {"speakers/routingOutputs", "nope"}}};
    const SavedSpeakerSetup read = ac3::hearth::load_speaker_setup(gaps);
    CHECK(read.layout == "5.1");
    REQUIRE(read.trim_db.size() == 3);
    CHECK(read.trim_db[0] == 2.5);
    CHECK(read.trim_db[1] == 0.0);
    CHECK(read.trim_db[2] == -1.5);
    REQUIRE(read.delay_ms.size() == 3);
    CHECK(read.delay_ms[0] == 0.0);
    CHECK(read.delay_ms[1] == 4.0);
    CHECK(read.delay_ms[2] == 0.0);
    CHECK(read.crossover_hz == ac3::render::LayoutRenderer::kDefaultCrossoverHz);
    CHECK(read.routing == "0,1");
    CHECK(read.routing_outputs == 0);

    // A slot count far past anything written stops at OutputLayout::kMaxSlots
    // rather than walking for ever - load_queue()'s own kMaxSavedItems bound,
    // for the same reason.
    const MemorySettingsStore huge{{{"speakers/slots", "18446744073709551615"},
                                    {"speakers/trimDb/1", "1"},
                                    {"speakers/delayMs/1", "2"}}};
    const SavedSpeakerSetup capped = ac3::hearth::load_speaker_setup(huge);
    CHECK(capped.trim_db.size() == ac3::render::OutputLayout::kMaxSlots);
    CHECK(capped.delay_ms.size() == ac3::render::OutputLayout::kMaxSlots);

    // Numbers that do not read at all.
    for (const char* slots : {"", "-1", "two", "3 ", "0x3"}) {
        INFO(slots);
        const MemorySettingsStore bad{{{"speakers/slots", slots}, {"speakers/trimDb/1", "1"}}};
        CHECK(ac3::hearth::load_speaker_setup(bad).trim_db.empty());
    }
}

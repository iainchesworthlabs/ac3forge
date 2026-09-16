#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>

#include "queue.hpp"
#include "transport.hpp"

// ac3::hearth::Transport (apps/hearth/engine/transport.cpp): play, pause,
// stop, next, previous, seek, and what the engine has to do about each.
//
// Tagged [transport-state] rather than [transport]: tests/sendspin/ uses
// [transport] for Sendspin's own transport (the memory pair and the
// WebSocket), and a filter that quietly runs both suites is how one of them
// gets blamed for the other's failure.
//
// The transport owns no device, so every transition is reachable here -
// including the ones that are awkward to produce by hand on a real player:
// an item ending into another at a different sample rate, the playing item
// being deleted, the end of the queue arriving, gapless turned off
// mid-session.

namespace {

using ac3::hearth::FailurePolicy;
using ac3::hearth::ItemFacts;
using ac3::hearth::OpenOutputFormat;
using ac3::hearth::OutputMode;
using ac3::hearth::Queue;
using ac3::hearth::QueueItem;
using ac3::hearth::Transport;
using ac3::hearth::TransportAction;
using ac3::hearth::TransportState;

QueueItem item(std::string title, std::uint32_t rate = 48000) {
    // Built field by field rather than in one braced initialiser that both
    // reads and moves from `title`: the two are sequenced either way, but GCC
    // at -O3 inlines the string operations and then reports a null dereference
    // inside libstdc++ - the misattribution cmake/CompilerWarnings.cmake and
    // tests/CMakeLists.txt already document for io/test_metadata_edit.cpp.
    // Spelling the steps out avoids both the warning and the argument.
    QueueItem entry;
    entry.path = title + ".ec3";
    entry.title = std::move(title);
    entry.facts.stream = ac3::audio::BitstreamFormat::kEac3;
    entry.facts.sample_rate = rate;
    entry.facts.channels = 6;
    return entry;
}

// The output the caller opened for a 48 kHz item, as it would report it back.
OpenOutputFormat open_at(std::uint32_t rate, OutputMode mode = OutputMode::kLocalPcm) {
    return OpenOutputFormat{.sample_rate = rate, .channels = 8, .mode = mode};
}

}  // namespace

TEST_CASE("transport: play on an empty queue says so rather than doing nothing silently",
          "[hearth][transport-state]") {
    Queue queue;
    Transport transport{queue};

    const auto outcome = transport.play();
    CHECK(outcome.state == TransportState::kStopped);
    CHECK(outcome.action == TransportAction::kNone);
    CHECK_FALSE(outcome.note.empty());
}

TEST_CASE("transport: play, pause, play, stop", "[hearth][transport-state]") {
    Queue queue;
    queue.add(item("a"));
    Transport transport{queue};

    auto outcome = transport.play();
    CHECK(outcome.state == TransportState::kPlaying);
    CHECK(outcome.action == TransportAction::kStartItem);
    CHECK(outcome.item == 0);

    // Play while playing changes nothing, and says nothing.
    outcome = transport.play();
    CHECK(outcome.action == TransportAction::kNone);
    CHECK(outcome.state == TransportState::kPlaying);

    outcome = transport.pause();
    CHECK(outcome.state == TransportState::kPaused);
    CHECK(outcome.action == TransportAction::kPauseOutput);

    // Pause while paused likewise.
    outcome = transport.pause();
    CHECK(outcome.action == TransportAction::kNone);

    outcome = transport.play();
    CHECK(outcome.state == TransportState::kPlaying);
    CHECK(outcome.action == TransportAction::kResumeOutput);

    outcome = transport.stop();
    CHECK(outcome.state == TransportState::kStopped);
    CHECK(outcome.action == TransportAction::kStopOutput);

    // Stop while stopped asks for nothing - the output is already closed.
    outcome = transport.stop();
    CHECK(outcome.action == TransportAction::kNone);

    // And play after a stop starts where the stop left off, not at the front.
    queue.add(item("b"));
    REQUIRE(queue.set_current(1));
    outcome = transport.play();
    CHECK(outcome.item == 1);
    CHECK(outcome.action == TransportAction::kStartItem);
}

TEST_CASE("transport: an item ending into one of the same rate joins the open output",
          "[hearth][transport-state]") {
    Queue queue;
    queue.add(item("a", 48000));
    queue.add(item("b", 48000));
    Transport transport{queue};

    REQUIRE(transport.play().action == TransportAction::kStartItem);
    transport.set_open_format(open_at(48000));

    const auto outcome = transport.item_finished();
    CHECK(outcome.state == TransportState::kPlaying);
    CHECK(outcome.action == TransportAction::kJoinItem);
    CHECK(outcome.item == 1);
    // A join is the quiet case: nothing to tell the person.
    CHECK(outcome.note.empty());
    CHECK(queue.current_index() == 1);
}

TEST_CASE("transport: a rate change reopens the output and says why", "[hearth][transport-state]") {
    Queue queue;
    queue.add(item("48k", 48000));
    queue.add(item("44k1", 44100));
    Transport transport{queue};

    REQUIRE(transport.play().action == TransportAction::kStartItem);
    transport.set_open_format(open_at(48000));

    const auto outcome = transport.item_finished();
    CHECK(outcome.action == TransportAction::kReopenForItem);
    CHECK(outcome.item == 1);
    // The plan's "the app says so": both rates in the sentence, and the gap
    // admitted rather than glossed.
    CHECK(outcome.note.find("44100") != std::string::npos);
    CHECK(outcome.note.find("48000") != std::string::npos);
    CHECK(outcome.note.find("gap") != std::string::npos);
}

TEST_CASE("transport: gapless off reopens between every item", "[hearth][transport-state]") {
    Queue queue;
    queue.add(item("a", 48000));
    queue.add(item("b", 48000));
    Transport transport{queue};
    transport.set_gapless(false);
    CHECK_FALSE(transport.gapless());

    REQUIRE(transport.play().action == TransportAction::kStartItem);
    transport.set_open_format(open_at(48000));

    const auto outcome = transport.item_finished();
    CHECK(outcome.action == TransportAction::kReopenForItem);
    CHECK(outcome.note.find("Gapless is off") != std::string::npos);
}

TEST_CASE("transport: an unprobed item cannot be promised a join", "[hearth][transport-state]") {
    // Nothing has read the next item yet, so its rate is unknown. Reopening
    // is the answer that cannot be wrong; claiming a join and then finding a
    // different rate would be a click at best.
    Queue queue;
    queue.add(item("a", 48000));
    queue.add(item("unknown", /*rate=*/0));
    Transport transport{queue};

    REQUIRE(transport.play().action == TransportAction::kStartItem);
    transport.set_open_format(open_at(48000));

    CHECK(transport.item_finished().action == TransportAction::kReopenForItem);
}

TEST_CASE("transport: a mode change is not a join either", "[hearth][transport-state]") {
    // The finishing item was bitstreamed and the next one will be decoded
    // (or the other way about): the sink is being handed a different kind of
    // stream, so the output cannot simply continue.
    Queue queue;
    queue.add(item("a", 48000));
    queue.add(item("b", 48000));
    Transport transport{queue};

    REQUIRE(transport.play().action == TransportAction::kStartItem);
    transport.set_open_format(open_at(48000, OutputMode::kBitstream));
    transport.set_open_format(OpenOutputFormat{.sample_rate = 48000, .channels = 2,
                                               .mode = OutputMode::kNone});

    CHECK(transport.item_finished().action == TransportAction::kReopenForItem);
}

TEST_CASE("transport: a bitstream joins only the same stream, played the same way",
          "[hearth][transport-state]") {
    // The output decision says how the next item would be played; a join
    // needs that to be what the output is already doing, and a bitstream
    // needs the stream on the link to stay what it is.
    Queue queue;
    queue.add(item("e1"));
    queue.add(item("e2"));
    QueueItem plain = item("a3");
    plain.facts.stream = ac3::audio::BitstreamFormat::kAc3;
    queue.add(plain);
    queue.add(item("e4"));
    queue.add(item("e5"));
    queue.add(item("e6"));
    Transport transport{queue};

    REQUIRE(transport.play().action == TransportAction::kStartItem);
    const auto link = [](ac3::audio::BitstreamFormat stream, OutputMode mode) {
        return OpenOutputFormat{
            .sample_rate = 48000, .channels = 2, .mode = mode, .stream = stream};
    };
    transport.set_open_format(link(ac3::audio::BitstreamFormat::kEac3, OutputMode::kBitstream));

    // E-AC-3 after E-AC-3, both bitstreamed: the link carries on.
    auto outcome = transport.item_finished(OutputMode::kBitstream);
    CHECK(outcome.action == TransportAction::kJoinItem);
    CHECK(outcome.item == 1);

    // AC-3 after E-AC-3: the link would change speed, so it starts again,
    // and says why.
    outcome = transport.item_finished(OutputMode::kBitstream);
    CHECK(outcome.action == TransportAction::kReopenForItem);
    CHECK(outcome.item == 2);
    CHECK(outcome.note.find("\"a3\" is AC-3") != std::string::npos);
    CHECK(outcome.note.find("carrying E-AC-3") != std::string::npos);

    // After the AC-3 link, an item to be decoded here: another mode.
    transport.set_open_format(link(ac3::audio::BitstreamFormat::kAc3, OutputMode::kBitstream));
    outcome = transport.item_finished(OutputMode::kLocalPcm);
    CHECK(outcome.action == TransportAction::kReopenForItem);
    CHECK(outcome.item == 3);
    CHECK(outcome.note.find("plays as local PCM") != std::string::npos);
    CHECK(outcome.note.find("open for bitstream") != std::string::npos);

    // A decoded output takes any stream at its rate, but not an item the
    // decision would bitstream.
    transport.set_open_format(open_at(48000));
    outcome = transport.item_finished(OutputMode::kBitstream);
    CHECK(outcome.action == TransportAction::kReopenForItem);
    CHECK(outcome.item == 4);
    CHECK(outcome.note.find("plays as bitstream") != std::string::npos);

    // E-AC-3 transcoded to AC-3 joins E-AC-3 transcoded to AC-3, though the
    // link carries AC-3. And a failed item passes the mode on under skip.
    transport.set_open_format(
        link(ac3::audio::BitstreamFormat::kAc3, OutputMode::kBitstreamAsAc3));
    outcome = transport.item_failed(4, OutputMode::kBitstreamAsAc3);
    CHECK(outcome.action == TransportAction::kJoinItem);
    CHECK(outcome.item == 5);

    // An item that is AC-3 already is not one to transcode.
    Queue again;
    again.add(item("e1"));
    again.add(plain);
    Transport other{again};
    REQUIRE(other.play().action == TransportAction::kStartItem);
    other.set_open_format(link(ac3::audio::BitstreamFormat::kAc3, OutputMode::kBitstreamAsAc3));
    CHECK(other.item_finished(OutputMode::kBitstreamAsAc3).action ==
          TransportAction::kReopenForItem);
}

TEST_CASE("transport: the end of the queue stops, and repeat makes the ends meet",
          "[hearth][transport-state]") {
    Queue queue;
    queue.add(item("only"));
    Transport transport{queue};

    REQUIRE(transport.play().action == TransportAction::kStartItem);
    transport.set_open_format(open_at(48000));

    auto outcome = transport.item_finished();
    CHECK(outcome.state == TransportState::kStopped);
    CHECK(outcome.action == TransportAction::kStopOutput);
    CHECK(outcome.note.find("finished") != std::string::npos);

    // With repeat on, the same item comes round again - and since it is the
    // same format, it joins.
    transport.set_repeat(true);
    REQUIRE(transport.play().action == TransportAction::kStartItem);
    transport.set_open_format(open_at(48000));
    outcome = transport.item_finished();
    CHECK(outcome.state == TransportState::kPlaying);
    CHECK(outcome.action == TransportAction::kJoinItem);
    CHECK(outcome.item == 0);
}

TEST_CASE("transport: next and previous while playing reopen rather than join",
          "[hearth][transport-state]") {
    // Skipping is abandoning the current item part-way: whatever is queued
    // for it has to go, which is a reopen and a flush, not a continuation.
    Queue queue;
    queue.add(item("a"));
    queue.add(item("b"));
    Transport transport{queue};

    REQUIRE(transport.play().action == TransportAction::kStartItem);
    transport.set_open_format(open_at(48000));

    auto outcome = transport.next();
    CHECK(outcome.action == TransportAction::kReopenForItem);
    CHECK(outcome.item == 1);
    CHECK(outcome.state == TransportState::kPlaying);

    outcome = transport.previous();
    CHECK(outcome.action == TransportAction::kReopenForItem);
    CHECK(outcome.item == 0);

    // At the front, previous restarts the current item, as every other
    // player does.
    outcome = transport.previous();
    CHECK(outcome.action == TransportAction::kSeekItem);
    CHECK(outcome.seek_to == std::chrono::milliseconds{0});

    // Past the end while playing, next stops.
    REQUIRE(queue.set_current(1));
    outcome = transport.next();
    CHECK(outcome.state == TransportState::kStopped);
    CHECK(outcome.action == TransportAction::kStopOutput);
    CHECK(outcome.note.find("last item") != std::string::npos);
}

TEST_CASE("transport: next while stopped starts playing", "[hearth][transport-state]") {
    Queue queue;
    queue.add(item("a"));
    queue.add(item("b"));
    Transport transport{queue};

    const auto outcome = transport.next();
    CHECK(outcome.state == TransportState::kPlaying);
    CHECK(outcome.action == TransportAction::kStartItem);
    CHECK(outcome.item == 1);
}

TEST_CASE("transport: seeking works while paused and stopped, and does not start playback",
          "[hearth][transport-state]") {
    Queue queue;
    queue.add(item("a"));
    Transport transport{queue};

    // Stopped: the position moves, the state stands.
    auto outcome = transport.seek(std::chrono::milliseconds{5000});
    CHECK(outcome.state == TransportState::kStopped);
    CHECK(outcome.action == TransportAction::kSeekItem);
    CHECK(outcome.seek_to == std::chrono::milliseconds{5000});

    REQUIRE(transport.play().action == TransportAction::kStartItem);
    REQUIRE(transport.pause().action == TransportAction::kPauseOutput);
    outcome = transport.seek(std::chrono::milliseconds{1000});
    CHECK(outcome.state == TransportState::kPaused);
    CHECK(outcome.action == TransportAction::kSeekItem);

    // A negative target is clamped rather than refused.
    outcome = transport.seek(std::chrono::milliseconds{-1});
    CHECK(outcome.seek_to == std::chrono::milliseconds{0});

    // With nothing current, there is nothing to seek within.
    Queue empty;
    Transport idle{empty};
    CHECK(idle.seek(std::chrono::milliseconds{10}).action == TransportAction::kNone);
}

TEST_CASE("transport: the playing item being deleted restarts on what took its place",
          "[hearth][transport-state]") {
    Queue queue;
    queue.add(item("a"));
    queue.add(item("b"));
    Transport transport{queue};

    REQUIRE(transport.play().action == TransportAction::kStartItem);
    transport.set_open_format(open_at(48000));
    REQUIRE(queue.remove(0));  // the item that was playing

    auto outcome = transport.current_item_removed();
    CHECK(outcome.state == TransportState::kPlaying);
    CHECK(outcome.action == TransportAction::kReopenForItem);
    CHECK(outcome.item == 0);
    CHECK(outcome.note.find("removed") != std::string::npos);

    // Emptying the queue under a playing item stops it.
    queue.clear();
    outcome = transport.current_item_removed();
    CHECK(outcome.state == TransportState::kStopped);
    CHECK(outcome.action == TransportAction::kStopOutput);
    CHECK(outcome.note.find("emptied") != std::string::npos);
}

TEST_CASE("transport: an item that cannot be played is reported, not started",
          "[hearth][transport-state]") {
    Queue queue;
    QueueItem ac4 = item("ac4");
    ac4.facts.unplayable_because = "AC-4 has no decoder in this build yet";
    queue.add(ac4);
    Transport transport{queue};

    const auto outcome = transport.play();
    CHECK(outcome.action == TransportAction::kNone);
    CHECK(outcome.note.find("AC-4") != std::string::npos);
    CHECK(outcome.state == TransportState::kStopped);
}

TEST_CASE("transport: an item that fails is passed over, or stops playback at it",
          "[hearth][transport-state]") {
    // The caller has marked b, which would not open, as it does before
    // asking.
    Queue queue;
    queue.add(item("a"));
    QueueItem b = item("b");
    b.facts.unplayable_because = "the file is not there";
    queue.add(b);
    queue.add(item("c"));
    Transport transport{queue};
    CHECK(transport.on_failure() == FailurePolicy::kSkip);

    // Skipping is what the end of an item does: on to the next that can play.
    REQUIRE(transport.play().action == TransportAction::kStartItem);
    transport.set_open_format(open_at(48000));
    auto outcome = transport.item_failed(1);
    CHECK(outcome.state == TransportState::kPlaying);
    CHECK(outcome.action == TransportAction::kJoinItem);
    CHECK(outcome.item == 2);
    CHECK(queue.current_index() == 2);

    // Stopping stops there, with the item current and the reason given.
    transport.set_on_failure(FailurePolicy::kStop);
    REQUIRE(queue.set_current(0));
    outcome = transport.item_failed(1);
    CHECK(outcome.state == TransportState::kStopped);
    CHECK(outcome.action == TransportAction::kStopOutput);
    CHECK(outcome.item == 1);
    CHECK(queue.current_index() == 1);
    CHECK(outcome.note.find("\"b\"") != std::string::npos);
    CHECK(outcome.note.find("the file is not there") != std::string::npos);
    CHECK(transport.state() == TransportState::kStopped);

    // Already stopped, there is nothing to close; past the end, nothing to
    // show.
    outcome = transport.item_failed(1);
    CHECK(outcome.action == TransportAction::kNone);
    CHECK(outcome.item == 1);
    outcome = transport.item_failed(7);
    CHECK(outcome.action == TransportAction::kNone);
    CHECK(outcome.item == Queue::kNone);
    CHECK(queue.current_index() == 1);
    REQUIRE(transport.play().action == TransportAction::kNone);
    transport.set_on_failure(FailurePolicy::kSkip);
    REQUIRE(transport.next().action == TransportAction::kStartItem);
    outcome = transport.item_failed(7);
    CHECK(outcome.action == TransportAction::kStopOutput);
    CHECK(outcome.note.find("finished") != std::string::npos);
}

TEST_CASE("transport: every state and action describes itself", "[hearth][transport-state]") {
    for (const auto state :
         {TransportState::kStopped, TransportState::kPlaying, TransportState::kPaused}) {
        const std::string_view text = ac3::hearth::describe(state);
        CHECK_FALSE(text.empty());
        CHECK(text != "unknown transport state");
    }
    for (const auto action :
         {TransportAction::kNone, TransportAction::kStartItem, TransportAction::kJoinItem,
          TransportAction::kReopenForItem, TransportAction::kPauseOutput,
          TransportAction::kResumeOutput, TransportAction::kStopOutput,
          TransportAction::kSeekItem}) {
        const std::string_view text = ac3::hearth::describe(action);
        CHECK_FALSE(text.empty());
        CHECK(text != "unknown transport action");
    }
    CHECK(ac3::hearth::describe(FailurePolicy::kSkip) == "skip to the next");
    CHECK(ac3::hearth::describe(FailurePolicy::kStop) == "stop");
}

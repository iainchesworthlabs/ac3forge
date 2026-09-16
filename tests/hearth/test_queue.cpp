#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include "queue.hpp"

// ac3::hearth::Queue (apps/hearth/engine/queue.cpp): the play queue, and what
// happens to the item that is playing when the list changes around it.
//
// Every case here is a person doing something ordinary to a queue while a
// track is playing - deleting the track, dragging it, dropping a folder in
// front of it - and the question is always the same one: is the item that was
// playing still the item that is playing? Nothing here reads a file.

namespace {

using ac3::hearth::ItemFacts;
using ac3::hearth::Queue;
using ac3::hearth::QueueItem;

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

QueueItem unplayable(std::string title, std::string because) {
    QueueItem entry = item(std::move(title));
    entry.facts.unplayable_because = std::move(because);
    return entry;
}

std::vector<std::string> titles(const Queue& queue) {
    std::vector<std::string> out;
    for (const auto& entry : queue.items()) {
        out.push_back(entry.title);
    }
    return out;
}

}  // namespace

TEST_CASE("queue: the first item added becomes current, so play has somewhere to start",
          "[hearth][queue]") {
    Queue queue;
    CHECK(queue.empty());
    CHECK(queue.current_index() == Queue::kNone);
    CHECK(queue.current() == nullptr);

    queue.add(item("first"));
    queue.add(item("second"));
    CHECK(queue.size() == 2);
    CHECK(queue.current_index() == 0);
    REQUIRE(queue.current() != nullptr);
    CHECK(queue.current()->title == "first");
}

TEST_CASE("queue: inserting before the playing item does not change what is playing",
          "[hearth][queue]") {
    Queue queue;
    queue.add(item("a"));
    queue.add(item("b"));
    REQUIRE(queue.set_current(1));

    queue.insert(0, item("dropped"));
    CHECK(titles(queue) == std::vector<std::string>{"dropped", "a", "b"});
    // Still "b", at its new index.
    CHECK(queue.current_index() == 2);
    CHECK(queue.current()->title == "b");
}

TEST_CASE("queue: deleting the playing item leaves the next one current", "[hearth][queue]") {
    Queue queue;
    queue.add(item("a"));
    queue.add(item("b"));
    queue.add(item("c"));
    REQUIRE(queue.set_current(1));

    // Deleting the track that is playing is a person saying "not this one":
    // what follows takes its place rather than the queue jumping to the top.
    CHECK(queue.remove(1));
    CHECK(titles(queue) == std::vector<std::string>{"a", "c"});
    CHECK(queue.current_index() == 1);
    CHECK(queue.current()->title == "c");

    // Deleting the last item leaves nothing playing.
    CHECK(queue.remove(1));
    CHECK(queue.current_index() == Queue::kNone);

    // And deleting an item before the current one only shifts its index.
    queue.add(item("d"));
    REQUIRE(queue.set_current(1));
    CHECK(queue.current()->title == "d");
    CHECK_FALSE(queue.remove(0));
    CHECK(queue.current_index() == 0);
    CHECK(queue.current()->title == "d");
}

TEST_CASE("queue: dragging an item carries the current marker with it", "[hearth][queue]") {
    Queue queue;
    queue.add(item("a"));
    queue.add(item("b"));
    queue.add(item("c"));
    REQUIRE(queue.set_current(0));

    // Drag the playing item to the end: it is still the playing item.
    CHECK(queue.move(0, 2));
    CHECK(titles(queue) == std::vector<std::string>{"b", "c", "a"});
    CHECK(queue.current()->title == "a");

    // Drag something else from after it to before it: the marker follows the
    // item, not the index.
    CHECK(queue.move(0, 2));
    CHECK(titles(queue) == std::vector<std::string>{"c", "a", "b"});
    CHECK(queue.current()->title == "a");

    CHECK_FALSE(queue.move(0, 9));
    CHECK_FALSE(queue.move(9, 0));
}

TEST_CASE("queue: next and previous skip what cannot be played", "[hearth][queue]") {
    Queue queue;
    queue.add(item("first"));
    // AC-4 today: listed, and skipped with the reason shown.
    queue.add(unplayable("ac4", "AC-4 has no decoder in this build yet"));
    queue.add(item("third"));
    REQUIRE(queue.set_current(0));

    CHECK(queue.next_index() == 2);
    REQUIRE(queue.set_current(2));
    CHECK(queue.previous_index() == 0);

    // The ends are ends, unless repeat is on.
    CHECK(queue.next_index() == Queue::kNone);
    CHECK(queue.next_index(/*repeat=*/true) == 0);
    REQUIRE(queue.set_current(0));
    CHECK(queue.previous_index() == Queue::kNone);
    CHECK(queue.previous_index(/*repeat=*/true) == 2);
}

TEST_CASE("queue: a queue of nothing playable has no next item, even with repeat on",
          "[hearth][queue]") {
    // The case a loop would hang on: repeat plus nothing to repeat.
    Queue queue;
    queue.add(unplayable("one", "no decoder"));
    queue.add(unplayable("two", "no decoder"));
    REQUIRE(queue.set_current(0));

    CHECK(queue.next_index() == Queue::kNone);
    CHECK(queue.next_index(/*repeat=*/true) == Queue::kNone);
    CHECK(queue.previous_index(/*repeat=*/true) == Queue::kNone);
}

TEST_CASE("queue: facts arrive after the item does", "[hearth][queue]") {
    Queue queue;
    queue.add(item("a", /*rate=*/0));
    CHECK(queue.items()[0].facts.sample_rate == 0);
    CHECK(queue.items()[0].playable());

    ItemFacts probed;
    probed.sample_rate = 44100;
    probed.channels = 6;
    CHECK(queue.set_facts(0, probed));
    CHECK(queue.items()[0].facts.sample_rate == 44100);
    CHECK(queue.items()[0].facts.channels == 6);
    CHECK_FALSE(queue.set_facts(9, ItemFacts{}));

    queue.clear();
    CHECK(queue.empty());
    CHECK(queue.current_index() == Queue::kNone);
}

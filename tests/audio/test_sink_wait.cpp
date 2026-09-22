#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

#include "sink_wait.hpp"

// apps/common/sink_wait.hpp: how ac3cli's play, monitor, identify and live
// wait on a sink. None of those commands reaches its wait without a render
// device, and a device cannot be pulled on a CI runner, so these cases hold
// the waits against sinks that refuse and stop on cue. That a real sink stops
// itself when its device goes is the backends' part, and the hidden
// "[passthrough-unplug]" and "[monitor-unplug]" cases are where a person
// checks it.

using namespace std::chrono_literals;

namespace {

// A sink with a queue that is full for `full_offers` offers and then has
// room, and a device that goes away at offer `gone_at_offer` (0: never). Once
// gone it refuses everything, as a PassthroughSink that stopped itself does.
struct ScriptedSink {
    int full_offers = 0;
    int gone_at_offer = 0;
    int offers = 0;
    bool gone = false;
    std::vector<int> taken{};
    std::vector<std::size_t> frames_taken{};

    bool submit(int payload, std::size_t frames = 1) {
        ++offers;
        if (offers == gone_at_offer) {
            gone = true;
        }
        if (gone) {
            return false;
        }
        if (full_offers > 0) {
            --full_offers;
            return false;
        }
        taken.push_back(payload);
        frames_taken.push_back(frames);
        return true;
    }

    [[nodiscard]] bool running() const { return !gone; }
};

// A sink whose render thread stops it, as every backend's does when its
// device goes. Its queue never has room, which is what a caller waiting on a
// dead device sees until running() says why.
struct RenderThreadSink {
    std::atomic<bool> alive{true};

    bool submit(int /*payload*/) { return false; }

    [[nodiscard]] bool running() const { return alive.load(std::memory_order_acquire); }
};

}  // namespace

TEST_CASE("sink wait: a full queue is waited out until the sink takes the payload",
          "[sink-wait][concurrency]") {
    ScriptedSink sink{.full_offers = 3};
    CHECK(ac3::apps::submit_while_running(sink, 1ms, 7));
    CHECK(sink.offers == 4);
    CHECK(sink.taken == std::vector<int>{7});
}

TEST_CASE("sink wait: a sink that stops itself ends the wait with nothing queued",
          "[sink-wait][concurrency]") {
    // The queue stays full for longer than the test runs: only running()
    // going false can end this wait.
    ScriptedSink sink{.full_offers = 1'000'000, .gone_at_offer = 3};
    CHECK_FALSE(ac3::apps::submit_while_running(sink, 1ms, 7));
    CHECK(sink.offers == 3);
    CHECK(sink.taken.empty());
}

TEST_CASE("sink wait: a sink already stopped is offered the payload once",
          "[sink-wait][concurrency]") {
    ScriptedSink sink{.gone = true};
    CHECK_FALSE(ac3::apps::submit_while_running(sink, 1ms, 7));
    CHECK(sink.offers == 1);
}

TEST_CASE("sink wait: every argument reaches submit", "[sink-wait][concurrency]") {
    // PcmOutput::submit takes the rendered channels and a frame count.
    ScriptedSink sink{.full_offers = 1};
    CHECK(ac3::apps::submit_while_running(sink, 1ms, 7, std::size_t{480}));
    CHECK(sink.taken == std::vector<int>{7});
    CHECK(sink.frames_taken == std::vector<std::size_t>{480});
}

TEST_CASE("sink wait: waiting for the queue to play out ends once it has",
          "[sink-wait][concurrency]") {
    const ScriptedSink sink;
    int looks = 0;
    CHECK(ac3::apps::wait_while_running(sink, 1ms, [&looks] { return ++looks == 3; }));
    CHECK(looks == 3);
}

TEST_CASE("sink wait: waiting for the queue to play out ends when the sink stops itself",
          "[sink-wait][concurrency]") {
    ScriptedSink sink;
    int looks = 0;
    CHECK_FALSE(ac3::apps::wait_while_running(sink, 1ms, [&] {
        // The device goes on the third look, with the queue still unplayed.
        if (++looks == 3) {
            sink.gone = true;
        }
        return false;
    }));
    CHECK(looks == 3);
}

TEST_CASE("sink wait: a queue that played out counts even if the sink has stopped since",
          "[sink-wait][concurrency]") {
    const ScriptedSink sink{.gone = true};
    CHECK(ac3::apps::wait_while_running(sink, 1ms, [] { return true; }));
}

TEST_CASE("sink wait: a sink stopped by its own render thread ends both waits",
          "[sink-wait][concurrency]") {
    {
        RenderThreadSink sink;
        const std::jthread render([&sink] {
            std::this_thread::sleep_for(20ms);
            sink.alive.store(false, std::memory_order_release);
        });
        CHECK_FALSE(ac3::apps::submit_while_running(sink, 1ms, 7));
    }
    {
        RenderThreadSink sink;
        const std::jthread render([&sink] {
            std::this_thread::sleep_for(20ms);
            sink.alive.store(false, std::memory_order_release);
        });
        CHECK_FALSE(ac3::apps::wait_while_running(sink, 1ms, [] { return false; }));
    }
}

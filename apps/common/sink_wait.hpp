#pragma once

#include <chrono>
#include <thread>

// Waiting on an ac3::audio sink without waiting for ever.
//
// A sink's submit() refuses for two reasons. Its queue is full, which time
// cures: the caller is ahead of real time and should wait. Or it is not
// running, which nothing cures: it stopped itself because its device went
// away (PassthroughSink::running(), MonitorSink::running()). A loop that
// retries on a refusal has to tell the two apart, and so does one waiting for
// the queue to play out, since a stopped sink's counts never move again.
// ac3cli's play, monitor, identify and live all wait both ways.
//
// Templates over the sink, since PassthroughSink, MonitorSink and PcmOutput
// share the shape but no base class. Compiled straight into ac3cli and
// ac3tests, as stream_playback.hpp beside it is, so a test can hold both
// helpers against a sink that stops on cue: no command reaches its wait
// without a render device, and no device can be pulled on a CI runner.

namespace ac3::apps {

// Offers `payload` to sink.submit() until the sink takes it, sleeping `pause`
// between offers. False, having queued nothing, once the sink is not running.
template <typename Sink, typename... Payload>
[[nodiscard]] bool submit_while_running(Sink& sink, std::chrono::milliseconds pause,
                                        const Payload&... payload) {
    while (!sink.submit(payload...)) {
        if (!sink.running()) {
            return false;
        }
        std::this_thread::sleep_for(pause);
    }
    return true;
}

// Sleeps `pause` at a time until `done()` is true: the queue has played out,
// say. False once the sink is not running and done() is still false, since
// what is left will never play.
template <typename Sink, typename Done>
[[nodiscard]] bool wait_while_running(const Sink& sink, std::chrono::milliseconds pause,
                                      Done done) {
    while (!done()) {
        if (!sink.running()) {
            return false;
        }
        std::this_thread::sleep_for(pause);
    }
    return true;
}

}  // namespace ac3::apps

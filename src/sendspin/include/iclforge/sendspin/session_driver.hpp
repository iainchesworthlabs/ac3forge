#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "ac3/sendspin/session.hpp"
#include "ac3/sendspin/transport.hpp"

// Runs one session over one transport::Connection on a computer. A reader thread hands the
// session each frame that arrives; a writer thread sends what the session answers, in the order
// the session produced it, and ticks the session when its timers are due. The owner's own calls
// go through call(), so the session sees one caller at a time, as PlayerSession and
// ServerSession require.
//
// Sending happens outside the session's lock, so a slow peer holds up only its own writer. What
// waits to be sent is bounded: a peer that stops reading until more than max_queued_bytes are
// waiting is disconnected, and an owner that paces audio reads queued_bytes() first.
//
// A session's listener callbacks run inside the driver's calls, on the driver's threads with
// the session's lock held. They must not call call() on the same driver, which would deadlock:
// an owner that answers an event, such as activating a client once its hello arrives, does so
// from a thread of its own.
//
// Drivers can share one lock. A client's sessions share its pairing state, which the owner
// serialises every call on them for, so a player gives all its drivers the same lock; a callback
// then must not call call() on any of them.
//
// A board has no driver: its WebSocket handler calls the session directly.

namespace ac3::sendspin {

// Local monotonic time from std::chrono::steady_clock.
class SteadyClock final : public Clock {
   public:
    [[nodiscard]] std::int64_t now_us() const override;
};

// What the driver calls on a session.
struct DrivenSession {
    std::function<SessionOutput(const transport::Frame&)> receive;
    std::function<SessionOutput()> tick;
    // Microseconds until tick() is due at the latest.
    std::function<std::int64_t()> next_tick_us;
    // Called once, from the reader thread without the lock held, when the connection has ended
    // and both threads are finishing. It must not destroy the driver.
    std::function<void()> ended;
};

class SessionDriver {
   public:
    static constexpr std::size_t kDefaultMaxQueuedBytes = 8 * 1024 * 1024;

    // `lock` is the session's lock, which other drivers may share; a new one when null.
    SessionDriver(std::unique_ptr<transport::Connection> connection, DrivenSession session,
                  std::size_t max_queued_bytes = kDefaultMaxQueuedBytes, std::shared_ptr<std::mutex> lock = nullptr);
    // Ends the connection and waits for both threads; never from a listener callback.
    ~SessionDriver();
    SessionDriver(const SessionDriver&) = delete;
    SessionDriver& operator=(const SessionDriver&) = delete;
    SessionDriver(SessionDriver&&) = delete;
    SessionDriver& operator=(SessionDriver&&) = delete;

    // Starts both threads. `first` goes out before anything else, such as the client/init
    // PlayerSession::open() returns.
    void start(SessionOutput first = {});

    // Runs `call` on the session with its lock held and queues the output it returns, a
    // SessionOutput or a std::expected<SessionOutput, E>. Returns nothing for the first, and
    // the refusal, if any, for the second.
    template <class Call>
    auto call(Call&& call) {
        std::unique_lock lock(*mutex_);
        return deliver(std::forward<Call>(call)(), lock);
    }

    // Runs `read` with the session's lock held and returns what it returns, for reading the
    // session's state.
    template <class Read>
    auto inspect(Read&& read) const {
        const std::lock_guard lock(*mutex_);
        return std::forward<Read>(read)();
    }

    // Ends the connection now, dropping what is still queued.
    void close();
    // Waits until the connection has ended and both threads have finished.
    void join();

    [[nodiscard]] bool ended() const;
    [[nodiscard]] std::size_t queued_bytes() const;
    [[nodiscard]] std::string peer() const { return connection_->peer(); }

   private:
    void deliver(SessionOutput out, const std::unique_lock<std::mutex>& lock);
    template <class Error>
    std::expected<void, Error> deliver(std::expected<SessionOutput, Error> out,
                                       const std::unique_lock<std::mutex>& lock) {
        if (!out) {
            return std::unexpected(out.error());
        }
        deliver(std::move(*out), lock);
        return {};
    }
    void end_locked();
    void read_loop();
    void write_loop();

    std::unique_ptr<transport::Connection> connection_;
    DrivenSession session_;
    std::size_t max_queued_bytes_;

    // The session's lock, which also guards everything below it.
    std::shared_ptr<std::mutex> mutex_;
    std::condition_variable wake_;
    std::deque<transport::Frame> queue_;
    std::size_t queued_bytes_ = 0;
    // The session asked to close once its frames are sent.
    bool closing_ = false;
    // Set by the reader when the connection ends, or by close(); the writer then stops.
    bool ended_ = false;
    // Something new for the writer: frames queued, or a session call that may move the tick.
    bool changed_ = false;

    std::thread reader_;
    std::thread writer_;
};

}  // namespace ac3::sendspin

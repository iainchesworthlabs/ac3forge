#include "ac3/sendspin/session_driver.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "ac3/sendspin/session.hpp"
#include "ac3/sendspin/transport.hpp"

namespace ac3::sendspin {

namespace {

using Steady = std::chrono::steady_clock;

[[nodiscard]] Steady::time_point after(std::int64_t microseconds) {
    return Steady::now() + std::chrono::microseconds(std::max<std::int64_t>(microseconds, 0));
}

}  // namespace

std::int64_t SteadyClock::now_us() const {
    return std::chrono::duration_cast<std::chrono::microseconds>(Steady::now().time_since_epoch()).count();
}

SessionDriver::SessionDriver(std::unique_ptr<transport::Connection> connection, DrivenSession session,
                             std::size_t max_queued_bytes, std::shared_ptr<std::mutex> lock)
    : connection_(std::move(connection)),
      session_(std::move(session)),
      max_queued_bytes_(max_queued_bytes),
      mutex_(lock ? std::move(lock) : std::make_shared<std::mutex>()) {}

SessionDriver::~SessionDriver() {
    close();
    join();
}

void SessionDriver::start(SessionOutput first) {
    {
        const std::unique_lock lock(*mutex_);
        deliver(std::move(first), lock);
    }
    writer_ = std::thread([this] { write_loop(); });
    reader_ = std::thread([this] { read_loop(); });
}

void SessionDriver::deliver(SessionOutput out, const std::unique_lock<std::mutex>& /*lock*/) {
    // Nothing more goes out once the session has asked to close or the connection has ended.
    if (ended_ || closing_) {
        return;
    }
    for (transport::Frame& frame : out.frames) {
        queued_bytes_ += frame.bytes.size();
        queue_.push_back(std::move(frame));
    }
    closing_ = out.close;
    if (queued_bytes_ > max_queued_bytes_) {
        // The peer has stopped reading. Closing the connection also ends the send the writer
        // is waiting in.
        end_locked();
        connection_->close();
        return;
    }
    changed_ = true;
    wake_.notify_all();
}

void SessionDriver::end_locked() {
    ended_ = true;
    queue_.clear();
    queued_bytes_ = 0;
    wake_.notify_all();
}

void SessionDriver::close() {
    {
        const std::lock_guard lock(*mutex_);
        end_locked();
    }
    connection_->close();
}

void SessionDriver::join() {
    if (reader_.joinable()) {
        reader_.join();
    }
    if (writer_.joinable()) {
        writer_.join();
    }
}

bool SessionDriver::ended() const {
    const std::lock_guard lock(*mutex_);
    return ended_;
}

std::size_t SessionDriver::queued_bytes() const {
    const std::lock_guard lock(*mutex_);
    return queued_bytes_;
}

void SessionDriver::read_loop() {
    while (true) {
        std::optional<transport::Frame> frame = connection_->receive();
        std::unique_lock lock(*mutex_);
        if (!frame) {
            end_locked();
            break;
        }
        if (!ended_ && !closing_) {
            deliver(session_.receive(*frame), lock);
        }
    }
    if (session_.ended) {
        session_.ended();
    }
}

void SessionDriver::write_loop() {
    std::unique_lock lock(*mutex_);
    Steady::time_point due = after(session_.next_tick_us());
    while (!ended_) {
        if (!queue_.empty()) {
            transport::Frame frame = std::move(queue_.front());
            queue_.pop_front();
            queued_bytes_ -= frame.bytes.size();
            lock.unlock();
            const bool sent = frame.kind == transport::FrameKind::kText ? connection_->send_text(frame.text())
                                                                         : connection_->send_binary(frame.bytes);
            lock.lock();
            if (!sent) {
                end_locked();
            }
            continue;
        }
        if (closing_) {
            break;
        }
        if (changed_) {
            // A session call can bring the next tick forward, and never pushes it back.
            changed_ = false;
            due = std::min(due, after(session_.next_tick_us()));
        }
        if (Steady::now() >= due) {
            deliver(session_.tick(), lock);
            due = after(session_.next_tick_us());
            continue;
        }
        wake_.wait_until(lock, due, [this] { return ended_ || changed_ || !queue_.empty(); });
    }
    end_locked();
    lock.unlock();
    // Ends the reader's receive().
    connection_->close();
}

}  // namespace ac3::sendspin

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ac3/sendspin/transport.hpp"

// The in-memory transport: two queues and one shared closed flag.

namespace ac3::sendspin::transport {

namespace {

struct Shared {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Frame> to_first;
    std::deque<Frame> to_second;
    bool closed = false;
};

class MemoryConnection final : public Connection {
   public:
    MemoryConnection(std::shared_ptr<Shared> shared, bool first, std::string peer)
        : shared_(std::move(shared)), first_(first), peer_(std::move(peer)) {}

    ~MemoryConnection() override { close(); }
    MemoryConnection(const MemoryConnection&) = delete;
    MemoryConnection& operator=(const MemoryConnection&) = delete;
    MemoryConnection(MemoryConnection&&) = delete;
    MemoryConnection& operator=(MemoryConnection&&) = delete;

    std::optional<Frame> receive() override {
        std::unique_lock lock(shared_->mutex);
        std::deque<Frame>& inbox = first_ ? shared_->to_first : shared_->to_second;
        shared_->changed.wait(lock, [&] { return shared_->closed || !inbox.empty(); });
        if (inbox.empty()) {
            return std::nullopt;
        }
        Frame frame = std::move(inbox.front());
        inbox.pop_front();
        return frame;
    }

    bool send_text(std::string_view text) override {
        Frame frame{.kind = FrameKind::kText, .bytes = {}};
        frame.bytes.assign(text.begin(), text.end());
        return push(std::move(frame));
    }

    bool send_binary(std::span<const std::uint8_t> bytes) override {
        return push(Frame{.kind = FrameKind::kBinary, .bytes = {bytes.begin(), bytes.end()}});
    }

    void close() override {
        {
            const std::lock_guard lock(shared_->mutex);
            shared_->closed = true;
        }
        shared_->changed.notify_all();
    }

    std::string peer() const override { return peer_; }

   private:
    bool push(Frame frame) {
        {
            const std::lock_guard lock(shared_->mutex);
            if (shared_->closed) {
                return false;
            }
            (first_ ? shared_->to_second : shared_->to_first).push_back(std::move(frame));
        }
        shared_->changed.notify_all();
        return true;
    }

    std::shared_ptr<Shared> shared_;
    bool first_;
    std::string peer_;
};

}  // namespace

std::pair<std::unique_ptr<Connection>, std::unique_ptr<Connection>> memory_pair(
    std::string first_name, std::string second_name) {
    auto shared = std::make_shared<Shared>();
    return {std::make_unique<MemoryConnection>(shared, true, std::move(second_name)),
            std::make_unique<MemoryConnection>(shared, false, std::move(first_name))};
}

}  // namespace ac3::sendspin::transport

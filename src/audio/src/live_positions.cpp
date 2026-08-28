#include "ac3/audio/live_positions.hpp"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include "ac3/oba/scene_osc.hpp"
#include "net/udp_socket.hpp"

namespace ac3::audio {

std::string_view describe(PositionSourceError error) {
    switch (error) {
        case PositionSourceError::kBadAddress:
            return "not a valid IPv4 dotted-quad address";
        case PositionSourceError::kSocketFailed:
            return "the socket layer failed";
        case PositionSourceError::kBindFailed:
            return "bind failed - the port may already be in use";
        case PositionSourceError::kAlreadyRunning:
            return "already listening";
    }
    return "unknown error";
}

namespace {

// The largest legal UDP payload (65,507 bytes for IPv4) rounded up to a
// round number - UDP's own 16-bit length field means nothing legal is ever
// larger, which is what UdpSocket::recv's own "cannot silently truncate"
// contract relies on.
constexpr std::size_t kRecvBufferSize = 65535;
// How often the receiver thread wakes to re-check whether it should stop,
// while nothing has arrived - short enough that stop() returns promptly
// (see LivePositionSource::stop's own comment), long enough not to spin.
constexpr std::uint32_t kRecvTimeoutMs = 250;

PositionSourceError map_error(UdpSocketError error) {
    switch (error) {
        case UdpSocketError::kBadAddress:
            return PositionSourceError::kBadAddress;
        case UdpSocketError::kSocketFailed:
            return PositionSourceError::kSocketFailed;
        case UdpSocketError::kBindFailed:
            return PositionSourceError::kBindFailed;
    }
    return PositionSourceError::kSocketFailed;
}

}  // namespace

struct LivePositionSource::Impl {
    explicit Impl(std::size_t objects) : pending(objects), recv_buffer(kRecvBufferSize) {}

    UdpSocket socket;
    std::thread thread;
    std::atomic_bool running{false};

    // Everything below is touched by both the receiver thread (merging a
    // parsed packet in) and drain_into (reading and clearing it) - guarded
    // by the same mutex the GUI's own live_object_mutex_ uses for the
    // identical writer-thread/frame-reader-thread shape
    // (apps/gui/encoder_controller.hpp), just with the roles that precedent
    // already establishes generalised to a network source instead of a
    // Q_INVOKABLE call from the GUI thread.
    mutable std::mutex mutex;
    // One slot per object, sized ONCE here - never grown, so drain_into
    // never allocates. A slot holds whatever fields have arrived and not
    // yet been applied: a gain/lfe-only update stays pending (not applied,
    // not dropped) until a position finally arrives for the same object,
    // per ac3::oba::apply's own contract.
    std::vector<std::optional<ac3::oba::SceneOscUpdate>> pending;
    PositionSourceStats stats;

    // Reused every recv() call rather than allocated per datagram - this is
    // the receiver thread, not the render path, but there is no reason to
    // allocate here either.
    std::vector<std::byte> recv_buffer;

    void merge(const ac3::oba::SceneOscUpdate& update) {
        // Not locked here - the caller (run()) already holds `mutex` for the
        // whole batch a single datagram produced, so one packet's messages
        // are merged atomically with respect to a concurrent drain_into.
        if (update.object >= pending.size()) {
            ++stats.messages_dropped;  // an address this session has no slot for
            return;
        }
        if (update.release) {
            pending[update.object] = ac3::oba::SceneOscUpdate{.object = update.object, .release = true};
            return;
        }
        auto& slot = pending[update.object];
        if (!slot || slot->release) {
            slot = ac3::oba::SceneOscUpdate{.object = update.object};
        }
        if (update.position) {
            slot->position = update.position;
        }
        if (update.gain) {
            slot->gain = update.gain;
        }
        if (update.lfe_send) {
            slot->lfe_send = update.lfe_send;
        }
    }

    void run() {
        while (running.load(std::memory_order_relaxed)) {
            const auto got = socket.recv(recv_buffer, kRecvTimeoutMs);
            if (!got) {
                continue;  // timeout - loop back and re-check `running`
            }
            // Allocates (parse_osc_packet's own vector) - fine here, this is
            // the receiver thread, never the audio/encode path drain_into
            // runs on.
            ac3::oba::OscParseStats parse_stats;
            const auto updates =
                ac3::oba::parse_osc_packet(std::span{recv_buffer}.first(*got), &parse_stats);

            const std::lock_guard<std::mutex> lock(mutex);
            ++stats.datagrams;
            stats.packets_rejected += parse_stats.packets_rejected;
            stats.messages_dropped += parse_stats.messages_dropped;
            for (const auto& update : updates) {
                merge(update);
            }
        }
    }
};

LivePositionSource::LivePositionSource(std::size_t objects)
    : impl_(std::make_unique<Impl>(objects)) {}

LivePositionSource::~LivePositionSource() { stop(); }

std::expected<void, PositionSourceError> LivePositionSource::start(std::string_view bind_address,
                                                                    std::uint16_t port) {
    if (impl_->running.load(std::memory_order_relaxed)) {
        return std::unexpected(PositionSourceError::kAlreadyRunning);
    }
    const auto bound = impl_->socket.bind(bind_address, port);
    if (!bound) {
        return std::unexpected(map_error(bound.error()));
    }
    impl_->running.store(true, std::memory_order_relaxed);
    impl_->thread = std::thread{[this] { impl_->run(); }};
    return {};
}

void LivePositionSource::stop() {
    // exchange(false) rather than a plain store(): this must be safe to call
    // unconditionally (the destructor does, whether or not start() ever
    // succeeded), and only actually joins when a thread was really started.
    if (impl_->running.exchange(false, std::memory_order_relaxed) && impl_->thread.joinable()) {
        // The receiver thread notices within kRecvTimeoutMs (its recv()
        // call returns on timeout and re-checks `running`), so this joins
        // in well under a second rather than needing a wakeup datagram or a
        // socket shutdown() trick.
        impl_->thread.join();
    }
    impl_->socket.close();
}

bool LivePositionSource::running() const {
    return impl_->running.load(std::memory_order_relaxed);
}

std::uint16_t LivePositionSource::local_port() const { return impl_->socket.local_port(); }

PositionSourceStats LivePositionSource::stats() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->stats;
}

void LivePositionSource::drain_into(ac3::oba::SceneCursor& cursor, double time_s) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    for (std::size_t i = 0; i < impl_->pending.size(); ++i) {
        auto& slot = impl_->pending[i];
        if (!slot) {
            continue;
        }
        if (slot->release) {
            cursor.release(i);
            slot.reset();
            continue;
        }
        // ALREADY rotated (ObjectScene::evaluate's own contract) - passed to
        // apply() as the base whose gain/lfe_send/extent carry through, and
        // NEVER as the position to push; see apply()'s own header comment
        // for why reusing it there would rotate the scene's orientation
        // onto a live object twice.
        const auto base = cursor.scene().evaluate(i, time_s);
        if (const auto merged = ac3::oba::apply(*slot, base)) {
            cursor.push({.object = i, .placement = *merged});
            ++impl_->stats.updates_applied;
            // Consumed: the cursor now holds this value durably (it keeps
            // reporting it until release()d or overwritten), so there is
            // nothing left to reapply next frame. A slot that stays pending
            // (gain/lfe with no position yet) is deliberately left alone -
            // it is retried, not dropped, on the next call.
            slot.reset();
        }
    }
}

}  // namespace ac3::audio

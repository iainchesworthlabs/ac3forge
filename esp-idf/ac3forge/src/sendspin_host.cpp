// The Sendspin player's WebSocket server and sessions. See
// ../include/ac3forge/sendspin_host.hpp.

#include "ac3forge/sendspin_host.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "ac3/sendspin/ac3forge_player.hpp"
#include "ac3/sendspin/arbiter.hpp"
#include "ac3/sendspin/base64url.hpp"
#include "ac3/sendspin/chunks.hpp"
#include "ac3/sendspin/clock_sync.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/pairing_flow.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/player_session.hpp"
#include "ac3/sendspin/session.hpp"
#include "ac3/sendspin/transport.hpp"

namespace ac3forge {
namespace {

namespace ss = ac3::sendspin;
namespace m = ss::messages;
namespace ac = ss::ac3forge;
namespace pm = ss::pairing_messages;
using ss::Arbiter;

// A board holds no more than this many connections, whatever the
// configuration asks for.
constexpr std::size_t kMaxConnections = 4;

// How often the timer looks at whether a session's timers are due. The
// sessions' own bursts of clock exchanges run at the server's pace inside
// receive(); what a timer adds is timeouts and the ten-second interval between
// bursts, so this bounds how late those can be.
constexpr std::int64_t kTimerPeriodUs = 10'000;

// The WebSocket message a peer may send, beyond the largest Sendspin message
// the player takes: the AEAD tag of the frame that carries it.
constexpr std::size_t kFrameOverhead = 64;

class EspClock final : public ss::Clock {
   public:
    [[nodiscard]] std::int64_t now_us() const override { return esp_timer_get_time(); }
};

void copy_text(std::span<char> out, std::string_view text) {
    if (out.empty()) {
        return;
    }
    const std::size_t n = std::min(text.size(), out.size() - 1);
    std::memcpy(out.data(), text.data(), n);
    out[n] = '\0';
}

[[nodiscard]] std::string_view abort_text(std::optional<pm::AbortReason> reason) {
    if (!reason) {
        return "ended";
    }
    switch (*reason) {
        case pm::AbortReason::kAttemptTimeout:
            return "timed out";
        case pm::AbortReason::kConcurrentAttempt:
            return "refused: another attempt";
        case pm::AbortReason::kMethodNotSupported:
            return "refused: method not supported";
        case pm::AbortReason::kCodeMismatch:
            return "code mismatch";
        case pm::AbortReason::kUserCancelled:
            return "cancelled";
        case pm::AbortReason::kPinLengthUnacceptable:
            return "refused: code length";
    }
    return "ended";
}

[[nodiscard]] const char* psk_text(std::optional<ss::handshake::PskCategory> psk) {
    if (!psk) {
        return "";
    }
    switch (*psk) {
        case ss::handshake::PskCategory::kLongTerm:
            return "long-term";
        case ss::handshake::PskCategory::kPairing:
            return "pairing";
        case ss::handshake::PskCategory::kSentinel:
            return "sentinel";
    }
    return "";
}

[[nodiscard]] bool has(const std::vector<std::string>& roles, std::string_view role) {
    return std::find(roles.begin(), roles.end(), role) != roles.end();
}

}  // namespace

class HostConnection;

struct SendspinHost::Impl {
    SendspinHostConfig config;
    SendspinEvents* events = nullptr;
    SendspinStore store;
    EspClock clock;
    ss::pairing_flow::ClientPairingState pairing;
    std::optional<Arbiter> arbiter;
    httpd_handle_t server = nullptr;
    esp_timer_handle_t timer = nullptr;
    std::atomic<bool> tick_queued{false};
    // When the earliest session timer is due, in esp_timer microseconds.
    std::atomic<std::int64_t> next_due{INT64_MAX};

    // Touched on the server's task only: the open connections, which their
    // sockets' session contexts own.
    std::array<HostConnection*, kMaxConnections> connections{};
    Arbiter::Id next_id = 1;
    // What the sessions report while nothing has changed it, kept up to date
    // so that a session that opens later reports the same.
    m::PlayerState player_state;
    ac::State ac3forge_state;
    bool external = false;
    std::uint32_t config_generation = 0;

    // The owner's requests, from any task, applied on the server's task.
    // A later request replaces an earlier one not yet applied.
    std::mutex pending_mutex;
    ss::PlayerConfig player;  // new sessions' configuration, under the mutex
    std::uint32_t player_generation = 0;
    std::optional<m::PlayerState> pending_player_state;
    std::optional<ac::State> pending_ac3forge_state;
    std::optional<bool> pending_external;
    bool pending_config = false;
    bool pending_reset_rounds = false;
    bool pending_cancel = false;
    bool pending_queued = false;

    // What status() and server_time() read.
    mutable std::mutex status_mutex;
    SendspinStatus status;
    struct ClockMap {
        bool valid = false;
        std::int64_t local = 0;
        std::int64_t server = 0;
        // Server microseconds per million local ones.
        std::int64_t per_second = 1'000'000;
    };
    ClockMap clock_map;
    std::size_t clock_updates = 0;
    Arbiter::Id clock_connection = 0;
    std::array<char, 48> client_id{};
    TaskHandle_t server_task = nullptr;
    // A frame on its way out, header and payload together; on the server's
    // task only.
    std::vector<std::uint8_t> frame_out;

    [[nodiscard]] ss::PlayerConfig new_session_config(std::uint32_t& generation) {
        const std::lock_guard lock(pending_mutex);
        generation = player_generation;
        ss::PlayerConfig copy = player;
        copy.identity = store.identity();
        copy.player_state = player_state;
        copy.ac3forge_state = ac3forge_state;
        return copy;
    }

    [[nodiscard]] HostConnection* find(Arbiter::Id id) const;
    [[nodiscard]] std::size_t open_connections() const;
    [[nodiscard]] bool send_frame(int fd, const ss::transport::Frame& frame);
    void deliver(HostConnection& connection, ss::SessionOutput out);
    void after_call();
    void refresh_status();
    void displace(Arbiter::Id id);
    void queue_pending();

    static esp_err_t on_open(httpd_handle_t server, int fd);
    static esp_err_t on_upgraded(httpd_req_t* req);
    static esp_err_t on_frame(httpd_req_t* req);
    static void free_connection(void* ctx);
    static void on_timer(void* arg);
    static void tick_work(void* arg);
    static void pending_work(void* arg);
};

// One server's connection: its session, and what it is playing.
class HostConnection final : public ss::PlayerListener {
   public:
    HostConnection(SendspinHost::Impl& host, int fd, Arbiter::Id id, std::uint32_t generation, ss::PlayerConfig config)
        : host_(&host), fd_(fd), id_(id), generation_(generation) {
        session_.emplace(std::move(config), host.store, host.pairing, *this, host.clock);
    }

    ~HostConnection() override {
        end_streams();
        // The session's destructor tells the shared pairing state its
        // connection has gone, which closes a pairing window bound to it.
        session_.reset();
        host_->arbiter->ended(id_);
    }

    HostConnection(const HostConnection&) = delete;
    HostConnection& operator=(const HostConnection&) = delete;

    [[nodiscard]] SendspinHost::Impl& host() { return *host_; }
    [[nodiscard]] int fd() const { return fd_; }
    [[nodiscard]] Arbiter::Id id() const { return id_; }
    [[nodiscard]] std::uint32_t generation() const { return generation_; }
    [[nodiscard]] ss::PlayerSession& session() { return *session_; }
    [[nodiscard]] const ss::PlayerSession& session() const { return *session_; }
    [[nodiscard]] ss::transport::Frame& incoming() { return incoming_; }
    [[nodiscard]] bool assembling() const { return assembling_; }
    void set_assembling(bool assembling) { assembling_ = assembling; }
    [[nodiscard]] bool closing() const { return closing_; }
    void set_closing() { closing_ = true; }

    // Whatever this connection was playing stops: it is going, or another
    // server has taken the board.
    void end_streams() {
        if (pcm_stream_) {
            pcm_stream_ = false;
            host_->events->on_stream_end();
        }
        if (burst_stream_) {
            burst_stream_ = false;
            host_->events->on_burst_stream_end();
        }
    }

    // --- PlayerListener, on the server's task -----------------------------------

    bool on_activation(const ss::crypto::Key32& server_key, const m::Activate& activate, bool first) override {
        const Arbiter::Rank rank = Arbiter::rank_of(activate.activities);
        const Arbiter::Verdict verdict = host_->arbiter->activation(id_, server_key, rank, first);
        if (verdict.displaced && *verdict.displaced != id_) {
            host_->displace(*verdict.displaced);
        }
        if (const std::optional<ss::crypto::Key32> last = host_->arbiter->last_playback()) {
            host_->store.set_last_playback(*last);
        }
        const std::string_view activity = rank == Arbiter::Rank::kPlayback  ? "playback"
                                          : rank == Arbiter::Rank::kPairing ? "pairing"
                                                                            : "nothing";
        std::printf("sendspin: [%u] %s for %.*s\n", static_cast<unsigned>(id_),
                    verdict.admit ? "activated" : "refused, another server holds the board:",
                    static_cast<int>(activity.size()), activity.data());
        return verdict.admit;
    }

    void on_pairing_attempt(bool in_progress) override { host_->arbiter->attempt(id_, in_progress); }

    void on_stream_start(const m::PlayerStream& stream) override {
        pcm_stream_ = true;
        host_->events->on_stream_start(stream);
    }
    void on_stream_clear() override { host_->events->on_stream_clear(); }
    void on_stream_end() override {
        pcm_stream_ = false;
        host_->events->on_stream_end();
    }
    void on_audio(std::span<const std::uint8_t> frame, std::int64_t local_time) override {
        const ss::ClockSync& clock = session_->clock();
        host_->events->on_audio(frame, clock.to_server(local_time), local_time);
    }
    void on_command(const m::PlayerCommandMessage& command) override { host_->events->on_player_command(command); }
    void on_group(const m::GroupUpdate& /*update*/) override {}

    void on_unpaired(const ss::crypto::Key32& server_key) override {
        host_->store.remove_record(server_key);
        std::printf("sendspin: [%u] the server unpaired this board\n", static_cast<unsigned>(id_));
    }

    void on_pairing_code(const ss::pairing_flow::Code& code) override {
        const auto* digits = std::get_if<std::string>(&code);
        if (digits == nullptr) {
            return;  // the QR form, which this board does not offer
        }
        {
            const std::lock_guard lock(host_->status_mutex);
            copy_text(host_->status.pairing_code, *digits);
            host_->status.pairing_held_back = false;
            host_->status.pairing_rounds = host_->pairing.rounds_since_verified;
        }
        // Grouped as a person reads it, 3-3 or 4-4.
        const std::size_t half = digits->size() / 2;
        std::printf("sendspin: PAIRING CODE %.*s-%.*s\n", static_cast<int>(half), digits->data(),
                    static_cast<int>(digits->size() - half), digits->data() + half);
        host_->events->on_pairing_code(*digits);
    }

    void on_pairing_held_back() override {
        {
            const std::lock_guard lock(host_->status_mutex);
            host_->status.pairing_held_back = true;
            host_->status.pairing_code[0] = '\0';
        }
        std::printf("sendspin: pairing waits for the operator: twenty codes since the last one that "
                    "matched; reset the round limit on the page or with 'pair reset'\n");
        host_->events->on_pairing_held_back();
    }

    // A pairing that completes is reported here and nowhere else: the
    // attempt is finished, and the re-handshake that follows ends nothing.
    void on_paired(const ss::crypto::Key32& server_key, const ss::crypto::Key32& long_term_psk) override {
        const bool stored = host_->store.add_record(server_key, long_term_psk);
        std::printf("sendspin: [%u] paired with server %s%s\n", static_cast<unsigned>(id_),
                    ss::base64url::encode(server_key).substr(0, 8).c_str(),
                    stored ? "" : " (the record could not be written to NVS)");
        {
            const std::lock_guard lock(host_->status_mutex);
            host_->status.pairing_code[0] = '\0';
            host_->status.pairing_held_back = false;
            copy_text(host_->status.pairing_outcome, "paired");
            host_->status.pairing_rounds = host_->pairing.rounds_since_verified;
        }
        host_->events->on_pairing_ended("paired");
    }

    void on_pairing_ended(std::optional<pm::AbortReason> reason) override {
        const std::string_view outcome = abort_text(reason);
        {
            const std::lock_guard lock(host_->status_mutex);
            host_->status.pairing_code[0] = '\0';
            copy_text(host_->status.pairing_outcome, outcome);
            host_->status.pairing_rounds = host_->pairing.rounds_since_verified;
        }
        std::printf("sendspin: [%u] pairing %.*s\n", static_cast<unsigned>(id_), static_cast<int>(outcome.size()),
                    outcome.data());
        host_->events->on_pairing_ended(outcome);
    }

    void on_burst_stream_start(const ac::StreamStart& stream) override {
        burst_stream_ = true;
        host_->events->on_burst_stream_start(stream);
    }
    void on_burst_stream_clear() override { host_->events->on_burst_stream_clear(); }
    void on_burst_stream_end() override {
        burst_stream_ = false;
        host_->events->on_burst_stream_end();
    }
    void on_burst(const ss::BurstChunk& chunk, std::int64_t local_time) override {
        host_->events->on_burst(chunk, local_time);
    }
    void on_invalid_burst() override { host_->events->on_invalid_burst(); }
    void on_ac3forge_command(const ac::CommandMessage& command) override {
        host_->events->on_ac3forge_command(command);
    }
    void on_settings_refused(const ac::SettingsError& error) override { host_->events->on_settings_refused(error); }

   private:
    SendspinHost::Impl* host_;
    int fd_;
    Arbiter::Id id_;
    std::uint32_t generation_;
    ss::transport::Frame incoming_;
    bool assembling_ = false;
    bool closing_ = false;
    bool pcm_stream_ = false;
    bool burst_stream_ = false;
    std::optional<ss::PlayerSession> session_;
};

HostConnection* SendspinHost::Impl::find(Arbiter::Id id) const {
    for (HostConnection* connection : connections) {
        if (connection != nullptr && connection->id() == id) {
            return connection;
        }
    }
    return nullptr;
}

std::size_t SendspinHost::Impl::open_connections() const {
    return static_cast<std::size_t>(
        std::count_if(connections.begin(), connections.end(), [](const HostConnection* c) { return c != nullptr; }));
}

// One WebSocket frame in one write, where httpd_ws_send_frame_async() writes
// the header and the payload apart. Two writes can reach the server as two
// TCP segments with a gap between them, and a server that reads with a short
// timeout may give up on the frame in that gap. A write cut short by the send
// timeout is finished here too, since part of a frame on the wire leaves the
// connection unusable.
bool SendspinHost::Impl::send_frame(int fd, const ss::transport::Frame& frame) {
    const std::uint64_t size = frame.bytes.size();
    frame_out.clear();
    // Final, unmasked (RFC 6455, section 5.2).
    frame_out.push_back(static_cast<std::uint8_t>(frame.kind == ss::transport::FrameKind::kText ? 0x81 : 0x82));
    if (size < 126) {
        frame_out.push_back(static_cast<std::uint8_t>(size));
    } else if (size <= 0xffff) {
        frame_out.push_back(126);
        frame_out.push_back(static_cast<std::uint8_t>(size >> 8));
        frame_out.push_back(static_cast<std::uint8_t>(size));
    } else {
        frame_out.push_back(127);
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame_out.push_back(static_cast<std::uint8_t>(size >> static_cast<unsigned>(shift)));
        }
    }
    frame_out.insert(frame_out.end(), frame.bytes.begin(), frame.bytes.end());
    std::size_t sent = 0;
    while (sent < frame_out.size()) {
        const auto* rest = static_cast<const char*>(static_cast<const void*>(frame_out.data() + sent));
        const int n = httpd_socket_send(server, fd, rest, frame_out.size() - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

void SendspinHost::Impl::deliver(HostConnection& connection, ss::SessionOutput out) {
    if (connection.closing()) {
        return;
    }
    for (const ss::transport::Frame& frame : out.frames) {
        if (!send_frame(connection.fd(), frame)) {
            out.close = true;
            break;
        }
    }
    if (out.close) {
        connection.set_closing();
        connection.end_streams();
        (void)httpd_sess_trigger_close(server, connection.fd());
    }
}

void SendspinHost::Impl::displace(Arbiter::Id id) {
    HostConnection* displaced = find(id);
    if (displaced == nullptr || displaced->closing()) {
        return;
    }
    std::printf("sendspin: [%u] displaced by another server\n", static_cast<unsigned>(id));
    displaced->end_streams();
    deliver(*displaced, displaced->session().displace());
}

void SendspinHost::Impl::refresh_status() {
    const std::optional<Arbiter::Id> admitted = arbiter->admitted();
    const HostConnection* const held = admitted ? find(*admitted) : nullptr;

    // The playback connection's clock, as an affine map a play-time report
    // can use from another task. Recomputed only when the filter has moved:
    // the filter's arithmetic is double, which this part does in software.
    ClockMap map = clock_map;
    if (held == nullptr || held->session().phase() != ss::PlayerSession::Phase::kActive) {
        map.valid = false;
        clock_updates = 0;
    } else {
        const ss::ClockSync& clock = held->session().clock();
        if (clock.updates() > 0 && (clock.updates() != clock_updates || held->id() != clock_connection)) {
            clock_updates = clock.updates();
            clock_connection = held->id();
            const std::int64_t now = esp_timer_get_time();
            map = ClockMap{.valid = true,
                           .local = now,
                           .server = clock.to_server(now),
                           .per_second = clock.to_server(now + 1'000'000) - clock.to_server(now)};
        }
    }

    const std::lock_guard lock(status_mutex);
    clock_map = map;
    SendspinStatus& s = status;
    s.connections = static_cast<std::uint8_t>(open_connections());
    s.paired_servers = static_cast<std::uint8_t>(store.records());
    s.pairing_rounds = pairing.rounds_since_verified;
    s.connected = held != nullptr;
    if (held == nullptr) {
        s.server_name[0] = '\0';
        s.server_id[0] = '\0';
        s.dialect = "";
        s.psk = "";
        s.activity = "";
        s.role = "";
        s.clock_converged = false;
        s.clock_error_us = 0;
        s.server_has_lost_pairing = false;
    } else {
        const ss::PlayerSession& session = held->session();
        copy_text(s.server_name, session.server_name());
        const std::string id = ss::base64url::encode(session.server_key());
        copy_text(s.server_id, std::string_view(id).substr(0, 8));
        s.dialect = session.dialect() == ss::Dialect::kAiosendspin911 ? "aiosendspin 9.1.1" : "specification";
        s.psk = psk_text(session.psk_category());
        const Arbiter::Rank rank = Arbiter::rank_of(session.activities());
        s.activity = rank == Arbiter::Rank::kPlayback ? "playback" : rank == Arbiter::Rank::kPairing ? "pairing" : "none";
        s.role = has(session.active_roles(), ac::kRole) ? "_ac3forge_player@v1"
                 : has(session.active_roles(), "player@v1") ? "player@v1"
                                                             : "";
        s.clock_converged = session.clock_converged();
        s.clock_error_us = session.clock().updates() > 0 ? session.clock().error_us() : 0;
        s.server_has_lost_pairing = session.fell_back();
    }
    if (server_task != nullptr) {
        s.stack_free = static_cast<std::size_t>(uxTaskGetStackHighWaterMark(server_task));
    }
}

void SendspinHost::Impl::after_call() {
    std::int64_t due = INT64_MAX;
    const std::int64_t now = esp_timer_get_time();
    for (HostConnection* connection : connections) {
        if (connection != nullptr && !connection->closing()) {
            due = std::min(due, now + connection->session().next_tick_us());
        }
    }
    next_due.store(due);
    refresh_status();
}

// esp_http_server's callback for each accepted socket, before anything is
// read from it: Nagle's algorithm off, as a computer's transport has it. With
// it on, a small message waits for the server to acknowledge the one before,
// which a server may delay by up to 200 ms: a clock exchange measures that wait
// as network delay, and a frame's second segment arrives that much later.
esp_err_t SendspinHost::Impl::on_open(httpd_handle_t /*server*/, int fd) {
    const int on = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) != 0) {
        std::printf("sendspin: could not turn off Nagle's algorithm on a connection\n");
    }
    return ESP_OK;
}

esp_err_t SendspinHost::Impl::on_upgraded(httpd_req_t* req) {
    auto* host = static_cast<Impl*>(req->user_ctx);
    if (host->server_task == nullptr) {
        host->server_task = xTaskGetCurrentTaskHandle();
    }
    const int fd = httpd_req_to_sockfd(req);
    const auto slot = std::find(host->connections.begin(), host->connections.end(), nullptr);
    if (host->open_connections() >= host->config.max_connections || slot == host->connections.end()) {
        std::printf("sendspin: refusing a connection: %u are open\n",
                    static_cast<unsigned>(host->open_connections()));
        return ESP_FAIL;
    }
    std::uint32_t generation = 0;
    ss::PlayerConfig player = host->new_session_config(generation);
    auto* connection = new (std::nothrow) HostConnection(*host, fd, host->next_id++, generation, std::move(player));
    if (connection == nullptr) {
        std::printf("sendspin: no memory for a connection\n");
        return ESP_FAIL;
    }
    *slot = connection;
    req->sess_ctx = connection;
    req->free_ctx = &Impl::free_connection;
    std::printf("sendspin: [%u] connected\n", static_cast<unsigned>(connection->id()));
    if (host->external) {
        (void)connection->session().set_external_source(true);
    }
    host->deliver(*connection, connection->session().open());
    host->after_call();
    return ESP_OK;
}

esp_err_t SendspinHost::Impl::on_frame(httpd_req_t* req) {
    auto* host = static_cast<Impl*>(req->user_ctx);
    auto* connection = static_cast<HostConnection*>(req->sess_ctx);
    httpd_ws_frame_t frame{};
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    if (frame.type == HTTPD_WS_TYPE_PONG) {
        // Consumed so the stream stays aligned; the server's pings are
        // answered by esp_http_server itself.
        std::array<std::uint8_t, 125> discard{};
        frame.payload = discard.data();
        return frame.len <= discard.size() && httpd_ws_recv_frame(req, &frame, frame.len) == ESP_OK ? ESP_OK : ESP_FAIL;
    }
    if (connection == nullptr || connection->closing()) {
        return ESP_FAIL;
    }
    const bool continuation = frame.type == HTTPD_WS_TYPE_CONTINUE;
    if (!continuation && frame.type != HTTPD_WS_TYPE_TEXT && frame.type != HTTPD_WS_TYPE_BINARY) {
        return ESP_FAIL;
    }
    if (continuation != connection->assembling()) {
        return ESP_FAIL;  // a continuation with nothing to continue, or a new message inside one
    }
    ss::transport::Frame& incoming = connection->incoming();
    if (!continuation) {
        incoming.kind =
            frame.type == HTTPD_WS_TYPE_TEXT ? ss::transport::FrameKind::kText : ss::transport::FrameKind::kBinary;
        incoming.bytes.clear();
    }
    const std::size_t have = incoming.bytes.size();
    const std::size_t limit = host->config.player.max_message_bytes + kFrameOverhead;
    if (frame.len > limit - have) {
        std::printf("sendspin: [%u] a %u-byte message is more than this board takes\n",
                    static_cast<unsigned>(connection->id()), static_cast<unsigned>(have + frame.len));
        return ESP_FAIL;
    }
    incoming.bytes.resize(have + frame.len);
    frame.payload = incoming.bytes.data() + have;
    if (frame.len > 0 && httpd_ws_recv_frame(req, &frame, frame.len) != ESP_OK) {
        return ESP_FAIL;
    }
    if (!frame.final) {
        connection->set_assembling(true);
        return ESP_OK;
    }
    connection->set_assembling(false);
    host->deliver(*connection, connection->session().receive(incoming));
    host->after_call();
    return ESP_OK;
}

// esp_http_server's session-context destructor: the socket has closed, on the
// server's task, whether the peer closed it, a session asked to, or the server
// is stopping.
void SendspinHost::Impl::free_connection(void* ctx) {
    auto* connection = static_cast<HostConnection*>(ctx);
    if (connection == nullptr) {
        return;
    }
    Impl& host = connection->host();
    const Arbiter::Id id = connection->id();
    for (HostConnection*& entry : host.connections) {
        if (entry == connection) {
            entry = nullptr;
        }
    }
    delete connection;
    std::printf("sendspin: [%u] closed\n", static_cast<unsigned>(id));
    host.after_call();
}

// The esp_timer task, every kTimerPeriodUs: the sessions' timers run on the
// server's task, so this only queues them there when one is due.
void SendspinHost::Impl::on_timer(void* arg) {
    auto* host = static_cast<Impl*>(arg);
    if (esp_timer_get_time() < host->next_due.load() || host->tick_queued.exchange(true)) {
        return;
    }
    if (httpd_queue_work(host->server, &Impl::tick_work, host) != ESP_OK) {
        host->tick_queued.store(false);
    }
}

void SendspinHost::Impl::tick_work(void* arg) {
    auto* host = static_cast<Impl*>(arg);
    host->tick_queued.store(false);
    for (HostConnection* connection : host->connections) {
        if (connection != nullptr && !connection->closing()) {
            host->deliver(*connection, connection->session().tick());
        }
    }
    host->after_call();
}

void SendspinHost::Impl::queue_pending() {
    bool queue = false;
    {
        const std::lock_guard lock(pending_mutex);
        queue = !pending_queued;
        pending_queued = true;
    }
    if (queue && (server == nullptr || httpd_queue_work(server, &Impl::pending_work, this) != ESP_OK)) {
        const std::lock_guard lock(pending_mutex);
        pending_queued = false;
    }
}

// The owner's requests, on the server's task.
void SendspinHost::Impl::pending_work(void* arg) {
    auto* host = static_cast<Impl*>(arg);
    std::optional<m::PlayerState> player_state;
    std::optional<ac::State> ac3forge_state;
    std::optional<bool> external;
    bool config = false;
    bool reset_rounds = false;
    bool cancel = false;
    std::uint32_t generation = 0;
    {
        const std::lock_guard lock(host->pending_mutex);
        player_state.swap(host->pending_player_state);
        ac3forge_state.swap(host->pending_ac3forge_state);
        external.swap(host->pending_external);
        config = std::exchange(host->pending_config, false);
        reset_rounds = std::exchange(host->pending_reset_rounds, false);
        cancel = std::exchange(host->pending_cancel, false);
        generation = host->player_generation;
        host->pending_queued = false;
    }
    if (player_state) {
        host->player_state = *player_state;
    }
    if (ac3forge_state) {
        host->ac3forge_state = std::move(*ac3forge_state);
    }
    if (external) {
        host->external = *external;
    }
    if (reset_rounds) {
        host->pairing.reset_rounds();
        std::printf("sendspin: pairing round limit reset\n");
    }
    for (HostConnection* connection : host->connections) {
        if (connection == nullptr || connection->closing()) {
            continue;
        }
        ss::PlayerSession& session = connection->session();
        if (config && connection->generation() != generation &&
            session.phase() != ss::PlayerSession::Phase::kHandshake) {
            // It said hello with what this board no longer is; its server
            // dials again and hears the new hello (C19).
            host->deliver(*connection, session.goodbye(m::GoodbyeReason::kRestart));
            continue;
        }
        if (player_state) {
            host->deliver(*connection, session.set_state(host->player_state));
        }
        if (ac3forge_state) {
            host->deliver(*connection, session.set_ac3forge_state(host->ac3forge_state));
        }
        if (external) {
            host->deliver(*connection, session.set_external_source(host->external));
        }
        if (reset_rounds) {
            host->deliver(*connection, session.resume_pairing());
        }
        if (cancel) {
            host->deliver(*connection, session.cancel_pairing());
        }
    }
    host->after_call();
}

// --- SendspinHost ---------------------------------------------------------------

SendspinHost::SendspinHost() : impl_(std::make_unique<Impl>()) {}

SendspinHost::~SendspinHost() { stop(); }

bool SendspinHost::start(SendspinHostConfig config, SendspinEvents& events) {
    Impl& im = *impl_;
    if (im.server != nullptr) {
        return true;
    }
    if (!im.store.load()) {
        return false;
    }
    config.max_connections = std::clamp<std::size_t>(config.max_connections, 1, kMaxConnections);
    im.events = &events;
    im.arbiter.emplace(im.store.last_playback());
    {
        const std::lock_guard lock(im.pending_mutex);
        im.player = config.player;
        im.player_state = config.player.player_state;
        im.ac3forge_state = config.player.ac3forge_state;
    }
    im.config = std::move(config);
    copy_text(im.client_id, ss::base64url::encode(im.store.identity().public_key()));

    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.server_port = im.config.port;
    http.ctrl_port = im.config.control_port;
    http.stack_size = im.config.stack_bytes;
    http.task_priority = im.config.priority;
    http.core_id = im.config.core;
    http.max_uri_handlers = 1;
    http.max_open_sockets = static_cast<std::uint16_t>(im.config.max_connections);
    // A new connection is never let in by closing the playback one: admission
    // between servers is the arbiter's (connection.md, Multiple servers).
    http.lru_purge_enable = false;
    // A server that has gone without a word is found by TCP within about
    // twenty seconds, and its connection closed.
    http.keep_alive_enable = true;
    http.keep_alive_idle = 5;
    http.keep_alive_interval = 5;
    http.keep_alive_count = 3;
    http.open_fn = &Impl::on_open;
    if (httpd_start(&im.server, &http) != ESP_OK) {
        std::printf("sendspin: could not start the WebSocket server on port %u\n",
                    static_cast<unsigned>(im.config.port));
        im.server = nullptr;
        return false;
    }
    httpd_uri_t route{};
    route.uri = "/sendspin";
    route.method = HTTP_GET;
    route.handler = &Impl::on_frame;
    route.user_ctx = &im;
    route.is_websocket = true;
    route.handle_ws_control_frames = false;
    route.ws_post_handshake_cb = &Impl::on_upgraded;
    if (httpd_register_uri_handler(im.server, &route) != ESP_OK) {
        std::printf("sendspin: could not register /sendspin\n");
        stop();
        return false;
    }

    const esp_timer_create_args_t timer_args = {.callback = &Impl::on_timer,
                                                .arg = &im,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "sendspin",
                                                .skip_unhandled_events = true};
    if (esp_timer_create(&timer_args, &im.timer) != ESP_OK ||
        esp_timer_start_periodic(im.timer, kTimerPeriodUs) != ESP_OK) {
        std::printf("sendspin: could not start the session timer\n");
        stop();
        return false;
    }
    {
        const std::lock_guard lock(im.status_mutex);
        im.status.running = true;
        im.status.port = im.config.port;
        im.status.client_id = im.client_id;
        im.status.paired_servers = static_cast<std::uint8_t>(im.store.records());
    }
    std::printf("sendspin: player on port %u, path /sendspin, client_id %s, %u pairing record(s)\n",
                static_cast<unsigned>(im.config.port), im.client_id.data(),
                static_cast<unsigned>(im.store.records()));
    return true;
}

void SendspinHost::stop() {
    Impl& im = *impl_;
    if (im.timer != nullptr) {
        (void)esp_timer_stop(im.timer);
        (void)esp_timer_delete(im.timer);
        im.timer = nullptr;
    }
    if (im.server != nullptr) {
        // Every session's context is freed on the server's task as it stops.
        (void)httpd_stop(im.server);
        im.server = nullptr;
    }
    // Its task is gone with it; a restarted server has a new one.
    im.server_task = nullptr;
    const std::lock_guard lock(im.status_mutex);
    im.status.running = false;
    im.status.connections = 0;
    im.status.connected = false;
    im.clock_map.valid = false;
}

void SendspinHost::set_player_state(const m::PlayerState& state) {
    {
        const std::lock_guard lock(impl_->pending_mutex);
        impl_->pending_player_state = state;
    }
    impl_->queue_pending();
}

void SendspinHost::set_ac3forge_state(const ac::State& state) {
    {
        const std::lock_guard lock(impl_->pending_mutex);
        impl_->pending_ac3forge_state = state;
    }
    impl_->queue_pending();
}

void SendspinHost::set_external_source(bool external) {
    {
        const std::lock_guard lock(impl_->pending_mutex);
        impl_->pending_external = external;
    }
    impl_->queue_pending();
}

void SendspinHost::set_player_config(const ss::PlayerConfig& player) {
    {
        const std::lock_guard lock(impl_->pending_mutex);
        impl_->player = player;
        ++impl_->player_generation;
        impl_->pending_config = true;
    }
    impl_->queue_pending();
}

void SendspinHost::reset_pairing_rounds() {
    {
        const std::lock_guard lock(impl_->pending_mutex);
        impl_->pending_reset_rounds = true;
    }
    impl_->queue_pending();
}

void SendspinHost::cancel_pairing() {
    {
        const std::lock_guard lock(impl_->pending_mutex);
        impl_->pending_cancel = true;
    }
    impl_->queue_pending();
}

bool SendspinHost::forget_pairings() {
    Impl& im = *impl_;
    const bool running = im.server != nullptr;
    SendspinHostConfig config = im.config;
    SendspinEvents* events = im.events;
    // The server goes first, so no session is using a key while it changes.
    stop();
    const bool forgotten = im.store.forget();
    {
        const std::lock_guard lock(im.pending_mutex);
        config.player = im.player;
    }
    if (running && events != nullptr && !start(std::move(config), *events)) {
        return false;
    }
    return forgotten;
}

SendspinStatus SendspinHost::status() const {
    const std::lock_guard lock(impl_->status_mutex);
    return impl_->status;
}

const SendspinStore& SendspinHost::store() const { return impl_->store; }

std::optional<std::int64_t> SendspinHost::server_time(std::int64_t local_us) const {
    const std::lock_guard lock(impl_->status_mutex);
    const Impl::ClockMap& map = impl_->clock_map;
    if (!map.valid) {
        return std::nullopt;
    }
    return map.server + (((local_us - map.local) * map.per_second) / 1'000'000);
}

}  // namespace ac3forge

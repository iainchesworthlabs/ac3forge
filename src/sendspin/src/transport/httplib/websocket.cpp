#include "ac3/sendspin/websocket.hpp"

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ac3/sendspin/firewall.hpp"
#include "ac3/sendspin/transport.hpp"
#include "listen_socket.hpp"

// The WebSocket transport over cpp-httplib 0.56. The macros that configure cpp-httplib come
// from src/sendspin/CMakeLists.txt.

namespace ac3::sendspin::transport::websocket {

namespace {

namespace ws = httplib::ws;

// Workers beyond max_connections, so a refusal or a request for another path is still
// answered while every connection holds one, and the accepted sockets that may queue for one.
constexpr std::size_t kSpareWorkers = 2;
constexpr std::size_t kQueuedSockets = 8;

// How often the accept loop wakes. cpp-httplib's default on Windows is every millisecond,
// which a desktop player that listens all day would pay for.
constexpr std::chrono::milliseconds kAcceptPoll{100};

// 1013, try again later, is in the IANA registry RFC 6455 set up but not in cpp-httplib's enum.
constexpr auto kTryAgainLater = static_cast<ws::CloseStatus>(1013);

const char* char_data(std::span<const std::uint8_t> bytes) {
    return static_cast<const char*>(static_cast<const void*>(bytes.data()));
}

// The next message from a ws::WebSocket or a ws::WebSocketClient, reading again after each
// poll-interval timeout, or nothing once the connection has failed or `ended` is set.
template <class Socket>
std::optional<Frame> next_frame(Socket& socket, const std::atomic<bool>& ended) {
    std::string message;
    while (!ended.load()) {
        const ws::ReadResult result = socket.read(message);
        if (result == ws::Timeout) {
            continue;
        }
        if (result != ws::Text && result != ws::Binary) {
            return std::nullopt;
        }
        Frame frame{.kind = result == ws::Text ? FrameKind::kText : FrameKind::kBinary, .bytes = {}};
        frame.bytes.assign(message.begin(), message.end());
        return frame;
    }
    return std::nullopt;
}

bool valid_path(std::string_view path) {
    // No regex metacharacters, so cpp-httplib matches the path literally.
    const auto allowed = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_' || c == '~' || c == '/';
    };
    return path.starts_with('/') && std::ranges::all_of(path, allowed);
}

std::string peer_address(const httplib::Request& request) {
    const std::string port = std::to_string(request.remote_port);
    if (request.remote_addr.find(':') != std::string::npos) {
        return "[" + request.remote_addr + "]:" + port;
    }
    return request.remote_addr + ":" + port;
}

// What an accepted connection's worker and the Connection handed out share. cpp-httplib keeps
// the ws::WebSocket on the worker's stack only while the upgrade handler runs, so the worker
// waits in serve_until_ended() for the connection to end, closes the WebSocket, and detaches
// it. Every call through the Connection after that finds nothing to call.
class Link {
   public:
    explicit Link(ws::WebSocket& socket) : socket_(&socket) {}

    // Sends fail from here on and a receive() in progress returns at its next poll. The
    // worker sends the Close frame, so ending never waits on the network.
    void end(ws::CloseStatus status) {
        {
            const std::lock_guard lock(mutex_);
            if (ended_.load()) {
                return;
            }
            status_ = status;
            ended_.store(true);
        }
        changed_.notify_all();
    }

    void serve_until_ended() {
        ws::CloseStatus status = ws::CloseStatus::Normal;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] { return ended_.load(); });
            status = status_;
        }
        {
            // Beside a reader still in read(), close() sends the Close frame and returns;
            // with none, it waits for the peer's answer, up to the close timeout.
            const std::shared_lock use(socket_mutex_);
            socket_->close(status);
        }
        const std::lock_guard detach(socket_mutex_);
        socket_ = nullptr;
    }

    std::optional<Frame> receive() {
        std::optional<Frame> frame;
        {
            const std::shared_lock use(socket_mutex_);
            if (socket_ != nullptr) {
                frame = next_frame(*socket_, ended_);
            }
        }
        if (!frame) {
            end(ws::CloseStatus::Normal);
        }
        return frame;
    }

    bool send_text(std::string_view text) {
        return send([text](ws::WebSocket& socket) { return socket.send(std::string(text)); });
    }

    bool send_binary(std::span<const std::uint8_t> bytes) {
        return send([bytes](ws::WebSocket& socket) { return socket.send(char_data(bytes), bytes.size()); });
    }

   private:
    template <class Write>
    bool send(const Write& write) {
        bool sent = false;
        {
            const std::shared_lock use(socket_mutex_);
            if (socket_ == nullptr || ended_.load()) {
                return false;
            }
            sent = write(*socket_);
        }
        // A send that failed may have left part of a message on the wire.
        if (!sent) {
            end(ws::CloseStatus::Normal);
        }
        return sent;
    }

    // Shared by every call into socket_; exclusive only to detach it.
    std::shared_mutex socket_mutex_;
    ws::WebSocket* socket_;

    std::mutex mutex_;
    std::condition_variable changed_;
    std::atomic<bool> ended_{false};
    ws::CloseStatus status_ = ws::CloseStatus::Normal;
};

class AcceptedConnection final : public Connection {
   public:
    AcceptedConnection(std::shared_ptr<Link> link, std::string peer)
        : link_(std::move(link)), peer_(std::move(peer)) {}

    ~AcceptedConnection() override { close(); }
    AcceptedConnection(const AcceptedConnection&) = delete;
    AcceptedConnection& operator=(const AcceptedConnection&) = delete;
    AcceptedConnection(AcceptedConnection&&) = delete;
    AcceptedConnection& operator=(AcceptedConnection&&) = delete;

    std::optional<Frame> receive() override { return link_->receive(); }
    bool send_text(std::string_view text) override { return link_->send_text(text); }
    bool send_binary(std::span<const std::uint8_t> bytes) override { return link_->send_binary(bytes); }
    void close() override { link_->end(ws::CloseStatus::Normal); }
    std::string peer() const override { return peer_; }

   private:
    std::shared_ptr<Link> link_;
    std::string peer_;
};

// A dialled connection owns its client, which stays connected until it is destroyed, so calls
// into it need no detaching.
class DialledConnection final : public Connection {
   public:
    DialledConnection(std::unique_ptr<ws::WebSocketClient> client, std::string peer)
        : client_(std::move(client)), peer_(std::move(peer)) {}

    ~DialledConnection() override { close(); }
    DialledConnection(const DialledConnection&) = delete;
    DialledConnection& operator=(const DialledConnection&) = delete;
    DialledConnection(DialledConnection&&) = delete;
    DialledConnection& operator=(DialledConnection&&) = delete;

    std::optional<Frame> receive() override {
        std::optional<Frame> frame = next_frame(*client_, ended_);
        if (!frame) {
            close();
        }
        return frame;
    }

    bool send_text(std::string_view text) override {
        return !ended_.load() && sent(client_->send(std::string(text)));
    }

    bool send_binary(std::span<const std::uint8_t> bytes) override {
        return !ended_.load() && sent(client_->send(char_data(bytes), bytes.size()));
    }

    void close() override {
        if (!ended_.exchange(true)) {
            client_->close();
        }
    }

    std::string peer() const override { return peer_; }

   private:
    // A send that failed may have left part of a message on the wire.
    bool sent(bool ok) {
        if (!ok) {
            close();
        }
        return ok;
    }

    std::unique_ptr<ws::WebSocketClient> client_;
    std::string peer_;
    std::atomic<bool> ended_{false};
};

}  // namespace

struct Listener::State {
    State(ListenerOptions options_in, Handler handler_in)
        : options(std::move(options_in)), handler(std::move(handler_in)) {}

    void serve(const httplib::Request& request, ws::WebSocket& socket) {
        socket.set_read_timeout(options.connection.poll_interval);
        const auto link = std::make_shared<Link>(socket);
        bool admitted = false;
        bool going_away = false;
        {
            const std::lock_guard lock(mutex);
            going_away = stopping;
            admitted = !stopping && links.size() < options.max_connections;
            if (admitted) {
                links.push_back(link);
            }
        }
        if (!admitted) {
            socket.close(going_away ? ws::CloseStatus::GoingAway : kTryAgainLater);
            return;
        }
        handler(std::make_unique<AcceptedConnection>(link, peer_address(request)));
        link->serve_until_ended();
        const std::lock_guard lock(mutex);
        std::erase(links, link);
    }

    const ListenerOptions options;
    const Handler handler;
    httplib::Server server;
    std::thread accept_thread;
    std::uint16_t port = 0;

    std::mutex mutex;
    bool stopping = false;
    std::vector<std::shared_ptr<Link>> links;
};

Listener::Listener(std::unique_ptr<State> state) : state_(std::move(state)) {}

Listener::~Listener() { stop(); }

std::unique_ptr<Listener> Listener::start(ListenerOptions options, Handler handler) {
    if (!valid_path(options.path) || options.max_connections == 0 || !handler) {
        return nullptr;
    }
    auto state = std::make_unique<State>(std::move(options), std::move(handler));
    State& shared = *state;
    httplib::Server& server = shared.server;

    // Every open connection holds a worker, so the pool is sized from the connection limit
    // rather than from cpp-httplib's default of four times the hardware threads.
    const std::size_t workers = shared.options.max_connections + kSpareWorkers;
    server.new_task_queue = [workers] { return new httplib::ThreadPool(1, workers, kQueuedSockets); };
    server.set_socket_options(set_listening_socket_options);
    // Sendspin's clock exchanges are small messages whose round trip is the measurement, so
    // Nagle's algorithm is off on both sides.
    server.set_tcp_nodelay(true);
    server.set_idle_interval(kAcceptPoll);
    server.set_write_timeout(shared.options.connection.write_timeout);
    server.set_websocket_ping_interval(shared.options.connection.ping_interval);
    server.set_websocket_max_missed_pongs(shared.options.connection.max_missed_pongs);
    server.WebSocket(shared.options.path, [&shared](const httplib::Request& request, ws::WebSocket& socket) {
        shared.serve(request, socket);
    });

    // A loopback-only listener needs no exception - Windows never gates loopback traffic - and
    // port 0 takes whichever free port the OS hands back, a different one on every run that a
    // persistent, name-keyed rule could never usefully track; both are skipped for that reason,
    // not asked and left unanswered.
    if (shared.options.address != "127.0.0.1" && shared.options.port != 0) {
        // "Listener", not "Server": this class is shared by both directions (this header's own
        // comment above) - a server's clients-that-dial-in port, and equally a player's own
        // listener for servers that dial it, which is what ac3hearth-testsink binds.
        firewall::ensure_inbound_rule(
            {.name = "Sendspin Listener", .protocol = firewall::Protocol::kTcp, .port = shared.options.port});
    }

    int bound = -1;
    if (shared.options.port == 0) {
        bound = server.bind_to_any_port(shared.options.address);
    } else if (server.bind_to_port(shared.options.address, shared.options.port)) {
        bound = shared.options.port;
    }
    if (bound <= 0) {
        return nullptr;
    }
    shared.port = static_cast<std::uint16_t>(bound);
    // The socket is already listening, so a peer that dials before this thread reaches
    // accept() waits in the backlog rather than being refused.
    shared.accept_thread = std::thread([&server] { server.listen_after_bind(); });
    return std::unique_ptr<Listener>(new Listener(std::move(state)));
}

std::uint16_t Listener::port() const { return state_->port; }

void Listener::stop() {
    {
        // Ending a link takes only the link's own lock, which no worker holds while it waits
        // for this one, so the links can be ended here without copying them out.
        const std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        for (const std::shared_ptr<Link>& link : state_->links) {
            link->end(ws::CloseStatus::GoingAway);
        }
    }
    state_->server.stop();
    if (state_->accept_thread.joinable()) {
        state_->accept_thread.join();
    }
}

std::expected<std::unique_ptr<Connection>, ConnectError> connect(const std::string& url,
                                                                 const ConnectOptions& options) {
    // cpp-httplib's client throws for a scheme it does not support, so that is checked first.
    if (!url.starts_with("ws://")) {
        return std::unexpected(ConnectError::kInvalidUrl);
    }
    auto client = std::make_unique<ws::WebSocketClient>(url);
    if (!client->is_valid()) {
        return std::unexpected(ConnectError::kInvalidUrl);
    }
    const ConnectionOptions& connection = options.connection;
    client->set_connection_timeout(options.connect_timeout);
    // The upgrade's response is read under the read timeout, so that holds the connect timeout
    // until the upgrade is done and the poll interval from then on.
    client->set_read_timeout(options.connect_timeout);
    client->set_write_timeout(connection.write_timeout);
    client->set_websocket_ping_interval(static_cast<std::time_t>(connection.ping_interval.count()));
    client->set_websocket_max_missed_pongs(connection.max_missed_pongs);
    client->set_tcp_nodelay(true);
    const ws::Result result = client->connect();
    if (!result) {
        return std::unexpected(result.status() > 0 ? ConnectError::kRejected : ConnectError::kUnreachable);
    }
    client->set_read_timeout(connection.poll_interval);
    return std::unique_ptr<Connection>(std::make_unique<DialledConnection>(std::move(client), url));
}

}  // namespace ac3::sendspin::transport::websocket

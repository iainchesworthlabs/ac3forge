#include "ac3/sendspin/server_host.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ac3/sendspin/ac3forge_player.hpp"
#include "ac3/sendspin/base64url.hpp"
#include "ac3/sendspin/chunks.hpp"
#include "ac3/sendspin/codec.hpp"
#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/discovery.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/handshake_session.hpp"
#include "ac3/sendspin/mdns.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/pairing.hpp"
#include "ac3/sendspin/pairing_flow.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/server_session.hpp"
#include "ac3/sendspin/server_store.hpp"
#include "ac3/sendspin/session.hpp"
#include "ac3/sendspin/session_driver.hpp"
#include "ac3/sendspin/transport.hpp"
#include "ac3/sendspin/websocket.hpp"

namespace ac3::sendspin {

namespace {

namespace m = messages;
namespace hs = handshake;
namespace pm = pairing_messages;
namespace websocket = transport::websocket;
using crypto::Key32;

constexpr std::string_view kPlayerRole = "player@v1";
// How far past its lead a group reads a buffered source ahead.
constexpr std::int64_t kReadAhead = 1'500'000;
// The lead every group adds for the network, above what its players ask for.
constexpr std::int64_t kNetworkLead = 100'000;
// A unit's longest play time, which a group counts each queued unit as lasting.
constexpr std::int64_t kLongestUnit = 150'000;
// The samples in every _ac3forge_player@v1 burst (planning/hearth-sendspin-extension.md, Burst
// chunks).
constexpr std::int64_t kSamplesPerBurst = 1536;
constexpr std::chrono::seconds kRedialAfter{10};

[[nodiscard]] bool contains(const std::vector<std::string>& roles, std::string_view role) {
    return std::find(roles.begin(), roles.end(), role) != roles.end();
}

// A sample at `from` bits as one at `to` bits.
[[nodiscard]] std::int32_t rescaled(std::int32_t sample, std::int32_t from, std::int32_t to) {
    if (to > from) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(sample) << static_cast<unsigned>(to - from));
    }
    return sample >> static_cast<unsigned>(from - to);
}

[[nodiscard]] bool producible(const m::AudioFormat& wanted, const m::AudioFormat& source) {
    if (wanted.channels != source.channels || wanted.sample_rate != source.sample_rate) {
        return false;
    }
    if (wanted.codec == m::Codec::kOpus) {
        return wanted.sample_rate == 48000 && wanted.channels <= 2;
    }
    return wanted.bit_depth == 16 || wanted.bit_depth == 24 || wanted.bit_depth == 32;
}

}  // namespace

class HostConnection;

struct ServerHost::State {
    ServerHostOptions options;
    ServerStore* store = nullptr;
    ServerHostEvents* events = nullptr;
    SteadyClock clock;

    mutable std::mutex mutex;
    std::map<std::uint64_t, std::shared_ptr<HostConnection>> connections;
    std::uint64_t next_id = 1;
    struct Requested {
        m::PairMethod method = m::PairMethod::kDynamicCode;
        std::optional<m::CodeFormat> format;
    };
    std::map<Key32, Requested> requested;
    // Services browsing found, by URL, and when to dial each again; and each one's URL by instance
    // name, so a service browsing loses is not dialled again.
    std::map<std::string, std::chrono::steady_clock::time_point> redial;
    std::map<std::string, std::string> found_urls;
    std::uint64_t next_group = 1;

    std::mutex posted_mutex;
    std::condition_variable posted_changed;
    std::deque<std::function<void()>> posted;
    bool stopping = false;
    std::thread worker;

    std::unique_ptr<websocket::Listener> listener;
    std::unique_ptr<discovery::Advertiser> advertiser;
    std::unique_ptr<discovery::BrowseListener> browse_listener;
    std::unique_ptr<discovery::Browser> browser;

    void post(std::function<void()> work) {
        {
            const std::lock_guard lock(posted_mutex);
            posted.push_back(std::move(work));
        }
        posted_changed.notify_all();
    }

    void log(std::string_view line) { events->on_log(line); }

    void run();
    void accept(std::unique_ptr<transport::Connection> transport, std::string url);
    void dial(const std::string& url);
    void remove(std::uint64_t id);
    [[nodiscard]] std::shared_ptr<HostConnection> find(const std::string& client_id) const;
    [[nodiscard]] std::vector<std::shared_ptr<HostConnection>> all() const;
};

// One connection to a client, whichever side dialled.
class HostConnection final : public ServerListener, public std::enable_shared_from_this<HostConnection> {
   public:
    HostConnection(ServerHost::State& host, std::uint64_t id, std::string url) : host_(&host), id_(id), url_(std::move(url)) {
        const ServerKeyringAdapter& keys = keyring_;
        session_.emplace(ServerConfig{.identity = host.options.identity,
                                      .name = host.options.name,
                                      .languages = host.options.languages,
                                      .max_message_bytes = 4 * 1024 * 1024},
                         keys, *this, host.clock);
    }

    ~HostConnection() override { driver_.reset(); }
    HostConnection(const HostConnection&) = delete;
    HostConnection& operator=(const HostConnection&) = delete;
    HostConnection(HostConnection&&) = delete;
    HostConnection& operator=(HostConnection&&) = delete;

    void start(std::unique_ptr<transport::Connection> transport) {
        peer_ = transport->peer();
        ServerHost::State* host = host_;
        driver_ = std::make_unique<SessionDriver>(
            std::move(transport),
            DrivenSession{.receive = [this](const transport::Frame& frame) { return session_->receive(frame); },
                          .tick = [this] { return session_->tick(); },
                          .next_tick_us = [this] { return session_->next_tick_us(); },
                          .ended = [host, id = id_] { host->post([host, id] { host->remove(id); }); }});
        driver_->start();
    }

    [[nodiscard]] SessionDriver& driver() { return *driver_; }
    [[nodiscard]] ServerSession& session() { return *session_; }
    [[nodiscard]] const std::string& url() const { return url_; }
    [[nodiscard]] std::uint64_t id() const { return id_; }

    [[nodiscard]] ClientView view() {
        return driver_->inspect([&] {
            ClientView view;
            const ServerSession& session = *session_;
            view.client_key = session.client_key();
            view.client_id = base64url::encode(view.client_key);
            view.peer = peer_;
            view.dialect = session.dialect();
            view.psk = session.psk_category();
            view.credential_mismatch = session.credential_mismatch();
            view.hello = session.hello().has_value();
            if (session.hello()) {
                view.name = session.hello()->name;
                view.offers_unpaired_access = session.hello()->unpaired_access;
                for (const m::PairMethodDescriptor& method : session.hello()->pair_methods) {
                    view.pair_methods.push_back(method.method);
                }
                view.player_support = session.hello()->player_support;
                if (contains(session.hello()->supported_roles, ac3forge::kRole)) {
                    view.ac3forge_support = session.hello()->ac3forge_support;
                }
            }
            view.pairing = session.pairing();
            view.wants_code = session.pairing_wants_code();
            view.bursts = contains(session.active_roles(), ac3forge::kRole);
            view.playing = view.bursts || contains(session.active_roles(), kPlayerRole);
            if (session.state()) {
                view.available = session.state()->available;
                view.player_state = session.state()->player;
                view.ac3forge_state = session.state()->ac3forge;
            }
            return view;
        });
    }

    // Chooses the client's activation from the store and what the operator asked for. On the
    // host's thread.
    void decide() {
        const ClientView client = view();
        if (!client.hello) {
            return;
        }
        // A second connection to a client the host already holds is closed.
        for (const std::shared_ptr<HostConnection>& other : host_->all()) {
            if (other.get() != this && other->id() < id_ && other->view().client_key == client.client_key) {
                host_->log(client.name + ": a second connection, closed");
                driver_->close();
                return;
            }
        }
        std::optional<ServerHost::State::Requested> requested;
        {
            const std::lock_guard lock(host_->mutex);
            if (const auto found = host_->requested.find(client.client_key); found != host_->requested.end()) {
                requested = found->second;
            }
        }
        const ServerStore& store = *host_->store;
        const bool approved = store.approved(client.client_key);

        if (client.pairing) {
            return;
        }
        m::Activate activate{.activities = {}, .active_roles = std::vector<std::string>{}, .pairing = std::nullopt};
        std::optional<hs::PskChoice> rehandshake;
        std::string decision;
        switch (client.psk) {
            case hs::PskCategory::kPairing:
                activate.activities = {m::Activity::kPairing};
                activate.pairing = m::PairingActivation{
                    .method = m::PairMethod::kPairingPsk, .format = std::nullopt, .pin_length = 0, .languages = {}};
                decision = "pairing by its pairing PSK";
                break;
            case hs::PskCategory::kLongTerm:
                if (requested && requested->method != m::PairMethod::kPairingPsk) {
                    // Paired again by a code: from the Sentinel, where code pairing runs.
                    rehandshake = hs::sentinel_choice();
                    decision = "back to the Sentinel to pair again";
                } else {
                    activate.activities = {m::Activity::kPlayback};
                    decision = "playback, paired";
                }
                break;
            case hs::PskCategory::kSentinel:
                if (requested && requested->method != m::PairMethod::kPairingPsk) {
                    activate.activities = {m::Activity::kPairing};
                    activate.pairing = m::PairingActivation{
                        .method = requested->method, .format = requested->format, .pin_length = 0, .languages = {}};
                    decision = "pairing by a code";
                } else if (store.has_pairing_psk(client.client_key)) {
                    rehandshake = store.choose(client.client_key);
                    decision = "re-handshaking to its pairing PSK";
                } else if (approved && client.offers_unpaired_access && !client.credential_mismatch) {
                    activate.activities = {m::Activity::kPlayback};
                    decision = "playback, approved unpaired";
                } else {
                    decision = client.credential_mismatch ? "waiting: it has lost its pairing and needs pairing again"
                                                          : "waiting for pairing or approval";
                }
                break;
        }
        if (!activate.activities.empty() && activate.activities.front() == m::Activity::kPlayback) {
            // The extension role only on a long-term PSK connection, and never beside player@v1
            // (planning/hearth-sendspin-extension.md, The role _ac3forge_player@v1).
            if (client.psk == hs::PskCategory::kLongTerm && client.ac3forge_support) {
                activate.active_roles = std::vector<std::string>{std::string(ac3forge::kRole)};
            } else if (client.player_support) {
                activate.active_roles = std::vector<std::string>{std::string(kPlayerRole)};
            }
        }
        if (decision == last_decision_) {
            return;
        }
        last_decision_ = decision;
        host_->log(client.name + ": " + decision);
        if (rehandshake) {
            (void)driver_->call([&] { return session_->rehandshake(*rehandshake); });
            return;
        }
        if (activate.activities.size() == 1 && activate.activities.front() == m::Activity::kPairing) {
            const std::lock_guard lock(host_->mutex);
            host_->requested.erase(client.client_key);
        }
        (void)driver_->call([&] { return session_->activate(activate); });
    }

    // Forgets the last decision, so the next decide() acts again.
    void reconsider() {
        last_decision_.clear();
        decide();
    }

    // --- ServerListener, with the session lock held --------------------------------------

    void on_hello(const m::ClientHello& /*hello*/) override { post_decision(); }

    void on_state(const m::ClientState& /*state*/) override { post_view(); }
    void on_goodbye(m::GoodbyeReason /*reason*/) override {}
    void on_leave() override {}

    void on_pairing_held_back(const std::optional<std::string>& /*message*/) override { post_view(); }

    void on_pairing_code_wanted() override {
        const std::string client_id = base64url::encode(session_->client_key());
        ServerHost::State* host = host_;
        host->post([host, client_id] { host->events->on_pairing_code_wanted(client_id); });
    }

    bool on_paired(const Key32& client_key, const Key32& long_term_psk) override {
        const bool stored = host_->store->store_record(client_key, long_term_psk);
        const std::string client_id = base64url::encode(client_key);
        ServerHost::State* host = host_;
        host->post([host, client_id] { host->events->on_paired(client_id); });
        return stored;
    }

    void on_pairing_ended(std::optional<pm::AbortReason> reason) override {
        const std::string client_id = base64url::encode(session_->client_key());
        ServerHost::State* host = host_;
        const std::weak_ptr<HostConnection> self = weak_from_this();
        host->post([host, client_id, reason, self] {
            host->events->on_pairing_ended(client_id, reason);
            if (const std::shared_ptr<HostConnection> connection = self.lock()) {
                connection->reconsider();
            }
        });
    }

   private:
    // The key ring the session names PSKs from: the store's.
    class ServerKeyringAdapter final : public hs::ServerKeyring {
       public:
        explicit ServerKeyringAdapter(ServerStore& store) : store_(&store) {}
        [[nodiscard]] hs::PskChoice choose(const Key32& client_key) const override { return store_->choose(client_key); }

       private:
        ServerStore* store_;
    };

    void post_decision() {
        const std::weak_ptr<HostConnection> self = weak_from_this();
        host_->post([self] {
            if (const std::shared_ptr<HostConnection> connection = self.lock()) {
                connection->last_decision_.clear();
                connection->decide();
                connection->host_->events->on_client(connection->view());
            }
        });
    }

    void post_view() {
        const std::weak_ptr<HostConnection> self = weak_from_this();
        host_->post([self] {
            if (const std::shared_ptr<HostConnection> connection = self.lock()) {
                connection->host_->events->on_client(connection->view());
            }
        });
    }

    ServerHost::State* host_;
    std::uint64_t id_;
    std::string url_;
    std::string peer_;
    ServerKeyringAdapter keyring_{*host_->store};
    // Read and written on the host's thread, and cleared under the session lock by on_paired.
    std::string last_decision_;
    std::optional<ServerSession> session_;
    std::unique_ptr<SessionDriver> driver_;
};

// Browsing's events, carried to the host's thread.
class HostBrowseListener final : public discovery::BrowseListener {
   public:
    explicit HostBrowseListener(ServerHost::State& host) : host_(&host) {}

    void on_found(const discovery::Service& service) override {
        const std::optional<std::string> url = service.url();
        if (!url) {
            return;
        }
        ServerHost::State* host = host_;
        const std::string name = service.instance;
        host->post([host, url = *url, name] {
            {
                const std::lock_guard lock(host->mutex);
                if (const auto previous = host->found_urls.find(name);
                    previous != host->found_urls.end() && previous->second != url) {
                    host->redial.erase(previous->second);
                }
                host->found_urls[name] = url;
                host->redial[url] = std::chrono::steady_clock::now();
            }
            host->log("found " + name + " at " + url);
        });
    }

    void on_lost(const std::string& instance) override {
        ServerHost::State* host = host_;
        host->post([host, instance] {
            {
                const std::lock_guard lock(host->mutex);
                if (const auto lost = host->found_urls.find(instance); lost != host->found_urls.end()) {
                    host->redial.erase(lost->second);
                    host->found_urls.erase(lost);
                }
            }
            host->log("lost " + instance);
        });
    }

   private:
    ServerHost::State* host_;
};

void ServerHost::State::run() {
    std::unique_lock lock(posted_mutex);
    while (true) {
        posted_changed.wait_for(lock, std::chrono::seconds(1), [this] { return stopping || !posted.empty(); });
        if (stopping && posted.empty()) {
            return;
        }
        if (!posted.empty()) {
            std::function<void()> work = std::move(posted.front());
            posted.pop_front();
            lock.unlock();
            work();
            lock.lock();
            continue;
        }
        // Dial the services browsing found that no connection serves.
        lock.unlock();
        std::vector<std::string> due;
        {
            const std::lock_guard state(mutex);
            const auto now = std::chrono::steady_clock::now();
            for (auto& [url, at] : redial) {
                const bool served = std::any_of(connections.begin(), connections.end(),
                                                [&](const auto& entry) { return entry.second->url() == url; });
                if (!served && at <= now) {
                    due.push_back(url);
                    at = now + kRedialAfter;
                }
            }
        }
        for (const std::string& url : due) {
            dial(url);
        }
        lock.lock();
    }
}

void ServerHost::State::accept(std::unique_ptr<transport::Connection> transport, std::string url) {
    std::shared_ptr<HostConnection> connection;
    {
        const std::lock_guard lock(mutex);
        const std::uint64_t id = next_id++;
        connection = std::make_shared<HostConnection>(*this, id, std::move(url));
        connections[id] = connection;
    }
    connection->start(std::move(transport));
}

void ServerHost::State::dial(const std::string& url) {
    std::expected<std::unique_ptr<transport::Connection>, websocket::ConnectError> dialled = websocket::connect(url);
    if (!dialled) {
        log("could not dial " + url);
        return;
    }
    log("dialled " + url);
    accept(std::move(*dialled), url);
}

void ServerHost::State::remove(std::uint64_t id) {
    std::shared_ptr<HostConnection> gone;
    {
        const std::lock_guard lock(mutex);
        const auto found = connections.find(id);
        if (found == connections.end()) {
            return;
        }
        gone = std::move(found->second);
        connections.erase(found);
    }
    const ClientView client = gone->view();
    if (client.hello) {
        events->on_client_gone(client.client_id);
    }
}

std::shared_ptr<HostConnection> ServerHost::State::find(const std::string& client_id) const {
    for (const std::shared_ptr<HostConnection>& connection : all()) {
        const ClientView client = connection->view();
        if (client.hello && client.client_id == client_id) {
            return connection;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<HostConnection>> ServerHost::State::all() const {
    std::vector<std::shared_ptr<HostConnection>> result;
    const std::lock_guard lock(mutex);
    for (const auto& [id, connection] : connections) {
        result.push_back(connection);
    }
    return result;
}

// --- ServerHost ------------------------------------------------------------------------------

ServerHost::ServerHost(std::unique_ptr<State> state) : state_(std::move(state)) {}

std::expected<std::unique_ptr<ServerHost>, std::string> ServerHost::start(ServerHostOptions options, ServerStore& store,
                                                                          ServerHostEvents& events) {
    auto state = std::make_unique<State>();
    state->options = std::move(options);
    state->store = &store;
    state->events = &events;
    State* raw = state.get();
    std::unique_ptr<ServerHost> host(new ServerHost(std::move(state)));
    raw->worker = std::thread([raw] { raw->run(); });

    if (raw->options.port) {
        websocket::ListenerOptions listening;
        listening.address = raw->options.address;
        listening.port = *raw->options.port;
        listening.max_connections = 32;
        raw->listener = websocket::Listener::start(
            listening, [raw](std::unique_ptr<transport::Connection> transport) { raw->accept(std::move(transport), {}); });
        if (!raw->listener) {
            return std::unexpected("cannot listen on " + raw->options.address + ":" + std::to_string(*raw->options.port));
        }
        if (raw->options.advertise) {
            raw->advertiser = discovery::mdns::advertise(
                {.service = std::string(discovery::kServerService),
                 .instance = raw->options.name,
                 .port = raw->listener->port(),
                 .txt = {{.key = "path", .value = std::string(websocket::kPath)}, {.key = "name", .value = raw->options.name}}},
                {.interfaces = raw->options.mdns_interfaces, .host = {}});
        }
    }
    if (raw->options.browse) {
        raw->browse_listener = std::make_unique<HostBrowseListener>(*raw);
        raw->browser = discovery::mdns::browse(std::string(discovery::kPlayerService), *raw->browse_listener,
                                               {.interfaces = raw->options.mdns_interfaces, .host = {}});
    }
    return host;
}

ServerHost::~ServerHost() {
    State& state = *state_;
    state.browser.reset();
    state.advertiser.reset();
    if (state.listener) {
        state.listener->stop();
    }
    for (const std::shared_ptr<HostConnection>& connection : state.all()) {
        connection->driver().close();
    }
    {
        const std::lock_guard lock(state.posted_mutex);
        state.stopping = true;
    }
    state.posted_changed.notify_all();
    state.worker.join();
    std::map<std::uint64_t, std::shared_ptr<HostConnection>> remaining;
    {
        const std::lock_guard lock(state.mutex);
        remaining.swap(state.connections);
    }
    remaining.clear();
}

std::optional<std::uint16_t> ServerHost::port() const {
    return state_->listener ? std::optional<std::uint16_t>(state_->listener->port()) : std::nullopt;
}

std::string ServerHost::server_id() const {
    return base64url::encode(state_->options.identity.public_key());
}

void ServerHost::dial(const std::string& url) {
    State* state = state_.get();
    state->post([state, url] { state->dial(url); });
}

std::vector<ClientView> ServerHost::clients() const {
    std::vector<ClientView> views;
    for (const std::shared_ptr<HostConnection>& connection : state_->all()) {
        ClientView view = connection->view();
        if (view.hello) {
            views.push_back(std::move(view));
        }
    }
    return views;
}

std::optional<ClientView> ServerHost::client(const std::string& client_id) const {
    const std::shared_ptr<HostConnection> connection = state_->find(client_id);
    return connection ? std::optional<ClientView>(connection->view()) : std::nullopt;
}

bool ServerHost::enter_pairing_token(std::string_view token) {
    const std::optional<pairing::PairingPskToken> decoded = pairing::decode_pairing_psk_token(token);
    if (!decoded) {
        return false;
    }
    state_->store->set_pairing_psk(decoded->client_key, decoded->pairing_psk);
    const std::string client_id = base64url::encode(decoded->client_key);
    State* state = state_.get();
    state->post([state, client_id] {
        if (const std::shared_ptr<HostConnection> connection = state->find(client_id)) {
            connection->reconsider();
        }
    });
    return true;
}

bool ServerHost::pair(const std::string& client_id, m::PairMethod method, std::optional<m::CodeFormat> format) {
    const std::shared_ptr<HostConnection> connection = state_->find(client_id);
    if (!connection || method == m::PairMethod::kPairingPsk) {
        return false;
    }
    {
        const std::lock_guard lock(state_->mutex);
        state_->requested[connection->view().client_key] = {.method = method, .format = format};
    }
    State* state = state_.get();
    const std::weak_ptr<HostConnection> weak = connection;
    state->post([weak] {
        if (const std::shared_ptr<HostConnection> held = weak.lock()) {
            held->reconsider();
        }
    });
    return true;
}

bool ServerHost::enter_code(const std::string& client_id, const pairing_flow::Code& code) {
    const std::shared_ptr<HostConnection> connection = state_->find(client_id);
    return connection &&
           connection->driver().call([&] { return connection->session().enter_code(code); }).has_value();
}

bool ServerHost::cancel_pairing(const std::string& client_id) {
    const std::shared_ptr<HostConnection> connection = state_->find(client_id);
    return connection &&
           connection->driver().call([&] { return connection->session().cancel_pairing(); }).has_value();
}

bool ServerHost::approve(const std::string& client_id, bool approved) {
    const std::shared_ptr<HostConnection> connection = state_->find(client_id);
    if (!connection) {
        return false;
    }
    state_->store->set_approved(connection->view().client_key, approved);
    State* state = state_.get();
    const std::weak_ptr<HostConnection> weak = connection;
    state->post([weak] {
        if (const std::shared_ptr<HostConnection> held = weak.lock()) {
            held->reconsider();
        }
    });
    return true;
}

bool ServerHost::unpair(const std::string& client_id) {
    const std::shared_ptr<HostConnection> connection = state_->find(client_id);
    if (!connection) {
        return false;
    }
    const Key32 key = connection->view().client_key;
    // The client removes its own record and leaves (messaging.md, server/unpair).
    (void)connection->driver().call([&] { return connection->session().unpair(); });
    state_->store->remove_record(key);
    return true;
}

// --- Group ------------------------------------------------------------------------------------

struct Group::State {
    ServerHost::State* host = nullptr;
    std::string id;
    std::string name;

    mutable std::mutex mutex;
    std::optional<m::AudioFormat> pcm;
    std::optional<ac3forge::StreamStart> bursts;
    std::int64_t sample_rate = 0;
    bool buffered = false;
    std::optional<std::int64_t> start_time;
    std::int64_t lead = 0;
    std::int64_t frames_pushed = 0;

    struct Member {
        std::string client_id;
        bool started = false;
        // Plays _ac3forge_player@v1's bursts rather than player@v1's PCM.
        bool bursts = false;
        std::optional<m::AudioFormat> format;
        std::unique_ptr<codec::Encoder> encoder;
        std::int64_t joined_frame = 0;
        std::uint64_t capacity = 0;
        // Each chunk sent and not yet played: when it has played, and its bytes.
        std::deque<std::pair<std::int64_t, std::size_t>> queued;
        std::size_t queued_bytes = 0;
        std::int64_t lead = 0;
    };
    std::vector<Member> members;

    [[nodiscard]] bool playing() const { return pcm || bursts; }

    // The lead a player needs from the moment a chunk is sent: its minimum buffer, or for a
    // buffered source its required lead time, beyond its output delay, and the network's.
    [[nodiscard]] std::int64_t lead_for(std::int64_t output_delay_ms, std::int64_t min_buffer_ms,
                                        std::int64_t required_lead_ms) const {
        const std::int64_t minimum = min_buffer_ms + output_delay_ms;
        const std::int64_t wanted = buffered ? required_lead_ms + output_delay_ms : 0;
        return (std::max(minimum, wanted) * 1000) + kNetworkLead;
    }

    // Starts `member`'s stream if its client can play the programme now.
    void try_start(Member& member) {
        const std::shared_ptr<HostConnection> connection = host->find(member.client_id);
        if (!connection || !playing()) {
            return;
        }
        const ClientView client = connection->view();
        if (!client.playing || !client.available) {
            return;
        }
        if (client.bursts) {
            if (!bursts || !client.ac3forge_support || !client.ac3forge_state ||
                !connection->driver()
                     .call([&] { return connection->session().start_burst_stream(*bursts); })
                     .has_value()) {
                return;
            }
            const ac3forge::State& state = *client.ac3forge_state;
            member.lead = lead_for(state.output_delay_ms, state.min_buffer_ms, state.required_lead_time_ms);
            member.capacity = client.ac3forge_support->buffer_capacity;
        } else {
            if (!pcm || !client.player_support || !client.player_state) {
                return;
            }
            const auto chosen = std::find_if(client.player_support->supported_formats.begin(),
                                             client.player_support->supported_formats.end(),
                                             [&](const m::AudioFormat& format) { return producible(format, *pcm); });
            if (chosen == client.player_support->supported_formats.end()) {
                return;
            }
            std::unique_ptr<codec::Encoder> encoder = codec::make_encoder(*chosen);
            if (!encoder) {
                return;
            }
            const m::PlayerStream stream{.format = *chosen, .codec_header = encoder->codec_header()};
            if (!connection->driver().call([&] { return connection->session().start_stream(stream); }).has_value()) {
                return;
            }
            const m::PlayerState& state = *client.player_state;
            member.lead = lead_for(state.output_delay_ms.value_or(0), state.min_buffer_ms.value_or(0),
                                   state.required_lead_time_ms.value_or(0));
            member.format = *chosen;
            member.encoder = std::move(encoder);
            member.capacity = client.player_support->buffer_capacity;
            member.joined_frame = frames_pushed;
        }
        (void)connection->driver().call([&] {
            return connection->session().update_group(
                {.playback_state = m::PlaybackState::kPlaying, .group_id = id, .group_name = name});
        });
        member.bursts = client.bursts;
        member.started = true;
        host->log(client.name + " joined group " + name);
    }

    // Starts every member whose client can play, and the timeline once one has: the time now, or
    // nothing while no member plays.
    [[nodiscard]] std::optional<std::int64_t> begin() {
        for (Member& member : members) {
            if (!member.started) {
                try_start(member);
            }
        }
        if (std::none_of(members.begin(), members.end(), [](const Member& member) { return member.started; })) {
            return std::nullopt;
        }
        const std::int64_t now = host->clock.now_us();
        if (!start_time) {
            // One timeline for every member, as far ahead as the member that needs the most lead.
            for (const Member& member : members) {
                lead = std::max(lead, member.lead);
            }
            start_time = now + lead;
        }
        return now;
    }

    // When programme frame `frame` plays, on the server clock.
    [[nodiscard]] std::int64_t time_of(std::int64_t frame) const {
        return *start_time + (frame * 1'000'000 / sample_rate);
    }

    // Whether a chunk of `bytes` bytes that plays from `frame` may go now to the members that play
    // `to_bursts`' kind: within the read-ahead, and while none of them holds three quarters of its
    // buffer capacity, nor a burst would take one past it.
    [[nodiscard]] bool may_send(std::int64_t frame, std::int64_t now, bool to_bursts, std::size_t bytes) {
        if (time_of(frame) > now + lead + (buffered ? kReadAhead : 0)) {
            return false;
        }
        for (Member& member : members) {
            if (!member.started || member.bursts != to_bursts) {
                continue;
            }
            while (!member.queued.empty() && member.queued.front().first <= now) {
                member.queued_bytes -= member.queued.front().second;
                member.queued.pop_front();
            }
            if (member.capacity > 0 && (member.queued_bytes > (member.capacity / 4) * 3 ||
                                        (to_bursts && member.queued_bytes + bytes > member.capacity))) {
                return false;
            }
        }
        return true;
    }

    static void forget(Member& member) {
        member.started = false;
        member.bursts = false;
        member.encoder.reset();
        member.queued.clear();
        member.queued_bytes = 0;
    }
};

Group::Group(std::shared_ptr<State> state) : state_(std::move(state)) {}

Group::~Group() {
    stop();
}

std::shared_ptr<Group> ServerHost::make_group(std::string name) {
    auto state = std::make_shared<Group::State>();
    state->host = state_.get();
    state->name = std::move(name);
    {
        const std::lock_guard lock(state_->mutex);
        state->id = "group-" + std::to_string(state_->next_group++);
    }
    return std::shared_ptr<Group>(new Group(std::move(state)));
}

const std::string& Group::id() const {
    return state_->id;
}

void Group::add(const std::string& client_id) {
    const std::lock_guard lock(state_->mutex);
    if (std::none_of(state_->members.begin(), state_->members.end(),
                     [&](const State::Member& member) { return member.client_id == client_id; })) {
        State::Member member;
        member.client_id = client_id;
        state_->members.push_back(std::move(member));
    }
}

void Group::remove(const std::string& client_id) {
    const std::lock_guard lock(state_->mutex);
    const auto found = std::find_if(state_->members.begin(), state_->members.end(),
                                    [&](const State::Member& member) { return member.client_id == client_id; });
    if (found == state_->members.end()) {
        return;
    }
    if (found->started) {
        if (const std::shared_ptr<HostConnection> connection = state_->host->find(client_id)) {
            const bool bursts = found->bursts;
            (void)connection->driver().call([&] {
                return bursts ? connection->session().end_burst_stream() : connection->session().end_stream();
            });
        }
    }
    state_->members.erase(found);
}

bool Group::start(const Programme& programme) {
    const std::optional<m::AudioFormat>& pcm = programme.pcm;
    const std::optional<ac3forge::StreamStart>& bursts = programme.bursts;
    if ((!pcm && !bursts) ||
        (pcm && (pcm->codec != m::Codec::kPcm || pcm->channels < 1 || pcm->sample_rate < 1 ||
                 (pcm->bit_depth != 16 && pcm->bit_depth != 24 && pcm->bit_depth != 32))) ||
        (bursts && bursts->sample_rate < 1) || (pcm && bursts && pcm->sample_rate != bursts->sample_rate)) {
        return false;
    }
    stop();
    const std::lock_guard lock(state_->mutex);
    state_->pcm = pcm;
    state_->bursts = bursts;
    state_->sample_rate = pcm ? pcm->sample_rate : bursts->sample_rate;
    state_->buffered = programme.buffered;
    state_->start_time.reset();
    state_->lead = 0;
    state_->frames_pushed = 0;
    for (State::Member& member : state_->members) {
        State::forget(member);
    }
    return true;
}

std::size_t Group::push(std::span<const std::int32_t> interleaved) {
    State& state = *state_;
    const std::lock_guard lock(state.mutex);
    if (!state.pcm) {
        return 0;
    }
    const auto channels = static_cast<std::size_t>(state.pcm->channels);
    const std::size_t frames = interleaved.size() / channels;
    if (frames == 0) {
        return 0;
    }
    const std::optional<std::int64_t> now = state.begin();
    if (!now || !state.may_send(state.frames_pushed, *now, false, 0)) {
        return 0;
    }

    const std::span<const std::int32_t> taken = interleaved.first(frames * channels);
    for (State::Member& member : state.members) {
        if (!member.started || member.bursts) {
            continue;
        }
        const std::shared_ptr<HostConnection> connection = state.host->find(member.client_id);
        if (!connection) {
            // The client has gone; it starts afresh if it comes back.
            State::forget(member);
            continue;
        }
        const std::int32_t depth = member.format->codec == m::Codec::kOpus ? 16 : member.format->bit_depth;
        std::vector<std::int32_t> samples(taken.begin(), taken.end());
        if (depth != state.pcm->bit_depth) {
            for (std::int32_t& sample : samples) {
                sample = rescaled(sample, state.pcm->bit_depth, depth);
            }
        }
        const std::optional<std::vector<codec::Unit>> units = member.encoder->encode(samples);
        if (!units) {
            continue;
        }
        for (const codec::Unit& unit : *units) {
            // Each unit at its first frame's time on the group's timeline, earlier by its codec's
            // look-ahead.
            const std::int64_t timestamp =
                state.time_of(member.joined_frame + unit.first_frame - member.encoder->delay_frames());
            if (connection->driver().call([&] { return connection->session().send_audio(timestamp, unit.bytes); })) {
                member.queued.emplace_back(timestamp + kLongestUnit, unit.bytes.size());
                member.queued_bytes += unit.bytes.size();
            }
        }
    }
    state.frames_pushed += static_cast<std::int64_t>(frames);
    return frames;
}

bool Group::push_burst(const Burst& burst) {
    State& state = *state_;
    const std::lock_guard lock(state.mutex);
    if (!state.bursts || burst.payload.empty()) {
        return false;
    }
    const std::size_t bytes = kBurstChunkHeaderBytes + burst.payload.size();
    const std::optional<std::int64_t> now = state.begin();
    if (!now || !state.may_send(burst.frame, *now, true, bytes)) {
        return false;
    }
    const std::int64_t timestamp = state.time_of(burst.frame);
    // A sink holds each chunk until its 1,536 samples have played.
    const std::int64_t played = state.time_of(burst.frame + kSamplesPerBurst);
    for (State::Member& member : state.members) {
        if (!member.started || !member.bursts) {
            continue;
        }
        const std::shared_ptr<HostConnection> connection = state.host->find(member.client_id);
        if (!connection) {
            State::forget(member);
            continue;
        }
        if (connection->driver().call([&] {
                return connection->session().send_burst(timestamp, burst.pc, burst.pd, burst.payload);
            })) {
            member.queued.emplace_back(played, bytes);
            member.queued_bytes += bytes;
        }
    }
    return true;
}

void Group::stop() {
    State& state = *state_;
    const std::lock_guard lock(state.mutex);
    if (!state.playing()) {
        return;
    }
    for (State::Member& member : state.members) {
        if (!member.started) {
            continue;
        }
        if (const std::shared_ptr<HostConnection> connection = state.host->find(member.client_id)) {
            if (member.bursts) {
                (void)connection->driver().call([&] { return connection->session().end_burst_stream(); });
            } else {
                if (std::optional<std::vector<codec::Unit>> units = member.encoder->finish()) {
                    for (const codec::Unit& unit : *units) {
                        const std::int64_t timestamp =
                            state.time_of(member.joined_frame + unit.first_frame - member.encoder->delay_frames());
                        (void)connection->driver().call(
                            [&] { return connection->session().send_audio(timestamp, unit.bytes); });
                    }
                }
                (void)connection->driver().call([&] { return connection->session().end_stream(); });
            }
            (void)connection->driver().call([&] {
                return connection->session().update_group(
                    {.playback_state = m::PlaybackState::kStopped, .group_id = state.id, .group_name = state.name});
            });
        }
        State::forget(member);
    }
    state.pcm.reset();
    state.bursts.reset();
    state.start_time.reset();
}

std::optional<std::int64_t> Group::start_time() const {
    const std::lock_guard lock(state_->mutex);
    return state_->start_time;
}

std::size_t Group::members_playing() const {
    const std::lock_guard lock(state_->mutex);
    return static_cast<std::size_t>(std::count_if(state_->members.begin(), state_->members.end(),
                                                   [](const State::Member& member) { return member.started; }));
}

}  // namespace ac3::sendspin

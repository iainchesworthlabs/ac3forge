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
#include <set>
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
    // The clients the operator has allowed source@v1, and every group made, which the host tells
    // of a member's changes.
    std::set<Key32> sources;
    std::vector<std::weak_ptr<Group::State>> groups;

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
    [[nodiscard]] std::vector<std::shared_ptr<Group::State>> live_groups();
    // A client's state or roles changed: every group it belongs to brings its members' roles up to
    // date. On the host's thread.
    void member_changed(const std::string& client_id);
    // A controller@v1 command from a client: volume and mute for its group, the rest for the engine.
    void controller_command(const std::string& client_id, const controller::CommandMessage& command);
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

    // Whether the client has said hello, and is `client_id`.
    [[nodiscard]] bool said_hello_as(const std::string& client_id) {
        return driver_->inspect(
            [&] { return session_->hello().has_value() && base64url::encode(session_->client_key()) == client_id; });
    }

    [[nodiscard]] ClientView view() {
        return driver_->inspect([&] {
            ClientView view;
            const ServerSession& session = *session_;
            view.client_key = session.client_key();
            view.client_id = base64url::encode(view.client_key);
            view.peer = peer_;
            view.url = url_;
            view.dialect = session.dialect();
            view.psk = session.psk_category();
            view.credential_mismatch = session.credential_mismatch();
            view.hello = session.hello().has_value();
            if (session.hello()) {
                view.name = session.hello()->name;
                view.device_info = session.hello()->device_info;
                view.offers_unpaired_access = session.hello()->unpaired_access;
                for (const m::PairMethodDescriptor& method : session.hello()->pair_methods) {
                    view.pair_methods.push_back(method.method);
                }
                view.player_support = session.hello()->player_support;
                if (contains(session.hello()->supported_roles, ac3forge::kRole)) {
                    view.ac3forge_support = session.hello()->ac3forge_support;
                }
                view.supported_roles = session.hello()->supported_roles;
                view.visualizer_support = session.hello()->visualizer_support;
                view.source_support = session.hello()->source_support;
            }
            view.pairing = session.pairing();
            view.wants_code = session.pairing_wants_code();
            view.bursts = contains(session.active_roles(), ac3forge::kRole);
            view.playing = view.bursts || contains(session.active_roles(), kPlayerRole);
            view.active_roles = session.active_roles();
            if (session.state()) {
                view.available = session.state()->available;
                view.player_state = session.state()->player;
                view.ac3forge_state = session.state()->ac3forge;
                view.artwork_state = session.state()->artwork;
                view.visualizer_state = session.state()->visualizer;
                view.source_state = session.state()->source;
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
            std::vector<std::string> roles;
            // The extension role only on a long-term PSK connection, and never beside player@v1
            // (planning/hearth-sendspin-extension.md, The role _ac3forge_player@v1).
            if (client.psk == hs::PskCategory::kLongTerm && client.ac3forge_support) {
                roles.emplace_back(ac3forge::kRole);
            } else if (client.player_support && contains(client.supported_roles, kPlayerRole)) {
                roles.emplace_back(kPlayerRole);
            }
            // The other roles by policy (planning/hearth-sendspin-extension.md, Other roles).
            bool source_allowed = false;
            {
                const std::lock_guard lock(host_->mutex);
                source_allowed = host_->sources.contains(client.client_key);
            }
            const bool spec = client.dialect == Dialect::kSpecification;
            for (const std::string_view role : {controller::kRole, metadata::kRole, color::kRole}) {
                if (contains(client.supported_roles, role)) {
                    roles.emplace_back(role);
                }
            }
            if (spec && contains(client.supported_roles, artwork::kRole)) {
                roles.emplace_back(artwork::kRole);
            }
            if (spec && contains(client.supported_roles, visualizer::kRole) && client.visualizer_support) {
                roles.emplace_back(visualizer::kRole);
            }
            if (spec && source_allowed && contains(client.supported_roles, source::kRole) && client.source_support) {
                roles.emplace_back(source::kRole);
            }
            for (const std::string& role : roles) {
                decision += " " + role;
            }
            activate.active_roles = std::move(roles);
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
        // The roles just activated get the state their groups hold, or none.
        host_->member_changed(client.client_id);
    }

    // Forgets the last decision, so the next decide() acts again.
    void reconsider() {
        last_decision_.clear();
        decide();
    }

    // --- ServerListener, with the session lock held --------------------------------------

    void on_hello(const m::ClientHello& /*hello*/) override { post_decision(); }

    void on_state(const m::ClientState& /*state*/) override {
        post_view();
        const std::string client_id = base64url::encode(session_->client_key());
        ServerHost::State* host = host_;
        host->post([host, client_id] { host->member_changed(client_id); });
    }
    void on_goodbye(m::GoodbyeReason /*reason*/) override {}
    void on_leave() override {}

    void on_controller_command(const controller::CommandMessage& command) override {
        const std::string client_id = base64url::encode(session_->client_key());
        ServerHost::State* host = host_;
        host->post([host, client_id, command] { host->controller_command(client_id, command); });
    }

    void on_source_stream_start(const m::ClientStreamStart& start) override {
        const std::string client_id = base64url::encode(session_->client_key());
        ServerHost::State* host = host_;
        host->post([host, client_id, start] { host->events->on_source_stream_start(client_id, start); });
    }
    void on_source_audio(std::int64_t timestamp_us, std::span<const std::uint8_t> frame) override {
        const std::string client_id = base64url::encode(session_->client_key());
        ServerHost::State* host = host_;
        host->post([host, client_id, timestamp_us, bytes = std::vector<std::uint8_t>(frame.begin(), frame.end())] {
            host->events->on_source_audio(client_id, timestamp_us, bytes);
        });
    }
    void on_source_stream_end() override {
        const std::string client_id = base64url::encode(session_->client_key());
        ServerHost::State* host = host_;
        host->post([host, client_id] { host->events->on_source_stream_end(client_id); });
    }

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
        // Its player no longer counts towards its groups' volume and mute.
        member_changed(client.client_id);
    }
}

std::shared_ptr<HostConnection> ServerHost::State::find(const std::string& client_id) const {
    for (const std::shared_ptr<HostConnection>& connection : all()) {
        if (connection->said_hello_as(client_id)) {
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

bool ServerHost::ac3forge_command(const std::string& client_id, const ac3forge::CommandMessage& command) {
    const std::shared_ptr<HostConnection> connection = state_->find(client_id);
    return connection &&
           connection->driver().call([&] { return connection->session().ac3forge_command(command); }).has_value();
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

bool ServerHost::allow_source(const std::string& client_id, bool allowed) {
    const std::shared_ptr<HostConnection> connection = state_->find(client_id);
    if (!connection) {
        return false;
    }
    {
        const std::lock_guard lock(state_->mutex);
        if (allowed) {
            state_->sources.insert(connection->view().client_key);
        } else {
            state_->sources.erase(connection->view().client_key);
        }
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

bool ServerHost::start_source(const std::string& client_id) {
    const std::shared_ptr<HostConnection> connection = state_->find(client_id);
    return connection &&
           connection->driver()
               .call([&] { return connection->session().source_command(source::Command::kStart); })
               .has_value();
}

bool ServerHost::stop_source(const std::string& client_id) {
    const std::shared_ptr<HostConnection> connection = state_->find(client_id);
    return connection &&
           connection->driver()
               .call([&] { return connection->session().source_command(source::Command::kStop); })
               .has_value();
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
        // The other roles: the version of each state last sent, the controller state last sent,
        // the artwork channels the images went to, and the visualizer stream started.
        std::uint64_t metadata_sent = 0;
        std::uint64_t colors_sent = 0;
        std::optional<controller::State> controller_sent;
        std::uint64_t artwork_sent = 0;
        std::optional<artwork::Channels> artwork_channels;
        std::optional<visualizer::StreamStart> visualizer_started;
    };
    std::vector<Member> members;

    // What the other roles show, each state with a version that changes whenever it is set.
    std::optional<metadata::State> metadata;
    std::uint64_t metadata_version = 0;
    std::optional<color::State> colors;
    std::uint64_t colors_version = 0;
    std::optional<Transport> transport;
    ArtworkImage artwork;
    std::int64_t artwork_timestamp = 0;
    std::uint64_t artwork_version = 0;
    std::vector<visualizer::Type> visualizer_types;
    std::int32_t visualizer_rate = 0;
    bool tracks_downbeats = false;

    [[nodiscard]] bool playing() const { return pcm || bursts; }

    [[nodiscard]] Member* member_of(const std::string& client_id) {
        const auto found = std::find_if(members.begin(), members.end(),
                                        [&](const Member& member) { return member.client_id == client_id; });
        return found == members.end() ? nullptr : &*found;
    }

    // A member's player as the group volume sees it, from whichever playback role is active.
    [[nodiscard]] static std::optional<controller::Player> player_of(const ClientView& client) {
        if (client.bursts && client.ac3forge_state) {
            const ac3forge::State& state = *client.ac3forge_state;
            const auto lists = [&](ac3forge::Command command) {
                return std::find(state.supported_commands.begin(), state.supported_commands.end(), command) !=
                       state.supported_commands.end();
            };
            return controller::Player{.volume = state.volume.value_or(100),
                                      .muted = state.muted.value_or(false),
                                      .volume_supported = lists(ac3forge::Command::kVolume),
                                      .mute_supported = lists(ac3forge::Command::kMute)};
        }
        if (client.playing && client.player_state) {
            const m::PlayerState& state = *client.player_state;
            // aiosendspin 9.1.1 lists volume and mute in the hello's support object (C28).
            const std::vector<m::PlayerCommand> none;
            const std::vector<m::PlayerCommand>& listed =
                client.dialect == Dialect::kAiosendspin911 && client.player_support ? client.player_support->commands
                : state.supported_commands                                          ? *state.supported_commands
                                                                                    : none;
            const auto lists = [&](m::PlayerCommand command) {
                return std::find(listed.begin(), listed.end(), command) != listed.end();
            };
            return controller::Player{.volume = state.volume.value_or(100),
                                      .muted = state.muted.value_or(false),
                                      .volume_supported = lists(m::PlayerCommand::kVolume),
                                      .mute_supported = lists(m::PlayerCommand::kMute)};
        }
        return std::nullopt;
    }

    // The controller state for the group now: the engine's transport, with volume and mute while a
    // player supports them and the group volume and mute its players give.
    [[nodiscard]] controller::State controller_state() {
        controller::State state;
        std::vector<controller::Player> players;
        for (const Member& member : members) {
            if (const std::shared_ptr<HostConnection> connection = host->find(member.client_id)) {
                if (const std::optional<controller::Player> player = player_of(connection->view())) {
                    players.push_back(*player);
                }
            }
        }
        for (const controller::Command command : transport->commands) {
            const bool ours = command == controller::Command::kVolume || command == controller::Command::kMute ||
                              command == controller::Command::kSwitch;
            const bool seekless = command == controller::Command::kSeek && !transport->seek_max_ms;
            if (!ours && !seekless) {
                state.supported_commands.push_back(command);
            }
        }
        if (std::any_of(players.begin(), players.end(), [](const controller::Player& p) { return p.volume_supported; })) {
            state.supported_commands.push_back(controller::Command::kVolume);
        }
        if (std::any_of(players.begin(), players.end(), [](const controller::Player& p) { return p.mute_supported; })) {
            state.supported_commands.push_back(controller::Command::kMute);
        }
        state.volume = controller::group_volume(players);
        state.muted = controller::group_muted(players);
        state.repeat = transport->repeat;
        state.shuffle = transport->shuffle;
        state.seek_max_ms = transport->seek_max_ms;
        return state;
    }

    // A controller's volume or mute, applied to the group's players (roles/controller/v1.md): the
    // volume by set_group_volume, the mute to every player that supports it. Each player that
    // changes gets one command; the controller state follows from the players' reports.
    void apply(const controller::CommandMessage& command) {
        std::vector<std::shared_ptr<HostConnection>> connections;
        std::vector<bool> extension;
        std::vector<controller::Player> players;
        for (const Member& member : members) {
            if (const std::shared_ptr<HostConnection> connection = host->find(member.client_id)) {
                const ClientView client = connection->view();
                if (const std::optional<controller::Player> player = player_of(client)) {
                    connections.push_back(connection);
                    extension.push_back(client.bursts);
                    players.push_back(*player);
                }
            }
        }
        const std::vector<std::int32_t> volumes =
            command.command == controller::Command::kVolume ? controller::set_group_volume(players, command.volume)
                                                            : std::vector<std::int32_t>{};
        for (std::size_t i = 0; i < players.size(); ++i) {
            const bool volume = command.command == controller::Command::kVolume && players[i].volume_supported &&
                                volumes[i] != players[i].volume;
            const bool mute = command.command == controller::Command::kMute && players[i].mute_supported &&
                              players[i].muted != command.mute;
            if (!volume && !mute) {
                continue;
            }
            HostConnection& connection = *connections[i];
            if (extension[i]) {
                ac3forge::CommandMessage message;
                message.command = volume ? ac3forge::Command::kVolume : ac3forge::Command::kMute;
                message.volume = volume ? volumes[i] : 0;
                message.mute = command.mute;
                (void)connection.driver().call([&] { return connection.session().ac3forge_command(message); });
            } else {
                const m::PlayerCommandMessage message{.command = volume ? m::PlayerCommand::kVolume : m::PlayerCommand::kMute,
                                                      .volume = volume ? volumes[i] : 0,
                                                      .mute = command.mute,
                                                      .output_delay_ms = 0};
                (void)connection.driver().call([&] { return connection.session().command(message); });
            }
        }
    }

    // One member's volume or mute, sent to it directly: no redistribution,
    // unlike apply()'s group-wide command - only this connection is asked,
    // on the same terms apply()'s own per-player loop already checks
    // (connected, has a playback role active, that role lists the command).
    void send_member_command(const std::string& client_id, controller::Command command, std::int32_t volume,
                             bool mute) {
        if (member_of(client_id) == nullptr) {
            return;
        }
        const std::shared_ptr<HostConnection> connection = host->find(client_id);
        if (!connection) {
            return;
        }
        const ClientView client = connection->view();
        const std::optional<controller::Player> player = player_of(client);
        if (!player) {
            return;
        }
        if ((command == controller::Command::kVolume && !player->volume_supported) ||
            (command == controller::Command::kMute && !player->mute_supported)) {
            return;
        }
        if (client.bursts) {
            ac3forge::CommandMessage message;
            message.command = command == controller::Command::kVolume ? ac3forge::Command::kVolume : ac3forge::Command::kMute;
            message.volume = volume;
            message.mute = mute;
            (void)connection->driver().call([&] { return connection->session().ac3forge_command(message); });
        } else {
            const m::PlayerCommandMessage message{
                .command = command == controller::Command::kVolume ? m::PlayerCommand::kVolume : m::PlayerCommand::kMute,
                .volume = volume,
                .mute = mute,
                .output_delay_ms = 0};
            (void)connection->driver().call([&] { return connection->session().command(message); });
        }
    }

    // Brings a member's other roles up to date with what the group shows. With the group's lock held.
    void sync_member(Member& member) {
        const std::shared_ptr<HostConnection> connection = host->find(member.client_id);
        if (!connection) {
            member.controller_sent.reset();
            member.artwork_channels.reset();
            member.visualizer_started.reset();
            return;
        }
        const ClientView client = connection->view();
        const auto active = [&](std::string_view role) { return contains(client.active_roles, role); };
        const auto sent = [&](std::string_view role) {
            return connection->driver().inspect([&] { return connection->session().state_sent(role); });
        };

        m::ServerState state;
        if (active(metadata::kRole) && (member.metadata_sent != metadata_version || !sent(metadata::kRole))) {
            state.metadata.emplace(metadata);
            member.metadata_sent = metadata_version;
        }
        if (active(color::kRole) && (member.colors_sent != colors_version || !sent(color::kRole))) {
            state.color.emplace(colors);
            member.colors_sent = colors_version;
        }
        if (active(controller::kRole)) {
            const std::optional<controller::State> wanted =
                transport ? std::optional<controller::State>(controller_state()) : std::nullopt;
            if (member.controller_sent != wanted || !sent(controller::kRole)) {
                state.controller.emplace(wanted);
                member.controller_sent = wanted;
            }
        }
        if (state.metadata || state.color || state.controller) {
            const auto sent_state = connection->driver().call([&] { return connection->session().send_state(state); });
            if (!sent_state && sent_state.error() == Refusal::kScheduled) {
                // A role's first state cannot wait for its time: what is scheduled shows now.
                const std::int64_t now = host->clock.now_us();
                if (state.metadata && *state.metadata) {
                    (**state.metadata).timestamp = std::min((**state.metadata).timestamp, now);
                }
                if (state.color && *state.color) {
                    (**state.color).timestamp = std::min((**state.color).timestamp, now);
                }
                (void)connection->driver().call([&] { return connection->session().send_state(state); });
            }
        }

        sync_artwork(member, *connection, client);
        sync_visualizer(member, *connection, client);
    }

    void sync_artwork(Member& member, HostConnection& connection, const ClientView& client) {
        if (!contains(client.active_roles, artwork::kRole) || !client.artwork_state || !client.available) {
            member.artwork_channels.reset();
            return;
        }
        const bool streaming = connection.driver().inspect([&] { return connection.session().artwork_streaming(); });
        if (!streaming || member.artwork_channels != client.artwork_state) {
            if (!connection.driver().call([&] { return connection.session().start_artwork_stream(); }).has_value()) {
                return;
            }
            member.artwork_channels = client.artwork_state;
            member.artwork_sent = 0;
        }
        if (member.artwork_sent == artwork_version) {
            return;
        }
        member.artwork_sent = artwork_version;
        for (std::size_t channel = 0; channel < artwork::kMaxChannels; ++channel) {
            const artwork::Channel wanted = client.artwork_state->at(channel);
            if (wanted.source == artwork::Source::kNone) {
                continue;
            }
            const std::optional<std::vector<std::uint8_t>> image =
                artwork ? artwork(wanted.source, wanted.format, wanted.width, wanted.height) : std::nullopt;
            send_image(connection, channel, image ? std::span<const std::uint8_t>(*image) : std::span<const std::uint8_t>{});
        }
    }

    // One image, or a clear for none, replacing a transfer still in flight.
    void send_image(HostConnection& connection, std::size_t channel, std::span<const std::uint8_t> image) {
        if (const std::optional<std::size_t> in_flight =
                connection.driver().inspect([&] { return connection.session().artwork_transfer(); })) {
            (void)connection.driver().call([&] { return connection.session().cancel_artwork(*in_flight); });
        }
        if (!connection.driver()
                 .call([&] {
                     return connection.session().announce_artwork(channel, artwork_timestamp,
                                                                  static_cast<std::uint32_t>(image.size()));
                 })
                 .has_value()) {
            return;
        }
        constexpr std::size_t kPart = artwork::kMaxMessageBytes - 2;
        for (std::size_t offset = 0; offset < image.size(); offset += kPart) {
            const std::span<const std::uint8_t> part = image.subspan(offset, std::min(kPart, image.size() - offset));
            if (!connection.driver().call([&] { return connection.session().send_artwork_part(part); }).has_value()) {
                return;
            }
        }
    }

    void sync_visualizer(Member& member, HostConnection& connection, const ClientView& client) {
        const bool active = contains(client.active_roles, visualizer::kRole);
        // The types both the client asked for and the engine analyses, if any.
        std::optional<visualizer::StreamStart> wanted;
        if (active && client.visualizer_state && client.available) {
            wanted = visualizer::derive(*client.visualizer_state, visualizer_types, visualizer_rate, tracks_downbeats);
            if (wanted->types.empty() || wanted->rate_max < 1) {
                wanted.reset();
            }
        }
        if (!wanted) {
            if (member.visualizer_started && active) {
                (void)connection.driver().call([&] { return connection.session().end_visualizer_stream(); });
            }
            member.visualizer_started.reset();
            return;
        }
        if (member.visualizer_started != wanted &&
            connection.driver().call([&] { return connection.session().start_visualizer_stream(*wanted); }).has_value()) {
            member.visualizer_started = wanted;
        }
    }

    // A member leaves the group: what the group showed its state roles is cleared, and its artwork
    // and visualizer streams end. With the group's lock held.
    void leave(const Member& member) {
        const std::shared_ptr<HostConnection> connection = host->find(member.client_id);
        if (!connection) {
            return;
        }
        const ClientView client = connection->view();
        m::ServerState cleared;
        if (contains(client.active_roles, metadata::kRole) && metadata) {
            cleared.metadata.emplace(std::nullopt);
        }
        if (contains(client.active_roles, color::kRole) && colors) {
            cleared.color.emplace(std::nullopt);
        }
        if (contains(client.active_roles, controller::kRole) && member.controller_sent) {
            cleared.controller.emplace(std::nullopt);
        }
        if (cleared.metadata || cleared.color || cleared.controller) {
            (void)connection->driver().call([&] { return connection->session().send_state(cleared); });
        }
        if (member.artwork_channels) {
            (void)connection->driver().call([&] { return connection->session().end_artwork_stream(); });
        }
        if (member.visualizer_started) {
            (void)connection->driver().call([&] { return connection->session().end_visualizer_stream(); });
        }
    }

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
    const std::lock_guard lock(state_->mutex);
    for (const State::Member& member : state_->members) {
        state_->leave(member);
    }
    state_->members.clear();
}

std::shared_ptr<Group> ServerHost::make_group(std::string name) {
    auto state = std::make_shared<Group::State>();
    state->host = state_.get();
    state->name = std::move(name);
    {
        const std::lock_guard lock(state_->mutex);
        state->id = "group-" + std::to_string(state_->next_group++);
        std::erase_if(state_->groups, [](const std::weak_ptr<Group::State>& group) { return group.expired(); });
        state_->groups.push_back(state);
    }
    return std::shared_ptr<Group>(new Group(std::move(state)));
}

std::vector<std::shared_ptr<Group::State>> ServerHost::State::live_groups() {
    std::vector<std::shared_ptr<Group::State>> result;
    const std::lock_guard lock(mutex);
    for (const std::weak_ptr<Group::State>& group : groups) {
        if (std::shared_ptr<Group::State> held = group.lock()) {
            result.push_back(std::move(held));
        }
    }
    return result;
}

void ServerHost::State::member_changed(const std::string& client_id) {
    bool grouped = false;
    for (const std::shared_ptr<Group::State>& group : live_groups()) {
        const std::lock_guard lock(group->mutex);
        if (group->member_of(client_id) != nullptr) {
            // Every member: a player's volume and mute are part of each controller's group state.
            for (Group::State::Member& member : group->members) {
                group->sync_member(member);
            }
            grouped = true;
        }
    }
    if (grouped) {
        return;
    }
    // A client in no group has no state for its state roles, which it is told promptly
    // (messaging.md, server/state).
    const std::shared_ptr<HostConnection> connection = find(client_id);
    if (!connection) {
        return;
    }
    m::ServerState nothing;
    connection->driver().inspect([&] {
        const ServerSession& session = connection->session();
        if (session.role_active(metadata::kRole) && !session.state_sent(metadata::kRole)) {
            nothing.metadata.emplace(std::nullopt);
        }
        if (session.role_active(controller::kRole) && !session.state_sent(controller::kRole)) {
            nothing.controller.emplace(std::nullopt);
        }
        if (session.role_active(color::kRole) && !session.state_sent(color::kRole)) {
            nothing.color.emplace(std::nullopt);
        }
        return 0;
    });
    if (nothing.metadata || nothing.controller || nothing.color) {
        (void)connection->driver().call([&] { return connection->session().send_state(nothing); });
    }
}

void ServerHost::State::controller_command(const std::string& client_id, const controller::CommandMessage& command) {
    for (const std::shared_ptr<Group::State>& group : live_groups()) {
        std::unique_lock lock(group->mutex);
        if (group->member_of(client_id) == nullptr) {
            continue;
        }
        if (command.command == controller::Command::kVolume || command.command == controller::Command::kMute) {
            group->apply(command);
            return;
        }
        const std::string group_id = group->id;
        lock.unlock();
        events->on_controller_command(group_id, client_id, command);
        return;
    }
    if (command.command != controller::Command::kVolume && command.command != controller::Command::kMute) {
        events->on_controller_command({}, client_id, command);
    }
}

const std::string& Group::id() const {
    return state_->id;
}

void Group::add(const std::string& client_id) {
    const std::lock_guard lock(state_->mutex);
    if (state_->member_of(client_id) == nullptr) {
        State::Member member;
        member.client_id = client_id;
        state_->members.push_back(std::move(member));
        // The newcomer's roles, and every controller's group volume and mute, which now count it.
        for (State::Member& each : state_->members) {
            state_->sync_member(each);
        }
    }
}

std::optional<controller::Player> Group::member_player(const std::string& client_id) const {
    const std::lock_guard lock(state_->mutex);
    if (state_->member_of(client_id) == nullptr) {
        return std::nullopt;
    }
    const std::shared_ptr<HostConnection> connection = state_->host->find(client_id);
    if (!connection) {
        return std::nullopt;
    }
    return State::player_of(connection->view());
}

void Group::set_member_volume(const std::string& client_id, std::int32_t volume) {
    const std::lock_guard lock(state_->mutex);
    state_->send_member_command(client_id, controller::Command::kVolume, volume, false);
}

void Group::set_member_muted(const std::string& client_id, bool muted) {
    const std::lock_guard lock(state_->mutex);
    state_->send_member_command(client_id, controller::Command::kMute, 0, muted);
}

void Group::set_group_volume(std::int32_t volume) {
    const std::lock_guard lock(state_->mutex);
    state_->apply(controller::CommandMessage{.command = controller::Command::kVolume, .volume = volume});
}

void Group::set_group_muted(bool muted) {
    const std::lock_guard lock(state_->mutex);
    state_->apply(controller::CommandMessage{.command = controller::Command::kMute, .mute = muted});
}

void Group::set_metadata(std::optional<metadata::State> state) {
    const std::lock_guard lock(state_->mutex);
    state_->metadata = std::move(state);
    ++state_->metadata_version;
    for (State::Member& member : state_->members) {
        state_->sync_member(member);
    }
}

void Group::set_colors(std::optional<color::State> state) {
    const std::lock_guard lock(state_->mutex);
    state_->colors = state ? std::optional<color::State>(color::with_contrast(*state)) : std::nullopt;
    ++state_->colors_version;
    for (State::Member& member : state_->members) {
        state_->sync_member(member);
    }
}

void Group::set_transport(std::optional<Transport> transport) {
    const std::lock_guard lock(state_->mutex);
    state_->transport = std::move(transport);
    for (State::Member& member : state_->members) {
        state_->sync_member(member);
    }
}

void Group::set_artwork(std::int64_t timestamp_us, ArtworkImage image) {
    const std::lock_guard lock(state_->mutex);
    state_->artwork = std::move(image);
    state_->artwork_timestamp = timestamp_us;
    ++state_->artwork_version;
    for (State::Member& member : state_->members) {
        state_->sync_member(member);
    }
}

void Group::set_visualizer(std::vector<visualizer::Type> types, std::int32_t rate_max, bool tracks_downbeats) {
    const std::lock_guard lock(state_->mutex);
    state_->visualizer_types = std::move(types);
    state_->visualizer_rate = rate_max;
    state_->tracks_downbeats = tracks_downbeats;
    for (State::Member& member : state_->members) {
        state_->sync_member(member);
    }
}

void Group::push_visualizer(const visualizer::Frame& frame) {
    const std::lock_guard lock(state_->mutex);
    for (const State::Member& member : state_->members) {
        if (!member.visualizer_started ||
            std::find(member.visualizer_started->types.begin(), member.visualizer_started->types.end(), frame.type) ==
                member.visualizer_started->types.end()) {
            continue;
        }
        if (const std::shared_ptr<HostConnection> connection = state_->host->find(member.client_id)) {
            (void)connection->driver().call([&] { return connection->session().send_visualizer_frame(frame); });
        }
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
    state_->leave(*found);
    state_->members.erase(found);
    for (State::Member& member : state_->members) {
        state_->sync_member(member);
    }
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

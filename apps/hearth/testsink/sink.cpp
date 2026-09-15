#include "sink.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
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
#include <variant>
#include <vector>

#include "ac3/sendspin/arbiter.hpp"
#include "ac3/sendspin/base64url.hpp"
#include "ac3/sendspin/discovery.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/mdns.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/pairing.hpp"
#include "ac3/sendspin/pairing_flow.hpp"
#include "ac3/sendspin/pairing_messages.hpp"
#include "ac3/sendspin/player_session.hpp"
#include "ac3/sendspin/session_driver.hpp"
#include "ac3/sendspin/transport.hpp"
#include "ac3/sendspin/websocket.hpp"
#include "store.hpp"
#include "wav_output.hpp"

namespace ac3::hearth::testsink {

namespace {

namespace m = sendspin::messages;
namespace flow = sendspin::pairing_flow;
namespace pm = sendspin::pairing_messages;
namespace websocket = sendspin::transport::websocket;
using sendspin::Arbiter;

[[nodiscard]] std::string key_text(const Key32& key) {
    return sendspin::base64url::encode(key);
}

// The first eight characters of a key's base64url, enough to tell servers apart in a log.
[[nodiscard]] std::string short_key(const Key32& key) {
    return key_text(key).substr(0, 8);
}

// A dynamic code as an operator types it: digits grouped 3-3 or 4-4, or the SP:1 token of a QR
// code (pairing.md, Pairing Code Presentation).
[[nodiscard]] std::string code_text(const flow::Code& code) {
    if (const auto* digits = std::get_if<std::string>(&code)) {
        if (digits->size() == 6 || digits->size() == 8) {
            const std::size_t half = digits->size() / 2;
            return digits->substr(0, half) + "-" + digits->substr(half);
        }
        return *digits;
    }
    const auto& bytes = std::get<std::array<std::uint8_t, 24>>(code);
    return sendspin::pairing::encode_token(sendspin::pairing::TokenVersion::kDynamicCode, bytes);
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

[[nodiscard]] std::string format_text(const m::AudioFormat& format) {
    const std::string_view codec = format.codec == m::Codec::kPcm    ? "PCM"
                                   : format.codec == m::Codec::kFlac ? "FLAC"
                                                                     : "Opus";
    return std::string(codec) + " " + std::to_string(format.sample_rate) + " Hz " + std::to_string(format.bit_depth) +
           "-bit " + std::to_string(format.channels) + " ch";
}

}  // namespace

// One server's connection: its session, driver and output.
class Connection final : public sendspin::PlayerListener, public std::enable_shared_from_this<Connection> {
   public:
    // Constructed with the session lock held: the session takes a number from the pairing state
    // its sessions share.
    Connection(Sink& sink, Arbiter::Id id, sendspin::PlayerConfig config)
        : sink_(&sink), id_(id), state_(config.player_state), output_(sink.options_.output_directory, "stream-" + std::to_string(id)) {
        session_.emplace(std::move(config), *sink.store_, sink.pairing_state_, *this, sink.clock_);
    }

    ~Connection() override {
        // The driver's threads stop first; the session's destructor then tells the shared pairing
        // state its connection has gone, which needs the lock the threads were using.
        driver_.reset();
        const std::lock_guard lock(*sink_->session_lock_);
        session_.reset();
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

    void start(std::unique_ptr<sendspin::transport::Connection> transport) {
        peer_ = transport->peer();
        Sink* sink = sink_;
        driver_ = std::make_unique<sendspin::SessionDriver>(
            std::move(transport),
            sendspin::DrivenSession{
                .receive = [this](const sendspin::transport::Frame& frame) { return session_->receive(frame); },
                .tick = [this] { return session_->tick(); },
                .next_tick_us = [this] { return session_->next_tick_us(); },
                .ended = [sink, id = id_] { sink->post([sink, id] { sink->remove(id); }); }},
            sendspin::SessionDriver::kDefaultMaxQueuedBytes, sink->session_lock_);
        log("connected from " + peer_);
        sendspin::SessionOutput first;
        {
            const std::lock_guard lock(*sink->session_lock_);
            first = session_->open();
        }
        driver_->start(std::move(first));
    }

    [[nodiscard]] sendspin::SessionDriver& driver() { return *driver_; }
    [[nodiscard]] sendspin::PlayerSession& session() { return *session_; }
    [[nodiscard]] const WavOutput& output() const { return output_; }

    // --- PlayerListener, called with the session lock held -------------------------------

    bool on_activation(const Key32& server_key, const m::Activate& activate, bool first) override {
        const Arbiter::Verdict verdict =
            sink_->arbiter_.activation(id_, server_key, Arbiter::rank_of(activate.activities), first);
        if (verdict.displaced) {
            Sink* sink = sink_;
            sink->post([sink, displaced = *verdict.displaced] { sink->displace(displaced); });
        }
        if (const std::optional<Key32> last = sink_->arbiter_.last_playback()) {
            sink_->store_->set_last_playback(*last);
        }
        const std::string activities = Arbiter::rank_of(activate.activities) == Arbiter::Rank::kPlayback ? "playback"
                                       : Arbiter::rank_of(activate.activities) == Arbiter::Rank::kPairing ? "pairing"
                                                                                                           : "nothing";
        log(std::string(verdict.admit ? "activated for " : "refused, another server holds the sink: ") + activities);
        return verdict.admit;
    }

    void on_pairing_attempt(bool in_progress) override { sink_->arbiter_.attempt(id_, in_progress); }

    void on_stream_start(const m::PlayerStream& stream) override {
        const bool writing = output_.start(stream);
        log("stream " + format_text(stream.format) +
            (writing ? (output_.file().empty() ? std::string{} : " to " + output_.file().string())
                     : std::string(" (not written)")));
    }
    void on_stream_clear() override {
        output_.clear();
        log("stream cleared");
    }
    void on_stream_end() override {
        output_.end();
        log("stream ended after " + std::to_string(output_.chunks()) + " chunks");
    }
    void on_audio(std::span<const std::uint8_t> frame, std::int64_t local_time) override {
        output_.write(frame, local_time);
    }

    void on_command(const m::PlayerCommandMessage& command) override {
        switch (command.command) {
            case m::PlayerCommand::kVolume:
                state_.volume = command.volume;
                log("volume " + std::to_string(command.volume));
                break;
            case m::PlayerCommand::kMute:
                state_.muted = command.mute;
                log(command.mute ? "muted" : "unmuted");
                break;
            case m::PlayerCommand::kSetOutputDelay:
                state_.output_delay_ms = command.output_delay_ms;
                log("output delay " + std::to_string(command.output_delay_ms) + " ms");
                break;
        }
        // The new state is reported from the sink's thread, outside this callback.
        const std::weak_ptr<Connection> self = weak_from_this();
        sink_->post([self] {
            if (const std::shared_ptr<Connection> connection = self.lock()) {
                connection->driver().call([&] { return connection->session_->set_state(connection->state_); });
            }
        });
    }

    void on_group(const m::GroupUpdate& update) override {
        if (update.group_name) {
            log("group " + *update.group_name);
        }
    }

    void on_unpaired(const Key32& server_key) override {
        sink_->store_->remove_record(server_key);
        log("unpaired by server " + short_key(server_key));
    }

    void on_pairing_code(const flow::Code& code) override { log("PAIRING CODE " + code_text(code)); }

    void on_pairing_held_back() override {
        log("pairing waits for the operator: type 'window' to open the static code's window, or 'reset' at the "
            "round limit");
    }

    void on_paired(const Key32& server_key, const Key32& long_term_psk) override {
        const bool stored = sink_->store_->add_record(server_key, long_term_psk);
        log("paired with server " + short_key(server_key) + (stored ? "" : " (the record could not be saved)"));
    }

    void on_pairing_ended(std::optional<pm::AbortReason> reason) override {
        log("pairing " + std::string(abort_text(reason)));
    }

   private:
    void log(std::string_view text) { sink_->log("[" + std::to_string(id_) + "] " + std::string(text)); }

    Sink* sink_;
    Arbiter::Id id_;
    std::string peer_;
    m::PlayerState state_;
    WavOutput output_;
    std::optional<sendspin::PlayerSession> session_;
    std::unique_ptr<sendspin::SessionDriver> driver_;
};

std::expected<std::unique_ptr<Sink>, std::string> Sink::start(SinkOptions options, SinkLog& log) {
    if (options.state_directory.empty()) {
        return std::unexpected(std::string("a state directory is required"));
    }
    if (options.code_method == CodeMethod::kStatic &&
        (options.static_code.size() != 8 ||
         !std::all_of(options.static_code.begin(), options.static_code.end(), [](char c) { return c >= '0' && c <= '9'; }))) {
        return std::unexpected(std::string("a static pairing code is eight digits"));
    }
    std::expected<std::unique_ptr<Store>, std::string> store = Store::open(options.state_directory);
    if (!store) {
        return std::unexpected(store.error());
    }
    if (!options.output_directory.empty()) {
        std::error_code error;
        std::filesystem::create_directories(options.output_directory, error);
        if (error) {
            return std::unexpected("cannot create " + options.output_directory.string());
        }
    }
    std::unique_ptr<Sink> sink(new Sink(std::move(options), log, std::move(*store)));

    websocket::ListenerOptions listening;
    listening.address = sink->options_.address;
    listening.port = sink->options_.port;
    listening.max_connections = sink->options_.max_connections;
    Sink* raw = sink.get();
    sink->listener_ = websocket::Listener::start(
        listening, [raw](std::unique_ptr<sendspin::transport::Connection> transport) { raw->accept(std::move(transport)); });
    if (!sink->listener_) {
        return std::unexpected("cannot listen on " + sink->options_.address + ":" + std::to_string(sink->options_.port));
    }
    if (sink->options_.advertise) {
        sendspin::discovery::Advertisement advertisement{
            .service = std::string(sendspin::discovery::kPlayerService),
            .instance = sink->options_.name,
            .port = sink->listener_->port(),
            .txt = {{.key = "path", .value = std::string(websocket::kPath)}, {.key = "name", .value = sink->options_.name}}};
        sink->advertiser_ = sendspin::discovery::mdns::advertise(
            std::move(advertisement), {.interfaces = sink->options_.mdns_interfaces, .host = {}});
        if (!sink->advertiser_) {
            sink->log("not advertised: no network interface could be opened for mDNS");
        }
    }
    return sink;
}

Sink::Sink(SinkOptions options, SinkLog& log, std::unique_ptr<Store> store)
    : options_(std::move(options)),
      log_(&log),
      store_(std::move(store)),
      arbiter_(store_->last_playback()),
      worker_([this] { run_posted(); }) {}

Sink::~Sink() {
    advertiser_.reset();
    if (listener_) {
        listener_->stop();
    }
    {
        const std::lock_guard lock(posted_mutex_);
        stopping_ = true;
    }
    posted_changed_.notify_all();
    worker_.join();
    std::map<Arbiter::Id, std::shared_ptr<Connection>> remaining;
    {
        const std::lock_guard lock(connections_mutex_);
        remaining.swap(connections_);
    }
    remaining.clear();
}

void Sink::accept(std::unique_ptr<sendspin::transport::Connection> transport) {
    sendspin::PlayerConfig config;
    config.identity = store_->identity();
    config.name = options_.name;
    config.device_info = {.product_name = "ac3hearth-testsink", .manufacturer = "AC3Forge", .software_version = {}, .mac_address = {}};
    config.supported_roles = {"player@v1"};
    std::vector<m::AudioFormat> formats;
    for (const m::Codec codec : options_.codecs) {
        formats.push_back({.codec = codec, .channels = 2, .sample_rate = 48000, .bit_depth = 16});
        if (codec != m::Codec::kOpus) {
            formats.push_back({.codec = codec, .channels = 2, .sample_rate = 44100, .bit_depth = 16});
            formats.push_back({.codec = codec, .channels = 2, .sample_rate = 48000, .bit_depth = 24});
        }
    }
    config.player_support = {.supported_formats = std::move(formats),
                             .buffer_capacity = 32 * 1024 * 1024,
                             .commands = {m::PlayerCommand::kVolume, m::PlayerCommand::kMute}};
    config.pair_methods = {{.method = m::PairMethod::kPairingPsk,
                            .locations = {m::SecretLocation::kOperator},
                            .out_channels = {},
                            .formats = {},
                            .min_pin_length = 0}};
    if (options_.code_method == CodeMethod::kDynamic) {
        config.pair_methods.push_back({.method = m::PairMethod::kDynamicCode,
                                       .locations = {},
                                       .out_channels = {m::OutChannel::kDisplay},
                                       .formats = {m::CodeFormat::kDigits, m::CodeFormat::kQrCode},
                                       .min_pin_length = 6});
    } else if (options_.code_method == CodeMethod::kStatic) {
        config.pair_methods.push_back({.method = m::PairMethod::kStaticCode,
                                       .locations = {m::SecretLocation::kOperator},
                                       .out_channels = {},
                                       .formats = {},
                                       .min_pin_length = 0});
        config.static_code = options_.static_code;
    }
    config.unpaired_access = options_.unpaired_access;
    config.player_state = {.volume = 100,
                           .muted = false,
                           .output_delay_ms = 0,
                           .required_lead_time_ms = 500,
                           .min_buffer_ms = 200,
                           .supported_commands = std::vector<m::PlayerCommand>{m::PlayerCommand::kVolume,
                                                                               m::PlayerCommand::kMute},
                           .format = std::nullopt};

    Arbiter::Id id = 0;
    {
        const std::lock_guard lock(connections_mutex_);
        id = next_id_++;
    }
    std::shared_ptr<Connection> connection;
    {
        const std::lock_guard lock(*session_lock_);
        connection = std::make_shared<Connection>(*this, id, std::move(config));
    }
    {
        const std::lock_guard lock(connections_mutex_);
        connections_[id] = connection;
    }
    connection->start(std::move(transport));
}

void Sink::remove(Arbiter::Id id) {
    std::shared_ptr<Connection> ended;
    {
        const std::lock_guard lock(connections_mutex_);
        const auto found = connections_.find(id);
        if (found == connections_.end()) {
            return;
        }
        ended = std::move(found->second);
        connections_.erase(found);
    }
    arbiter_.ended(id);
    Totals counted;
    ended->driver().inspect([&] {
        counted.streams = ended->output().streams();
        counted.chunks = ended->output().chunks();
        counted.frames = ended->output().frames();
        return 0;
    });
    {
        const std::lock_guard lock(connections_mutex_);
        ended_totals_.streams += counted.streams;
        ended_totals_.chunks += counted.chunks;
        ended_totals_.frames += counted.frames;
    }
    log("[" + std::to_string(id) + "] closed");
}

void Sink::post(std::function<void()> work) {
    {
        const std::lock_guard lock(posted_mutex_);
        posted_.push_back(std::move(work));
    }
    posted_changed_.notify_all();
}

void Sink::run_posted() {
    std::unique_lock lock(posted_mutex_);
    while (true) {
        posted_changed_.wait(lock, [this] { return stopping_ || !posted_.empty(); });
        if (posted_.empty()) {
            return;
        }
        std::function<void()> work = std::move(posted_.front());
        posted_.pop_front();
        lock.unlock();
        work();
        lock.lock();
    }
}

void Sink::displace(Arbiter::Id id) {
    std::shared_ptr<Connection> displaced;
    {
        const std::lock_guard lock(connections_mutex_);
        const auto found = connections_.find(id);
        if (found != connections_.end()) {
            displaced = found->second;
        }
    }
    if (displaced) {
        log("[" + std::to_string(id) + "] displaced by another server");
        displaced->driver().call([&] { return displaced->session().displace(); });
    }
}

void Sink::for_each_connection(const std::function<void(Connection&)>& visit) {
    std::vector<std::shared_ptr<Connection>> all;
    {
        const std::lock_guard lock(connections_mutex_);
        for (const auto& [id, connection] : connections_) {
            all.push_back(connection);
        }
    }
    for (const std::shared_ptr<Connection>& connection : all) {
        visit(*connection);
    }
}

void Sink::open_window() {
    {
        const std::lock_guard lock(*session_lock_);
        pairing_state_.open_window(clock_.now_us());
    }
    log("pairing window open for five minutes");
    for_each_connection([](Connection& connection) {
        connection.driver().call([&] { return connection.session().resume_pairing(); });
    });
}

void Sink::reset_rounds() {
    {
        const std::lock_guard lock(*session_lock_);
        pairing_state_.reset_rounds();
    }
    log("round limit reset");
    for_each_connection([](Connection& connection) {
        connection.driver().call([&] { return connection.session().resume_pairing(); });
    });
}

void Sink::cancel_pairing() {
    for_each_connection([](Connection& connection) {
        connection.driver().call([&] { return connection.session().cancel_pairing(); });
    });
}

std::uint16_t Sink::port() const {
    return listener_ ? listener_->port() : std::uint16_t{0};
}

std::string Sink::client_id() const {
    return key_text(store_->identity().public_key());
}

std::string Sink::pairing_token() const {
    std::array<std::uint8_t, 64> payload{};
    std::copy(store_->identity().public_key().begin(), store_->identity().public_key().end(), payload.begin());
    std::copy(store_->pairing_psk().begin(), store_->pairing_psk().end(), payload.begin() + 32);
    return sendspin::pairing::encode_token(sendspin::pairing::TokenVersion::kPairingPsk, payload);
}

Sink::Totals Sink::totals() const {
    Totals totals;
    std::vector<std::shared_ptr<Connection>> all;
    {
        const std::lock_guard lock(connections_mutex_);
        totals = ended_totals_;
        for (const auto& [id, connection] : connections_) {
            all.push_back(connection);
        }
    }
    totals.connections = static_cast<std::uint32_t>(all.size());
    for (const std::shared_ptr<Connection>& connection : all) {
        connection->driver().inspect([&] {
            totals.streams += connection->output().streams();
            totals.chunks += connection->output().chunks();
            totals.frames += connection->output().frames();
            return 0;
        });
    }
    return totals;
}

void Sink::log(std::string_view text) {
    log_->line(text);
}

}  // namespace ac3::hearth::testsink

#include "network_sinks.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <string_view>
#include <utility>

#include "ac3/sendspin/mdns.hpp"

namespace ac3::hearth {

namespace ss = ac3::sendspin;

namespace {

using namespace std::chrono_literals;

// The wait before each dial after a failure: the first comes quickly, since a
// sink that has just restarted is usually back within a second or two, and
// the rest back off to one dial every half minute for a sink that is off.
constexpr std::array<std::chrono::seconds, 6> kBackOff{1s, 2s, 4s, 8s, 15s, 30s};
// A connection that lasted this long clears the back-off when it ends; one
// that ended sooner counts as a failure, so a sink that accepts and then drops
// every connection is not dialled every second.
constexpr std::chrono::seconds kSettled{30};
// Dials in a row that fail while a pairing waits for one, before the page says
// the sink cannot be reached (pairing starts anyway once it can).
constexpr std::uint32_t kPairingDialsBeforeError = 3;

[[nodiscard]] std::chrono::seconds back_off(std::uint32_t failures) {
    const std::size_t index = std::min<std::size_t>(failures == 0 ? 0 : failures - 1, kBackOff.size() - 1);
    return kBackOff.at(index);
}

[[nodiscard]] std::string lower_ascii(std::string_view text) {
    std::string out{text};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

[[nodiscard]] bool contains_ci(std::string_view haystack, std::string_view needle) {
    return lower_ascii(haystack).find(lower_ascii(needle)) != std::string::npos;
}

[[nodiscard]] bool paired(const std::optional<ss::ClientView>& view) {
    return view.has_value() && view->psk == ss::handshake::PskCategory::kLongTerm;
}

// Whether a sink that has said hello offers the one pairing method the page
// takes; one that has not said is given the benefit of the doubt.
[[nodiscard]] bool offers_code_pairing(const std::optional<ss::ClientView>& view) {
    if (!view.has_value() || !view->hello) {
        return true;
    }
    return std::find(view->pair_methods.begin(), view->pair_methods.end(), ss::messages::PairMethod::kDynamicCode) !=
           view->pair_methods.end();
}

}  // namespace

NetworkSinks::NetworkSinks(ss::noise::KeyPair identity, std::string name, PairingStore& store,
                           bool request_firewall_exception)
    : store_(store) {
    ss::ServerHostOptions options{
        .identity = identity,
        .name = std::move(name),
        .languages = {"en"},
        .address = "0.0.0.0",
        // Dial-out only: Hearth finds players over mDNS and connects to
        // them, and nothing needs to find Hearth the other way (no
        // _sendspin-server._tcp advertisement, no listening socket).
        .port = std::nullopt,
        .advertise = false,
        // Discovery is this class's own (below): browsing here too would
        // dial everything twice, once under ServerHost's own timing and once
        // under this class's, and ServerHost's own redial would take back a
        // paired sink another server holds every ten seconds - exactly what
        // this class's own policy (network_sinks.hpp) does not do.
        .browse = false,
        .mdns_interfaces = {},
    };
    auto started = ss::ServerHost::start(std::move(options), store, *this);
    if (!started.has_value()) {
        return;
    }
    host_ = std::move(*started);
    browser_ = ss::discovery::mdns::browse(std::string(ss::discovery::kPlayerService), *this,
                                           {.request_firewall_exception = request_firewall_exception});
}

NetworkSinks::~NetworkSinks() {
    // browser_ and host_ each dispatch events (on_found()/on_lost()/
    // on_client()/...) back into this object from their own background
    // thread until their destructor actually stops that thread (Browser's
    // joins its mdns thread; ServerHost's joins its worker). A defaulted
    // destructor destroys members in reverse declaration order, which would
    // tear down sinks_/instance_by_url_/instance_by_client_id_ first, while
    // either thread can still be running - confirmed by a real crash: the
    // mdns browse thread received a genuine packet mid-teardown and faulted
    // inside sinks_'s std::map internals while the main thread was blocked
    // in browser_'s own destructor, joining that same thread. Stopping both
    // explicitly here, before any other member's destructor runs, closes
    // that window regardless of member declaration order.
    browser_.reset();
    // groups_ holds shared_ptr<Group>, whose destructor (~Group -> Group::State::leave()/stop())
    // calls back into host_'s ServerHost::State through Group::State::host, a non-owning pointer
    // - the same hazard this destructor already guards against for browser_/host_ themselves, just
    // in the opposite direction: groups_ must be torn down while host_ is still alive, not after.
    // Confirmed by a real hang otherwise: ~Group -> leave() -> host->find() -> State::all()
    // locking a mutex on a ServerHost::State host_.reset() below had already destroyed.
    groups_.clear();
    host_.reset();
}

std::optional<NetworkSinks::Dial> NetworkSinks::start_dial_locked(Entry& entry) {
    const std::optional<std::string> url = entry.service.url();
    if (!url) {
        entry.link = SinkLink::kIdle;
        return std::nullopt;
    }
    entry.link = SinkLink::kConnecting;
    return Dial{.url = *url, .to_pair = entry.pairing_requested};
}

void NetworkSinks::schedule_retry_locked(Entry& entry, Clock::time_point now) {
    if (entry.held_elsewhere) {
        entry.link = SinkLink::kIdle;
        return;
    }
    ++entry.failed_dials;
    entry.link = SinkLink::kRetrying;
    entry.next_dial = now + back_off(entry.failed_dials);
}

bool NetworkSinks::forget_if_unlisted_locked(std::map<std::string, Entry>::iterator it) {
    const Entry& entry = it->second;
    if (entry.listed || entry.client.has_value()) {
        return false;
    }
    if (const std::optional<std::string> url = entry.service.url()) {
        if (const auto mapped = instance_by_url_.find(*url);
            mapped != instance_by_url_.end() && mapped->second == it->first) {
            instance_by_url_.erase(mapped);
        }
    }
    if (!entry.client_id.empty()) {
        if (const auto mapped = instance_by_client_id_.find(entry.client_id);
            mapped != instance_by_client_id_.end() && mapped->second == it->first) {
            instance_by_client_id_.erase(mapped);
        }
    }
    sinks_.erase(it);
    return true;
}

void NetworkSinks::dial(const std::vector<Dial>& dials) {
    if (!host_) {
        return;
    }
    for (const Dial& one : dials) {
        if (one.to_pair) {
            (void)host_->dial_to_pair(one.url, ss::messages::PairMethod::kDynamicCode, ss::messages::CodeFormat::kDigits);
        } else {
            host_->dial(one.url);
        }
    }
}

void NetworkSinks::tick(Clock::time_point now) {
    std::vector<Dial> dials;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [instance, entry] : sinks_) {
            if (entry.link == SinkLink::kRetrying && entry.next_dial <= now) {
                if (const std::optional<Dial> one = start_dial_locked(entry)) {
                    dials.push_back(*one);
                }
            }
        }
        if (!dials.empty()) {
            publish_locked();
        }
    }
    dial(dials);
}

void NetworkSinks::rescan() {
    if (browser_) {
        browser_->refresh();
    }
    std::vector<Dial> dials;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [instance, entry] : sinks_) {
            if (!entry.listed || entry.link == SinkLink::kConnected || entry.link == SinkLink::kConnecting) {
                continue;
            }
            // Dialling a paired sink takes it: that is connect_sink()'s to do,
            // on the person's word, not a side effect of looking again.
            if (entry.held_elsewhere && paired(entry.known)) {
                continue;
            }
            entry.failed_dials = 0;
            if (const std::optional<Dial> one = start_dial_locked(entry)) {
                dials.push_back(*one);
            }
        }
        if (!dials.empty()) {
            publish_locked();
        }
    }
    dial(dials);
}

void NetworkSinks::select_sink(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    selected_id_ = id;
    selected_group_id_.clear();
    pairing_error_.clear();
    publish_locked();
}

void NetworkSinks::pair_sink(const std::string& id) {
    std::string pair_client_id;
    std::optional<std::string> url;
    std::vector<Dial> dials;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sinks_.find(id);
        if (it == sinks_.end()) {
            return;
        }
        Entry& entry = it->second;
        if (id == selected_id_) {
            pairing_error_.clear();
        }
        if (!offers_code_pairing(entry.known)) {
            if (id == selected_id_) {
                pairing_error_ = "This sink does not offer pairing by a code, which is how Hearth pairs.";
            }
            publish_locked();
            return;
        }
        if (entry.client.has_value() && entry.client->pairing && entry.client->pairing_attempt) {
            // Already running: the code boxes are the next step.
            return;
        }
        entry.pairing_requested = true;
        // Asked for by the person: pairing takes the sink from another server
        // if it has to, which is the point.
        entry.held_elsewhere = false;
        url = entry.service.url();
        if (entry.client.has_value() && entry.client->hello) {
            pair_client_id = entry.client_id;
        } else if (const std::optional<Dial> one = start_dial_locked(entry)) {
            // No live connection, or one still dialling: ServerHost attaches
            // the request to a dial already out at this URL rather than
            // making a second one.
            dials.push_back(*one);
        }
        publish_locked();
    }
    if (!pair_client_id.empty() && host_) {
        if (!host_->pair(pair_client_id, ss::messages::PairMethod::kDynamicCode, ss::messages::CodeFormat::kDigits) &&
            url) {
            // The connection went between the lock and the call.
            dials.push_back(Dial{.url = *url, .to_pair = true});
        }
    }
    dial(dials);
}

void NetworkSinks::submit_pairing_code(const std::string& id, const std::string& code) {
    std::string client_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sinks_.find(id);
        if (it == sinks_.end() || !it->second.client.has_value()) {
            return;
        }
        client_id = it->second.client_id;
        // Marked before the code goes, and with the request it answers: the
        // sink can refuse it and ask again before enter_code() returns, and
        // on_client() tells that request from this one by its count.
        it->second.code_entered = true;
        it->second.code_round = it->second.client->code_requests;
        if (id == selected_id_) {
            pairing_error_.clear();
        }
        publish_locked();
    }
    if (!host_ || !host_->enter_code(client_id, ss::pairing_flow::Code{code})) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sinks_.find(id);
        if (it != sinks_.end()) {
            it->second.code_entered = false;
            publish_locked();
        }
    }
}

void NetworkSinks::cancel_pairing(const std::string& id) {
    std::string client_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sinks_.find(id);
        if (it != sinks_.end()) {
            it->second.pairing_requested = false;
            if (it->second.client.has_value()) {
                client_id = it->second.client_id;
            }
            publish_locked();
        }
    }
    if (!client_id.empty() && host_) {
        host_->cancel_pairing(client_id);
    }
}

void NetworkSinks::forget_pairing(const std::string& id) {
    std::string client_id;
    std::optional<ss::crypto::Key32> client_key;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sinks_.find(id);
        if (it == sinks_.end()) {
            return;
        }
        Entry& entry = it->second;
        if (entry.client.has_value()) {
            client_id = entry.client_id;
        } else if (entry.known.has_value()) {
            client_key = entry.known->client_key;
            // Nothing is connected to tell: what the row shows follows the
            // record, which goes below.
            entry.known->psk = ss::handshake::PskCategory::kSentinel;
        }
        entry.held_elsewhere = false;
        publish_locked();
    }
    if (!client_id.empty() && host_) {
        host_->unpair(client_id);
    } else if (client_key) {
        store_.forget(*client_key);
    }
}

void NetworkSinks::connect_sink(const std::string& id) {
    std::vector<Dial> dials;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sinks_.find(id);
        if (it == sinks_.end()) {
            return;
        }
        Entry& entry = it->second;
        entry.held_elsewhere = false;
        if (entry.link != SinkLink::kConnected && entry.link != SinkLink::kConnecting) {
            entry.failed_dials = 0;
            if (const std::optional<Dial> one = start_dial_locked(entry)) {
                dials.push_back(*one);
            }
        }
        publish_locked();
    }
    dial(dials);
}

bool NetworkSinks::push_sink_settings(const std::string& id, ss::ac3forge::Settings settings) {
    std::string client_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sinks_.find(id);
        if (it == sinks_.end() || !it->second.client.has_value() ||
            !it->second.client->ac3forge_support.has_value()) {
            return false;
        }
        client_id = it->second.client_id;
        // Incremented here, under the lock, whether or not the send below
        // succeeds: a number spent on a failed attempt is harmless (the
        // wire has no monotonicity rule to violate), while two calls racing
        // to read the same number before either bumps it is not.
        settings.revision = it->second.next_settings_revision++;
    }
    if (client_id.empty() || !host_) {
        return false;
    }
    ss::ac3forge::CommandMessage message;
    message.command = ss::ac3forge::Command::kSettings;
    message.settings = settings;
    const bool sent = host_->ac3forge_command(client_id, message);
    if (sent) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sinks_.find(id);
        if (it != sinks_.end()) {
            it->second.intended_settings = settings;
            publish_locked();
        }
    }
    return sent;
}

bool NetworkSinks::push_sink_identify(const std::string& id, std::optional<ss::ac3forge::Identify> identify) {
    std::string client_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sinks_.find(id);
        if (it == sinks_.end() || !it->second.client.has_value() ||
            !it->second.client->ac3forge_support.has_value()) {
            return false;
        }
        client_id = it->second.client_id;
    }
    if (client_id.empty() || !host_) {
        return false;
    }
    ss::ac3forge::CommandMessage message;
    message.command = ss::ac3forge::Command::kIdentify;
    message.identify = identify;
    const bool sent = host_->ac3forge_command(client_id, message);
    if (sent) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sinks_.find(id);
        if (it != sinks_.end()) {
            it->second.identify_slot = identify.has_value() ? std::optional<std::int32_t>(identify->output) : std::nullopt;
            publish_locked();
        }
    }
    return sent;
}

std::string NetworkSinks::create_group(const std::string& name) {
    if (!host_) {
        return {};
    }
    std::shared_ptr<ss::Group> group = host_->make_group(name);
    const std::string id = group->id();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        GroupEntry entry;
        entry.name = name;
        entry.group = std::move(group);
        groups_[id] = std::move(entry);
        selected_group_id_ = id;
        selected_id_.clear();
        publish_locked();
    }
    return id;
}

void NetworkSinks::rename_group(const std::string& group_id, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = groups_.find(group_id);
    if (it != groups_.end()) {
        it->second.name = name;
        publish_locked();
    }
}

void NetworkSinks::delete_group(const std::string& group_id) {
    std::shared_ptr<ss::Group> group;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = groups_.find(group_id);
        if (it == groups_.end()) {
            return;
        }
        group = std::move(it->second.group);
        groups_.erase(it);
        if (selected_group_id_ == group_id) {
            selected_group_id_.clear();
        }
        publish_locked();
    }
    // group's own destructor (stop(), then every member leaves) runs here,
    // outside mutex_ - this class never holds its own lock across a call
    // into ac3::sendspin, on either side of it.
}

void NetworkSinks::select_group(const std::string& group_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    selected_group_id_ = group_id;
    selected_id_.clear();
    publish_locked();
}

void NetworkSinks::add_group_member(const std::string& group_id, const std::string& sink_id) {
    std::shared_ptr<ss::Group> group;
    std::string client_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = groups_.find(group_id);
        if (it == groups_.end()) {
            return;
        }
        client_id = member_client_id_locked(sink_id);
        if (client_id.empty()) {
            return;
        }
        if (std::find(it->second.member_sink_ids.begin(), it->second.member_sink_ids.end(), sink_id) ==
            it->second.member_sink_ids.end()) {
            it->second.member_sink_ids.push_back(sink_id);
        }
        group = it->second.group;
        publish_locked();
    }
    group->add(client_id);
}

void NetworkSinks::remove_group_member(const std::string& group_id, const std::string& sink_id) {
    std::shared_ptr<ss::Group> group;
    std::string client_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = groups_.find(group_id);
        if (it == groups_.end()) {
            return;
        }
        std::erase(it->second.member_sink_ids, sink_id);
        client_id = member_client_id_locked(sink_id);
        group = it->second.group;
        publish_locked();
    }
    if (!client_id.empty()) {
        group->remove(client_id);
    }
}

void NetworkSinks::set_group_volume(const std::string& group_id, std::int32_t volume) {
    std::shared_ptr<ss::Group> group;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = groups_.find(group_id);
        if (it != groups_.end()) {
            group = it->second.group;
        }
    }
    if (group) {
        group->set_group_volume(volume);
    }
}

void NetworkSinks::set_group_muted(const std::string& group_id, bool muted) {
    std::shared_ptr<ss::Group> group;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = groups_.find(group_id);
        if (it != groups_.end()) {
            group = it->second.group;
        }
    }
    if (group) {
        group->set_group_muted(muted);
    }
}

void NetworkSinks::set_member_volume(const std::string& group_id, const std::string& sink_id, std::int32_t volume) {
    std::shared_ptr<ss::Group> group;
    std::string client_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = groups_.find(group_id);
        if (it != groups_.end()) {
            group = it->second.group;
            client_id = member_client_id_locked(sink_id);
        }
    }
    if (group && !client_id.empty()) {
        group->set_member_volume(client_id, volume);
    }
}

void NetworkSinks::set_member_muted(const std::string& group_id, const std::string& sink_id, bool muted) {
    std::shared_ptr<ss::Group> group;
    std::string client_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = groups_.find(group_id);
        if (it != groups_.end()) {
            group = it->second.group;
            client_id = member_client_id_locked(sink_id);
        }
    }
    if (group && !client_id.empty()) {
        group->set_member_muted(client_id, muted);
    }
}

NetworkStatus NetworkSinks::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    NetworkStatus status;
    status.generation = generation_;
    status.selected_id = selected_id_;
    status.pairing_error = pairing_error_;
    status.sinks.reserve(sinks_.size());
    for (const auto& [instance, entry] : sinks_) {
        status.sinks.push_back(facts_locked(instance, entry));
    }
    status.selected_group_id = selected_group_id_;
    status.groups.reserve(groups_.size());
    for (const auto& [id, entry] : groups_) {
        status.groups.push_back(group_facts_locked(id, entry));
    }
    return status;
}

std::shared_ptr<sendspin::Group> NetworkSinks::group(const std::string& group_id) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = groups_.find(group_id);
    return found != groups_.end() ? found->second.group : nullptr;
}

std::vector<std::string> NetworkSinks::take_log() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(log_, {});
}

SinkFacts NetworkSinks::facts_locked(const std::string& instance, const Entry& entry) const {
    SinkFacts facts;
    facts.id = instance;
    facts.address = entry.service.addresses.empty() ? entry.service.host : entry.service.addresses.front();
    facts.port = entry.service.port;
    facts.path = entry.service.txt_value("path").value_or(std::string());
    // mDNS's own name until the sink says its own: the TXT record's `name`
    // (connection.md recommends one) where it has one, else the instance.
    facts.name = entry.service.txt_value("name").value_or(entry.service.instance);

    facts.link = entry.link;
    facts.failed_dials = entry.failed_dials;
    facts.dial_failed = entry.dial_failed;
    facts.held_elsewhere = entry.held_elsewhere;
    facts.pairing_requested = entry.pairing_requested;
    facts.offers_code_pairing = offers_code_pairing(entry.known);

    // What the sink last said about itself: the live connection's view, or
    // the one kept from its last connection.
    if (entry.known.has_value()) {
        const ss::ClientView& client = *entry.known;
        if (!client.name.empty()) {
            facts.name = client.name;
        }
        facts.pair_state =
            client.psk == ss::handshake::PskCategory::kLongTerm ? PairState::kPaired : PairState::kNotPaired;
        facts.lost_pairing = client.credential_mismatch;
        facts.roles = client.supported_roles;
        facts.hardware = client.device_info.product_name;
        facts.firmware = client.device_info.software_version;
        facts.ac3forge_support = client.ac3forge_support;

        if (client.ac3forge_support.has_value()) {
            facts.kind = SinkKind::kHearthSink;
            for (const ss::ac3forge::DataType type : client.ac3forge_support->data_types) {
                facts.data_types.emplace_back(ss::ac3forge::data_type_name(type));
            }
            facts.output_slots = static_cast<std::uint32_t>(client.ac3forge_support->outputs.count);
            facts.output_bit_depth = static_cast<std::uint32_t>(client.ac3forge_support->outputs.bit_depth);
        }
        if (client.player_support.has_value()) {
            if (facts.kind != SinkKind::kHearthSink) {
                facts.kind = SinkKind::kStandardPlayer;
            }
            for (const ss::messages::AudioFormat& format : client.player_support->supported_formats) {
                facts.codecs.emplace_back(ss::messages::codec_name(format.codec));
            }
        }
        // ac3hearth-testsink names itself in DeviceInfo::product_name;
        // nothing else in client/hello says "this is a test double" more
        // directly than that, so this is a heuristic, not a protocol fact.
        if (contains_ci(client.name, "testsink")) {
            facts.kind = SinkKind::kTestSink;
        }
    }

    // What only a live connection can say: the sink's state now, and where
    // pairing on it stands.
    if (entry.client.has_value()) {
        const ss::ClientView& client = *entry.client;
        facts.ac3forge_state = client.ac3forge_state;
        if (client.ac3forge_state.has_value()) {
            facts.required_lead_time_ms = static_cast<std::uint32_t>(client.ac3forge_state->required_lead_time_ms);
        } else if (client.player_state.has_value() && client.player_state->required_lead_time_ms.has_value()) {
            facts.required_lead_time_ms = static_cast<std::uint32_t>(*client.player_state->required_lead_time_ms);
        }
        // available (messaging.md, S6) is a player not reporting itself
        // ready until its time filter has converged - the nearest thing to
        // "the clock is synchronised" this app can read today.
        facts.clock_converged = client.available;
        facts.pairing_active = client.pairing && client.pairing_attempt;
        facts.wants_code = client.wants_code;
    }

    if (entry.held_elsewhere) {
        facts.notice = "In use by another server.";
    } else if (facts.lost_pairing) {
        facts.notice = "This sink has lost its pairing with this computer. Pair it again.";
    }

    if (facts.pair_state == PairState::kPaired && entry.known.has_value()) {
        for (const PairingRecordView& record : store_.records()) {
            if (record.client_key == entry.known->client_key) {
                facts.paired_on = record.paired_on;
                break;
            }
        }
    }

    facts.intended_settings = entry.intended_settings;
    facts.identify_slot = entry.identify_slot;

    return facts;
}

std::string NetworkSinks::member_client_id_locked(const std::string& sink_id) const {
    auto it = sinks_.find(sink_id);
    if (it == sinks_.end()) {
        return {};
    }
    return it->second.client_id;
}

GroupFacts NetworkSinks::group_facts_locked(const std::string& group_id, const GroupEntry& entry) const {
    GroupFacts facts;
    facts.id = group_id;
    facts.name = entry.name;
    facts.members.reserve(entry.member_sink_ids.size());
    for (const std::string& sink_id : entry.member_sink_ids) {
        GroupMemberFacts member;
        member.sink_id = sink_id;
        auto sink_it = sinks_.find(sink_id);
        if (sink_it == sinks_.end()) {
            // mDNS let the sink go and nothing is connected to it - still a
            // member, shown by its bare id.
            member.name = sink_id;
            facts.members.push_back(std::move(member));
            continue;
        }
        const SinkFacts sink_facts = facts_locked(sink_id, sink_it->second);
        member.name = sink_facts.name;
        member.kind = sink_facts.kind;
        member.required_lead_time_ms = sink_facts.required_lead_time_ms;
        member.connected = sink_it->second.client.has_value();
        if (member.connected && !sink_it->second.client_id.empty()) {
            if (const std::optional<ss::controller::Player> player =
                    entry.group->member_player(sink_it->second.client_id)) {
                member.volume = player->volume;
                member.muted = player->muted;
                member.volume_supported = player->volume_supported;
                member.mute_supported = player->mute_supported;
            }
        }
        facts.members.push_back(std::move(member));
    }
    return facts;
}

void NetworkSinks::publish_locked() {
    ++generation_;
}

void NetworkSinks::on_found(const ss::discovery::Service& service) {
    std::vector<Dial> dials;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry& entry = sinks_[service.instance];
        const std::optional<std::string> old_url = entry.service.url();
        entry.service = service;
        entry.listed = true;
        const std::optional<std::string> url = service.url();
        if (old_url && old_url != url) {
            if (const auto mapped = instance_by_url_.find(*old_url);
                mapped != instance_by_url_.end() && mapped->second == service.instance) {
                instance_by_url_.erase(mapped);
            }
        }
        if (url) {
            instance_by_url_[*url] = service.instance;
        }
        // Every sink found is dialled (this file's header comment says what it
        // then gets), bar one another server holds; a sink waiting out its
        // back-off is dialled at once, since mDNS hearing from it again is the
        // best sign yet that it is back.
        if (!entry.held_elsewhere && (entry.link == SinkLink::kIdle || entry.link == SinkLink::kRetrying)) {
            if (const std::optional<Dial> one = start_dial_locked(entry)) {
                dials.push_back(*one);
            }
        }
        publish_locked();
    }
    dial(dials);
}

void NetworkSinks::on_lost(const std::string& instance) {
    std::lock_guard<std::mutex> lock(mutex_);
    // An mDNS record expiring a few seconds late is common, so a row whose
    // connection is still live stays until that ends too.
    auto it = sinks_.find(instance);
    if (it == sinks_.end()) {
        return;
    }
    it->second.listed = false;
    (void)forget_if_unlisted_locked(it);
    publish_locked();
}

void NetworkSinks::on_client(const ss::ClientView& client) {
    if (!client.hello) {
        // Not yet a client this page can say anything about.
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto url_it = instance_by_url_.find(client.url);
    if (url_it == instance_by_url_.end()) {
        // A client that dialled Hearth rather than the other way round -
        // does not happen here (nothing listens), but the guard costs
        // nothing.
        return;
    }
    const std::string instance = url_it->second;
    auto sink_it = sinks_.find(instance);
    if (sink_it == sinks_.end()) {
        return;
    }
    Entry& entry = sink_it->second;
    if (!entry.client.has_value()) {
        entry.connected_at = Clock::now();
    }
    instance_by_client_id_[client.client_id] = instance;
    entry.client = client;
    entry.known = client;
    entry.client_id = client.client_id;
    entry.link = SinkLink::kConnected;
    entry.dial_failed = false;
    // A connection that says hello supersedes whatever ended the last one.
    entry.held_elsewhere = false;
    if (client.pairing_attempt) {
        // The attempt runs: what is left is the person's code.
        entry.pairing_requested = false;
    }
    if (entry.code_entered && client.wants_code && client.code_requests > entry.code_round) {
        // Asked for the code again after one was entered: it did not match, and the attempt goes
        // on to another round under the same code (pairing.md, rounds). A view that still wants
        // the code this one answered is only older than the answer.
        entry.code_entered = false;
        if (instance == selected_id_) {
            pairing_error_ = "That code was not right. Type the code the sink shows.";
        }
    } else if (!client.pairing_attempt) {
        entry.code_entered = false;
    }
    publish_locked();
}

void NetworkSinks::on_client_gone(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto id_it = instance_by_client_id_.find(client_id);
    if (id_it == instance_by_client_id_.end()) {
        return;
    }
    auto sink_it = sinks_.find(id_it->second);
    if (sink_it == sinks_.end()) {
        return;
    }
    Entry& entry = sink_it->second;
    entry.client.reset();
    entry.dial_failed = false;
    const Clock::time_point now = Clock::now();
    if (now - entry.connected_at >= kSettled) {
        entry.failed_dials = 0;
    }
    if (!forget_if_unlisted_locked(sink_it)) {
        schedule_retry_locked(entry, now);
    }
    publish_locked();
}

void NetworkSinks::on_dial_failed(const std::string& url, bool answered) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto url_it = instance_by_url_.find(url);
    if (url_it == instance_by_url_.end()) {
        return;
    }
    auto sink_it = sinks_.find(url_it->second);
    if (sink_it == sinks_.end()) {
        return;
    }
    Entry& entry = sink_it->second;
    if (entry.client.has_value()) {
        // A second connection failing beside a live first one changes nothing.
        return;
    }
    if (forget_if_unlisted_locked(sink_it)) {
        publish_locked();
        return;
    }
    entry.dial_failed = true;
    schedule_retry_locked(entry, Clock::now());
    if (entry.pairing_requested && entry.failed_dials >= kPairingDialsBeforeError && sink_it->first == selected_id_) {
        pairing_error_ = answered ? "The sink answers, but the connection to it fails. Pairing starts once it succeeds."
                                  : "The sink is not answering. Pairing starts once it does.";
    }
    publish_locked();
}

void NetworkSinks::on_client_goodbye(const std::string& client_id, ss::messages::GoodbyeReason reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto id_it = instance_by_client_id_.find(client_id);
    if (id_it == instance_by_client_id_.end()) {
        return;
    }
    auto sink_it = sinks_.find(id_it->second);
    if (sink_it == sinks_.end()) {
        return;
    }
    using ss::messages::GoodbyeReason;
    switch (reason) {
        case GoodbyeReason::kAnotherServer:
        case GoodbyeReason::kConcurrentAttempt:
            // A pairing asked for since is dialled regardless: this is the
            // sink refusing the connection that was waiting, not the pairing.
            if (!sink_it->second.pairing_requested) {
                sink_it->second.held_elsewhere = true;
            }
            break;
        case GoodbyeReason::kShutdown:
        case GoodbyeReason::kRestart:
        case GoodbyeReason::kUserRequest:
        case GoodbyeReason::kUnauthorized:
        case GoodbyeReason::kPairingRequired:
        case GoodbyeReason::kUnpaired:
        default:
            // Not "someone else is using this sink" - on_client() and
            // on_client_gone() already cover what the row shows for these.
            return;
    }
    publish_locked();
}

void NetworkSinks::on_pairing_code_wanted(const std::string& /*client_id*/) {
    // Nothing to do: enter_code() is called from submit_pairing_code() once
    // the person has filled in every digit box, not from here.
}

void NetworkSinks::on_paired(const std::string& /*client_id*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    pairing_error_.clear();
    publish_locked();
}

void NetworkSinks::on_pairing_ended(const std::string& client_id,
                                    std::optional<ss::pairing_messages::AbortReason> reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto id_it = instance_by_client_id_.find(client_id);
    if (id_it == instance_by_client_id_.end()) {
        return;
    }
    if (auto sink_it = sinks_.find(id_it->second); sink_it != sinks_.end()) {
        sink_it->second.pairing_requested = false;
    }
    if (!reason.has_value() || id_it->second != selected_id_) {
        publish_locked();
        return;
    }
    using ss::pairing_messages::AbortReason;
    switch (*reason) {
        case AbortReason::kCodeMismatch:
            pairing_error_ = "That code was not right. Pair again for a new one.";
            break;
        case AbortReason::kAttemptTimeout:
            pairing_error_ = "Took too long. Pair again for a new code.";
            break;
        case AbortReason::kUserCancelled:
            pairing_error_.clear();
            break;
        case AbortReason::kConcurrentAttempt:
            pairing_error_ = "Another pairing attempt is already in progress.";
            break;
        case AbortReason::kMethodNotSupported:
        case AbortReason::kPinLengthUnacceptable:
        default:
            pairing_error_ = "The sink could not complete pairing.";
            break;
    }
    publish_locked();
}

void NetworkSinks::on_log(std::string_view line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (log_.size() == kLogLines) {
        log_.erase(log_.begin());
    }
    log_.emplace_back(line);
}

}  // namespace ac3::hearth

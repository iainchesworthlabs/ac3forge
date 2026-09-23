#include "network_sinks.hpp"

#include <algorithm>

#include "ac3/sendspin/mdns.hpp"

namespace ac3::hearth {

namespace ss = ac3::sendspin;

namespace {

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

}  // namespace

NetworkSinks::NetworkSinks(ss::noise::KeyPair identity, std::string name, PairingStore& store) : store_(store) {
    ss::ServerHostOptions options{
        .identity = identity,
        .name = std::move(name),
        .languages = {"en"},
        .address = "0.0.0.0",
        // Dial-out only: Hearth finds players over mDNS and connects to
        // them, and nothing needs to find Hearth the other way in this slice
        // (no _sendspin-server._tcp advertisement, no listening socket).
        .port = std::nullopt,
        .advertise = false,
        // Discovery is this class's own (below): browsing here too would
        // dial everything twice, once under ServerHost's own timing and once
        // under this class's, for no second opinion worth having.
        .browse = false,
        .mdns_interfaces = {},
    };
    auto started = ss::ServerHost::start(std::move(options), store, *this);
    if (!started.has_value()) {
        return;
    }
    host_ = std::move(*started);
    browser_ = ss::discovery::mdns::browse(std::string(ss::discovery::kPlayerService), *this);
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
    host_.reset();
}

void NetworkSinks::rescan() {
    if (browser_) {
        browser_->refresh();
    }
}

void NetworkSinks::select_sink(const std::string& id) {
    std::string pair_client_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        selected_id_ = id;
        selected_group_id_.clear();
        pairing_error_.clear();
        auto it = sinks_.find(id);
        if (it != sinks_.end()) {
            if (it->second.client.has_value() && !it->second.client_id.empty() &&
                it->second.client->psk != ss::handshake::PskCategory::kLongTerm && !it->second.client->pairing) {
                pair_client_id = it->second.client_id;
            } else if (!it->second.client.has_value()) {
                // Hello has not arrived yet (on_found() only just dialled
                // it, or the dial is still in flight): on_client() starts
                // the attempt itself once it can.
                it->second.pairing_requested = true;
            }
        }
        publish_locked();
    }
    if (!pair_client_id.empty() && host_) {
        host_->pair(pair_client_id, ss::messages::PairMethod::kDynamicCode, ss::messages::CodeFormat::kDigits);
    }
}

void NetworkSinks::submit_pairing_code(const std::string& id, const std::string& code) {
    std::string client_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sinks_.find(id);
        if (it != sinks_.end()) {
            client_id = it->second.client_id;
        }
    }
    if (!client_id.empty() && host_) {
        host_->enter_code(client_id, ss::pairing_flow::Code{code});
    }
}

void NetworkSinks::cancel_pairing(const std::string& id) {
    std::string client_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sinks_.find(id);
        if (it != sinks_.end()) {
            client_id = it->second.client_id;
            it->second.pairing_requested = false;
        }
    }
    if (!client_id.empty() && host_) {
        host_->cancel_pairing(client_id);
    }
}

void NetworkSinks::forget_pairing(const std::string& id) {
    std::string client_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sinks_.find(id);
        if (it != sinks_.end()) {
            client_id = it->second.client_id;
        }
    }
    if (!client_id.empty() && host_) {
        host_->unpair(client_id);
    }
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

SinkFacts NetworkSinks::facts_locked(const std::string& instance, const Entry& entry) const {
    SinkFacts facts;
    facts.id = instance;
    facts.name = entry.client.has_value() && !entry.client->name.empty() ? entry.client->name
                                                                          : entry.service.instance;
    facts.address = entry.service.addresses.empty() ? entry.service.host : entry.service.addresses.front();
    facts.port = entry.service.port;
    facts.path = entry.service.txt_value("path").value_or(std::string());

    if (entry.client.has_value()) {
        const ss::ClientView& client = *entry.client;
        facts.pair_state =
            client.psk == ss::handshake::PskCategory::kLongTerm ? PairState::kPaired : PairState::kNotPaired;
        facts.roles = client.supported_roles;

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
        if (client.ac3forge_state.has_value()) {
            facts.required_lead_time_ms = static_cast<std::uint32_t>(client.ac3forge_state->required_lead_time_ms);
        } else if (client.player_state.has_value() && client.player_state->required_lead_time_ms.has_value()) {
            facts.required_lead_time_ms = static_cast<std::uint32_t>(*client.player_state->required_lead_time_ms);
        }
        // available (messaging.md, S6) is a player not reporting itself
        // ready until its time filter has converged - the nearest thing to
        // "the clock is synchronised" this app can read today.
        facts.clock_converged = client.available;
    }

    // ac3hearth-testsink names itself in DeviceInfo::product_name; nothing
    // else in client/hello says "this is a test double" more directly than
    // that, so this is a heuristic, not a protocol fact.
    if (entry.client.has_value() && contains_ci(entry.client->name, "testsink")) {
        facts.kind = SinkKind::kTestSink;
    }

    if (facts.pair_state == PairState::kPaired && entry.client.has_value()) {
        for (const PairingRecordView& record : store_.records()) {
            if (record.client_key == entry.client->client_key) {
                facts.paired_on = record.paired_on;
                break;
            }
        }
    }

    return facts;
}

std::string NetworkSinks::member_client_id_locked(const std::string& sink_id) const {
    auto it = sinks_.find(sink_id);
    if (it == sinks_.end() || !it->second.client.has_value()) {
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
            // Disconnected long enough that on_client_gone() erased its row
            // entirely (sinks_'s own churn, not this class's group
            // bookkeeping) - still a member, shown by its bare id.
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
    std::string url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sinks_[service.instance].service = service;
        url = service.url().value_or(std::string());
        if (!url.empty()) {
            instance_by_url_[url] = service.instance;
        }
        publish_locked();
    }
    // Safe to dial every sink found, paired or not: an unpaired connection
    // authenticates under the Sentinel PSK and claims no role
    // (connection.md, E7), and a paired one reconnecting is meant to hold
    // its sink - see this file's own header comment on why that is this
    // library's actual design, not a risk this class is taking on its own.
    if (!url.empty() && host_) {
        host_->dial(url);
    }
}

void NetworkSinks::on_lost(const std::string& instance) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Left in sinks_ if a connection is still live (on_client_gone() is what
    // removes a row once the sink is truly unreachable) - an mDNS record
    // expiring a few seconds late is common and should not blank a row that
    // is still answering.
    auto it = sinks_.find(instance);
    if (it != sinks_.end() && !it->second.client.has_value()) {
        instance_by_url_.erase(it->second.service.url().value_or(std::string()));
        sinks_.erase(it);
        publish_locked();
    }
}

void NetworkSinks::on_client(const ss::ClientView& client) {
    std::string pair_client_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto url_it = instance_by_url_.find(client.url);
        if (url_it == instance_by_url_.end()) {
            // A client that dialled Hearth rather than the other way round -
            // does not happen in this slice (advertise is off), but the
            // guard costs nothing.
            return;
        }
        const std::string& instance = url_it->second;
        instance_by_client_id_[client.client_id] = instance;
        Entry& entry = sinks_[instance];
        entry.client = client;
        entry.client_id = client.client_id;

        if (entry.pairing_requested && client.hello && client.psk != ss::handshake::PskCategory::kLongTerm &&
            !client.pairing) {
            entry.pairing_requested = false;
            pair_client_id = client.client_id;
        }
        publish_locked();
    }
    if (!pair_client_id.empty() && host_) {
        host_->pair(pair_client_id, ss::messages::PairMethod::kDynamicCode, ss::messages::CodeFormat::kDigits);
    }
}

void NetworkSinks::on_client_gone(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto id_it = instance_by_client_id_.find(client_id);
    if (id_it == instance_by_client_id_.end()) {
        return;
    }
    auto sink_it = sinks_.find(id_it->second);
    if (sink_it != sinks_.end()) {
        instance_by_url_.erase(sink_it->second.service.url().value_or(std::string()));
        sinks_.erase(sink_it);
    }
    instance_by_client_id_.erase(id_it);
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
    if (!reason.has_value() || id_it == instance_by_client_id_.end() || id_it->second != selected_id_) {
        return;
    }
    using ss::pairing_messages::AbortReason;
    switch (*reason) {
        case AbortReason::kCodeMismatch:
            pairing_error_ = "That code was not right. The sink is showing a new one.";
            break;
        case AbortReason::kAttemptTimeout:
            pairing_error_ = "Took too long - the sink is showing a new code.";
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

void NetworkSinks::on_log(std::string_view /*line*/) {
    // ServerHost's own diagnostic trail - nothing this class's callers need
    // yet (no diagnostics ring is threaded through here this slice; see
    // Engine's own diagnostics_ for the pattern a later slice would follow).
}

}  // namespace ac3::hearth

#include "packets.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <mdns.h>

#include "ac3/sendspin/discovery.hpp"

namespace ac3::sendspin::discovery::mdns_packets {

namespace {

// RFC 6762, section 10: 120 s for records that name a host, 75 minutes for the rest, and at
// most 10 s in a legacy unicast response.
constexpr std::uint32_t kHostTtl = 120;
constexpr std::uint32_t kOtherTtl = 4500;
constexpr std::uint32_t kLegacyTtl = 10;
constexpr std::int64_t kSecond = 1'000'000;
constexpr std::size_t kHeaderBytes = 12;
constexpr std::size_t kMaxLabel = 63;
constexpr std::uint16_t kResponseFlags = 0x8400;

[[nodiscard]] char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] std::string_view without_dot(std::string_view name) {
    return !name.empty() && name.back() == '.' ? name.substr(0, name.size() - 1) : name;
}

[[nodiscard]] std::string key_of(std::string_view name) {
    std::string key(without_dot(name));
    std::transform(key.begin(), key.end(), key.begin(), lower);
    return key;
}

[[nodiscard]] std::uint16_t read16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<unsigned>(bytes[offset]) << 8U) | bytes[offset + 1]);
}

[[nodiscard]] std::uint32_t read32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(read16(bytes, offset)) << 16U) | read16(bytes, offset + 2);
}

[[nodiscard]] std::string text_of(const mdns_string_t& text) {
    return text.str != nullptr ? std::string(text.str, text.length) : std::string{};
}

[[nodiscard]] std::string name_at(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::array<char, 256> text{};
    std::size_t cursor = offset;
    return text_of(mdns_string_extract(bytes.data(), bytes.size(), &cursor, text.data(), text.size()));
}

[[nodiscard]] mdns_string_t view_of(std::string_view text) {
    return {.str = text.data(), .length = text.size()};
}

// A packet being written, in the 32-bit aligned buffer the library's writers need.
class Builder {
   public:
    Builder() : storage_((kMaxWrite / sizeof(std::uint32_t)) + 1, 0U) {}

    [[nodiscard]] void* buffer() { return storage_.data(); }
    [[nodiscard]] std::size_t capacity() const { return storage_.size() * sizeof(std::uint32_t); }
    [[nodiscard]] std::uint8_t* bytes() { return static_cast<std::uint8_t*>(static_cast<void*>(storage_.data())); }
    [[nodiscard]] void* at(std::size_t offset) { return bytes() + offset; }

    void header(std::uint16_t id, std::uint16_t flags, std::uint16_t questions, std::uint16_t answers,
                std::uint16_t additional) {
        const std::array<std::uint16_t, 6> fields{id, flags, questions, answers, 0, additional};
        for (std::size_t i = 0; i < fields.size(); ++i) {
            bytes()[2 * i] = static_cast<std::uint8_t>(fields[i] >> 8U);
            bytes()[(2 * i) + 1] = static_cast<std::uint8_t>(fields[i] & 0xFFU);
        }
    }

    // The packet up to `end`, which a writer returned; nothing when it ran out of room.
    [[nodiscard]] std::vector<std::uint8_t> until(void* end) {
        if (end == nullptr) {
            return {};
        }
        const std::uint8_t* first = bytes();
        const std::uint8_t* last = static_cast<const std::uint8_t*>(end);
        return std::vector<std::uint8_t>(first, last);
    }

   private:
    std::vector<std::uint32_t> storage_;
};

enum class Form : std::uint8_t {
    kMulticast,  // cache-flush bits on unique records, full TTLs
    kLegacy,     // no cache-flush bits, TTLs of at most 10 s
    kGoodbye,    // TTL zero
};

constexpr auto kClassIn = static_cast<std::uint16_t>(MDNS_CLASS_IN);

[[nodiscard]] std::uint16_t class_for(Form form, bool unique) {
    return form == Form::kMulticast && unique ? static_cast<std::uint16_t>(kClassIn | MDNS_CACHE_FLUSH) : kClassIn;
}

[[nodiscard]] std::uint32_t ttl_for(Form form, std::uint32_t ttl) {
    switch (form) {
        case Form::kMulticast:
            return ttl;
        case Form::kLegacy:
            // Parenthesised: windows.h, which mdns.h includes, defines a min macro.
            return (std::min)(ttl, kLegacyTtl);
        case Form::kGoodbye:
            return 0;
    }
    return ttl;
}

// Writes a response with `answers` and `additional`, repeating `question` for a legacy query.
[[nodiscard]] std::vector<std::uint8_t> response(std::uint16_t id, const Question* question,
                                                 const std::vector<mdns_record_t>& answers,
                                                 const std::vector<mdns_record_t>& additional) {
    Builder packet;
    packet.header(id, kResponseFlags, question != nullptr ? 1 : 0,
                  mdns_answer_get_record_count(answers.data(), answers.size()),
                  mdns_answer_get_record_count(additional.data(), additional.size()));
    mdns_string_table_t table{};
    void* data = packet.at(kHeaderBytes);
    if (question != nullptr && !question->name.empty()) {
        data = mdns_answer_add_question_unicast(packet.buffer(), packet.capacity(), data,
                                                static_cast<mdns_record_type_t>(question->type),
                                                question->name.data(), question->name.size(), &table);
    }
    for (const std::vector<mdns_record_t>* section : {&answers, &additional}) {
        for (const mdns_record_t& record : *section) {
            data = mdns_answer_add_record(packet.buffer(), packet.capacity(), data, record, &table);
        }
        // TXT entries become one record, which keeps each entry's class and TTL unless the TTL
        // passed here is zero.
        const auto txt = std::find_if(section->begin(), section->end(),
                                      [](const mdns_record_t& record) { return record.type == MDNS_RECORDTYPE_TXT; });
        if (txt != section->end() && data != nullptr) {
            data = mdns_answer_add_txt_record(packet.buffer(), packet.capacity(), data, section->data(),
                                              section->size(), txt->rclass, txt->ttl, &table);
        }
    }
    return packet.until(data);
}

// The records one advertisement is made of, in `form`. They refer to the strings passed in.
struct Advertised {
    mdns_record_t ptr{};
    mdns_record_t srv{};
    mdns_record_t a{};
    std::vector<mdns_record_t> txt;
};

[[nodiscard]] Advertised advertised(const std::string& service, const std::string& instance, const std::string& host,
                                    std::uint16_t port, const Ipv4& address, const std::vector<TxtEntry>& txt,
                                    Form form) {
    Advertised records;
    records.ptr.name = view_of(service);
    records.ptr.type = MDNS_RECORDTYPE_PTR;
    records.ptr.data.ptr.name = view_of(instance);
    records.ptr.rclass = class_for(form, false);
    records.ptr.ttl = ttl_for(form, kOtherTtl);

    records.srv.name = view_of(instance);
    records.srv.type = MDNS_RECORDTYPE_SRV;
    records.srv.data.srv.name = view_of(host);
    records.srv.data.srv.port = port;
    records.srv.rclass = class_for(form, true);
    records.srv.ttl = ttl_for(form, kHostTtl);

    records.a.name = view_of(host);
    records.a.type = MDNS_RECORDTYPE_A;
    records.a.data.a.addr.sin_family = AF_INET;
    std::memcpy(&records.a.data.a.addr.sin_addr, address.data(), address.size());
    records.a.rclass = class_for(form, true);
    records.a.ttl = ttl_for(form, kHostTtl);

    for (const TxtEntry& entry : txt) {
        mdns_record_t record{};
        record.name = view_of(instance);
        record.type = MDNS_RECORDTYPE_TXT;
        record.data.txt.key = view_of(entry.key);
        record.data.txt.value = view_of(entry.value);
        record.rclass = class_for(form, true);
        record.ttl = ttl_for(form, kOtherTtl);
        records.txt.push_back(record);
    }
    return records;
}

}  // namespace

std::optional<Packet> parse(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeaderBytes) {
        return std::nullopt;
    }
    Packet packet;
    packet.id = read16(bytes, 0);
    packet.response = (read16(bytes, 2) & 0x8000U) != 0;
    const std::size_t questions = read16(bytes, 4);
    const std::size_t records = std::size_t{read16(bytes, 6)} + read16(bytes, 8) + read16(bytes, 10);
    std::size_t offset = kHeaderBytes;

    for (std::size_t i = 0; i < questions; ++i) {
        const std::size_t name_offset = offset;
        if (mdns_string_skip(bytes.data(), bytes.size(), &offset) == 0 || offset + 4 > bytes.size()) {
            return std::nullopt;
        }
        packet.questions.push_back({.name = name_at(bytes, name_offset),
                                    .type = read16(bytes, offset),
                                    .unicast = (read16(bytes, offset + 2) & MDNS_UNICAST_RESPONSE) != 0});
        offset += 4;
    }

    for (std::size_t i = 0; i < records; ++i) {
        const std::size_t name_offset = offset;
        if (mdns_string_skip(bytes.data(), bytes.size(), &offset) == 0 || offset + 10 > bytes.size()) {
            break;
        }
        Record record;
        record.name = name_at(bytes, name_offset);
        record.type = read16(bytes, offset);
        record.ttl = read32(bytes, offset + 4);
        const std::size_t length = read16(bytes, offset + 8);
        offset += 10;
        if (length > bytes.size() - offset) {
            break;
        }
        std::array<char, 256> text{};
        switch (record.type) {
            case kTypePtr:
                record.target = text_of(
                    mdns_record_parse_ptr(bytes.data(), bytes.size(), offset, length, text.data(), text.size()));
                break;
            case kTypeSrv: {
                const mdns_record_srv_t srv =
                    mdns_record_parse_srv(bytes.data(), bytes.size(), offset, length, text.data(), text.size());
                record.port = srv.port;
                record.target = text_of(srv.name);
                break;
            }
            case kTypeA:
                if (length == 4) {
                    Ipv4 address{};
                    std::copy_n(bytes.subspan(offset, 4).begin(), 4, address.begin());
                    record.address = address;
                }
                break;
            case kTypeTxt: {
                std::array<mdns_record_txt_t, 32> entries{};
                const std::size_t count = mdns_record_parse_txt(bytes.data(), bytes.size(), offset, length,
                                                                entries.data(), entries.size());
                for (std::size_t e = 0; e < count; ++e) {
                    record.txt.push_back({.key = text_of(entries[e].key), .value = text_of(entries[e].value)});
                }
                break;
            }
            default:
                break;
        }
        offset += length;
        packet.records.push_back(std::move(record));
    }
    return packet;
}

bool same_name(std::string_view a, std::string_view b) {
    const std::string_view left = without_dot(a);
    const std::string_view right = without_dot(b);
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

std::vector<std::uint8_t> query(const Question& question) {
    if (question.name.empty()) {
        return {};
    }
    Builder packet;
    packet.header(0, 0, 1, 0, 0);
    void* data = mdns_string_make(packet.buffer(), packet.capacity(), packet.at(kHeaderBytes), question.name.data(),
                                  question.name.size(), nullptr);
    if (data == nullptr) {
        return {};
    }
    data = mdns_htons(data, question.type);
    data = mdns_htons(data, question.unicast ? static_cast<std::uint16_t>(kClassIn | MDNS_UNICAST_RESPONSE) : kClassIn);
    return packet.until(data);
}

std::string instance_label(std::string_view friendly_name) {
    std::string label(friendly_name);
    std::replace(label.begin(), label.end(), '.', '-');
    if (label.size() > kMaxLabel) {
        std::size_t cut = kMaxLabel;
        while (cut > 0 && (static_cast<unsigned char>(label[cut]) & 0xC0U) == 0x80U) {
            --cut;
        }
        label.resize(cut);
    }
    return label.empty() ? std::string("Sendspin") : label;
}

std::string host_label(std::string_view host_name) {
    std::string label;
    for (const char c : host_name) {
        if (c == '.') {
            break;
        }
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        label.push_back(plain ? c : '-');
        if (label.size() == kMaxLabel) {
            break;
        }
    }
    return label.empty() ? std::string("sendspin") : label;
}

// --- Responder ------------------------------------------------------------------------------

Responder::Responder(const Advertisement& advertisement, std::string_view host, Ipv4 address)
    : service_(std::string(without_dot(advertisement.service)) + ".local."),
      instance_(instance_label(advertisement.instance) + "." + service_),
      host_(host_label(host) + ".local."),
      port_(advertisement.port),
      address_(address),
      txt_(advertisement.txt) {}

std::optional<std::vector<std::uint8_t>> Responder::respond(const Question& question, std::uint16_t id,
                                                              bool legacy) const {
    const Form form = legacy ? Form::kLegacy : Form::kMulticast;
    const bool any = question.type == kTypeAny;
    const Advertised records = advertised(service_, instance_, host_, port_, address_, txt_, form);

    std::vector<mdns_record_t> answers;
    std::vector<mdns_record_t> additional;
    if (same_name(question.name, kServiceTypes) && (any || question.type == kTypePtr)) {
        mdns_record_t types = records.ptr;
        types.name = view_of(kServiceTypes);
        types.data.ptr.name = view_of(service_);
        answers = {types};
    } else if (same_name(question.name, service_) && (any || question.type == kTypePtr)) {
        answers = {records.ptr};
        additional = {records.srv, records.a};
        additional.insert(additional.end(), records.txt.begin(), records.txt.end());
    } else if (same_name(question.name, instance_) && (any || question.type == kTypeSrv)) {
        answers = {records.srv};
        additional = {records.a};
        additional.insert(additional.end(), records.txt.begin(), records.txt.end());
    } else if (same_name(question.name, instance_) && question.type == kTypeTxt) {
        answers = records.txt;
    } else if (same_name(question.name, host_) && (any || question.type == kTypeA)) {
        answers = {records.a};
    } else {
        return std::nullopt;
    }
    if (answers.empty()) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> packet = response(legacy ? id : 0, legacy ? &question : nullptr, answers, additional);
    if (packet.empty()) {
        return std::nullopt;
    }
    return packet;
}

std::vector<std::uint8_t> Responder::announcement(bool goodbye) const {
    const Advertised records =
        advertised(service_, instance_, host_, port_, address_, txt_, goodbye ? Form::kGoodbye : Form::kMulticast);
    std::vector<mdns_record_t> answers{records.ptr, records.srv, records.a};
    answers.insert(answers.end(), records.txt.begin(), records.txt.end());
    return response(0, nullptr, answers, {});
}

// --- BrowseState ----------------------------------------------------------------------------

BrowseState::BrowseState(std::string_view service) : service_(std::string(without_dot(service)) + ".local.") {}

BrowseState::Changes BrowseState::receive(const Packet& packet, std::int64_t now) {
    if (!packet.response) {
        return {};
    }
    const std::string suffix = "." + key_of(service_);
    const auto expiry = [now](std::uint32_t ttl) {
        // A TTL of zero is a goodbye, which takes effect a second later (RFC 6762, section 10.1).
        return ttl == 0 ? now + kSecond : now + (static_cast<std::int64_t>(ttl) * kSecond);
    };
    const auto instance_for = [&](const std::string& name) -> Instance* {
        const std::string key = key_of(name);
        if (key.size() <= suffix.size() || !key.ends_with(suffix)) {
            return nullptr;
        }
        const auto found = instances_.find(key);
        if (found != instances_.end()) {
            return &found->second;
        }
        if (instances_.size() >= kMaxEntries) {
            return nullptr;
        }
        Instance& created = instances_[key];
        created.name = std::string(without_dot(name));
        return &created;
    };

    for (const Record& record : packet.records) {
        if (record.type == kTypePtr && same_name(record.name, service_)) {
            if (Instance* instance = instance_for(record.target)) {
                instance->expires = expiry(record.ttl);
            }
        } else if (record.type == kTypeSrv) {
            if (Instance* instance = instance_for(record.name)) {
                instance->host = std::string(without_dot(record.target));
                instance->port = record.port;
                instance->location_expires = expiry(record.ttl);
            }
        } else if (record.type == kTypeTxt) {
            if (Instance* instance = instance_for(record.name)) {
                instance->txt = record.txt;
            }
        } else if (record.type == kTypeA && record.address) {
            const std::string key = key_of(record.name);
            if (!hosts_.contains(key) && hosts_.size() >= kMaxEntries) {
                continue;
            }
            std::vector<Address>& addresses = hosts_[key];
            const auto same = std::find_if(addresses.begin(), addresses.end(),
                                           [&](const Address& known) { return known.address == *record.address; });
            if (same != addresses.end()) {
                same->expires = expiry(record.ttl);
            } else if (addresses.size() < kMaxEntries) {
                addresses.push_back({.address = *record.address, .expires = expiry(record.ttl)});
            }
        }
    }
    return report(now);
}

BrowseState::Changes BrowseState::expire(std::int64_t now) {
    Changes changes;
    for (auto it = instances_.begin(); it != instances_.end();) {
        Instance& instance = it->second;
        if (instance.expires != 0 && instance.expires <= now) {
            if (instance.reported) {
                changes.lost.push_back(instance.reported->instance);
            }
            it = instances_.erase(it);
            continue;
        }
        if (instance.location_expires != 0 && instance.location_expires <= now) {
            instance.host.clear();
            instance.port = 0;
            instance.location_expires = 0;
        }
        ++it;
    }
    for (auto it = hosts_.begin(); it != hosts_.end();) {
        std::erase_if(it->second, [now](const Address& address) { return address.expires <= now; });
        it = it->second.empty() ? hosts_.erase(it) : std::next(it);
    }
    Changes reported = report(now);
    changes.found = std::move(reported.found);
    return changes;
}

std::vector<Question> BrowseState::missing(std::int64_t now) const {
    std::vector<Question> questions;
    for (const auto& [key, instance] : instances_) {
        if (instance.expires <= now) {
            continue;
        }
        if (instance.port == 0) {
            questions.push_back({.name = instance.name + ".", .type = kTypeSrv, .unicast = true});
        }
        if (!instance.txt) {
            questions.push_back({.name = instance.name + ".", .type = kTypeTxt, .unicast = true});
        }
        if (!instance.host.empty() && !hosts_.contains(key_of(instance.host))) {
            questions.push_back({.name = instance.host + ".", .type = kTypeA, .unicast = true});
        }
    }
    return questions;
}

BrowseState::Changes BrowseState::report(std::int64_t now) {
    Changes changes;
    const std::string_view suffix = without_dot(service_);
    for (auto& [key, instance] : instances_) {
        if (instance.expires <= now || instance.port == 0 || instance.host.empty()) {
            continue;
        }
        const auto host = hosts_.find(key_of(instance.host));
        if (host == hosts_.end() || host->second.empty()) {
            continue;
        }
        Service service;
        // The instance's own label: its name less "._sendspin._tcp.local".
        service.instance = instance.name.substr(0, instance.name.size() - suffix.size() - 1);
        service.host = instance.host;
        for (const Address& address : host->second) {
            service.addresses.push_back(std::to_string(address.address[0]) + "." + std::to_string(address.address[1]) +
                                        "." + std::to_string(address.address[2]) + "." +
                                        std::to_string(address.address[3]));
        }
        service.port = instance.port;
        service.txt = instance.txt.value_or(std::vector<TxtEntry>{});
        if (!instance.reported || !(*instance.reported == service)) {
            instance.reported = service;
            changes.found.push_back(std::move(service));
        }
    }
    return changes;
}

}  // namespace ac3::sendspin::discovery::mdns_packets

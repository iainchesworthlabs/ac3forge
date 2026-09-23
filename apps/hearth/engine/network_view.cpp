#include "network_view.hpp"

#include <algorithm>
#include <cctype>

#include <fmt/format.h>

#include "ac3/sendspin/state_roles.hpp"

namespace ac3::hearth {

namespace {

namespace controller = ac3::sendspin::controller;

[[nodiscard]] std::string join(const std::vector<std::string>& items, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            out += sep;
        }
        out += items[i];
    }
    return out;
}

// "AC-3", "E-AC-3" or "AC-3 and E-AC-3" - the extension spec's own data type
// names, in the order the sink listed them (ac3::sendspin::ac3forge::DataType
// has only two values, so "X, Y and Z" never arises here).
[[nodiscard]] std::string data_types_text(const std::vector<std::string>& data_types) {
    std::vector<std::string> named;
    named.reserve(data_types.size());
    for (const std::string& type : data_types) {
        if (type == "ac3") {
            named.emplace_back("AC-3");
        } else if (type == "eac3") {
            named.emplace_back("E-AC-3");
        }
    }
    if (named.size() == 2) {
        return named[0] + " and " + named[1];
    }
    return join(named, ", ");
}

// "PCM and FLAC", "FLAC, PCM and Opus" - the codec list a player's own hello
// gave, in its preference order (kept as given rather than re-sorted, since
// order is the one piece of meaning a bare list of codecs carries here).
[[nodiscard]] std::string codecs_text(const std::vector<std::string>& codecs) {
    std::vector<std::string> named;
    named.reserve(codecs.size());
    for (const std::string& codec : codecs) {
        if (codec == "pcm") {
            named.emplace_back("PCM");
        } else if (codec == "flac") {
            named.emplace_back("FLAC");
        } else if (codec == "opus") {
            named.emplace_back("Opus");
        }
    }
    if (named.empty()) {
        return {};
    }
    if (named.size() == 1) {
        return named[0];
    }
    std::string out = join(std::vector<std::string>(named.begin(), named.end() - 1), ", ");
    out += " and ";
    out += named.back();
    return out;
}

// Up to two letters from `name`'s own words - "Living room" -> "LR", "Den"
// -> "D" - the same two-letter-tile idea to_row() uses for a sink's kind,
// applied to whatever the person actually named the group.
[[nodiscard]] std::string initials(std::string_view name) {
    std::string out;
    bool at_word_start = true;
    for (const char c : name) {
        if (c == ' ' || c == '\t') {
            at_word_start = true;
            continue;
        }
        if (at_word_start) {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            at_word_start = false;
            if (out.size() == 2) {
                break;
            }
        }
    }
    return out;
}

// "2 Hearth sinks · 1 Sendspin player" - member counts by kind, in the same
// kind order to_row()'s own icon switch uses, empty kinds left out.
[[nodiscard]] std::string member_counts_text(const std::vector<GroupMemberFacts>& members) {
    std::size_t hearth_sinks = 0;
    std::size_t standard_players = 0;
    std::size_t test_sinks = 0;
    for (const GroupMemberFacts& member : members) {
        switch (member.kind) {
            case SinkKind::kHearthSink: ++hearth_sinks; break;
            case SinkKind::kTestSink: ++test_sinks; break;
            case SinkKind::kStandardPlayer: default: ++standard_players; break;
        }
    }
    std::vector<std::string> parts;
    if (hearth_sinks > 0) {
        parts.push_back(fmt::format("{} Hearth sink{}", hearth_sinks, hearth_sinks == 1 ? "" : "s"));
    }
    if (standard_players > 0) {
        parts.push_back(fmt::format("{} Sendspin player{}", standard_players, standard_players == 1 ? "" : "s"));
    }
    if (test_sinks > 0) {
        parts.push_back(fmt::format("{} test sink{}", test_sinks, test_sinks == 1 ? "" : "s"));
    }
    return parts.empty() ? "No members yet" : join(parts, " · ");
}

}  // namespace

std::string_view describe(SinkKind kind) {
    switch (kind) {
        case SinkKind::kHearthSink:
            return "Hearth sink";
        case SinkKind::kTestSink:
            return "test sink";
        case SinkKind::kStandardPlayer:
        default:
            return "Sendspin player";
    }
}

SinkRow to_row(const SinkFacts& facts) {
    SinkRow row;
    row.id = facts.id;
    row.name = facts.name;
    switch (facts.kind) {
        case SinkKind::kHearthSink:
            row.icon = "HS";
            break;
        case SinkKind::kTestSink:
            row.icon = "TS";
            break;
        case SinkKind::kStandardPlayer:
        default:
            row.icon = "SP";
            break;
    }

    std::string kind_line{describe(facts.kind)};
    if (!facts.hardware.empty()) {
        kind_line += " · ";
        kind_line += facts.hardware;
    } else if (facts.output_slots.has_value()) {
        kind_line += fmt::format(" · {} slots, {}-bit", *facts.output_slots,
                                 facts.output_bit_depth.value_or(0));
    } else if (!facts.codecs.empty()) {
        kind_line += " · ";
        kind_line += codecs_text(facts.codecs);
    }
    row.subtitle = kind_line;

    switch (facts.pair_state) {
        case PairState::kPaired:
            row.badge = "paired";
            row.badge_text = "paired";
            break;
        case PairState::kNotPaired:
        default:
            row.badge = "notPaired";
            row.badge_text = "not paired";
            break;
    }
    return row;
}

SinkDetail to_detail(const SinkFacts& facts) {
    SinkDetail detail;
    detail.id = facts.id;
    detail.name = facts.name;

    std::string kind_text{describe(facts.kind)};
    if (!facts.hardware.empty()) {
        kind_text += " · ";
        kind_text += facts.hardware;
    }
    detail.kind_text = kind_text;

    switch (facts.pair_state) {
        case PairState::kPaired:
            detail.badge = "paired";
            break;
        case PairState::kNotPaired:
        default:
            detail.badge = "notPaired";
            break;
    }

    if (!facts.address.empty()) {
        detail.address = facts.port != 0 ? fmt::format("{}:{}", facts.address, facts.port) : facts.address;
        if (!facts.path.empty()) {
            detail.address += " · ";
            detail.address += facts.path;
        }
    }

    detail.roles_text = join(facts.roles, " · ");

    const std::string data_types = data_types_text(facts.data_types);
    const std::string codecs = codecs_text(facts.codecs);
    if (!data_types.empty() && !codecs.empty()) {
        detail.takes_text = data_types + " · " + codecs;
    } else if (!data_types.empty()) {
        detail.takes_text = data_types;
    } else {
        detail.takes_text = codecs;
    }

    if (facts.output_slots.has_value()) {
        detail.outputs_text =
            fmt::format("{} slots at {}-bit, as it reports", *facts.output_slots,
                       facts.output_bit_depth.value_or(0));
    }

    if (facts.required_lead_time_ms.has_value()) {
        detail.latency_text = fmt::format("{} ms, as it reports", *facts.required_lead_time_ms);
    } else if (facts.pair_state == PairState::kPaired) {
        detail.latency_text = "not reported yet";
    }

    if (facts.pair_state != PairState::kPaired) {
        detail.clock_text = "not synchronised until paired";
    } else {
        detail.clock_text = facts.clock_converged ? "synchronised" : "not synchronised yet";
    }

    detail.paired_on_text = facts.paired_on;

    return detail;
}

GroupRow to_group_row(const GroupFacts& facts) {
    GroupRow row;
    row.id = facts.id;
    row.name = facts.name;
    row.icon = initials(facts.name);
    row.subtitle = member_counts_text(facts.members);
    return row;
}

GroupDetail to_group_detail(const GroupFacts& facts) {
    GroupDetail detail;
    detail.id = facts.id;
    detail.name = facts.name;
    detail.members.reserve(facts.members.size());

    std::vector<controller::Player> players;
    std::size_t connected = 0;
    std::optional<std::uint32_t> lead_time_ms;
    for (const GroupMemberFacts& member : facts.members) {
        GroupMemberRow row;
        row.sink_id = member.sink_id;
        row.name = member.name;
        switch (member.kind) {
            case SinkKind::kHearthSink:
                row.gets_text = member.connected ? "E-AC-3 · this sink's own render" : "E-AC-3 · not connected";
                break;
            case SinkKind::kTestSink:
            case SinkKind::kStandardPlayer:
            default:
                row.gets_text = member.connected ? "Stereo, this application's own mix" : "Stereo · not connected";
                break;
        }
        row.volume = member.volume;
        row.muted = member.muted;
        row.volume_supported = member.volume_supported;
        row.mute_supported = member.mute_supported;
        row.connected = member.connected;
        detail.members.push_back(std::move(row));

        if (member.connected) {
            ++connected;
            players.push_back(controller::Player{.volume = member.volume,
                                                 .muted = member.muted,
                                                 .volume_supported = member.volume_supported,
                                                 .mute_supported = member.mute_supported});
            if (member.required_lead_time_ms.has_value()) {
                lead_time_ms = std::max(lead_time_ms.value_or(0), *member.required_lead_time_ms);
            }
        }
    }
    detail.group_volume = controller::group_volume(players);
    detail.group_muted = controller::group_muted(players);
    detail.members_connected_text = fmt::format("{} of {} connected", connected, facts.members.size());
    detail.lead_time_text =
        lead_time_ms.has_value() ? fmt::format("{} ms · the largest a member asks for", *lead_time_ms)
                                 : (facts.members.empty() ? std::string() : "not reported yet");

    return detail;
}

}  // namespace ac3::hearth

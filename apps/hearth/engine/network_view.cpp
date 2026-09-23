#include "network_view.hpp"

#include <fmt/format.h>

namespace ac3::hearth {

namespace {

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

}  // namespace ac3::hearth

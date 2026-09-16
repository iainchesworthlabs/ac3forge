#include "diagnostics_report.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "decoder_settings.hpp"
#include "output_decision.hpp"
#include "player.hpp"
#include "queue.hpp"
#include "transport.hpp"

// See diagnostics_report.hpp.

namespace ac3::hearth {

namespace {

[[nodiscard]] std::string_view on_off(bool on) {
    return on ? "on" : "off";
}

[[nodiscard]] std::string or_none(std::string_view text) {
    return text.empty() ? std::string{"(none)"} : std::string{text};
}

// A value kept to one row of the file: a title or a reason can hold a line
// break.
[[nodiscard]] std::string flat(std::string text) {
    for (char& c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20U || byte == 0x7FU) {
            c = ' ';
        }
    }
    return text;
}

[[nodiscard]] bool withheld_setting(std::string_view key) {
    return std::ranges::any_of(kWithheldSettings,
                               [key](std::string_view prefix) { return key.starts_with(prefix); });
}

}  // namespace

std::string render_report(const ReportFacts& facts, const EngineStatus& engine,
                          const DiagnosticLog& log, const Secrets& secrets) {
    std::string out;
    const auto line = [&out](std::string_view text) {
        out += text;
        out += '\n';
    };
    const auto row = [&out](std::string_view name, std::string_view value) {
        out += name;
        out += ": ";
        out += value;
        out += '\n';
    };

    line("AC3Forge Hearth diagnostics");
    row("written", facts.written_at);
    row("log started", facts.log_started_at);

    line("");
    line("# version");
    line(facts.version);

    line("");
    line("# platform");
    for (const auto& [name, value] : facts.platform) {
        row(name, flat(value));
    }

    line("");
    line("# output");
    row("chosen", facts.output_name.empty() ? std::string{"(none)"}
                                            : fmt::format("\"{}\"", flat(facts.output_name)));
    row("reason", flat(or_none(facts.output_reason)));
    const OpenOutputFormat& open = engine.output;
    row("open", open.sample_rate == 0
                    ? std::string{"no"}
                    : fmt::format("{}, {} Hz, {} channels", describe(open.mode), open.sample_rate,
                                  open.channels));
    row("times opened", std::to_string(engine.output_opens));

    // Named fields only: EngineStatus::note and ::error are never read here.
    line("");
    line("# playback");
    row("state", describe(engine.state));
    row("gapless", on_off(engine.gapless));
    row("repeat", on_off(engine.repeat));
    row("queue", fmt::format("{} items, {}", engine.queue.size(),
                             engine.current == Queue::kNone
                                 ? std::string{"none current"}
                                 : fmt::format("item {} current", engine.current + 1)));
    row("decoder", describe(engine.settings));

    // Titles and reasons; an item's path is read below only to withhold it.
    std::vector<std::string> unplayable;
    std::size_t unplayable_count = 0;
    for (std::size_t i = 0; i < engine.queue.size(); ++i) {
        const QueueItem& item = engine.queue[i];
        if (item.playable()) {
            continue;
        }
        ++unplayable_count;
        if (unplayable.size() < kReportListLimit) {
            unplayable.push_back(flat(
                fmt::format("{}: {}", describe_item(i, item.title), item.facts.unplayable_because)));
        }
    }
    line("");
    line(fmt::format("# items that cannot be played ({})", unplayable_count));
    if (unplayable.empty()) {
        line("(none)");
    }
    for (const std::string& entry : unplayable) {
        line(entry);
    }
    if (unplayable_count > unplayable.size()) {
        line(fmt::format("... and {} more", unplayable_count - unplayable.size()));
    }

    const std::size_t played = engine.history.size();
    const std::size_t first = played > kReportListLimit ? played - kReportListLimit : 0;
    line("");
    line(fmt::format("# played (oldest first, {} of {})", played - first, played));
    if (played == 0) {
        line("(none)");
    }
    for (std::size_t k = first; k < played; ++k) {
        const PlayedItem& item = engine.history[k];
        line(flat(fmt::format("{}: {} of {} frames, from frame {} of output open {}",
                              describe_item(item.queue_index, item.title), item.frames,
                              item.expected_frames, item.first_frame, item.output_opens)));
    }

    // A list the caller chose, with the rule applied again here, so a caller
    // that forgets it cannot leak through this section.
    line("");
    line("# settings");
    if (facts.settings.empty()) {
        line("(none)");
    }
    for (const auto& [key, value] : facts.settings) {
        line(flat(key + " = " + (withheld_setting(key) ? std::string{kWithheld} : value)));
    }

    line("");
    const auto notes = log.lines();
    line(fmt::format("# recent messages (oldest first, {} of {}; {} dropped)", notes.size(),
                     log.capacity(), log.dropped()));
    for (const std::string& note : notes) {
        line(note);
    }

    Secrets withheld = secrets;
    for (const QueueItem& item : engine.queue) {
        withhold_path(withheld, item.path);
    }
    return scrub(std::move(out), withheld);
}

}  // namespace ac3::hearth

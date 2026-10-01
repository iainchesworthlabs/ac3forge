#include "ac4_presentations.hpp"

#include <fmt/format.h>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "ac4_channels.hpp"

namespace forge_gui {

std::string ac4_speaker_names(std::span<const iclforge::ac4::Speaker> speakers) {
    std::string out;
    for (const iclforge::ac4::Speaker speaker : speakers) {
        if (!out.empty()) {
            out += ' ';
        }
        out += iclforge::eac3::chanmap::name(iclforge::apps::ac4_location(speaker));
    }
    return out;
}

std::string ac4_presentation_label(const iclforge::ac4::PresentationInfo& info) {
    std::string label = fmt::format("{}: {}", info.index,
                                    info.speakers.empty() ? std::string{"-"}
                                                          : ac4_speaker_names(info.speakers));
    if (!info.language.empty()) {
        label += fmt::format(", {}", info.language);
    }
    if (info.presentation_id) {
        label += fmt::format(", id {}", *info.presentation_id);
    }
    return label;
}

std::vector<Ac4PresentationRow> ac4_presentation_rows(
    std::span<const iclforge::ac4::SyncFrame> frames) {
    iclforge::ac4::Decoder decoder;
    for (const iclforge::ac4::SyncFrame& frame : frames) {
        if (!decoder.parse(frame.raw_ac4_frame).has_value()) {
            continue;
        }
        const std::span<const iclforge::ac4::PresentationInfo> presentations =
            decoder.presentations();
        if (presentations.empty()) {
            continue;
        }
        std::vector<Ac4PresentationRow> rows;
        rows.reserve(presentations.size());
        for (const iclforge::ac4::PresentationInfo& info : presentations) {
            rows.push_back(Ac4PresentationRow{.index = info.index,
                                              .label = ac4_presentation_label(info),
                                              .decodable = info.decodable});
        }
        return rows;
    }
    return {};
}

}  // namespace forge_gui

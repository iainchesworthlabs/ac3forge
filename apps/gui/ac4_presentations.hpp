#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "iclforge/ac4/ac4.hpp"
#include "iclforge/ac4dec/decoder.hpp"

// What the AC-4 pages show of a stream's presentations, Qt-free so iclforge-tests
// holds it: each presentation of the table of contents, by its position,
// which is what `forge ... presentation=` takes, and a label made of what the
// decoder reports of it (its channels as coded, its language and its
// presentation_id), in the stream's own terms rather than words the page would
// have to translate.

namespace forge_gui {

struct Ac4PresentationRow {
    std::size_t index = 0;
    std::string label;
    bool decodable = false;
};

// "0: L R C LFE Ls Rs, en, id 3" - the position, the channels, and the
// language and presentation_id where the stream sends them.
[[nodiscard]] std::string ac4_presentation_label(const iclforge::ac4::PresentationInfo& info);

// The presentations the first frame that parses reports; empty where none
// does.
[[nodiscard]] std::vector<Ac4PresentationRow> ac4_presentation_rows(
    std::span<const iclforge::ac4::SyncFrame> frames);

// The channel names forge-gui shows for AC-4 speakers (A/52 Table E2.5's short
// names, as the player's meters name them).
[[nodiscard]] std::string ac4_speaker_names(std::span<const iclforge::ac4::Speaker> speakers);

}  // namespace forge_gui

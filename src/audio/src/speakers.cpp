#include "ac3/audio/speakers.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/core/eac3_tables.hpp"

namespace ac3::audio {

namespace {

namespace chanmap = ac3::eac3::chanmap;

struct Position {
    std::uint32_t speaker;
    Location location;
};

// Ascending by bit, which is the order an interleaved stream with a given mask
// carries its channels in. SPEAKER_BACK_LEFT and SPEAKER_BACK_RIGHT are listed
// as the surrounds of a 5.1 ring; locations_of() turns them into the rear
// surrounds when the mask names the sides as well. SPEAKER_TOP_BACK_CENTRE has
// no entry: Table E2.5 has no position above and behind the listener.
constexpr std::array<Position, 17> kPositions{{
    {kSpeakerFrontLeft, Location::kLeft},
    {kSpeakerFrontRight, Location::kRight},
    {kSpeakerFrontCentre, Location::kCentre},
    {kSpeakerLowFrequency, Location::kLfe},
    {kSpeakerBackLeft, Location::kLeftSurround},
    {kSpeakerBackRight, Location::kRightSurround},
    {kSpeakerFrontLeftOfCentre, Location::kLc},
    {kSpeakerFrontRightOfCentre, Location::kRc},
    {kSpeakerBackCentre, Location::kCs},
    {kSpeakerSideLeft, Location::kLeftSurround},
    {kSpeakerSideRight, Location::kRightSurround},
    {kSpeakerTopCentre, Location::kTs},
    {kSpeakerTopFrontLeft, Location::kVhl},
    {kSpeakerTopFrontCentre, Location::kVhc},
    {kSpeakerTopFrontRight, Location::kVhr},
    {kSpeakerTopBackLeft, Location::kLts},
    {kSpeakerTopBackRight, Location::kRts},
}};

[[nodiscard]] bool names_sides(std::uint32_t mask) {
    return (mask & (kSpeakerSideLeft | kSpeakerSideRight)) != 0;
}

[[nodiscard]] bool holds(std::span<const Location> locations, Location location) {
    return std::find(locations.begin(), locations.end(), location) != locations.end();
}

struct NamedPosition {
    std::uint32_t speaker;
    std::string_view name;
};

// WAVEFORMATEXTENSIBLE's own bit order, each with the abbreviation that names
// it - unlike kPositions above, there is no sides-vs-backs ambiguity here: an
// OUTPUT's own name does not depend on the company its mask keeps, only on
// which bit it is.
constexpr std::array<NamedPosition, 18> kNamedPositions{{
    {kSpeakerFrontLeft, "FL"},
    {kSpeakerFrontRight, "FR"},
    {kSpeakerFrontCentre, "FC"},
    {kSpeakerLowFrequency, "LFE"},
    {kSpeakerBackLeft, "BL"},
    {kSpeakerBackRight, "BR"},
    {kSpeakerFrontLeftOfCentre, "FLC"},
    {kSpeakerFrontRightOfCentre, "FRC"},
    {kSpeakerBackCentre, "BC"},
    {kSpeakerSideLeft, "SL"},
    {kSpeakerSideRight, "SR"},
    {kSpeakerTopCentre, "TC"},
    {kSpeakerTopFrontLeft, "TFL"},
    {kSpeakerTopFrontCentre, "TFC"},
    {kSpeakerTopFrontRight, "TFR"},
    {kSpeakerTopBackLeft, "TBL"},
    {kSpeakerTopBackCentre, "TBC"},
    {kSpeakerTopBackRight, "TBR"},
}};

}  // namespace

std::uint16_t speaker_count(std::uint32_t mask) {
    return static_cast<std::uint16_t>(std::popcount(mask));
}

std::optional<Location> location_of(std::uint32_t speaker) {
    if (!std::has_single_bit(speaker)) {
        return std::nullopt;
    }
    const auto found = std::find_if(kPositions.begin(), kPositions.end(),
                                    [&](const Position& position) { return position.speaker == speaker; });
    if (found == kPositions.end()) {
        return std::nullopt;
    }
    return found->location;
}

std::uint32_t speaker_of(Location location) {
    const auto found = std::find_if(kPositions.begin(), kPositions.end(),
                                    [&](const Position& position) { return position.location == location; });
    if (found != kPositions.end()) {
        return found->speaker;
    }
    // The rear surrounds share the back pair's bits with the 5.1 surrounds; a
    // mask that carries both resolves them in speakers_of().
    if (location == Location::kLrs) {
        return kSpeakerBackLeft;
    }
    if (location == Location::kRrs) {
        return kSpeakerBackRight;
    }
    return 0;
}

std::vector<Location> locations_of(std::uint32_t mask) {
    const bool rears = names_sides(mask);
    std::vector<Location> locations;
    locations.reserve(speaker_count(mask & kSpeakerAllPositions));
    for (const Position& position : kPositions) {
        if ((mask & position.speaker) == 0) {
            continue;
        }
        if (rears && position.speaker == kSpeakerBackLeft) {
            locations.push_back(Location::kLrs);
        } else if (rears && position.speaker == kSpeakerBackRight) {
            locations.push_back(Location::kRrs);
        } else {
            locations.push_back(position.location);
        }
    }
    return locations;
}

std::uint32_t speakers_of(std::span<const Location> locations) {
    const bool rears = holds(locations, Location::kLrs) || holds(locations, Location::kRrs);
    std::uint32_t mask = 0;
    for (const Location location : locations) {
        std::uint32_t speaker = speaker_of(location);
        if (rears && location == Location::kLeftSurround) {
            speaker = kSpeakerSideLeft;
        } else if (rears && location == Location::kRightSurround) {
            speaker = kSpeakerSideRight;
        }
        mask |= speaker;
    }
    return mask;
}

std::uint32_t default_speakers(std::uint16_t channels) {
    // Only the widths one arrangement fits. Ten channels is 5.1.4 or 7.1.2 and
    // fourteen is 7.1.6 or 9.1.4; a device that does not say which cannot be
    // guessed at, and saying nothing is the honest answer.
    switch (channels) {
        case 1: return kSpeakersMono;
        case 2: return kSpeakersStereo;
        case 4: return kSpeakersQuad;
        case 6: return kSpeakers5_1;
        case 8: return kSpeakers7_1;
        case 12: return kSpeakers7_1_4;
        default: return 0;
    }
}

std::string describe_speakers(std::uint32_t mask) {
    std::string text;
    for (const Location location : locations_of(mask)) {
        if (!text.empty()) {
            text.push_back(' ');
        }
        text += chanmap::name(location);
    }
    const std::uint16_t named = speaker_count(mask & kSpeakerAllPositions);
    const auto placed = static_cast<std::uint16_t>(locations_of(mask).size());
    if (named > placed) {
        if (!text.empty()) {
            text.push_back(' ');
        }
        text += "+" + std::to_string(named - placed);
    }
    return text;
}

std::optional<std::string> mask_position_name(std::uint32_t speaker) {
    if (!std::has_single_bit(speaker)) {
        return std::nullopt;
    }
    const auto found =
        std::find_if(kNamedPositions.begin(), kNamedPositions.end(),
                     [&](const NamedPosition& position) { return position.speaker == speaker; });
    if (found == kNamedPositions.end()) {
        return std::nullopt;
    }
    return std::string(found->name);
}

std::vector<std::string> output_names(std::uint32_t mask, std::uint16_t outputs) {
    std::vector<std::string> names(outputs);
    const std::uint32_t effective = mask != 0 ? mask : default_speakers(outputs);
    if (effective == 0) {
        return names;
    }
    std::size_t index = 0;
    for (const NamedPosition& position : kNamedPositions) {
        if (index >= names.size()) {
            break;
        }
        if ((effective & position.speaker) == 0) {
            continue;
        }
        names[index] = std::string(position.name);
        ++index;
    }
    return names;
}

}  // namespace ac3::audio

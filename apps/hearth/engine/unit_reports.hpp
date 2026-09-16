#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "stream_decoder.hpp"

// What the unit playing now said about itself (planning/hearth-reference-
// player.md, Media information: "per frame from DecodedFrame", and the
// Monitor's object view), released when the device's clock reaches the unit.
//
// The same arrangement as the meters (play_meters.hpp): the player keeps each
// unit's report, stamped with the output frame its first played frame lands
// on, and hands the newest one out once the clock - less the output path's
// delay - has passed it.

namespace ac3::hearth {

class UnitReports {
public:
    // A unit whose first played frame the output plays at `output_frame`.
    // Reports come in stream order.
    void add(const UnitReport& report, std::uint64_t output_frame);

    // The output was flushed or reopened: every report not yet released goes,
    // since its unit will not be heard.
    void clear();

    // Releases every report whose frame `heard` has passed and copies the
    // latest of them into `latest`, whose storage is reused. False, leaving
    // `latest` alone, when the clock has passed none since the last call.
    [[nodiscard]] bool release(std::uint64_t heard, UnitReport& latest);

    // Reports kept and not yet released.
    [[nodiscard]] std::size_t queued() const { return count_; }

private:
    struct Entry {
        std::uint64_t output_frame = 0;
        UnitReport report{};
    };

    // Oldest first: a ring whose entries keep their storage, so steady
    // playback allocates nothing for the reports.
    std::vector<Entry> ring_;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

}  // namespace ac3::hearth

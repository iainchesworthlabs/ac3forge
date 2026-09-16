#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ac3/analysis/levels.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/render/layout.hpp"

// Meters on what the output plays (planning/hearth-reference-player.md,
// Monitor): a level meter per output slot - peak, hold, RMS and a clip latch,
// ac3::analysis::LevelMeter's - and the programme's loudness from
// ac3::meta::LoudnessMeter, measured as the blocks are rendered and released
// only when the device's clock reaches them.
//
// The GUI's stream player meters each chunk as it is queued, so its meters run
// ahead of the sound by however much the device holds. Here each snapshot is
// stamped with the output frame its audio ends on, counted the way the
// device's clock counts, and a snapshot is only handed out once the clock -
// less the output path's delay - has passed that frame.
//
// Loudness is measured over the slots that name a Table E2.5 location, in
// BS.1770-5 Annex 3's weighting, the LFE excluded; a slot placed by angle
// alone has no location to weight and is left out of it.
//
// Each item has a loudness meter of its own, so its integrated loudness,
// loudness range and true peak are the item's alone. Momentary and short-term
// loudness describe what was just heard, so they run on through a gapless
// join: for a new item's first three seconds, the short-term window, they are
// read from the meter of the item before, which is fed the new item too. A
// flush or a reopen starts every meter again, since what they hold will not
// be heard.
//
// The library works integrated loudness and loudness range out over the whole
// programme at each read, at a cost that grows with the programme's length, so
// they are read once a second of audio and each snapshot carries the latest.

namespace ac3::hearth {

struct MeterSnapshot {
    // The output frame the audio this describes ends on, counted since the
    // output was last opened or flushed.
    std::uint64_t output_frame = 0;
    // One per output slot, in slot order.
    std::vector<analysis::ChannelLevel> levels{};
    std::optional<double> momentary_lkfs = std::nullopt;
    std::optional<double> short_term_lkfs = std::nullopt;
    std::optional<double> integrated_lkfs = std::nullopt;
    std::optional<double> loudness_range = std::nullopt;
    std::optional<double> true_peak_dbtp = std::nullopt;
};

class PlayMeters {
public:
    // A snapshot every `interval` frames: 50 ms at 48 kHz by default.
    PlayMeters(const render::OutputLayout& layout, std::uint32_t sample_rate,
               std::size_t interval = 2400);

    // A rendered block, one span per slot, whose last frame the output plays
    // just before `output_end`. Meters it, and keeps a snapshot each time
    // another interval has been metered.
    void meter(std::span<const std::span<const float>> slots, std::size_t frames,
               std::uint64_t output_end);

    // A new item follows without a break: integrated loudness, loudness range
    // and true peak start again. The levels and their ballistics run on, and
    // so do momentary and short-term loudness.
    void restart_programme();

    // The output was flushed or reopened: every snapshot not yet released
    // goes, since its audio never will be heard, and every meter starts again.
    void restart_timeline();

    // Releases every snapshot whose frame `heard` has reached and copies the
    // latest of them into `latest`, whose storage is reused. False, leaving
    // `latest` alone, when the clock has reached none since the last call.
    [[nodiscard]] bool release(std::uint64_t heard, MeterSnapshot& latest);

    [[nodiscard]] std::uint32_t sample_rate() const { return rate_; }
    // Snapshots taken and not yet released.
    [[nodiscard]] std::size_t queued() const { return count_; }
    // Whether the meter of the item before is still being fed, as it is for a
    // new item's first three seconds.
    [[nodiscard]] bool bridging() const { return outgoing_.has_value(); }

private:
    void take_snapshot(std::uint64_t output_frame);
    [[nodiscard]] meta::LoudnessMeter make_loudness() const;

    std::uint32_t rate_;
    std::size_t interval_;
    std::size_t slots_;
    analysis::LevelMeter levels_;
    // The slots that name a location, in slot order, and that layout.
    std::vector<std::size_t> loudness_slots_;
    eac3::chanmap::Layout loudness_layout_{};
    std::vector<std::span<const float>> loudness_views_;
    // This item's loudness, and the item before's while its windows still
    // reach back further than this one's.
    std::optional<meta::LoudnessMeter> loudness_;
    std::optional<meta::LoudnessMeter> outgoing_;
    // Frames this item's meter has been fed, and fed since integrated
    // loudness and loudness range were last read, with that reading.
    std::uint64_t programme_frames_ = 0;
    std::uint64_t since_programme_read_ = 0;
    std::optional<double> integrated_lkfs_ = std::nullopt;
    std::optional<double> loudness_range_ = std::nullopt;
    std::size_t since_snapshot_ = 0;
    // Snapshots waiting for the clock, oldest first: a ring whose entries keep
    // their level vectors, so steady playback allocates nothing for them.
    std::vector<MeterSnapshot> ring_;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

}  // namespace ac3::hearth

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ac3/audio/passthrough.hpp"

// The play queue (planning/hearth-reference-player.md, Media): the items the
// player was given, in the order they will play, and which one is current.
//
// Deliberately a plain container with no I/O in it. An item arrives as a path
// and gains what a probe found out about it - format, width, rate, duration -
// and nothing here reads a file, so the whole of the queue's behaviour is
// testable on any machine. The probe itself belongs to the session that
// opens the item.
//
// What "current" means when the queue is empty, when the current item is
// removed under the transport's feet, and when a folder's worth of items is
// added mid-play are all decided here rather than in the transport, because
// they are questions about a list rather than about playback.

namespace ac3::hearth {

// What a probe found out about one item. Every field is optional in effect:
// an item that has not been probed yet has zeros, and the transport treats
// that as "not known yet" rather than as a format.
struct ItemFacts {
    // What it carries, and nullopt for anything IEC 61937 cannot wrap - a
    // WAV, or an AC-4 stream, which is listed but not playable until chip D.
    std::optional<audio::BitstreamFormat> stream = std::nullopt;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    bool has_objects = false;
    // From the access-unit count and the samples each one carries - NOT a
    // fixed 1536, which is wrong for an E-AC-3 frame with fewer than six
    // blocks (the plan's own note on apps/gui/stream_player_controller.cpp).
    std::optional<std::chrono::milliseconds> duration = std::nullopt;
    // Set when the item was recognised but cannot be played here: AC-4
    // today. It stays in the queue and is skipped, with the reason shown.
    std::string unplayable_because{};
};

struct QueueItem {
    std::string path{};
    // What to show. The file's own name until metadata has been read.
    std::string title{};
    ItemFacts facts{};

    [[nodiscard]] bool playable() const { return facts.unplayable_because.empty(); }
};

class Queue {
public:
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    [[nodiscard]] std::size_t size() const { return items_.size(); }
    [[nodiscard]] bool empty() const { return items_.empty(); }
    [[nodiscard]] std::span<const QueueItem> items() const { return items_; }

    // The item playing, or kNone when the queue is empty or has been stopped
    // past its end.
    [[nodiscard]] std::size_t current_index() const { return current_; }
    [[nodiscard]] const QueueItem* current() const {
        return current_ < items_.size() ? &items_[current_] : nullptr;
    }
    [[nodiscard]] QueueItem* current_mutable() {
        return current_ < items_.size() ? &items_[current_] : nullptr;
    }

    // Appends, and makes the first item added to an empty queue the current
    // one so that a play command has somewhere to start.
    void add(QueueItem item);
    void add(std::span<const QueueItem> items);

    // Inserts before `index` (clamped to the end). The current item stays the
    // same item, whatever its index becomes.
    void insert(std::size_t index, QueueItem item);

    // Removes `index`. Removing the current item leaves the NEXT item
    // current - which is what a person deleting the playing track expects -
    // and removing the last item leaves kNone. Returns whether the current
    // item changed, since the transport has to restart playback if it did.
    bool remove(std::size_t index);

    void clear();

    // Moves one item, for a drag in the queue list. The current item follows
    // its own move. False, changing nothing, for an index out of range.
    bool move(std::size_t from, std::size_t to);

    // Makes `index` current without saying anything about playback: the
    // transport decides what that means. False for an index out of range.
    bool set_current(std::size_t index);

    // The next and previous PLAYABLE items, skipping any the probe marked
    // unplayable, or kNone at either end. `repeat` makes the ends meet.
    [[nodiscard]] std::size_t next_index(bool repeat = false) const;
    [[nodiscard]] std::size_t previous_index(bool repeat = false) const;

    // Facts for an item, as a probe learns them.
    bool set_facts(std::size_t index, ItemFacts facts);

private:
    std::vector<QueueItem> items_;
    std::size_t current_ = kNone;
};

}  // namespace ac3::hearth

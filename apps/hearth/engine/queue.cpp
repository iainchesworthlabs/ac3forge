#include "queue.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

// See queue.hpp. The only judgements in here are what happens to "current"
// when the list changes underneath it, and each is a case in
// tests/hearth/test_queue.cpp.

namespace ac3::hearth {

void Queue::add(QueueItem item) {
    items_.push_back(std::move(item));
    if (current_ == kNone) {
        current_ = items_.size() - 1;
    }
}

void Queue::add(std::span<const QueueItem> items) {
    for (const auto& item : items) {
        add(item);
    }
}

void Queue::insert(std::size_t index, QueueItem item) {
    const std::size_t at = std::min(index, items_.size());
    items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(at), std::move(item));
    if (current_ == kNone) {
        current_ = at;
    } else if (at <= current_) {
        // The same item is still current; only its index moved along.
        ++current_;
    }
}

bool Queue::remove(std::size_t index) {
    if (index >= items_.size()) {
        return false;
    }
    const bool was_current = index == current_;
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(index));
    if (items_.empty()) {
        current_ = kNone;
        return was_current;
    }
    if (index < current_) {
        --current_;
    } else if (was_current) {
        // The next item takes the removed one's place, and removing the last
        // item leaves nothing playing rather than wrapping to the front.
        current_ = index < items_.size() ? index : kNone;
    }
    return was_current;
}

void Queue::clear() {
    items_.clear();
    current_ = kNone;
}

bool Queue::move(std::size_t from, std::size_t to) {
    if (from >= items_.size() || to >= items_.size()) {
        return false;
    }
    if (from == to) {
        return true;
    }
    QueueItem held = std::move(items_[from]);
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(from));
    items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(to), std::move(held));
    if (current_ == from) {
        current_ = to;
    } else if (from < current_ && to >= current_) {
        --current_;
    } else if (from > current_ && to <= current_) {
        ++current_;
    }
    return true;
}

bool Queue::set_current(std::size_t index) {
    if (index >= items_.size()) {
        return false;
    }
    current_ = index;
    return true;
}

std::size_t Queue::next_index(bool repeat) const {
    if (items_.empty()) {
        return kNone;
    }
    const std::size_t from = current_ == kNone ? 0 : current_ + 1;
    for (std::size_t i = from; i < items_.size(); ++i) {
        if (items_[i].playable()) {
            return i;
        }
    }
    if (!repeat) {
        return kNone;
    }
    // Round the end, and stop at the current item rather than looping for
    // ever through a queue whose every other item is unplayable.
    for (std::size_t i = 0; i < items_.size() && i <= current_; ++i) {
        if (items_[i].playable()) {
            return i;
        }
    }
    return kNone;
}

std::size_t Queue::previous_index(bool repeat) const {
    if (items_.empty()) {
        return kNone;
    }
    if (current_ != kNone && current_ > 0) {
        for (std::size_t i = current_; i-- > 0;) {
            if (items_[i].playable()) {
                return i;
            }
        }
    }
    if (!repeat) {
        return kNone;
    }
    for (std::size_t i = items_.size(); i-- > 0;) {
        if (items_[i].playable() && (current_ == kNone || i >= current_)) {
            return i;
        }
    }
    return kNone;
}

bool Queue::set_facts(std::size_t index, ItemFacts facts) {
    if (index >= items_.size()) {
        return false;
    }
    items_[index].facts = std::move(facts);
    return true;
}

std::string describe_item(std::size_t index, std::string_view title) {
    if (index == Queue::kNone && title.empty()) {
        return "an item no longer in the queue";
    }
    std::string out;
    if (index != Queue::kNone) {
        out = "item " + std::to_string(index + 1) + " ";
    }
    out += '"';
    out += title;
    out += '"';
    if (index == Queue::kNone) {
        out += " (no longer in the queue)";
    }
    return out;
}

}  // namespace ac3::hearth

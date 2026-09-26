#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// The console's last bytes, kept for GET /log (planning/esp32-ota.md, O4): a
// ring over storage the owner gives it. `written` counts every byte ever put,
// so a reader asks for what came after the count it last saw, and learns when
// the ring has moved past what it asked for.
//
// Free of ESP-IDF, like firmware_image.hpp beside it: log.cpp feeds it the
// console's writes under its own lock. tests/io/test_log_ring.cpp runs it on a
// laptop.

namespace ac3forge {

class LogRing {
   public:
    explicit LogRing(std::span<char> storage) : storage_(storage) {}

    // Appends, overwriting the oldest bytes once the ring is full.
    void put(std::string_view text) {
        const std::size_t size = storage_.size();
        if (size == 0) {
            written_ += text.size();
            return;
        }
        // Only the last `size` bytes of a long write can be kept.
        if (text.size() > size) {
            written_ += text.size() - size;
            text.remove_prefix(text.size() - size);
        }
        auto at = static_cast<std::size_t>(written_ % size);
        const std::size_t first = std::min(text.size(), size - at);
        std::copy_n(text.data(), first, storage_.data() + at);
        std::copy_n(text.data() + first, text.size() - first, storage_.data());
        written_ += text.size();
    }

    struct Read {
        std::string text;
        std::uint64_t from = 0;  // where `text` starts: past `from` asked for when bytes were lost
        std::uint64_t next = 0;  // the count to ask from next
    };

    // What was written from byte `from` on, as much of it as the ring still
    // holds, and at most `limit` bytes of it.
    [[nodiscard]] Read read(std::uint64_t from, std::size_t limit) const {
        Read out;
        out.text.resize(std::min(limit, storage_.size()));
        const Copied copied = copy(from, out.text);
        out.text.resize(copied.size);
        out.from = copied.from;
        out.next = copied.next;
        return out;
    }

    struct Copied {
        std::uint64_t from = 0;
        std::uint64_t next = 0;
        std::size_t size = 0;
    };

    // read() into storage the caller has: what log.cpp calls under its lock,
    // having allocated outside it.
    Copied copy(std::uint64_t from, std::span<char> out) const {
        Copied c;
        const std::uint64_t oldest = written_ > storage_.size() ? written_ - storage_.size() : 0;
        c.from = std::clamp(from, oldest, written_);
        c.size = static_cast<std::size_t>(std::min<std::uint64_t>(written_ - c.from, out.size()));
        c.next = c.from + c.size;
        if (c.size == 0) {
            return c;
        }
        const std::size_t size = storage_.size();
        auto at = static_cast<std::size_t>(c.from % size);
        const std::size_t first = std::min(c.size, size - at);
        std::copy_n(storage_.data() + at, first, out.data());
        std::copy_n(storage_.data(), c.size - first, out.data() + first);
        return c;
    }

    [[nodiscard]] std::uint64_t written() const { return written_; }
    [[nodiscard]] std::size_t capacity() const { return storage_.size(); }

   private:
    std::span<char> storage_;
    std::uint64_t written_ = 0;
};

}  // namespace ac3forge

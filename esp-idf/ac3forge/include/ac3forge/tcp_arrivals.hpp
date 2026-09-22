#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

// When the bytes of a TCP stream reached the board, for a reader that gets to
// them later.
//
// A Sendspin player dates a clock reply by when it reads it
// (ac3::sendspin::ClockSync). Its server task reads the socket, and on a part
// with one core that task waits behind the decode: on an ESP32-C6 on
// 2026-09-17 every reply of a burst was read tens of milliseconds after it
// came while a stream played, and the clock left out all of them. lwIP's IPv4
// input hook sees each segment as the network task takes it, well above the
// decode. src/tcp_arrivals.cpp keeps an ArrivalLog for each stream to the
// watched port from that hook, and the reader, counting the bytes it has
// read, asks when the last byte of a message came.
//
// ArrivalLog is free of ESP-IDF, so tests/io/test_tcp_arrivals.cpp builds it
// on the host. The functions after it are the board's.
//
// Offsets count a stream's bytes from its first, as its reader counts them.
// The log grows only where the stream's bytes are contiguous: a segment past
// a gap cannot be read until the gap fills, so its bytes are dated by the
// segment that fills it. When the log has dropped entries, or missed
// segments, bytes are dated by a later entry or not at all. No error makes a
// date earlier than the bytes came, so a clock sample it dates can only look
// slower than it was, which the clock's filter is built to leave out.

namespace ac3forge {

class ArrivalLog {
   public:
    // Segments a log holds while its reader is behind. A board's TCP window
    // (4 segments at the default 5,760 bytes) bounds how many are waiting.
    static constexpr std::size_t kCapacity = 32;

    // A new stream, whose first byte has the sequence number `first`: the
    // initial sequence number in its SYN, plus one.
    void start(std::uint32_t first) {
        first_ = first;
        contiguous_ = 0;
        head_ = 0;
        count_ = 0;
        started_ = true;
    }

    // A segment carrying `length` bytes from sequence number `sequence`,
    // taken at `at_us`.
    void segment(std::uint32_t sequence, std::size_t length, std::int64_t at_us) {
        if (!started_ || length == 0) {
            return;
        }
        // Where it starts against the contiguous end, within 2^31 either
        // way, so a stream longer than 4 GB still counts on.
        const auto distance =
            static_cast<std::int32_t>(sequence - first_ - static_cast<std::uint32_t>(contiguous_));
        if (distance > 0) {
            return;  // past a gap
        }
        const std::int64_t start = static_cast<std::int64_t>(contiguous_) + distance;
        if (start < 0) {
            return;  // before the stream began
        }
        const std::uint64_t end = static_cast<std::uint64_t>(start) + length;
        if (end <= contiguous_) {
            return;  // bytes already here: a retransmission
        }
        contiguous_ = end;
        if (count_ == kCapacity) {
            head_ = (head_ + 1) % kCapacity;
            --count_;
        }
        entries_[(head_ + count_) % kCapacity] = Entry{.end = end, .at_us = at_us};
        ++count_;
    }

    // When the stream's first `end` bytes had all come: the time of the
    // earliest segment that completed them. Entries before it are dropped,
    // as the reader has read past them. Nothing when no entry covers `end`:
    // the reader has bytes the log never saw, and the log carries on from
    // there.
    [[nodiscard]] std::optional<std::int64_t> arrival(std::uint64_t end) {
        if (!started_ || end == 0) {
            return std::nullopt;
        }
        if (end > contiguous_) {
            contiguous_ = end;
            count_ = 0;
            return std::nullopt;
        }
        while (count_ > 0) {
            const Entry& entry = entries_[head_];
            if (entry.end >= end) {
                return entry.at_us;
            }
            head_ = (head_ + 1) % kCapacity;
            --count_;
        }
        return std::nullopt;
    }

    [[nodiscard]] bool started() const { return started_; }
    [[nodiscard]] std::uint64_t contiguous() const { return contiguous_; }

   private:
    struct Entry {
        std::uint64_t end = 0;
        std::int64_t at_us = 0;
    };
    std::array<Entry, kCapacity> entries_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    std::uint32_t first_ = 0;
    std::uint64_t contiguous_ = 0;
    bool started_ = false;
};

namespace tcp_arrivals {

// The TCP port whose streams the hook logs, or 0 for none. A stream is logged
// from its SYN, so a port is watched before its first connection comes.
void watch(std::uint16_t port);

// The logged stream on the connected socket `fd`, held for its reader until
// release(): a handle, or -1 when the hook did not see it begin, as when the
// project has not installed the hook (lwip_hooks/ac3forge_lwip_hooks.h) or
// the peer is on IPv6.
[[nodiscard]] int claim(int fd);
void release(int handle);

// ArrivalLog::arrival() for a claimed stream, in esp_timer microseconds.
[[nodiscard]] std::optional<std::int64_t> arrival(int handle, std::uint64_t end);

}  // namespace tcp_arrivals

}  // namespace ac3forge

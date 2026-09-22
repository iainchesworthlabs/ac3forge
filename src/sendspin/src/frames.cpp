#include "ac3/sendspin/frames.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace ac3::sendspin {

namespace {

constexpr std::uint8_t kFlagLast = 0x01;
constexpr std::uint8_t kFlagFirst = 0x02;
constexpr std::uint8_t kReservedFlagBits = 0xFC;

// Data bytes each form fits in its first and later frames.
struct Capacity {
    std::size_t first;
    std::size_t later;
};

[[nodiscard]] constexpr Capacity capacity(Dialect dialect) {
    if (dialect == Dialect::kSpecification) {
        return {.first = kMaxFramePlaintext - 3, .later = kMaxFramePlaintext - 2};
    }
    return {.first = kMaxFramePlaintext - 2, .later = kMaxFramePlaintext - 1};
}

[[nodiscard]] constexpr bool is_fragment_id(std::uint8_t id) {
    return id == message_id::kFragment || id == message_id::kLegacyFragment ||
           id == message_id::kLegacyFragmentLast;
}

}  // namespace

std::size_t frame_count(std::size_t message_bytes, Dialect dialect) {
    if (message_bytes <= kMaxFramePlaintext) {
        return 1;
    }
    const Capacity cap = capacity(dialect);
    const std::size_t data = message_bytes - 1;
    return 1 + ((data - cap.first) + cap.later - 1) / cap.later;
}

std::size_t write_frame(std::span<const std::uint8_t> message, std::size_t index,
                        std::span<std::uint8_t> frame, Dialect dialect) {
    if (message.empty() || frame.size() < kMaxFramePlaintext) {
        return 0;
    }
    const std::size_t frames = frame_count(message.size(), dialect);
    if (index >= frames) {
        return 0;
    }
    if (frames == 1) {
        std::memcpy(frame.data(), message.data(), message.size());
        return message.size();
    }
    const Capacity cap = capacity(dialect);
    const std::span<const std::uint8_t> data = message.subspan(1);
    const bool first = index == 0;
    const bool last = index + 1 == frames;
    const std::size_t start = first ? 0 : cap.first + ((index - 1) * cap.later);
    const std::size_t length = std::min(first ? cap.first : cap.later, data.size() - start);

    std::size_t header = 0;
    if (dialect == Dialect::kSpecification) {
        frame[header++] = message_id::kFragment;
        frame[header++] = static_cast<std::uint8_t>((first ? kFlagFirst : 0U) |
                                                    (last ? kFlagLast : 0U));
    } else {
        frame[header++] = last ? message_id::kLegacyFragmentLast : message_id::kLegacyFragment;
    }
    if (first) {
        frame[header++] = message.front();
    }
    std::memcpy(frame.data() + header, data.data() + start, length);
    return header + length;
}

std::string_view describe(FrameError error) {
    switch (error) {
        case FrameError::kNone:
            return "no error";
        case FrameError::kEmpty:
            return "a frame with no plaintext";
        case FrameError::kTruncatedFragment:
            return "a fragment too short for its header";
        case FrameError::kReservedFlags:
            return "a fragment with reserved flag bits set";
        case FrameError::kFirstWhileInFlight:
            return "a new fragmented message while another is in flight";
        case FrameError::kContinuationWithNone:
            return "a later fragment with no fragmented message in flight";
        case FrameError::kInterleaved:
            return "an unfragmented message while a fragmented one is in flight";
        case FrameError::kMixedForms:
            return "fragments of two forms in one message";
        case FrameError::kBadOriginalId:
            return "a fragment whose original message ID is a fragment ID";
        case FrameError::kTooLarge:
            return "a reassembled message larger than the receiver allows";
    }
    return "unknown error";
}

void Reassembler::reset() {
    buffer_.clear();
    state_ = State::kIdle;
    delivered_ = false;
}

Reassembler::Result Reassembler::fail(FrameError error) {
    reset();
    return {.error = error, .message = {}};
}

Reassembler::Result Reassembler::append(std::span<const std::uint8_t> data, bool last) {
    if (data.size() > max_ || buffer_.size() > max_ - data.size()) {
        return fail(FrameError::kTooLarge);
    }
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    if (!last) {
        return {};
    }
    state_ = State::kIdle;
    delivered_ = true;
    return {.error = FrameError::kNone, .message = buffer_};
}

Reassembler::Result Reassembler::push(std::span<const std::uint8_t> frame) {
    if (delivered_) {
        buffer_.clear();
        delivered_ = false;
    }
    if (frame.empty()) {
        return fail(FrameError::kEmpty);
    }
    const std::uint8_t id = frame.front();

    if (id == message_id::kFragment) {
        if (frame.size() < 2) {
            return fail(FrameError::kTruncatedFragment);
        }
        const std::uint8_t flags = frame[1];
        if ((flags & kReservedFlagBits) != 0) {
            return fail(FrameError::kReservedFlags);
        }
        const bool last = (flags & kFlagLast) != 0;
        if ((flags & kFlagFirst) != 0) {
            if (in_flight()) {
                return fail(FrameError::kFirstWhileInFlight);
            }
            if (frame.size() < 3) {
                return fail(FrameError::kTruncatedFragment);
            }
            if (is_fragment_id(frame[2])) {
                return fail(FrameError::kBadOriginalId);
            }
            state_ = State::kSpecification;
            return append(frame.subspan(2), last);
        }
        if (state_ == State::kIdle) {
            return fail(FrameError::kContinuationWithNone);
        }
        if (state_ != State::kSpecification) {
            return fail(FrameError::kMixedForms);
        }
        return append(frame.subspan(2), last);
    }

    if (id == message_id::kLegacyFragment || id == message_id::kLegacyFragmentLast) {
        const bool last = id == message_id::kLegacyFragmentLast;
        if (state_ == State::kIdle) {
            if (last) {
                return fail(FrameError::kContinuationWithNone);
            }
            if (frame.size() < 2) {
                return fail(FrameError::kTruncatedFragment);
            }
            if (is_fragment_id(frame[1])) {
                return fail(FrameError::kBadOriginalId);
            }
            state_ = State::kAiosendspin911;
            return append(frame.subspan(1), false);
        }
        if (state_ != State::kAiosendspin911) {
            return fail(FrameError::kMixedForms);
        }
        return append(frame.subspan(1), last);
    }

    if (in_flight()) {
        return fail(FrameError::kInterleaved);
    }
    if (frame.size() > max_) {
        return fail(FrameError::kTooLarge);
    }
    return {.error = FrameError::kNone, .message = frame};
}

}  // namespace ac3::sendspin

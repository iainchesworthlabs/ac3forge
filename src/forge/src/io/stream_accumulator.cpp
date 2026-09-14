#include "ac3/io/stream_accumulator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/io/elementary.hpp"

namespace ac3::io {

namespace {

// Enough to see a sync word and know there is something after it. Anything
// shorter cannot be classified at all, so it is always "feed me more".
constexpr std::size_t kMinimumHeaderBytes = 6;

[[nodiscard]] bool starts_with_sync(std::span<const std::byte> at) {
    return at.size() >= 2 && static_cast<std::uint8_t>(at[0]) == (kSyncWord >> 8) &&
           static_cast<std::uint8_t>(at[1]) == (kSyncWord & 0xFF);
}

// §E3.8.2's grouping rule, the same one split_access_units applies: an access
// unit begins at every AC-3 syncframe and at every Annex E INDEPENDENT
// substream. A dependent joins the unit ahead of it rather than starting one -
// which is the whole reason this class exists, since a decoder handed a
// dependent on its own has nothing to extend.
[[nodiscard]] bool begins_access_unit(const FrameHeader& header) {
    return header.kind == StreamKind::kAc3 ||
           header.strmtyp == eac3::StreamType::kIndependent;
}

}  // namespace

void AccessUnitAccumulator::consume_front(std::size_t count) {
    count = std::min(count, filled_);
    if (count == 0) {
        return;
    }
    const auto remaining = filled_ - count;
    if (remaining > 0) {
        std::copy_n(storage_.begin() + static_cast<std::ptrdiff_t>(count), remaining,
                    storage_.begin());
    }
    filled_ = remaining;
}

AccessUnitAccumulator::Result AccessUnitAccumulator::next() {
    // The span handed out last time pointed into storage_, so nothing may move
    // until the caller has come back for the next unit. This is where it moves.
    if (emitted_ > 0) {
        consume_front(emitted_);
        emitted_ = 0;
        unit_bytes_ = 0;
    }

    for (;;) {
        // --- 1. the front of the buffer must BE a frame ----------------------
        // Only while no unit is being assembled: once one is, the front is a
        // syncframe by construction and re-searching it would be wrong.
        if (unit_bytes_ == 0) {
            std::size_t skip = 0;
            while (skip + 2 <= filled_ &&
                   !starts_with_sync(std::span<const std::byte>{storage_}.subspan(skip))) {
                ++skip;
            }
            if (skip > 0) {
                resync_bytes_ += skip;
                consume_front(skip);
            }
            if (filled_ < 2) {
                // A single trailing byte cannot be a sync word. Drop it at end
                // of stream rather than reporting a lost-sync error over what
                // is almost always a file whose length is odd.
                if (finished_) {
                    resync_bytes_ += filled_;
                    filled_ = 0;
                    return {Status::kEndOfStream, {}};
                }
                return need_more();
            }
        }

        // --- 2. read the header of the frame at the assembly point -----------
        const auto at = std::span<const std::byte>{storage_}.subspan(unit_bytes_,
                                                                     filled_ - unit_bytes_);
        if (at.size() < kMinimumHeaderBytes || !starts_with_sync(at)) {
            // Not a frame here. If a unit is already assembled, that unit is
            // complete and whatever follows is the next call's problem -
            // emitting now is what lets a stream with trailing padding still
            // deliver its last unit.
            if (at.size() >= kMinimumHeaderBytes && unit_bytes_ > 0) {
                return emit();
            }
            if (finished_) {
                return unit_bytes_ > 0 ? emit() : Result{Status::kEndOfStream, {}};
            }
            return need_more();
        }

        const auto header = read_frame_header(at);
        if (!header) {
            if (header.error() == ScanError::kTruncated) {
                if (finished_) {
                    // The tail is a partial frame. A unit already assembled is
                    // still good; the fragment is not, and saying so beats
                    // handing back a short frame that would fail to decode.
                    return unit_bytes_ > 0 ? emit() : fail(ScanError::kTruncated);
                }
                return need_more();
            }
            return fail(header.error());
        }

        // --- 3. does it join this unit, or start the next? -------------------
        if (unit_bytes_ > 0 && begins_access_unit(*header)) {
            return emit();
        }

        // It belongs to this unit, so the whole of it has to be present before
        // the unit can be handed over.
        if (unit_bytes_ + header->bytes > storage_.size()) {
            return {Status::kBufferTooSmall, {}};
        }
        if (unit_bytes_ + header->bytes > filled_) {
            if (finished_) {
                return unit_bytes_ > 0 ? emit() : fail(ScanError::kTruncated);
            }
            return need_more();
        }
        unit_bytes_ += header->bytes;
    }
}

AccessUnitAccumulator::Result AccessUnitAccumulator::emit() {
    emitted_ = unit_bytes_;
    return {Status::kUnit, std::span<const std::byte>{storage_}.subspan(0, unit_bytes_)};
}

AccessUnitAccumulator::Result AccessUnitAccumulator::need_more() {
    // A full buffer that still cannot produce a unit is not going to be helped
    // by more bytes - there is nowhere to put them.
    if (filled_ >= storage_.size()) {
        return {Status::kBufferTooSmall, {}};
    }
    return {Status::kNeedMoreInput, {}};
}

AccessUnitAccumulator::Result AccessUnitAccumulator::fail(ScanError error) {
    error_ = error;
    return {Status::kError, {}};
}

}  // namespace ac3::io

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "ac3/export.hpp"
#include "ac3/io/elementary.hpp"

// Turning a BYTE STREAM into whole access units, without owning any memory.
//
// ac3::split_frames and ac3::split_access_units both take a span over the
// entire stream and hand back every boundary in it at once. That is the right
// shape for a file that is already in memory and the wrong one for anything
// arriving over time: an HTTP body, an SD card read in blocks, a UART, a flash
// partition read a page at a time. None of those can be spanned before the last
// byte has arrived, and on a part with 280 KB of RAM the whole stream is not
// going to be in memory anyway.
//
// This is the same boundary rule, applied incrementally. The caller owns the
// storage and drives the loop:
//
//     std::array<std::byte, 16384> buf;
//     ac3::io::AccessUnitAccumulator acc{buf};
//     for (;;) {
//         auto unit = acc.next();
//         if (unit.status == Status::kNeedMoreInput) {
//             const auto n = read_from_somewhere(acc.writable());
//             if (n == 0) { acc.finish(); continue; }
//             acc.commit(n);
//             continue;
//         }
//         if (unit.status != Status::kUnit) break;   // kEndOfStream or an error
//         decoder.decode_access_unit_into(unit.bytes, channels);
//     }
//
// WHY ACCESS UNITS AND NOT SYNCFRAMES. Because that is what the decoder takes.
// Eac3Decoder::decode_access_unit_into wants an independent substream together
// with the dependents that extend it (§E3.8.2), and handing it one syncframe at
// a time would lose exactly the grouping it needs. For AC-3 the two are the
// same thing, so a caller that only ever sees AC-3 pays nothing for the
// distinction.
//
// NO ALLOCATION, ANYWHERE. The storage is the caller's span and nothing here
// grows it - which is what lets this be used from the minimum-footprint profile
// (roadmap PF7) whose whole subject is not allocating during decode. The cost
// of that choice is that the buffer has to be big enough for the largest access
// unit in the stream, and the caller finds out it was not by getting
// kBufferTooSmall rather than by silently reallocating. See kMinimumBuffer.

namespace ac3::io {

// The largest a single syncframe can be: E-AC-3's frmsiz is 11 bits and the
// frame is (frmsiz + 1) * 2 bytes, so 4,096. AC-3's worst case is smaller -
// 640 kbit/s at 32 kHz is 3,840 - so this covers both.
inline constexpr std::size_t kMaxSyncframeBytes = 4096;

// The floor. Holds any single syncframe plus enough of the next one to read its
// header, which is what deciding where an access unit ENDS requires - the
// boundary is only known once the following frame's strmtyp has been read.
//
// It is only enough for a stream whose access units are ONE syncframe: plain
// AC-3, or E-AC-3 with no dependent substreams. An Atmos access unit is an
// independent substream plus its dependents and needs the sum of them, so a
// caller that might see one wants kRecommendedBuffer instead.
inline constexpr std::size_t kMinimumBuffer = kMaxSyncframeBytes + 64;

// What to reach for when the stream's shape is not known in advance: four
// maximum syncframes, which covers an independent substream plus three
// dependents. §E3.8.4 permits more, but this decoder refuses those arrangements
// anyway (ScanError::kUnsupportedStructure), so a unit it would accept and this
// cannot hold is not a case that arises.
inline constexpr std::size_t kRecommendedBuffer = 16384;

class AC3FORGE_EXPORT AccessUnitAccumulator {
   public:
    enum class Status : std::uint8_t {
        // `bytes` holds one complete access unit. Valid until the next call to
        // next(), writable() or commit() - it points into the caller's buffer,
        // which those may compact.
        kUnit,
        // Feed more bytes through writable()/commit(), or call finish() if
        // there are none.
        kNeedMoreInput,
        // finish() was called and everything buffered has been handed back.
        kEndOfStream,
        // The buffer cannot hold the access unit being assembled. Not
        // recoverable by feeding more: the caller needs a bigger buffer.
        kBufferTooSmall,
        // The bytes are not a stream this reader can walk. error() says which.
        kError,
    };

    struct Result {
        Status status = Status::kNeedMoreInput;
        std::span<const std::byte> bytes{};
    };

    // `storage` must be at least kMinimumBuffer. A smaller span is accepted
    // rather than asserted on - the first next() reports kBufferTooSmall - so
    // that a caller sizing a buffer from configuration finds out through the
    // same channel as one whose stream simply had a big access unit in it.
    explicit AccessUnitAccumulator(std::span<std::byte> storage) : storage_(storage) {}

    // Where the caller appends. Always a suffix of the storage; empty when the
    // buffer is full, which next() will report as kBufferTooSmall.
    [[nodiscard]] std::span<std::byte> writable() { return storage_.subspan(filled_); }

    // How many of writable()'s bytes were actually written.
    void commit(std::size_t bytes) { filled_ += bytes; }

    // No more input will arrive. What is already buffered still comes out of
    // next(); after that it reports kEndOfStream.
    //
    // This is what closes the last access unit. A unit's end is normally found
    // by reading the START of the next one, so without an explicit end of
    // stream the final unit would sit in the buffer forever waiting for a
    // successor that is never coming.
    void finish() { finished_ = true; }

    [[nodiscard]] Result next();

    // Set when next() returns kError.
    [[nodiscard]] ScanError error() const { return error_; }

    // Bytes skipped looking for a sync word, over the accumulator's life.
    // Non-zero means the stream did not begin on a frame boundary, or that
    // something between frames was not a frame - worth reporting rather than
    // silently absorbing, because it is the signature of a mis-muxed file or a
    // wrong byte offset.
    [[nodiscard]] std::size_t resynchronised_bytes() const { return resync_bytes_; }

   private:
    // Drops `count` bytes from the front and moves the remainder down.
    void consume_front(std::size_t count);
    Result emit();
    Result need_more();
    Result fail(ScanError error);

    std::span<std::byte> storage_;
    std::size_t filled_ = 0;
    // Length of the access unit assembled so far, always a prefix of storage_.
    std::size_t unit_bytes_ = 0;
    // Length of the unit handed to the caller last time, still occupying the
    // front of the buffer because the span they hold points at it. Dropped at
    // the top of the next next().
    std::size_t emitted_ = 0;
    bool finished_ = false;
    ScanError error_ = ScanError::kEmpty;
    std::size_t resync_bytes_ = 0;
};

}  // namespace ac3::io

#include "ac3/io/object_strip.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/core/crc16.hpp"
#include "ac3/emdf/frame_layout.hpp"

namespace ac3::io {

namespace {

using emdf::BitRange;
using emdf::FrameLayout;

// §E2.3.1: the syncframe's tail is auxdatae (1) + crcrsv (1) + crc2 (16),
// with auxbits padding in front of it. Same constant, same reason, as
// eac3_frame.cpp's own kTailBits on the encode side.
constexpr std::size_t kTailBits = 18;
constexpr std::uint16_t kSyncWord = 0x0B77;

[[nodiscard]] bool sync_at(std::span<const std::byte> stream, std::size_t offset) {
    return offset + 2 <= stream.size() &&
           std::to_integer<std::uint8_t>(stream[offset]) == 0x0B &&
           std::to_integer<std::uint8_t>(stream[offset + 1]) == 0x77;
}

// A bit-at-a-time writer, deliberately: the whole point here is to move a
// run of bits that starts and ends on no particular boundary, so there is no
// byte-wise shortcut that would not just be this loop with extra steps.
class BitAppender {
   public:
    explicit BitAppender(std::size_t reserve_bits) { bytes_.reserve((reserve_bits + 7) / 8); }

    void push(bool bit) {
        if ((count_ & 7) == 0) {
            bytes_.push_back(std::byte{0});
        }
        if (bit) {
            bytes_.back() |= static_cast<std::byte>(1u << (7 - (count_ & 7)));
        }
        ++count_;
    }

    void push_zeros(std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            push(false);
        }
    }

    [[nodiscard]] std::size_t bit_count() const { return count_; }
    [[nodiscard]] std::vector<std::byte> take() { return std::move(bytes_); }

   private:
    std::vector<std::byte> bytes_{};
    std::size_t count_ = 0;
};

[[nodiscard]] bool bit_at(std::span<const std::byte> f, std::size_t p) {
    if ((p >> 3) >= f.size()) {
        return false;
    }
    return ((std::to_integer<std::uint32_t>(f[p >> 3]) >> (7 - (p & 7))) & 1) != 0;
}

struct FrameResult {
    std::vector<std::byte> bytes;
    bool changed = false;
};

// Rewrites one syncframe with its object layer taken out. `layout` must be a
// fully mapped frame (supported) that has something to remove.
[[nodiscard]] std::expected<FrameResult, StripError> rewrite_frame(
    std::span<const std::byte> frame, const FrameLayout& layout) {
    if (layout.blkstrtinfoe) {
        return std::unexpected(StripError::kFrameSizeDependentField);
    }

    // What comes out, and what is merely forced to zero in place.
    //
    // A skip field goes whole - flag bit, length and data - because §5.4.3.58's
    // skiple is only present at all when the frame-level skipflde is set, and
    // that flag is cleared below; leaving the per-block bits behind would
    // desync every mantissa after them.
    //
    // addbsi is the other way round: addbsie is UNCONDITIONAL in Table E1.2,
    // so the flag bit has to stay and be cleared, and only addbsil plus the
    // payload come out. Removing the flag as well would delete a field the
    // syntax always has.
    std::vector<BitRange> removed;
    removed.reserve(layout.skip_fields.size() + 1);
    for (const auto& field : layout.skip_fields) {
        removed.push_back(field.range);
    }
    const bool strip_addbsi = layout.addbsi_object_extension && layout.addbsi.has_value();
    if (strip_addbsi && layout.addbsi->last > layout.addbsi->first) {
        removed.push_back(BitRange{.first = layout.addbsi->first + 1, .last = layout.addbsi->last});
    }
    std::sort(removed.begin(), removed.end(),
              [](const BitRange& a, const BitRange& b) { return a.first < b.first; });

    std::size_t removed_bits = 0;
    for (const auto& range : removed) {
        removed_bits += range.bits();
    }
    if (removed_bits == 0) {
        return FrameResult{.bytes = {frame.begin(), frame.end()}, .changed = false};
    }

    const std::size_t kept_bits = layout.audio_end_bits - removed_bits;
    // A syncframe is a whole number of 16-bit words (§E2.3.1.3), so the
    // padding runs to the next word boundary that still fits the tail.
    const std::size_t words = (kept_bits + kTailBits + 15) / 16;
    const std::size_t total_bits = words * 16;
    const std::size_t spare = total_bits - kept_bits - kTailBits;

    BitAppender out{total_bits};
    std::size_t next = 0;  // index into `removed`
    for (std::size_t bit = 0; bit < layout.audio_end_bits;) {
        if (next < removed.size() && bit == removed[next].first) {
            bit = removed[next].last + 1;
            ++next;
            continue;
        }
        // The two flags whose fields have just gone: skipflde says there are
        // no per-block skip fields, addbsie that there is no addbsi element.
        const bool force_zero =
            bit == layout.skipflde_bit || (strip_addbsi && bit == layout.addbsi->first);
        out.push(!force_zero && bit_at(frame, bit));
        ++bit;
    }
    out.push_zeros(spare);  // auxbits: padding, and nothing else
    out.push_zeros(1);      // auxdatae
    out.push_zeros(1);      // crcrsv
    out.push_zeros(16);     // crc2, stamped below

    std::vector<std::byte> bytes = out.take();
    if (bytes.size() != words * 2) {
        // Unreachable for a mapped frame - kept_bits came from the same walk
        // the copy loop above followed - but a size mismatch here would put
        // the frmsiz patch and the CRC on the wrong bytes.
        return std::unexpected(StripError::kUnsupportedFrame);
    }

    // §E2.3.1.3: frmsiz is the word count minus one, in the 11 bits that
    // straddle bytes 2 and 3 (the low 3 bits of byte 2, then all of byte 3).
    const auto frmsiz = static_cast<std::uint32_t>(words - 1);
    bytes[2] = static_cast<std::byte>((std::to_integer<std::uint32_t>(bytes[2]) & 0xF8u) |
                                      ((frmsiz >> 8) & 0x07u));
    bytes[3] = static_cast<std::byte>(frmsiz & 0xFFu);

    // §5.4.5.2: crc2 covers everything after the syncword up to itself, and
    // it is the LAST field in the frame, so this is a plain forward recompute
    // over the finished bytes. A crc2 that lands on the syncword would give a
    // stream scanner a false frame start, so the same escape the encoder uses
    // applies here: flip crcrsv (§5.4.5.1, a reserved bit with no other
    // meaning) and recompute.
    const std::span<const std::byte> view{bytes};
    std::uint16_t crc2 = crc16(view.subspan(2, bytes.size() - 4));
    if (crc2 == kSyncWord) {
        bytes[bytes.size() - 3] ^= std::byte{0x01};
        crc2 = crc16(view.subspan(2, bytes.size() - 4));
    }
    bytes[bytes.size() - 2] = static_cast<std::byte>(crc2 >> 8);
    bytes[bytes.size() - 1] = static_cast<std::byte>(crc2 & 0xFF);
    return FrameResult{.bytes = std::move(bytes), .changed = true};
}

}  // namespace

std::string_view describe(StripError error) {
    switch (error) {
        case StripError::kEmpty:
            return "no frames in stream";
        case StripError::kLostSync:
            return "lost sync: expected 0x0B77";
        case StripError::kTruncated:
            return "stream ends mid-frame";
        case StripError::kNotEac3:
            return "not an E-AC-3 stream: only Annex E frames can carry an object layer";
        case StripError::kUnsupportedFrame:
            return "frame carries an object layer in a bitstream shape this build cannot rewrite";
        case StripError::kFrameSizeDependentField:
            return "frame carries blkstrtinfo, whose width depends on the frame size";
    }
    return "unknown error";
}

std::expected<StrippedStream, StripError> strip_objects(std::span<const std::byte> stream) {
    if (stream.size() < 6) {
        return std::unexpected(StripError::kEmpty);
    }
    // Both formats put bsid at bit 40 (see elementary.hpp), and only Annex E
    // has substreams, skip fields or an addbsi object extension - so an AC-3
    // stream is refused outright rather than walked and reported as "nothing
    // to strip", which would read as a successful no-op on the wrong input.
    const auto bsid = static_cast<int>((std::to_integer<std::uint32_t>(stream[5]) >> 3) & 0x1F);
    if (bsid != 16) {
        return std::unexpected(StripError::kNotEac3);
    }

    StrippedStream result;
    result.bytes.reserve(stream.size());
    std::size_t offset = 0;
    while (offset < stream.size()) {
        if (!sync_at(stream, offset) || offset + 4 > stream.size()) {
            return std::unexpected(StripError::kLostSync);
        }
        const std::size_t size = emdf::syncframe_size(stream.subspan(offset));
        if (offset + size > stream.size()) {
            return std::unexpected(StripError::kTruncated);
        }
        const auto frame = stream.subspan(offset, size);
        ++result.frames_total;

        const FrameLayout layout = emdf::walk_frame(frame);
        // A frame with neither signal has no object layer, whatever its shape.
        const bool carries_objects =
            layout.object_signals && (layout.skipflde || layout.addbsi_object_extension);
        if (!carries_objects) {
            result.bytes.insert(result.bytes.end(), frame.begin(), frame.end());
            offset += size;
            continue;
        }
        if (!layout.supported) {
            return std::unexpected(StripError::kUnsupportedFrame);
        }
        auto rewritten = rewrite_frame(frame, layout);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        if (rewritten->changed) {
            ++result.frames_stripped;
            result.bytes_removed += size - rewritten->bytes.size();
        }
        result.bytes.insert(result.bytes.end(), rewritten->bytes.begin(), rewritten->bytes.end());
        offset += size;
    }
    if (result.frames_total == 0) {
        return std::unexpected(StripError::kEmpty);
    }
    return result;
}

}  // namespace ac3::io

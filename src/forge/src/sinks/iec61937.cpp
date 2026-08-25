#include "ac3/sinks/iec61937.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/core/eac3_tables.hpp"

namespace ac3::iec61937 {

namespace {

void put_word_le(std::vector<std::byte>& out, std::uint16_t word) {
    out.push_back(static_cast<std::byte>(word & 0xFF));
    out.push_back(static_cast<std::byte>(word >> 8));
}

// Shared by wrap_frame and Eac3BurstPacker: the elementary stream is
// big-endian within each 16-bit word, the IEC 61937 carrier is little-endian
// PCM16, and the burst is zero-padded to its fixed length either way. Both
// AC-3 and E-AC-3 syncframes are measured in whole 16-bit words (Table 5.18's
// byte counts and Annex E's frmsiz are both word counts), so payload is
// always even here — nothing legal reaches the odd-trailing-byte case IEC
// 61937 otherwise has to handle.
void pack_payload_words(std::vector<std::byte>& burst, std::span<const std::byte> payload,
                        std::size_t burst_bytes) {
    for (std::size_t i = 0; i + 1 < payload.size(); i += 2) {
        put_word_le(burst, static_cast<std::uint16_t>(
                               (std::to_integer<std::uint16_t>(payload[i]) << 8) |
                               std::to_integer<std::uint16_t>(payload[i + 1])));
    }
    burst.resize(burst_bytes, std::byte{0});
}

}  // namespace

std::expected<std::vector<std::byte>, WrapError> wrap_frame(std::span<const std::byte> frame) {
    if (frame.size() < 6 || (frame.size() & 1) != 0 ||
        std::to_integer<std::uint8_t>(frame[0]) != 0x0B ||
        std::to_integer<std::uint8_t>(frame[1]) != 0x77) {
        return std::unexpected(WrapError::kNotAFrame);
    }
    if (frame.size() + 8 > kBurstBytes) {
        return std::unexpected(WrapError::kFrameTooLarge);
    }
    // bsmod: the 3 bits after the 5-bit bsid in byte 5.
    const auto bsmod = std::to_integer<std::uint16_t>(frame[5]) & 0x7;

    std::vector<std::byte> burst;
    burst.reserve(kBurstBytes);
    put_word_le(burst, 0xF872);  // Pa
    put_word_le(burst, 0x4E1F);  // Pb
    put_word_le(burst, static_cast<std::uint16_t>(1 | (bsmod << 8)));  // Pc: type 1 = AC-3
    put_word_le(burst, static_cast<std::uint16_t>(frame.size() * 8));  // Pd: bits
    pack_payload_words(burst, frame, kBurstBytes);
    return burst;
}

struct Eac3BurstPacker::Impl {
    std::vector<std::byte> pending_;
    int blocks_pending_ = 0;
};

Eac3BurstPacker::Eac3BurstPacker() : impl_(std::make_unique<Impl>()) {}
Eac3BurstPacker::~Eac3BurstPacker() = default;
Eac3BurstPacker::Eac3BurstPacker(Eac3BurstPacker&&) noexcept = default;
Eac3BurstPacker& Eac3BurstPacker::operator=(Eac3BurstPacker&&) noexcept = default;

std::expected<std::optional<std::vector<std::byte>>, WrapError> Eac3BurstPacker::push(
    std::span<const std::byte> access_unit) {
    if (access_unit.size() < 6 || (access_unit.size() & 1) != 0 ||
        std::to_integer<std::uint8_t>(access_unit[0]) != 0x0B ||
        std::to_integer<std::uint8_t>(access_unit[1]) != 0x77) {
        return std::unexpected(WrapError::kNotAFrame);
    }
    if (impl_->pending_.size() + access_unit.size() + 8 > kEac3BurstBytes) {
        impl_->pending_.clear();
        impl_->blocks_pending_ = 0;
        return std::unexpected(WrapError::kFrameTooLarge);
    }

    // byte 4: fscod (2 bits) | numblkscod (2 bits) | acmod (3 bits) | lfeon
    // (1 bit). byte 5's top 5 bits are bsid. Same layout this project's own
    // eac3_decoder.cpp parses, read directly here the way spdif_header_eac3
    // does (pkt->data[4], pkt->data[5]) rather than pulling in a decoder.
    const auto byte4 = std::to_integer<std::uint32_t>(access_unit[4]);
    const auto bsid = std::to_integer<std::uint32_t>(access_unit[5]) >> 3;
    const auto fscod = byte4 >> 6;
    const auto numblkscod = (byte4 >> 4) & 0x3;
    // fscod == 3 selects the reduced-sample-rate path (§E2.3.1.3), which does
    // not transmit numblkscod at all — it is implicitly always the six-block
    // code. Mirrors spdif_header_eac3's own bsid > 10 && fscod != 3 guard.
    const int blocks = (bsid > 10 && fscod != 3)
                           ? eac3::blocks_per_syncframe(static_cast<int>(numblkscod))
                           : 6;

    impl_->pending_.insert(impl_->pending_.end(), access_unit.begin(), access_unit.end());
    impl_->blocks_pending_ += blocks;
    if (impl_->blocks_pending_ < 6) {
        return std::nullopt;
    }

    std::vector<std::byte> burst;
    burst.reserve(kEac3BurstBytes);
    put_word_le(burst, 0xF872);  // Pa
    put_word_le(burst, 0x4E1F);  // Pb
    put_word_le(burst, 0x0015);  // Pc: IEC61937_EAC3, no data-type-dependent bits
    put_word_le(burst, static_cast<std::uint16_t>(impl_->pending_.size()));  // Pd: BYTES, not bits
    pack_payload_words(burst, impl_->pending_, kEac3BurstBytes);

    impl_->pending_.clear();
    impl_->blocks_pending_ = 0;
    return std::optional<std::vector<std::byte>>{std::move(burst)};
}

std::expected<std::vector<std::byte>, WrapError> wrap_stream(
    std::span<const std::span<const std::byte>> units, bool eac3) {
    std::vector<std::byte> payload;
    if (eac3) {
        Eac3BurstPacker packer;
        for (const auto& unit : units) {
            const auto burst = packer.push(unit);
            if (!burst) {
                return std::unexpected(burst.error());
            }
            if (*burst) {
                payload.insert(payload.end(), (**burst).begin(), (**burst).end());
            }
        }
        return payload;
    }
    payload.reserve(units.size() * kBurstBytes);
    for (const auto& unit : units) {
        const auto burst = wrap_frame(unit);
        if (!burst) {
            return std::unexpected(burst.error());
        }
        payload.insert(payload.end(), burst->begin(), burst->end());
    }
    return payload;
}

// ---------------------------------------------------------------------------
// De-framing.
// ---------------------------------------------------------------------------

namespace {

// Pa 0xF872 then Pb 0x4E1F, as they appear in carrier bytes under each word
// order. Distinct four-byte strings, and neither is a substring of the
// other's repetition, so finding one identifies the order outright - there is
// no heuristic here to get wrong.
constexpr std::array<std::byte, 4> kPreambleLe{std::byte{0x72}, std::byte{0xF8}, std::byte{0x1F},
                                               std::byte{0x4E}};
constexpr std::array<std::byte, 4> kPreambleBe{std::byte{0xF8}, std::byte{0x72}, std::byte{0x4E},
                                               std::byte{0x1F}};

constexpr std::size_t kPreambleBytes = 8;  // Pa Pb Pc Pd

// The elementary-stream syncword, 0x0B77, as the first payload word. Every
// AC-3 and E-AC-3 syncframe starts with it, so a "preamble" not followed by
// one is a false match inside payload bytes or stuffing.
constexpr std::uint16_t kSyncword = 0x0B77;

[[nodiscard]] std::uint16_t read_word(std::span<const std::byte> bytes, std::size_t index,
                                      WordOrder order) {
    const auto low = std::to_integer<std::uint16_t>(bytes[index]);
    const auto high = std::to_integer<std::uint16_t>(bytes[index + 1]);
    return order == WordOrder::kLittleEndian ? static_cast<std::uint16_t>(low | (high << 8))
                                             : static_cast<std::uint16_t>((low << 8) | high);
}

// Where a preamble starts at or after `from`, and which order it was in.
// Scans byte-wise rather than word-wise: nothing guarantees a capture began
// on a word boundary, and a carrier read from the middle of a file routinely
// does not.
struct Preamble {
    std::size_t offset;
    WordOrder order;
};

[[nodiscard]] std::optional<Preamble> find_preamble(std::span<const std::byte> carrier,
                                                    std::size_t from,
                                                    std::optional<WordOrder> locked) {
    if (carrier.size() < 4) {
        return std::nullopt;
    }
    for (std::size_t i = from; i + 4 <= carrier.size(); ++i) {
        const auto four = carrier.subspan(i, 4);
        if ((!locked || *locked == WordOrder::kLittleEndian) &&
            std::equal(four.begin(), four.end(), kPreambleLe.begin())) {
            return Preamble{i, WordOrder::kLittleEndian};
        }
        if ((!locked || *locked == WordOrder::kBigEndian) &&
            std::equal(four.begin(), four.end(), kPreambleBe.begin())) {
            return Preamble{i, WordOrder::kBigEndian};
        }
    }
    return std::nullopt;
}

// Pd's unit is data-type-dependent: bits for AC-3 (and for IEC 61937-2's
// general rule), bytes for E-AC-3. Returns the payload length in elementary
// bytes. A bit count that is not a whole number of bytes rounds up, so the
// carrier words that hold it are still accounted for.
[[nodiscard]] std::size_t payload_bytes_from_pd(std::uint16_t pd,
                                                std::optional<BurstDataType> type) {
    if (type == BurstDataType::kEac3) {
        return pd;
    }
    return (static_cast<std::size_t>(pd) + 7) / 8;
}

[[nodiscard]] std::optional<BurstDataType> known_data_type(std::uint16_t pc) {
    switch (pc & 0x1F) {
        case 0x01: return BurstDataType::kAc3;
        case 0x15: return BurstDataType::kEac3;
        default: return std::nullopt;
    }
}

// Carrier bytes one payload occupies: whole 16-bit words, so an odd byte
// count still costs a full word.
[[nodiscard]] std::size_t payload_carrier_bytes(std::size_t payload_bytes) {
    return ((payload_bytes + 1) / 2) * 2;
}

// Un-swap payload words into elementary-stream order, appending exactly
// `payload_bytes` of them. The mirror of pack_payload_words above; a
// big-endian carrier already holds them in stream order and only copies.
void unpack_payload_words(std::span<const std::byte> carrier, std::size_t offset,
                          std::size_t payload_bytes, WordOrder order, std::vector<std::byte>& out) {
    const auto first = out.size();
    out.resize(first + payload_bytes);
    for (std::size_t i = 0; i < payload_bytes; ++i) {
        // Little-endian words put the stream's first byte second: word
        // (b0 b1) carries stream bytes (b1 b0).
        const std::size_t source =
            order == WordOrder::kLittleEndian ? offset + (i ^ 1u) : offset + i;
        out[first + i] = carrier[source];
    }
}

}  // namespace

std::string_view describe(UnwrapError error) {
    switch (error) {
        case UnwrapError::kNoSync: return "no IEC 61937 preamble found";
        case UnwrapError::kTruncatedBurst: return "burst payload cut off by end of input";
        case UnwrapError::kPayloadTooLarge: return "burst length exceeds its repetition period";
    }
    return "unknown error";
}

std::size_t repetition_period(BurstDataType type) {
    return type == BurstDataType::kEac3 ? kEac3BurstBytes : kBurstBytes;
}

void BurstReader::compact() {
    if (pos_ == 0) {
        return;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(pos_));
    pos_ = 0;
}

std::expected<void, UnwrapError> BurstReader::push(std::span<const std::byte> carrier,
                                                   std::vector<std::byte>& out) {
    buffer_.insert(buffer_.end(), carrier.begin(), carrier.end());

    for (;;) {
        if (state_ == State::kPayload) {
            const auto available = buffer_.size() - pos_;
            if (available < payload_needed_) {
                break;
            }
            if (emitting_) {
                unpack_payload_words(buffer_, pos_, payload_bytes_, payload_order_, out);
                ++bursts_;
            }
            pos_ += payload_needed_;
            state_ = State::kSyncing;
            continue;
        }

        const auto found = find_preamble(buffer_, pos_, order_);
        if (!found) {
            // Keep only what a preamble could still straddle into the next
            // chunk: three bytes, one short of the pattern. std::max, because
            // a burst that ended within those last three bytes has already
            // put pos_ past them and must not be walked back into.
            pos_ = std::max(pos_, buffer_.size() >= 3 ? buffer_.size() - 3 : 0);
            break;
        }
        // Ten bytes decide a candidate: the four preamble words plus the
        // first payload word, which must be the syncframe's own.
        if (found->offset + kPreambleBytes + 2 > buffer_.size()) {
            pos_ = found->offset;
            break;
        }

        const std::span<const std::byte> view{buffer_};
        const auto pc = read_word(view, found->offset + 4, found->order);
        const auto pd = read_word(view, found->offset + 6, found->order);
        const auto type = known_data_type(pc);
        const auto payload_bytes = payload_bytes_from_pd(pd, type);

        if (!type) {
            // Another codec's passthrough, or a null/pause burst. Its length
            // is still readable under the general bits rule, so step over the
            // payload rather than rescanning through it - a false preamble
            // inside somebody else's payload would only cost accuracy here.
            // A length that could not fit any burst period is not believed,
            // and the resync simply starts after the preamble instead.
            ++skipped_bursts_;
            payload_needed_ = payload_bytes <= kEac3BurstBytes - kPreambleBytes
                                  ? payload_carrier_bytes(payload_bytes)
                                  : 0;
            payload_bytes_ = 0;
            emitting_ = false;
            pos_ = found->offset + kPreambleBytes;
            state_ = State::kPayload;
            continue;
        }
        if (payload_bytes > repetition_period(*type) - kPreambleBytes) {
            return std::unexpected(UnwrapError::kPayloadTooLarge);
        }
        if (payload_bytes < 2 ||
            read_word(view, found->offset + kPreambleBytes, found->order) != kSyncword) {
            // The preamble bytes turned up inside payload or stuffing. Resync
            // one byte on rather than trusting a length nothing corroborates.
            ++false_syncs_;
            pos_ = found->offset + 1;
            continue;
        }

        data_type_ = data_type_.value_or(*type);
        order_ = found->order;
        last_header_ = BurstHeader{.data_type = *type,
                                   .data_type_dependent = static_cast<std::uint8_t>((pc >> 8) & 0x1F),
                                   .stream_number = static_cast<std::uint8_t>(pc >> 13),
                                   .error_flag = (pc & 0x80) != 0,
                                   .payload_bytes = payload_bytes};
        payload_bytes_ = payload_bytes;
        payload_needed_ = payload_carrier_bytes(payload_bytes);
        payload_order_ = found->order;
        emitting_ = true;
        pos_ = found->offset + kPreambleBytes;
        state_ = State::kPayload;
    }

    compact();
    return {};
}

std::expected<void, UnwrapError> BurstReader::finish() const {
    if (state_ == State::kPayload) {
        return std::unexpected(UnwrapError::kTruncatedBurst);
    }
    return {};
}

std::expected<std::vector<std::byte>, UnwrapError> unwrap_stream(
    std::span<const std::byte> carrier) {
    BurstReader reader;
    std::vector<std::byte> out;
    if (const auto pushed = reader.push(carrier, out); !pushed) {
        return std::unexpected(pushed.error());
    }
    if (const auto done = reader.finish(); !done) {
        return std::unexpected(done.error());
    }
    if (reader.bursts() == 0) {
        return std::unexpected(UnwrapError::kNoSync);
    }
    return out;
}

void carrier_from_capture(std::span<const float> interleaved, std::uint16_t channels,
                          std::vector<std::byte>& out) {
    if (channels == 0) {
        return;
    }
    const auto stride = static_cast<std::size_t>(channels);
    // Only the first two channels: IEC 61937 is a stereo carrier, and a
    // capture that offers more is padding the rest.
    const auto carried = std::min<std::size_t>(stride, 2);
    for (std::size_t base = 0; base + stride <= interleaved.size(); base += stride) {
        for (std::size_t ch = 0; ch < carried; ++ch) {
            // The exact inverse of every backend's int16 -> float step
            // (x / 32768.0f), so a word that came in as PCM16 comes back
            // bit-identical. Clamping matters only for a float source, which
            // by construction is not carrying bursts anyway.
            const auto clamped = std::clamp(interleaved[base + ch], -1.0f, 1.0f);
            const auto word = static_cast<std::int32_t>(std::lround(clamped * 32768.0f));
            const auto sample = static_cast<std::uint16_t>(
                std::clamp(word, std::int32_t{-32768}, std::int32_t{32767}));
            out.push_back(static_cast<std::byte>(sample & 0xFF));
            out.push_back(static_cast<std::byte>(sample >> 8));
        }
    }
}

void PassthroughDetector::push(std::span<const float> interleaved, std::uint16_t channels) {
    if (decided() || channels == 0) {
        return;
    }
    // Take only what is left of the inspection budget, so a caller handing
    // over a second of audio at a time cannot make this hold a second of
    // audio: undecided or not, buffered_ never exceeds kInspectBytes by more
    // than the frame that crossed it.
    const auto stride = static_cast<std::size_t>(channels);
    const auto per_frame = 2 * std::min<std::size_t>(stride, 2);
    const auto budget = (kInspectBytes - inspected_ + per_frame - 1) / per_frame;
    const auto take = std::min(budget, interleaved.size() / stride);
    const auto before = buffered_.size();
    carrier_from_capture(interleaved.first(take * stride), channels, buffered_);
    inspected_ += buffered_.size() - before;

    // Same acceptance test the reader applies: a preamble AND a syncframe
    // behind it. A lone preamble pattern shows up in ordinary loud audio
    // often enough that it cannot be the whole answer.
    const std::span<const std::byte> view{buffered_};
    std::size_t from = 0;
    while (const auto found = find_preamble(view, from, std::nullopt)) {
        if (found->offset + kPreambleBytes + 2 > view.size()) {
            break;
        }
        const auto pc = read_word(view, found->offset + 4, found->order);
        const auto type = known_data_type(pc);
        const auto pd = read_word(view, found->offset + 6, found->order);
        const auto payload_bytes = payload_bytes_from_pd(pd, type);
        if (type && payload_bytes >= 2 &&
            payload_bytes <= repetition_period(*type) - kPreambleBytes &&
            read_word(view, found->offset + kPreambleBytes, found->order) == kSyncword) {
            detected_ = type;
            order_ = found->order;
            // Keep the carrier from this burst on: everything before it is
            // whatever the capture was doing beforehand, and no reader wants
            // to resync through it.
            buffered_.erase(buffered_.begin(),
                            buffered_.begin() + static_cast<std::ptrdiff_t>(found->offset));
            return;
        }
        from = found->offset + 1;
    }
    if (inspected_ >= kInspectBytes) {
        buffered_.clear();
    }
}

}  // namespace ac3::iec61937

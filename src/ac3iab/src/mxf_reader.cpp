#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <istream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "ac3iab/ac3iab.hpp"
#include "ac3iab/mxf.hpp"

// SMPTE ST 336:2017 KLV/BER mechanics plus just enough of SMPTE ST 377-1:2019/ST 379-1:2009/
// ST 2067-201:2021 to find one IAB Track File's single clip-wrapped essence KLV - see mxf.hpp's
// own header comment for the overall design and what is deliberately out of scope.

namespace ac3iab {

namespace {

constexpr std::size_t kKeyLength = 16;

// SMPTE ST 336:2017 §5.3 + Annex I (quoting ISO/IEC 8825-1 §8.1.3.3-8.1.3.5): a KLV Length field
// is BER-coded. Short form: bit 8 of the first byte is 0, bits 7-1 are the length, 0-127. Long
// form: bit 8 is 1, bits 7-1 give the COUNT of big-endian length bytes that follow; the all-ones
// count (0x7F) is the ASN.1 "reserved for future extension" marker and 0x80 ("indefinite length")
// is likewise forbidden here - SMPTE ST 377-1 §6.3.4 caps a KLV Length field at 9 bytes total, so
// at most 8 following bytes.
struct BerLength {
    std::uint64_t value = 0;
    std::size_t consumed = 0;  // bytes the Length field itself occupied
};

[[nodiscard]] std::expected<BerLength, IabError> read_ber_length(std::span<const std::byte> data) {
    if (data.empty()) {
        return std::unexpected(IabError::kTruncated);
    }
    const auto first = std::to_integer<std::uint8_t>(data[0]);
    if ((first & 0x80u) == 0) {
        return BerLength{.value = first, .consumed = 1};
    }
    const unsigned count = first & 0x7Fu;
    if (count == 0 || count > 8) {  // 0x80 (indefinite) and the >8-byte long form are both invalid
        return std::unexpected(IabError::kMxfBadKlv);
    }
    if (data.size() < 1 + count) {
        return std::unexpected(IabError::kTruncated);
    }
    std::uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) {
        value = (value << 8) | std::to_integer<std::uint8_t>(data[1 + i]);
    }
    return BerLength{.value = value, .consumed = 1 + count};
}

// One top-level KLV triplet: the raw 16-byte Key, and the Value span (already past the Key and
// BER Length). `next` is the file offset immediately after this triplet's Value - where the next
// top-level KLV, if any, begins.
struct Klv {
    std::span<const std::byte> key;  // exactly kKeyLength bytes
    std::span<const std::byte> value;
    std::size_t next = 0;
};

[[nodiscard]] std::expected<Klv, IabError> read_klv(std::span<const std::byte> data,
                                                    std::size_t offset) {
    if (data.size() < offset + kKeyLength) {
        return std::unexpected(IabError::kTruncated);
    }
    const auto key = data.subspan(offset, kKeyLength);
    const auto length = read_ber_length(data.subspan(offset + kKeyLength));
    if (!length) {
        return std::unexpected(length.error());
    }
    const std::size_t value_offset = offset + kKeyLength + length->consumed;
    if (data.size() < value_offset + length->value) {
        return std::unexpected(IabError::kTruncated);
    }
    return Klv{.key = key,
               .value = data.subspan(value_offset, static_cast<std::size_t>(length->value)),
               .next = value_offset + static_cast<std::size_t>(length->value)};
}

// SMPTE ST 2067-201:2021 Table 4.2 ("IMF IAB Essence Clip-Wrapped Element"):
// urn:smpte:ul:060E2B34.01020101.0D010301.16cc0Dnn - the Essence Element Key an IAB Track File's
// one Sound Element (§5.5) uses, per the generic structure SMPTE ST 379-1:2009 Table 2 defines for
// every Generic Container essence element (picture/sound/data/compound). Byte 8 (Table 2's own
// "vvh", Version Number - "Registry Version at the point of registration of this Key") and byte 16
// (Table 2's "zzh", Essence Element Number - nominally 0x01 for IAB's single Sound Element, but
// Table 4.2 itself writes it as "nn") are both wildcarded rather than hardcoded to the values the
// registered UL happens to use today, since neither byte is defined to be fixed - matching how
// real MXF readers are expected to treat a registry-version byte, and erring toward accepting a
// real file over a byte-for-byte guess at a value the standard itself does not fix.
constexpr std::array<std::uint8_t, kKeyLength> kIabEssenceKeyMask = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
constexpr std::array<std::uint8_t, kKeyLength> kIabEssenceKey = {
    0x06, 0x0E, 0x2B, 0x34, 0x01, 0x02, 0x01, 0x00, 0x0D, 0x01, 0x03, 0x01, 0x16, 0xCC, 0x0D, 0x00};

[[nodiscard]] bool is_iab_essence_key(std::span<const std::byte> key) {
    if (key.size() != kKeyLength) {
        return false;
    }
    for (std::size_t i = 0; i < kKeyLength; ++i) {
        if (kIabEssenceKeyMask[i] == 0) {
            continue;
        }
        if (std::to_integer<std::uint8_t>(key[i]) != kIabEssenceKey[i]) {
            return false;
        }
    }
    return true;
}

// Walks every top-level KLV triplet from the start of the file (no Run-In - see mxf.hpp's own
// header comment) until one Key matches the IAB Essence Element Key above. Every non-matching KLV
// - Partition Packs (SMPTE ST 377-1 §7.1 Table 4), the Primer Pack and every Header Metadata Local
// Set (§9), Index Table segments, Fill Items (§6.3.3) - is skipped by its own declared Length
// without being interpreted at all: locating essence is a KLV-Key matter, not an object-graph one
// (see mxf.hpp's own comment on what this deliberately does not parse).
[[nodiscard]] std::expected<std::span<const std::byte>, IabError> find_iab_essence(
    std::span<const std::byte> data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        auto klv = read_klv(data, offset);
        if (!klv) {
            return std::unexpected(klv.error());
        }
        if (is_iab_essence_key(klv->key)) {
            return klv->value;
        }
        offset = klv->next;
    }
    return std::unexpected(IabError::kMxfNoIabEssence);
}

[[nodiscard]] std::expected<std::vector<std::byte>, IabError> read_all_bytes(std::istream& in) {
    in.seekg(0, std::ios::end);
    if (!in) {
        return std::unexpected(IabError::kCannotOpen);
    }
    const auto size = in.tellg();
    if (size < 0) {
        return std::unexpected(IabError::kCannotOpen);
    }
    in.seekg(0, std::ios::beg);

    std::vector<std::byte> data(static_cast<std::size_t>(size));
    if (!data.empty()) {
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!in) {
            return std::unexpected(IabError::kCannotOpen);
        }
    }
    return data;
}

}  // namespace

std::expected<std::vector<IABitstreamFrame>, IabError> parse_mxf_iab(std::istream& in) {
    auto bytes = read_all_bytes(in);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }

    auto essence = find_iab_essence(*bytes);
    if (!essence) {
        return std::unexpected(essence.error());
    }

    // The one clip-wrapped KLV Value is byte-identical to ST 2098-2 Clause 7's IABitstream syntax
    // (mxf.hpp's own header comment, finding 2) - hand it to the existing, unmodified reader via
    // an istream view rather than duplicating any Preamble/IAFrame framing logic here.
    std::string bytes_view(reinterpret_cast<const char*>(essence->data()), essence->size());
    std::istringstream essence_stream(std::move(bytes_view), std::ios::binary);
    return parse_iabitstream(essence_stream);
}

std::expected<std::vector<IABitstreamFrame>, IabError> parse_mxf_iab(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(IabError::kCannotOpen);
    }
    return parse_mxf_iab(in);
}

}  // namespace ac3iab

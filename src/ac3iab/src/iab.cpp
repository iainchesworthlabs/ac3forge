#include <cstddef>
#include <fstream>
#include <istream>
#include <span>
#include <utility>
#include <vector>

#include "ac3iab/ac3iab.hpp"
#include "bitreader.hpp"

// §7 Table 2 (IABitstream Syntax) / §8 (IABitstream Field Description): the outer TLV framing
// around IAFrame - a repeating Preamble segment (opaque; content is out of this spec's scope)
// followed by an IAFrame segment, read until end of file/stream. Confirmed against
// DTSProAudio/iab-validator's own real sample bitstreams (test/bitstreams/*.iab, MIT) as an
// external oracle: their first bytes trace exactly to this framing - a 5-byte
// PreambleTag(0x01)+PreambleLength(0) header, then a 5-byte IAFrameTag(0x02)+IAFrameLength
// header, then IAFrameLength bytes that are themselves one whole IAElement(IA_FRAME) - i.e.
// the §8.1.6 "IAFrame" field is bounded by, but is NOT the same span as, the Table 5 IAFrame
// fields: it still carries its own ElementID(0x08)/ElementSize header (§9 Table 3) in front of
// them, which this file strips before handing the inner payload to iab_reader.cpp's
// parse_iaframe() (documented there as taking a bare payload, no header of its own - the same
// convention every other parse_* helper in that file follows). Confirmed against the oracle's
// real byte layout, never consulted for its code, per CONTRIBUTING.md's clean-room rule.

namespace ac3iab {

namespace {

[[nodiscard]] std::expected<std::vector<std::byte>, IabError> read_all(std::istream& in) {
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

// §5.1's default MSB-first/big-endian rule applies to PreambleLength/IAFrameLength (§8.1.2/
// §8.1.5) - unlike PCMData (§10.8.1), neither field carries a little-endian override.
[[nodiscard]] std::expected<std::uint32_t, IabError> read_be32(std::span<const std::byte> data) {
    if (data.size() < 4) {
        return std::unexpected(IabError::kTruncated);
    }
    return (std::to_integer<std::uint32_t>(data[0]) << 24) | (std::to_integer<std::uint32_t>(data[1]) << 16) |
           (std::to_integer<std::uint32_t>(data[2]) << 8) | std::to_integer<std::uint32_t>(data[3]);
}

}  // namespace

std::string_view describe(IabError error) {
    switch (error) {
        case IabError::kCannotOpen:
            return "could not open the input";
        case IabError::kTruncated:
            return "fewer bytes remained than a declared field, element or segment size";
        case IabError::kBadEscape:
            return "a Plex(n) escape chain exceeded this format's own 32-bit symbol ceiling";
        case IabError::kBadPreambleTag:
            return "PreambleTag was not 0x01";
        case IabError::kBadFrameTag:
            return "IAFrameTag was not 0x02, or the wrapped element was not itself an IA_FRAME";
        case IabError::kReservedVersion:
            return "IAFrame.Version was not 1 (0 and 2 are forbidden)";
        case IabError::kReservedSampleRate:
            return "SampleRate code was Reserved (0x2 or 0x3)";
        case IabError::kReservedBitDepth:
            return "BitDepth code was Reserved (0x2 or 0x3)";
        case IabError::kReservedFrameRate:
            return "FrameRate code was Reserved (0xA-0xF)";
        case IabError::kUnterminatedString:
            return "a NUL-terminated ASCII field ran off the end of its element without a terminator";
    }
    return "unknown ac3iab error";
}

std::expected<std::vector<IABitstreamFrame>, IabError> parse_iabitstream(std::istream& in) {
    auto bytes = read_all(in);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }

    std::vector<IABitstreamFrame> frames;
    std::span<const std::byte> remaining(*bytes);

    while (!remaining.empty()) {
        if (remaining.size() < 5) {
            return std::unexpected(IabError::kTruncated);
        }
        if (std::to_integer<std::uint8_t>(remaining[0]) != 0x01) {  // §8.1.1 PreambleTag
            return std::unexpected(IabError::kBadPreambleTag);
        }
        auto preamble_length = read_be32(remaining.subspan(1));  // §8.1.2
        if (!preamble_length) {
            return std::unexpected(preamble_length.error());
        }
        remaining = remaining.subspan(5);
        if (remaining.size() < *preamble_length) {
            return std::unexpected(IabError::kTruncated);
        }

        IABitstreamFrame entry;
        entry.preamble.assign(remaining.begin(), remaining.begin() + *preamble_length);  // §8.1.3
        remaining = remaining.subspan(*preamble_length);

        if (remaining.size() < 5) {
            return std::unexpected(IabError::kTruncated);
        }
        if (std::to_integer<std::uint8_t>(remaining[0]) != 0x02) {  // §8.1.4 IAFrameTag
            return std::unexpected(IabError::kBadFrameTag);
        }
        auto frame_length = read_be32(remaining.subspan(1));  // §8.1.5
        if (!frame_length) {
            return std::unexpected(frame_length.error());
        }
        remaining = remaining.subspan(5);
        if (remaining.size() < *frame_length) {
            return std::unexpected(IabError::kTruncated);
        }

        // §8.1.6's "IAFrame" field is itself one whole IAElement(IA_FRAME) (see this file's own
        // header comment) - strip its ElementID/ElementSize header (§9 Table 3) before handing
        // the inner payload to parse_iaframe(), which takes a bare payload.
        detail::BitReader element_header(remaining.subspan(0, *frame_length));
        auto element_id = element_header.read_plex(8);
        if (!element_id) {
            return std::unexpected(element_id.error());
        }
        constexpr std::uint32_t kIaFrameElementId = 0x08;  // §10.1.1 Table 14
        if (*element_id != kIaFrameElementId) {
            return std::unexpected(IabError::kBadFrameTag);
        }
        auto element_size = element_header.read_plex(8);
        if (!element_size) {
            return std::unexpected(element_size.error());
        }
        auto element_payload = element_header.read_bytes(static_cast<std::size_t>(*element_size));
        if (!element_payload) {
            return std::unexpected(element_payload.error());
        }

        auto frame = parse_iaframe(*element_payload);
        if (!frame) {
            return std::unexpected(frame.error());
        }
        entry.frame = std::move(*frame);
        remaining = remaining.subspan(*frame_length);

        frames.push_back(std::move(entry));
    }

    return frames;
}

std::expected<std::vector<IABitstreamFrame>, IabError> parse_iabitstream(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(IabError::kCannotOpen);
    }
    return parse_iabitstream(in);
}

}  // namespace ac3iab

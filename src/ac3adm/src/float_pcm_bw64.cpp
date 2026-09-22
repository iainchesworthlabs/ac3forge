#include "float_pcm_bw64.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3adm/model.hpp"

namespace ac3adm::detail {

namespace {

constexpr std::uint16_t kFormatIeeeFloat = 0x0003;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;
// EBU Tech 3306 §3.1 / BS.2088-1 §7: the 32-bit size a <ds64> 64-bit one
// supersedes.
constexpr std::uint32_t kSizeSentinel = 0xFFFFFFFFu;
// Enough of the file to hold the RIFF header, <ds64>, <fmt > and whatever
// metadata chunks a producer put ahead of them - the same 64 KiB window
// ac3::io::WavStreamReader settles on for the same reason.
constexpr std::size_t kHeaderWindow = 64 * 1024;
// BS.2088-1 §8.2: trackIndex (2) + UID (12) + trackRef (14) + packRef (11)
// + one pad byte.
constexpr std::size_t kChnaRecordBytes = 40;

[[nodiscard]] std::uint16_t read_u16(std::span<const char> data, std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[at]) |
                                      (static_cast<std::uint8_t>(data[at + 1]) << 8));
}

[[nodiscard]] std::uint32_t read_u32(std::span<const char> data, std::size_t at) {
    return static_cast<std::uint32_t>(read_u16(data, at)) |
           (static_cast<std::uint32_t>(read_u16(data, at + 2)) << 16);
}

[[nodiscard]] std::uint64_t read_u64(std::span<const char> data, std::size_t at) {
    return static_cast<std::uint64_t>(read_u32(data, at)) |
           (static_cast<std::uint64_t>(read_u32(data, at + 4)) << 32);
}

[[nodiscard]] std::string_view four_cc(std::span<const char> data, std::size_t at) {
    return at + 4 <= data.size() ? std::string_view{data.data() + at, 4} : std::string_view{};
}

[[nodiscard]] bool is_riff_wave(std::span<const char> data) {
    if (data.size() < 12) {
        return false;
    }
    const auto id = four_cc(data, 0);
    return (id == "RIFF" || id == "RF64" || id == "BW64") && four_cc(data, 8) == "WAVE";
}

struct Chunk {
    std::size_t payload_at = 0;
    std::uint64_t size = 0;
};

// A flat walk of the chunk list from offset 12. A 0xFFFFFFFF length is
// RF64's "the real size is in <ds64>" placeholder and cannot be stepped
// over, so the walk stops there - in practice only <data> carries it, and it
// is the last chunk in the file.
[[nodiscard]] std::optional<Chunk> find_chunk(std::span<const char> data, std::string_view tag) {
    for (std::size_t at = 12; at + 8 <= data.size();) {
        const auto id = four_cc(data, at);
        const auto declared = read_u32(data, at + 4);
        if (id == tag) {
            return Chunk{.payload_at = at + 8, .size = declared};
        }
        if (declared == kSizeSentinel) {
            break;
        }
        at += 8 + declared + (declared & 1u);  // RIFF chunks are word-aligned
    }
    return std::nullopt;
}

// The <fmt > formatTag, with WAVE_FORMAT_EXTENSIBLE unwrapped to the first
// two bytes of its SubFormat GUID.
[[nodiscard]] std::optional<std::uint16_t> format_tag(std::span<const char> data,
                                                      const Chunk& fmt) {
    if (fmt.payload_at + 16 > data.size()) {
        return std::nullopt;
    }
    const auto tag = read_u16(data, fmt.payload_at);
    if (tag != kFormatExtensible) {
        return tag;
    }
    if (fmt.payload_at + 26 > data.size()) {
        return std::nullopt;
    }
    return read_u16(data, fmt.payload_at + 24);
}

// Reads at most `limit` bytes from the head of the file. Returns an empty
// vector when the path cannot be opened at all.
[[nodiscard]] std::vector<char> read_head(const std::string& path, std::size_t limit) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return {};
    }
    std::vector<char> raw(limit);
    in.read(raw.data(), static_cast<std::streamsize>(raw.size()));
    raw.resize(static_cast<std::size_t>(in.gcount()));
    return raw;
}

// BS.2088-1 §8.2 pads an unused or "not required" ID field with NUL, and a
// producer may pad with spaces instead - the same two characters adm.cpp's
// own trim_padding() strips, for the same reason (see its comment there).
[[nodiscard]] std::string trimmed_field(std::span<const char> data, std::size_t at,
                                        std::size_t width) {
    std::string field{data.data() + at, width};
    static constexpr std::string_view kPaddingChars(" \0", 2);
    const auto last = field.find_last_not_of(kPaddingChars);
    field.resize(last == std::string::npos ? 0 : last + 1);
    return field;
}

[[nodiscard]] std::vector<ChnaEntry> read_chna(std::span<const char> data) {
    std::vector<ChnaEntry> entries;
    const auto chunk = find_chunk(data, "chna");
    if (!chunk || chunk->payload_at + 4 > data.size()) {
        return entries;
    }
    const auto uid_count = read_u16(data, chunk->payload_at + 2);
    for (std::size_t i = 0; i < uid_count; ++i) {
        const std::size_t at = chunk->payload_at + 4 + i * kChnaRecordBytes;
        if (at + kChnaRecordBytes > data.size()) {
            break;
        }
        ChnaEntry entry;
        entry.track_index = read_u16(data, at);
        entry.uid = trimmed_field(data, at + 2, 12);
        entry.track_ref = trimmed_field(data, at + 14, 14);
        entry.pack_ref = trimmed_field(data, at + 28, 11);
        entries.push_back(std::move(entry));
    }
    return entries;
}

}  // namespace

bool is_ieee_float_wave(const std::string& path) {
    const auto head = read_head(path, kHeaderWindow);
    const std::span<const char> data{head};
    if (!is_riff_wave(data)) {
        return false;
    }
    const auto fmt = find_chunk(data, "fmt ");
    if (!fmt) {
        return false;
    }
    const auto tag = format_tag(data, *fmt);
    return tag && *tag == kFormatIeeeFloat;
}

std::expected<AdmDocument, AdmError> parse_float_pcm_bw64(const std::string& path) {
    // Whole-file, like the libbw64 path this stands in for: PcmAudio is a
    // by-value planar buffer either way (ac3adm/model.hpp), so there is no
    // memory to save by streaming the source in.
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::unexpected(AdmError::kCannotOpen);
    }
    const std::vector<char> raw{std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>()};
    const std::span<const char> data{raw};
    if (!is_riff_wave(data)) {
        return std::unexpected(AdmError::kNotRiff);
    }
    const auto fmt = find_chunk(data, "fmt ");
    if (!fmt) {
        return std::unexpected(AdmError::kMissingFmt);
    }
    const auto data_chunk = find_chunk(data, "data");
    if (!data_chunk) {
        return std::unexpected(AdmError::kMissingData);
    }
    const auto tag = format_tag(data, *fmt);
    const auto channel_count = read_u16(data, fmt->payload_at + 2);
    const auto sample_rate = read_u32(data, fmt->payload_at + 4);
    const auto bits = read_u16(data, fmt->payload_at + 14);
    if (!tag || *tag != kFormatIeeeFloat || channel_count == 0 ||
        (bits != 32 && bits != 64)) {
        return std::unexpected(AdmError::kUnsupportedFormat);
    }

    std::uint64_t declared = data_chunk->size;
    if (declared == kSizeSentinel) {
        // BS.2088-1 §7: <ds64> carries riffSize, dataSize and sampleCount as
        // 64-bit words in that order.
        const auto ds64 = find_chunk(data, "ds64");
        if (ds64 && ds64->payload_at + 24 <= raw.size()) {
            declared = read_u64(data, ds64->payload_at + 8);
        }
    }
    const std::uint64_t available =
        raw.size() > data_chunk->payload_at ? raw.size() - data_chunk->payload_at : 0;
    const auto payload = static_cast<std::size_t>(std::min(declared, available));
    const std::size_t width = bits / 8;
    const std::size_t stride = static_cast<std::size_t>(channel_count) * width;
    const std::size_t frames = payload / stride;

    AdmDocument document;
    document.audio.sample_rate = sample_rate;
    document.audio.bits_per_sample = bits;
    document.audio.channels.assign(channel_count, std::vector<float>(frames));
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::uint16_t channel = 0; channel < channel_count; ++channel) {
            const std::size_t at =
                data_chunk->payload_at + frame * stride + static_cast<std::size_t>(channel) * width;
            document.audio.channels[channel][frame] =
                bits == 32 ? std::bit_cast<float>(read_u32(data, at))
                           : static_cast<float>(std::bit_cast<double>(read_u64(data, at)));
        }
    }
    document.chna = read_chna(data);

    // BS.2088-1 §9 rule 2: no <axml> chunk is still a valid file, just one
    // with an empty model - the same stance read_adm_model takes.
    const auto axml = find_chunk(data, "axml");
    if (!axml) {
        return document;
    }
    const auto xml_bytes = static_cast<std::size_t>(
        std::min<std::uint64_t>(axml->size, raw.size() - axml->payload_at));
    auto model = parse_axml(std::string{raw.data() + axml->payload_at, xml_bytes});
    if (!model) {
        return std::unexpected(model.error());
    }
    document.model = std::move(*model);
    return document;
}

}  // namespace ac3adm::detail

#include "wav_format.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

#include "ac3/io/wav.hpp"

namespace ac3::io::detail {

namespace {

constexpr std::uint16_t kFormatPcm = 0x0001;
constexpr std::uint16_t kFormatIeeeFloat = 0x0003;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;
// EBU Tech 3306 §3.1: the 32-bit size field a 64-bit one supersedes.
constexpr std::uint32_t kSizeSentinel = 0xFFFFFFFFu;

[[nodiscard]] std::string_view four_cc(std::span<const char> data, std::size_t at) {
    return at + 4 <= data.size() ? std::string_view{data.data() + at, 4} : std::string_view{};
}

}  // namespace

std::uint16_t read_u16(std::span<const char> data, std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[at]) |
                                      (static_cast<std::uint8_t>(data[at + 1]) << 8));
}

std::uint32_t read_u32(std::span<const char> data, std::size_t at) {
    return static_cast<std::uint32_t>(read_u16(data, at)) |
           (static_cast<std::uint32_t>(read_u16(data, at + 2)) << 16);
}

std::uint64_t read_u64(std::span<const char> data, std::size_t at) {
    return static_cast<std::uint64_t>(read_u32(data, at)) |
           (static_cast<std::uint64_t>(read_u32(data, at + 4)) << 32);
}

bool is_riff_wave(std::span<const char> data) {
    if (data.size() < 12) {
        return false;
    }
    const auto id = four_cc(data, 0);
    return (id == "RIFF" || id == "RF64" || id == "BW64") && four_cc(data, 8) == "WAVE";
}

bool is_rf64(std::span<const char> data) {
    const auto id = four_cc(data, 0);
    return id == "RF64" || id == "BW64";
}

std::optional<Chunk> find_chunk(std::span<const char> data, std::string_view tag) {
    // 12: past the RIFF/RF64 id, its size field and "WAVE".
    for (std::size_t at = 12; at + 8 <= data.size();) {
        const auto id = four_cc(data, at);
        const auto declared = read_u32(data, at + 4);
        if (id == tag) {
            return Chunk{.payload_at = at + 8, .size = declared};
        }
        if (declared == kSizeSentinel) {
            // Nothing to step over: this chunk's real length lives in ds64
            // and is not knowable from here without knowing which chunk it
            // describes. Stop rather than walk off into the payload.
            break;
        }
        // RIFF chunks are word-aligned: an odd payload carries a pad byte
        // that is not counted in the size field.
        at += 8 + declared + (declared & 1u);
    }
    // The pre-widening behaviour, kept as a fallback - see the header.
    const std::string_view view{data.data(), data.size()};
    const auto found = view.find(tag);
    if (found == std::string_view::npos || found + 8 > data.size()) {
        return std::nullopt;
    }
    return Chunk{.payload_at = found + 8, .size = read_u32(data, found + 4)};
}

std::expected<WavFormat, WavError> parse_format(std::span<const char> data, const Chunk& fmt) {
    // The common (PCMWAVEFORMAT) prefix is 16 bytes; EXTENSIBLE adds cbSize
    // and a 22-byte extension whose last 16 bytes are the SubFormat GUID.
    if (fmt.payload_at + 16 > data.size()) {
        return std::unexpected(WavError::kNotRiffWave);
    }
    auto tag = read_u16(data, fmt.payload_at);
    const auto channels = read_u16(data, fmt.payload_at + 2);
    const auto sample_rate = read_u32(data, fmt.payload_at + 4);
    const auto bits = read_u16(data, fmt.payload_at + 14);
    if (tag == kFormatExtensible) {
        // WAVE_FORMAT_EXTENSIBLE: the real tag is the first two bytes of the
        // SubFormat GUID in the extension. wValidBitsPerSample (payload+18)
        // is deliberately NOT consulted - it says how many of the container's
        // bits carry signal (20-in-24, say), which changes nothing about how
        // the container is read or scaled.
        if (fmt.payload_at + 26 > data.size()) {
            return std::unexpected(WavError::kUnsupportedFormat);
        }
        tag = read_u16(data, fmt.payload_at + 24);
    }
    if (channels == 0) {
        return std::unexpected(WavError::kUnsupportedFormat);
    }

    std::optional<SampleFormat> format;
    if (tag == kFormatPcm) {
        switch (bits) {
            case 8: format = SampleFormat::kPcm8; break;
            case 16: format = SampleFormat::kPcm16; break;
            case 24: format = SampleFormat::kPcm24; break;
            case 32: format = SampleFormat::kPcm32; break;
            default: break;  // 20-in-24 packed and friends: not carried
        }
    } else if (tag == kFormatIeeeFloat) {
        switch (bits) {
            case 32: format = SampleFormat::kFloat32; break;
            case 64: format = SampleFormat::kFloat64; break;
            default: break;
        }
    }
    if (!format) {
        return std::unexpected(WavError::kUnsupportedFormat);
    }

    WavFormat out;
    out.sample_rate = sample_rate;
    out.channels = channels;
    out.format = *format;
    out.stride = static_cast<std::size_t>(channels) * sample_bytes(*format);
    return out;
}

std::uint64_t data_chunk_size(std::span<const char> data, const Chunk& data_chunk) {
    if (data_chunk.size != kSizeSentinel || !is_rf64(data)) {
        return data_chunk.size;
    }
    // EBU Tech 3306 §3.1 / BS.2088-1 §7: ds64 carries riffSize, dataSize and
    // sampleCount as 64-bit words, in that order, at the top of its payload.
    const auto ds64 = find_chunk(data, "ds64");
    if (!ds64 || ds64->payload_at + 24 > data.size()) {
        return data_chunk.size;
    }
    return read_u64(data, ds64->payload_at + 8);
}

float convert_sample(std::span<const char> raw, std::size_t at, SampleFormat format) {
    switch (format) {
        case SampleFormat::kPcm8: {
            // Unsigned, biased: 0x80 is silence. Scaled by 128 so full-scale
            // negative reaches exactly -1.0, matching every other depth here.
            const auto value = static_cast<int>(static_cast<std::uint8_t>(raw[at])) - 128;
            return static_cast<float>(value) / 128.0f;
        }
        case SampleFormat::kPcm16: {
            const auto sample = static_cast<std::int16_t>(read_u16(raw, at));
            return static_cast<float>(sample) / 32768.0f;
        }
        case SampleFormat::kPcm24: {
            // Sign-extend the three little-endian bytes into 32 bits by
            // placing them in the HIGH three bytes and arithmetic-shifting
            // back down - no branch on the sign bit, and exact.
            const auto packed = static_cast<std::uint32_t>(
                (static_cast<std::uint32_t>(static_cast<std::uint8_t>(raw[at])) << 8) |
                (static_cast<std::uint32_t>(static_cast<std::uint8_t>(raw[at + 1])) << 16) |
                (static_cast<std::uint32_t>(static_cast<std::uint8_t>(raw[at + 2])) << 24));
            const auto sample = static_cast<std::int32_t>(packed) >> 8;
            return static_cast<float>(sample) / 8388608.0f;
        }
        case SampleFormat::kPcm32: {
            const auto sample = static_cast<std::int32_t>(read_u32(raw, at));
            // The divisor as a double, then one conversion to float: 2^31
            // does not fit a float's 24-bit significand exactly as a
            // divisor's reciprocal would need, and the whole point of a
            // 32-bit source is the bits below 24.
            return static_cast<float>(static_cast<double>(sample) / 2147483648.0);
        }
        case SampleFormat::kFloat32: {
            std::uint32_t bits = read_u32(raw, at);
            return std::bit_cast<float>(bits);
        }
        case SampleFormat::kFloat64: {
            std::uint64_t bits = read_u64(raw, at);
            return static_cast<float>(std::bit_cast<double>(bits));
        }
    }
    return 0.0f;
}

}  // namespace ac3::io::detail

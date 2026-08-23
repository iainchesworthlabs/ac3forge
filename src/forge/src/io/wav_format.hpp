#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

#include "ac3/io/wav.hpp"

// The RIFF/RF64 header walk and the sample conversion, shared by wav.cpp's
// whole-file readers and wav_stream_reader.cpp's block-at-a-time one.
//
// Those two used to carry the same parse twice, field for field, with a
// comment in each saying so - which held while "the format" meant PCM16 or
// float32 and nothing else. Widening both to the rest of the integer depths
// (8/24/32), to WAVE_FORMAT_EXTENSIBLE wrapping any of them, and to RF64/
// BW64's 64-bit sizes turns that duplication into two chances to disagree
// about what a file says, so the parse lives here once instead. Private to
// src/io/ - not installed, not part of ac3::io's public surface (see
// ac3/io/wav.hpp for that).

namespace ac3::io::detail {

// How samples sit in the data chunk. The container width in bytes is what
// the reader strides by; the interpretation is what it converts with.
enum class SampleFormat : std::uint8_t {
    kPcm8,     // WAVE_FORMAT_PCM, 8-bit - UNSIGNED, biased by 128 (§2 of the
               // Multimedia Programming Interface spec; the one integer depth
               // WAV does not store two's-complement)
    kPcm16,    // WAVE_FORMAT_PCM, 16-bit signed
    kPcm24,    // WAVE_FORMAT_PCM, 24-bit signed, three bytes little-endian
    kPcm32,    // WAVE_FORMAT_PCM, 32-bit signed
    kFloat32,  // WAVE_FORMAT_IEEE_FLOAT, 32-bit
    kFloat64,  // WAVE_FORMAT_IEEE_FLOAT, 64-bit
};

[[nodiscard]] constexpr std::size_t sample_bytes(SampleFormat format) {
    switch (format) {
        case SampleFormat::kPcm8: return 1;
        case SampleFormat::kPcm16: return 2;
        case SampleFormat::kPcm24: return 3;
        // 32-bit integer and 32-bit float share a container width and are
        // deliberately one case: the WIDTH is all this function reports, and
        // splitting them into two arms that return the same number is the
        // sort of duplication that drifts.
        case SampleFormat::kPcm32:
        case SampleFormat::kFloat32: return 4;
        case SampleFormat::kFloat64: return 8;
    }
    return 0;
}

struct WavFormat {
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    SampleFormat format = SampleFormat::kPcm16;
    // Bytes per interleaved frame: channels * sample_bytes(format).
    std::size_t stride = 0;
};

// One chunk located in a RIFF/RF64 file: where its payload starts and how
// long the header says it is.
struct Chunk {
    std::size_t payload_at = 0;
    std::uint64_t size = 0;
};

[[nodiscard]] std::uint16_t read_u16(std::span<const char> data, std::size_t at);
[[nodiscard]] std::uint32_t read_u32(std::span<const char> data, std::size_t at);
[[nodiscard]] std::uint64_t read_u64(std::span<const char> data, std::size_t at);

// True for "RIFF", "RF64" and "BW64" at offset 0 with "WAVE" at offset 8.
// RF64 (EBU Tech 3306) and BW64 (ITU-R BS.2088-1) are the same container
// with the same ds64 chunk; only the four-character id at offset 0 differs,
// so both are accepted wherever one is.
[[nodiscard]] bool is_riff_wave(std::span<const char> data);

// True when this file uses RF64/BW64's 64-bit sizes rather than RIFF's.
[[nodiscard]] bool is_rf64(std::span<const char> data);

// Walks the chunk list from offset 12 looking for `tag`. `window` may be the
// whole file or only its leading header window (WavStreamReader reads 64 KiB
// and no more), so a chunk whose payload runs past the end is still reported
// - the caller clamps.
//
// A declared size of 0xFFFFFFFF is RF64's "look in ds64" sentinel; the walk
// cannot step over such a chunk, so it stops there and reports it if it is
// the tag being looked for. In practice only `data` carries the sentinel and
// it is the last chunk in every RF64 file this can arise from.
//
// Falls back to a raw substring search when the walk finds nothing: that is
// what both readers did before this existed, and a file with one malformed
// chunk length ahead of `fmt `/`data` kept working under it. Keeping the
// fallback means widening the format support cannot narrow the set of files
// that already loaded.
[[nodiscard]] std::optional<Chunk> find_chunk(std::span<const char> data, std::string_view tag);

// The `fmt ` chunk's contents, with WAVE_FORMAT_EXTENSIBLE unwrapped to the
// SubFormat GUID's own tag. kUnsupportedFormat for a tag/width pair this
// reader does not carry, or for zero channels.
[[nodiscard]] std::expected<WavFormat, WavError> parse_format(std::span<const char> data,
                                                              const Chunk& fmt);

// The data chunk's real byte length: its own 32-bit size field, or the ds64
// chunk's 64-bit dataSize when this is an RF64/BW64 file (EBU Tech 3306 §3.1
// / BS.2088-1 §7 - the 32-bit field then reads 0xFFFFFFFF and is a
// placeholder, not a length). Clamped by the caller to what the file holds.
[[nodiscard]] std::uint64_t data_chunk_size(std::span<const char> data, const Chunk& data_chunk);

// One sample, normalized to [-1, 1). `at` is a byte offset into `raw`, and
// the caller has already checked that sample_bytes(format) bytes are there.
[[nodiscard]] float convert_sample(std::span<const char> raw, std::size_t at, SampleFormat format);

}  // namespace ac3::io::detail

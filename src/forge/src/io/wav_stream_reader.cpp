#include "ac3/io/wav.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <ios>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "wav_format.hpp"

// Separate translation unit from wav.cpp for the same reason
// wav_stream_writer.cpp is: the one-shot readers there hold the whole file
// in memory and parse it in one pass, while this is a stateful object
// keeping a file handle, a parse of the header, and a read position alive
// across many calls. The header walk and the sample conversion themselves
// are shared with wav.cpp (src/io/wav_format.hpp), so a block-at-a-time
// consumer sees exactly the samples read_wav produces.

namespace ac3::io {

namespace {

// The fmt and data chunks must sit inside this much of the file - see the
// class comment in wav.hpp. Every WAV this project writes puts them at
// fixed offsets 12 and 36; 64 KiB leaves room for the metadata chunks other
// producers put in front (an RF64/BW64 file's own ds64 chunk included, which
// by EBU Tech 3306 §3 sits immediately after "WAVE" and so is always well
// inside this window).
constexpr std::size_t kHeaderWindow = 64 * 1024;

}  // namespace

struct WavStreamReader::Impl {
    std::ifstream file;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    detail::SampleFormat format = detail::SampleFormat::kPcm16;
    std::size_t stride = 0;
    std::uint64_t frames_total = 0;
    std::uint64_t frames_read = 0;
    // Interleaved read scratch, reused across read_planar calls - its
    // capacity settles at one call's worth and stays there.
    std::vector<char> raw;
    bool open = false;
};

WavStreamReader::WavStreamReader() : impl_(std::make_unique<Impl>()) {}

WavStreamReader::~WavStreamReader() = default;

WavStreamReader::WavStreamReader(WavStreamReader&&) noexcept = default;
WavStreamReader& WavStreamReader::operator=(WavStreamReader&&) noexcept = default;

std::expected<void, WavError> WavStreamReader::open(const std::string& path) {
    close();
    auto& im = *impl_;
    im.file.open(path, std::ios::binary);
    if (!im.file) {
        return std::unexpected(WavError::kCannotOpen);
    }

    // Every early return past this point must release im.file: leaving it
    // open would keep the OS handle held (a Windows sharing violation on any
    // caller that reacts to the rejection by deleting/rewriting the file)
    // even though is_open() reports false.
    auto fail = [this](WavError err) -> std::expected<void, WavError> {
        close();
        return std::unexpected(err);
    };

    // File size first: the data chunk's declared size is clamped to what the
    // file actually holds, exactly as read_wav does, so a truncated capture
    // still yields its real frames rather than a kTruncated refusal.
    im.file.seekg(0, std::ios::end);
    const auto end_pos = im.file.tellg();
    if (end_pos < 0) {
        return fail(WavError::kCannotOpen);
    }
    const auto file_size = static_cast<std::uint64_t>(end_pos);
    im.file.seekg(0);

    im.raw.resize(static_cast<std::size_t>(std::min<std::uint64_t>(kHeaderWindow, file_size)));
    im.file.read(im.raw.data(), static_cast<std::streamsize>(im.raw.size()));
    if (im.file.gcount() != static_cast<std::streamsize>(im.raw.size())) {
        return fail(WavError::kCannotOpen);
    }

    // The same walk and the same format table wav.cpp's parse_wav uses
    // (src/io/wav_format.hpp), over the header window instead of the whole
    // file.
    const std::span<const char> bytes{im.raw};
    if (im.raw.size() < 44 || !detail::is_riff_wave(bytes)) {
        return fail(WavError::kNotRiffWave);
    }
    const auto fmt_chunk = detail::find_chunk(bytes, "fmt ");
    const auto data_chunk = detail::find_chunk(bytes, "data");
    if (!fmt_chunk || !data_chunk) {
        return fail(WavError::kNotRiffWave);
    }
    const auto format = detail::parse_format(bytes, *fmt_chunk);
    if (!format) {
        return fail(format.error());
    }
    im.sample_rate = format->sample_rate;
    im.channels = format->channels;
    im.format = format->format;
    im.stride = format->stride;

    const std::uint64_t declared = detail::data_chunk_size(bytes, *data_chunk);
    const std::uint64_t payload_at = data_chunk->payload_at;
    const std::uint64_t available = file_size > payload_at ? file_size - payload_at : 0;
    const std::uint64_t payload = std::min<std::uint64_t>(declared, available);
    im.frames_total = payload / im.stride;
    im.frames_read = 0;
    im.file.seekg(static_cast<std::streamoff>(payload_at));
    if (!im.file) {
        return fail(WavError::kCannotOpen);
    }
    im.open = true;
    return {};
}

bool WavStreamReader::is_open() const { return impl_->open; }

std::uint32_t WavStreamReader::sample_rate() const { return impl_->sample_rate; }

std::uint16_t WavStreamReader::channels() const { return impl_->channels; }

std::uint64_t WavStreamReader::frame_count() const { return impl_->frames_total; }

std::expected<std::size_t, WavError> WavStreamReader::read_planar(
    std::span<const std::span<float>> channels, std::size_t frames) {
    auto& im = *impl_;
    if (!im.open) {
        return std::unexpected(WavError::kCannotOpen);
    }
    if (channels.size() < im.channels) {
        return std::unexpected(WavError::kUnsupportedFormat);
    }
    const std::uint64_t remaining = im.frames_total - im.frames_read;
    const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(frames, remaining));
    if (n == 0) {
        return 0;
    }
    const std::size_t width = detail::sample_bytes(im.format);
    const std::size_t stride = im.stride;
    im.raw.resize(n * stride);
    im.file.read(im.raw.data(), static_cast<std::streamsize>(im.raw.size()));
    if (im.file.gcount() != static_cast<std::streamsize>(im.raw.size())) {
        // frames_total was clamped to the file's real size at open(), so a
        // short read here means the file shrank underneath us.
        return std::unexpected(WavError::kTruncated);
    }

    // The same sample conversion as wav.cpp's parse_wav - literally the same
    // function - so a block-at-a-time consumer reconstructs bit-identical
    // floats to the whole-file read.
    const std::span<const char> bytes{im.raw};
    for (std::size_t frame = 0; frame < n; ++frame) {
        for (std::uint16_t ch = 0; ch < im.channels; ++ch) {
            const std::size_t at = frame * stride + static_cast<std::size_t>(ch) * width;
            channels[ch][frame] = detail::convert_sample(bytes, at, im.format);
        }
    }
    im.frames_read += n;
    return n;
}

void WavStreamReader::close() {
    auto& im = *impl_;
    if (im.file.is_open()) {
        im.file.close();
    }
    im.open = false;
    im.frames_total = 0;
    im.frames_read = 0;
    im.channels = 0;
    im.sample_rate = 0;
    im.stride = 0;
    im.format = detail::SampleFormat::kPcm16;
}

}  // namespace ac3::io

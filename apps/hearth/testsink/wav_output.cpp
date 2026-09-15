#include "wav_output.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ac3/io/wav.hpp"
#include "ac3/sendspin/messages.hpp"

namespace ac3::hearth::testsink {

namespace {

namespace m = sendspin::messages;

[[nodiscard]] bool writable(const m::AudioFormat& format) {
    return format.codec == m::Codec::kPcm && format.channels > 0 && format.channels <= 32 && format.sample_rate > 0 &&
           (format.bit_depth == 16 || format.bit_depth == 24 || format.bit_depth == 32);
}

// One little-endian signed sample of `bytes` bytes, as a float in [-1, 1).
[[nodiscard]] float sample_at(std::span<const std::uint8_t> frame, std::size_t offset, std::size_t bytes) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
        value |= std::uint32_t{frame[offset + i]} << (8U * i);
    }
    // Sign-extend from the sample's width to 32 bits.
    const unsigned shift = 32U - (8U * static_cast<unsigned>(bytes));
    const auto extended = static_cast<std::int32_t>(value << shift) >> shift;
    const double scale = static_cast<double>(std::uint64_t{1} << ((8U * bytes) - 1U));
    return static_cast<float>(static_cast<double>(extended) / scale);
}

}  // namespace

WavOutput::WavOutput(std::filesystem::path directory, std::string prefix)
    : directory_(std::move(directory)), prefix_(std::move(prefix)) {}

WavOutput::~WavOutput() {
    end();
}

bool WavOutput::start(const m::PlayerStream& stream) {
    end();
    ++streams_;
    stream_frames_ = 0;
    if (!writable(stream.format)) {
        format_.reset();
        return false;
    }
    format_ = stream.format;
    if (directory_.empty()) {
        return true;
    }
    file_ = directory_ / (prefix_ + "-" + std::to_string(streams_) + ".wav");
    if (!writer_.open(file_.string(), static_cast<std::uint32_t>(stream.format.sample_rate),
                      static_cast<std::uint16_t>(stream.format.channels))) {
        format_.reset();
        return false;
    }
    log_.open(std::filesystem::path(file_).replace_extension(".times.csv"), std::ios::trunc);
    log_ << "local_time_us,first_frame,frames\n";
    return true;
}

void WavOutput::clear() {
    if (log_.is_open()) {
        log_ << "clear," << stream_frames_ << "\n";
    }
}

void WavOutput::end() {
    writer_.close();
    if (log_.is_open()) {
        log_.close();
    }
}

void WavOutput::write(std::span<const std::uint8_t> frame, std::int64_t local_time) {
    ++chunks_;
    if (!format_) {
        return;
    }
    const auto channels = static_cast<std::size_t>(format_->channels);
    const auto bytes = static_cast<std::size_t>(format_->bit_depth / 8);
    const std::size_t frame_count = frame.size() / (channels * bytes);
    frames_ += frame_count;
    if (log_.is_open()) {
        log_ << local_time << "," << stream_frames_ << "," << frame_count << "\n";
    }
    stream_frames_ += frame_count;
    if (!writer_.is_open()) {
        return;
    }
    samples_.resize(frame_count * channels);
    for (std::size_t i = 0; i < samples_.size(); ++i) {
        samples_[i] = sample_at(frame, i * bytes, bytes);
    }
    (void)writer_.write(samples_);
}

}  // namespace ac3::hearth::testsink

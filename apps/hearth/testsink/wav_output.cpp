#include "wav_output.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ac3/io/wav.hpp"
#include "ac3/sendspin/codec.hpp"
#include "ac3/sendspin/messages.hpp"

namespace ac3::hearth::testsink {

WavOutput::WavOutput(std::filesystem::path directory, std::string prefix)
    : directory_(std::move(directory)), prefix_(std::move(prefix)) {}

WavOutput::~WavOutput() {
    end();
}

bool WavOutput::start(const sendspin::messages::PlayerStream& stream) {
    end();
    ++streams_;
    stream_frames_ = 0;
    decoder_ = sendspin::codec::make_decoder(stream);
    if (!decoder_ || stream.format.channels < 1 || stream.format.sample_rate < 1) {
        decoder_.reset();
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
        decoder_.reset();
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
    if (!decoder_ || !format_) {
        return;
    }
    const std::optional<std::vector<std::int32_t>> decoded = decoder_->decode(frame);
    if (!decoded) {
        ++undecodable_;
        return;
    }
    const auto channels = static_cast<std::size_t>(format_->channels);
    const std::size_t frame_count = decoded->size() / channels;
    frames_ += frame_count;
    if (log_.is_open()) {
        log_ << local_time << "," << stream_frames_ << "," << frame_count << "\n";
    }
    stream_frames_ += frame_count;
    if (!writer_.is_open()) {
        return;
    }
    const double scale = std::ldexp(1.0, decoder_->bit_depth() - 1);
    samples_.resize(frame_count * channels);
    for (std::size_t i = 0; i < samples_.size(); ++i) {
        samples_[i] = static_cast<float>(static_cast<double>((*decoded)[i]) / scale);
    }
    (void)writer_.write(samples_);
}

}  // namespace ac3::hearth::testsink

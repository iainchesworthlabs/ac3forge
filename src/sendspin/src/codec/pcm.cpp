#include "codecs.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "ac3/sendspin/codec.hpp"
#include "ac3/sendspin/messages.hpp"

namespace ac3::sendspin::codec {

namespace {

// Little-endian signed samples of `bytes` bytes each (roles/player/v1.md, PCM Encoding Convention).
class PcmEncoder final : public Encoder {
   public:
    PcmEncoder(std::int32_t channels, std::int32_t bytes, std::int32_t frames)
        : channels_(static_cast<std::size_t>(channels)),
          bytes_(static_cast<std::size_t>(bytes)),
          frames_(static_cast<std::size_t>(frames)) {}

    [[nodiscard]] const std::vector<std::uint8_t>& codec_header() const override { return header_; }
    [[nodiscard]] std::int32_t delay_frames() const override { return 0; }

    [[nodiscard]] std::optional<std::vector<Unit>> encode(std::span<const std::int32_t> interleaved) override {
        if (interleaved.size() % channels_ != 0) {
            return std::nullopt;
        }
        held_.insert(held_.end(), interleaved.begin(), interleaved.end());
        std::vector<Unit> units;
        const std::size_t samples = frames_ * channels_;
        std::size_t used = 0;
        while (held_.size() - used >= samples) {
            units.push_back(unit(std::span<const std::int32_t>(held_).subspan(used, samples)));
            used += samples;
        }
        held_.erase(held_.begin(), held_.begin() + static_cast<std::ptrdiff_t>(used));
        return units;
    }

    [[nodiscard]] std::optional<std::vector<Unit>> finish() override {
        std::vector<Unit> units;
        if (!held_.empty()) {
            units.push_back(unit(held_));
            held_.clear();
        }
        return units;
    }

   private:
    [[nodiscard]] Unit unit(std::span<const std::int32_t> samples) {
        Unit out{.first_frame = position_, .bytes = {}};
        out.bytes.reserve(samples.size() * bytes_);
        for (const std::int32_t sample : samples) {
            const auto bits = static_cast<std::uint32_t>(sample);
            for (std::size_t b = 0; b < bytes_; ++b) {
                out.bytes.push_back(static_cast<std::uint8_t>((bits >> (8U * b)) & 0xFFU));
            }
        }
        position_ += static_cast<std::int64_t>(samples.size() / channels_);
        return out;
    }

    std::size_t channels_;
    std::size_t bytes_;
    std::size_t frames_;
    std::vector<std::uint8_t> header_;
    std::vector<std::int32_t> held_;
    std::int64_t position_ = 0;
};

class PcmDecoder final : public Decoder {
   public:
    PcmDecoder(std::int32_t channels, std::int32_t bit_depth)
        : channels_(static_cast<std::size_t>(channels)), bit_depth_(bit_depth) {}

    [[nodiscard]] std::optional<std::vector<std::int32_t>> decode(std::span<const std::uint8_t> unit) override {
        const auto bytes = static_cast<std::size_t>(bit_depth_ / 8);
        if (unit.size() % (bytes * channels_) != 0) {
            return std::nullopt;
        }
        std::vector<std::int32_t> samples(unit.size() / bytes);
        const unsigned shift = 32U - static_cast<unsigned>(bit_depth_);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            std::uint32_t bits = 0;
            for (std::size_t b = 0; b < bytes; ++b) {
                bits |= std::uint32_t{unit[(i * bytes) + b]} << (8U * b);
            }
            // Sign-extended from the sample's width.
            samples[i] = static_cast<std::int32_t>(bits << shift) >> shift;
        }
        return samples;
    }

    [[nodiscard]] std::int32_t bit_depth() const override { return bit_depth_; }

   private:
    std::size_t channels_;
    std::int32_t bit_depth_;
};

[[nodiscard]] bool pcm_depth(std::int32_t bit_depth) {
    return bit_depth == 16 || bit_depth == 24 || bit_depth == 32;
}

}  // namespace

std::unique_ptr<Encoder> make_pcm_encoder(const messages::AudioFormat& format, const EncoderOptions& options) {
    if (!pcm_depth(format.bit_depth) || format.channels < 1 || format.sample_rate < 1) {
        return nullptr;
    }
    const std::int32_t frames = options.frames > 0 ? options.frames : format.sample_rate / 50;
    return std::make_unique<PcmEncoder>(format.channels, format.bit_depth / 8, frames > 0 ? frames : 1);
}

std::unique_ptr<Decoder> make_pcm_decoder(const messages::AudioFormat& format) {
    if (!pcm_depth(format.bit_depth) || format.channels < 1) {
        return nullptr;
    }
    return std::make_unique<PcmDecoder>(format.channels, format.bit_depth);
}

}  // namespace ac3::sendspin::codec

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <opus.h>

#include "../codecs.hpp"
#include "ac3/sendspin/codec.hpp"
#include "ac3/sendspin/messages.hpp"

// Opus for player@v1: one 20 ms packet a unit, no header, configured from the format's rate and
// channels (roles/player/v1.md, Codec framing).

namespace ac3::sendspin::codec {

namespace {

// Opus's largest packet, and its longest frame at 48 kHz (120 ms).
constexpr std::size_t kMaxPacket = 4000;
constexpr std::size_t kMaxFrames = 5760;

[[nodiscard]] bool opus_rate(std::int32_t sample_rate) {
    return sample_rate == 8000 || sample_rate == 12000 || sample_rate == 16000 || sample_rate == 24000 ||
           sample_rate == 48000;
}

class OpusEncoderUnit final : public Encoder {
   public:
    OpusEncoderUnit() = default;
    ~OpusEncoderUnit() override {
        if (encoder_ != nullptr) {
            opus_encoder_destroy(encoder_);
        }
    }
    OpusEncoderUnit(const OpusEncoderUnit&) = delete;
    OpusEncoderUnit& operator=(const OpusEncoderUnit&) = delete;
    OpusEncoderUnit(OpusEncoderUnit&&) = delete;
    OpusEncoderUnit& operator=(OpusEncoderUnit&&) = delete;

    [[nodiscard]] bool open(const messages::AudioFormat& format, std::int32_t bitrate) {
        int error = OPUS_OK;
        encoder_ = opus_encoder_create(format.sample_rate, format.channels, OPUS_APPLICATION_AUDIO, &error);
        if (encoder_ == nullptr || error != OPUS_OK ||
            opus_encoder_ctl(encoder_, OPUS_SET_BITRATE_REQUEST, static_cast<opus_int32>(bitrate)) != OPUS_OK) {
            return false;
        }
        opus_int32 lookahead = 0;
        if (opus_encoder_ctl(encoder_, OPUS_GET_LOOKAHEAD_REQUEST, &lookahead) != OPUS_OK) {
            return false;
        }
        delay_ = lookahead;
        channels_ = static_cast<std::size_t>(format.channels);
        frame_ = static_cast<std::size_t>(format.sample_rate / 50);
        return true;
    }

    [[nodiscard]] const std::vector<std::uint8_t>& codec_header() const override { return header_; }
    [[nodiscard]] std::int32_t delay_frames() const override { return delay_; }

    [[nodiscard]] std::optional<std::vector<Unit>> encode(std::span<const std::int32_t> interleaved) override {
        if (interleaved.size() % channels_ != 0) {
            return std::nullopt;
        }
        for (const std::int32_t sample : interleaved) {
            held_.push_back(static_cast<opus_int16>(std::clamp(sample, -32768, 32767)));
        }
        std::vector<Unit> units;
        const std::size_t samples = frame_ * channels_;
        std::size_t used = 0;
        while (held_.size() - used >= samples) {
            std::optional<Unit> packet = encode_frame(std::span<const opus_int16>(held_).subspan(used, samples));
            if (!packet) {
                return std::nullopt;
            }
            units.push_back(std::move(*packet));
            used += samples;
        }
        held_.erase(held_.begin(), held_.begin() + static_cast<std::ptrdiff_t>(used));
        return units;
    }

    [[nodiscard]] std::optional<std::vector<Unit>> finish() override {
        std::vector<Unit> units;
        if (!held_.empty()) {
            held_.resize(frame_ * channels_, 0);
            std::optional<Unit> packet = encode_frame(held_);
            if (!packet) {
                return std::nullopt;
            }
            units.push_back(std::move(*packet));
            held_.clear();
        }
        return units;
    }

   private:
    [[nodiscard]] std::optional<Unit> encode_frame(std::span<const opus_int16> samples) {
        std::vector<std::uint8_t> packet(kMaxPacket);
        const opus_int32 length = opus_encode(encoder_, samples.data(), static_cast<int>(frame_), packet.data(),
                                              static_cast<opus_int32>(packet.size()));
        if (length < 0) {
            return std::nullopt;
        }
        packet.resize(static_cast<std::size_t>(length));
        Unit unit{.first_frame = position_, .bytes = std::move(packet)};
        position_ += static_cast<std::int64_t>(frame_);
        return unit;
    }

    OpusEncoder* encoder_ = nullptr;
    std::int32_t delay_ = 0;
    std::size_t channels_ = 1;
    std::size_t frame_ = 960;
    std::vector<std::uint8_t> header_;
    std::vector<opus_int16> held_;
    std::int64_t position_ = 0;
};

class OpusDecoderUnit final : public Decoder {
   public:
    OpusDecoderUnit() = default;
    ~OpusDecoderUnit() override {
        if (decoder_ != nullptr) {
            opus_decoder_destroy(decoder_);
        }
    }
    OpusDecoderUnit(const OpusDecoderUnit&) = delete;
    OpusDecoderUnit& operator=(const OpusDecoderUnit&) = delete;
    OpusDecoderUnit(OpusDecoderUnit&&) = delete;
    OpusDecoderUnit& operator=(OpusDecoderUnit&&) = delete;

    [[nodiscard]] bool open(const messages::AudioFormat& format) {
        int error = OPUS_OK;
        decoder_ = opus_decoder_create(format.sample_rate, format.channels, &error);
        channels_ = static_cast<std::size_t>(format.channels);
        // 120 ms at the stream's rate.
        max_frames_ = std::min(kMaxFrames, static_cast<std::size_t>(format.sample_rate) * 120 / 1000);
        return decoder_ != nullptr && error == OPUS_OK;
    }

    [[nodiscard]] std::optional<std::vector<std::int32_t>> decode(std::span<const std::uint8_t> unit) override {
        if (unit.empty()) {
            return std::nullopt;
        }
        std::vector<opus_int16> pcm(max_frames_ * channels_);
        const int frames = opus_decode(decoder_, unit.data(), static_cast<opus_int32>(unit.size()), pcm.data(),
                                       static_cast<int>(max_frames_), 0);
        if (frames < 0) {
            return std::nullopt;
        }
        return std::vector<std::int32_t>(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(frames) * channels_));
    }

    [[nodiscard]] std::int32_t bit_depth() const override { return 16; }

   private:
    OpusDecoder* decoder_ = nullptr;
    std::size_t channels_ = 1;
    std::size_t max_frames_ = kMaxFrames;
};

}  // namespace

std::unique_ptr<Encoder> make_opus_encoder(const messages::AudioFormat& format, const EncoderOptions& options) {
    if (!opus_rate(format.sample_rate) || format.channels < 1 || format.channels > 2) {
        return nullptr;
    }
    auto encoder = std::make_unique<OpusEncoderUnit>();
    const std::int32_t bitrate = options.opus_bitrate > 0 ? options.opus_bitrate : 64000 * format.channels;
    if (!encoder->open(format, bitrate)) {
        return nullptr;
    }
    return encoder;
}

std::unique_ptr<Decoder> make_opus_decoder(const messages::AudioFormat& format) {
    if (!opus_rate(format.sample_rate) || format.channels < 1 || format.channels > 2) {
        return nullptr;
    }
    auto decoder = std::make_unique<OpusDecoderUnit>();
    if (!decoder->open(format)) {
        return nullptr;
    }
    return decoder;
}

}  // namespace ac3::sendspin::codec

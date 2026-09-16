#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <FLAC/format.h>
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>

#include "../codecs.hpp"
#include "ac3/sendspin/codec.hpp"
#include "ac3/sendspin/messages.hpp"

// FLAC for player@v1 over libFLAC: each unit is one FLAC frame, and codec_header is the fLaC
// marker with the STREAMINFO block (roles/player/v1.md, Codec framing).

namespace ac3::sendspin::codec {

namespace {

constexpr std::size_t kMarkerBytes = 4;
constexpr std::size_t kStreamInfoBytes = 4 + 34;
// The streamable subset's largest block at up to 48 kHz.
constexpr std::int32_t kSubsetBlock = 4096;

[[nodiscard]] bool flac_depth(std::int32_t bit_depth) {
    return bit_depth == 16 || bit_depth == 24 || bit_depth == 32;
}

class FlacEncoder final : public Encoder {
   public:
    FlacEncoder() = default;
    ~FlacEncoder() override {
        if (encoder_ != nullptr) {
            FLAC__stream_encoder_delete(encoder_);
        }
    }
    FlacEncoder(const FlacEncoder&) = delete;
    FlacEncoder& operator=(const FlacEncoder&) = delete;
    FlacEncoder(FlacEncoder&&) = delete;
    FlacEncoder& operator=(FlacEncoder&&) = delete;

    [[nodiscard]] bool open(const messages::AudioFormat& format, std::int32_t block) {
        encoder_ = FLAC__stream_encoder_new();
        block_ = block;
        channels_ = static_cast<std::size_t>(format.channels);
        if (encoder_ == nullptr ||
            FLAC__stream_encoder_set_channels(encoder_, static_cast<unsigned>(format.channels)) == 0 ||
            FLAC__stream_encoder_set_bits_per_sample(encoder_, static_cast<unsigned>(format.bit_depth)) == 0 ||
            FLAC__stream_encoder_set_sample_rate(encoder_, static_cast<unsigned>(format.sample_rate)) == 0 ||
            FLAC__stream_encoder_set_blocksize(encoder_, static_cast<unsigned>(block)) == 0 ||
            FLAC__stream_encoder_set_compression_level(encoder_, 5) == 0 ||
            FLAC__stream_encoder_set_streamable_subset(encoder_, format.bit_depth <= 24 ? 1 : 0) == 0 ||
            FLAC__stream_encoder_set_verify(encoder_, 0) == 0) {
            return false;
        }
        if (FLAC__stream_encoder_init_stream(encoder_, &FlacEncoder::write, nullptr, nullptr, nullptr, this) !=
            FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
            return false;
        }
        // What init wrote: the marker, STREAMINFO and any further metadata. The header a
        // player needs is the marker and STREAMINFO alone, marked as the last block.
        if (metadata_.size() < kMarkerBytes + kStreamInfoBytes) {
            return false;
        }
        header_.assign(metadata_.begin(), metadata_.begin() + static_cast<std::ptrdiff_t>(kMarkerBytes + kStreamInfoBytes));
        header_[kMarkerBytes] = static_cast<std::uint8_t>(header_[kMarkerBytes] | 0x80U);
        return true;
    }

    [[nodiscard]] const std::vector<std::uint8_t>& codec_header() const override { return header_; }
    [[nodiscard]] std::int32_t delay_frames() const override { return 0; }

    [[nodiscard]] std::optional<std::vector<Unit>> encode(std::span<const std::int32_t> interleaved) override {
        if (interleaved.size() % channels_ != 0) {
            return std::nullopt;
        }
        const auto frames = static_cast<unsigned>(interleaved.size() / channels_);
        if (frames > 0 && FLAC__stream_encoder_process_interleaved(encoder_, interleaved.data(), frames) == 0) {
            return std::nullopt;
        }
        return std::exchange(ready_, {});
    }

    [[nodiscard]] std::optional<std::vector<Unit>> finish() override {
        if (FLAC__stream_encoder_finish(encoder_) == 0) {
            return std::nullopt;
        }
        return std::exchange(ready_, {});
    }

   private:
    static FLAC__StreamEncoderWriteStatus write(const FLAC__StreamEncoder* /*encoder*/, const FLAC__byte buffer[],
                                                size_t bytes, uint32_t samples, uint32_t current_frame, void* self) {
        auto* encoder = static_cast<FlacEncoder*>(self);
        const std::span<const FLAC__byte> written(buffer, bytes);
        if (samples == 0) {
            encoder->metadata_.insert(encoder->metadata_.end(), written.begin(), written.end());
        } else {
            encoder->ready_.push_back({.first_frame = static_cast<std::int64_t>(current_frame) * encoder->block_,
                                       .bytes = {written.begin(), written.end()}});
        }
        return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
    }

    FLAC__StreamEncoder* encoder_ = nullptr;
    std::int32_t block_ = 0;
    std::size_t channels_ = 1;
    std::vector<std::uint8_t> metadata_;
    std::vector<std::uint8_t> header_;
    std::vector<Unit> ready_;
};

// libFLAC's decoder pulls its input; each unit is handed to it whole, and the decoder is told the
// input has ended once it asks for more, then flushed to wait for the next unit.
class FlacDecoder final : public Decoder {
   public:
    FlacDecoder() = default;
    ~FlacDecoder() override {
        if (decoder_ != nullptr) {
            FLAC__stream_decoder_delete(decoder_);
        }
    }
    FlacDecoder(const FlacDecoder&) = delete;
    FlacDecoder& operator=(const FlacDecoder&) = delete;
    FlacDecoder(FlacDecoder&&) = delete;
    FlacDecoder& operator=(FlacDecoder&&) = delete;

    [[nodiscard]] bool open(const messages::PlayerStream& stream) {
        if (stream.codec_header.size() < kMarkerBytes + kStreamInfoBytes) {
            return false;
        }
        decoder_ = FLAC__stream_decoder_new();
        if (decoder_ == nullptr ||
            FLAC__stream_decoder_init_stream(decoder_, &FlacDecoder::read, nullptr, nullptr, nullptr, nullptr,
                                             &FlacDecoder::write, &FlacDecoder::metadata, &FlacDecoder::error,
                                             this) != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
            return false;
        }
        input_ = stream.codec_header;
        if (FLAC__stream_decoder_process_until_end_of_metadata(decoder_) == 0 || failed_ || !stream_info_) {
            return false;
        }
        return settle();
    }

    [[nodiscard]] std::optional<std::vector<std::int32_t>> decode(std::span<const std::uint8_t> unit) override {
        input_.assign(unit.begin(), unit.end());
        offset_ = 0;
        output_.clear();
        failed_ = false;
        // No FLAC frame is under 8 bytes, which bounds the steps a malformed unit can take.
        for (std::size_t steps = 0; steps <= (unit.size() / 8) + 4; ++steps) {
            if (FLAC__stream_decoder_get_state(decoder_) == FLAC__STREAM_DECODER_END_OF_STREAM) {
                break;
            }
            if (FLAC__stream_decoder_process_single(decoder_) == 0 || failed_) {
                (void)FLAC__stream_decoder_flush(decoder_);
                return std::nullopt;
            }
        }
        if (!settle()) {
            return std::nullopt;
        }
        return std::exchange(output_, {});
    }

    [[nodiscard]] std::int32_t bit_depth() const override { return bit_depth_; }

   private:
    // Back to waiting for a frame once the input has run out.
    [[nodiscard]] bool settle() {
        if (FLAC__stream_decoder_get_state(decoder_) == FLAC__STREAM_DECODER_END_OF_STREAM) {
            return FLAC__stream_decoder_flush(decoder_) != 0;
        }
        return true;
    }

    static FLAC__StreamDecoderReadStatus read(const FLAC__StreamDecoder* /*decoder*/, FLAC__byte buffer[], size_t* bytes,
                                              void* self) {
        auto* decoder = static_cast<FlacDecoder*>(self);
        const std::size_t available = decoder->input_.size() - decoder->offset_;
        if (available == 0) {
            *bytes = 0;
            return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
        }
        const std::size_t count = std::min(*bytes, available);
        std::copy_n(decoder->input_.begin() + static_cast<std::ptrdiff_t>(decoder->offset_), count, buffer);
        decoder->offset_ += count;
        *bytes = count;
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }

    static FLAC__StreamDecoderWriteStatus write(const FLAC__StreamDecoder* /*decoder*/, const FLAC__Frame* frame,
                                                const FLAC__int32* const buffer[], void* self) {
        auto* decoder = static_cast<FlacDecoder*>(self);
        const std::size_t channels = frame->header.channels;
        const std::size_t samples = frame->header.blocksize;
        const std::span<const FLAC__int32* const> planes(buffer, channels);
        for (std::size_t i = 0; i < samples; ++i) {
            for (std::size_t c = 0; c < channels; ++c) {
                decoder->output_.push_back(planes[c][i]);
            }
        }
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

    static void metadata(const FLAC__StreamDecoder* /*decoder*/, const FLAC__StreamMetadata* block, void* self) {
        auto* decoder = static_cast<FlacDecoder*>(self);
        if (block->type == FLAC__METADATA_TYPE_STREAMINFO) {
            decoder->stream_info_ = true;
            decoder->bit_depth_ = static_cast<std::int32_t>(block->data.stream_info.bits_per_sample);
        }
    }

    static void error(const FLAC__StreamDecoder* /*decoder*/, FLAC__StreamDecoderErrorStatus /*status*/, void* self) {
        static_cast<FlacDecoder*>(self)->failed_ = true;
    }

    FLAC__StreamDecoder* decoder_ = nullptr;
    std::vector<std::uint8_t> input_;
    std::size_t offset_ = 0;
    std::vector<std::int32_t> output_;
    std::int32_t bit_depth_ = 16;
    bool stream_info_ = false;
    bool failed_ = false;
};

}  // namespace

std::unique_ptr<Encoder> make_flac_encoder(const messages::AudioFormat& format, const EncoderOptions& options) {
    if (!flac_depth(format.bit_depth) || format.channels < 1 || format.channels > 8 || format.sample_rate < 1) {
        return nullptr;
    }
    const std::int32_t block =
        options.frames > 0 ? options.frames : std::clamp(format.sample_rate / 10, 16, kSubsetBlock);
    auto encoder = std::make_unique<FlacEncoder>();
    if (!encoder->open(format, block)) {
        return nullptr;
    }
    return encoder;
}

std::unique_ptr<Decoder> make_flac_decoder(const messages::PlayerStream& stream) {
    auto decoder = std::make_unique<FlacDecoder>();
    if (!decoder->open(stream)) {
        return nullptr;
    }
    return decoder;
}

}  // namespace ac3::sendspin::codec

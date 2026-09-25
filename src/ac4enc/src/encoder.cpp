#include "ac4enc/encoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

#include "asf/analysis.hpp"
#include "asf/coder.hpp"
#include "asf/layout.hpp"
#include "asf/psycho.hpp"
#include "asf/stereo.hpp"
#include "bit_writer.hpp"
#include "frame/frame_writer.hpp"

namespace ac4 {

std::string_view describe(EncodeError error) {
    switch (error) {
        case EncodeError::kInvalidConfig:
            return "the configuration is not one this encoder writes";
        case EncodeError::kInvalidInput:
            return "the input does not match the configuration, or holds a sample that is not finite";
    }
    return "unknown error";
}

namespace {

using detail::BitWriter;
using detail::FrameLayout;

constexpr int kFrameLength = 2048;  // frame_rate_index 13, at 44.1 and 48 kHz
// The silence ahead of the input: the frame's long window starts at its first
// output sample, so a frame codes input from half a frame before it to half a
// frame after it, and this delay puts the next frame's transients, which set
// the frame's last window, inside the input read before the frame is coded.
// A frame and a half, which is also what DEE's encoder gives.
constexpr int kDelay = kFrameLength * 3 / 2;
// The decoder's delay at index 13, which flush() codes enough frames to
// cover: d_pcm (Part 1 Table 188), the QMF banks' 577 samples and the six
// QMF slots the synthesis works behind (5.7.1).
constexpr int kDecoderDelay = 352 + 577 + 6 * 64;
constexpr int kSubBlocks = 16;      // transient detection, a sixteenth of a frame each
constexpr int kSubBlock = kFrameLength / kSubBlocks;
constexpr double kAttackRatio = 10.0;     // 10 dB over the sub-blocks before
constexpr double kAttackFloor = 1e-7;     // per sample: nothing below -70 dBFS is an attack
constexpr int kLargestOffset = 255;
// The rate loop's steps below the thresholds, and what one is worth: a scale
// factor step, 2^(1/4) on the quantiser's step, moves its noise by 2^(3/8).
constexpr int kLowestStep = -120;
constexpr double kStepDb = 1.1289;
// Below the thresholds, how far towards the level a band's allowance is taken,
// in dB: 1 holds every band to the level, which gives the most SNR for the
// bits and leaves the quietest bands at their thresholds; 0 lowers every band
// together. Measured on the race's sources, 2026-09-25.
constexpr double kLevelWeight = 0.75;

// The bandwidth coded, by bit rate per channel.
[[nodiscard]] double cutoff_hz(double kbps_per_channel) {
    if (kbps_per_channel >= 96.0) {
        return 20000.0;
    }
    if (kbps_per_channel >= 64.0) {
        return 16000.0;
    }
    if (kbps_per_channel >= 48.0) {
        return 14000.0;
    }
    return 11000.0;
}

// The first bands of a transform whose lines start below `cutoff`.
[[nodiscard]] int bands_below(int transform_length, double cutoff, int sample_rate) {
    const std::span<const std::uint16_t> offsets = detail::band_offsets(transform_length);
    const double line_hz = static_cast<double>(sample_rate) / (2.0 * transform_length);
    int bands = 0;
    while (bands + 1 < static_cast<int>(offsets.size()) &&
           static_cast<double>(offsets[static_cast<std::size_t>(bands)]) * line_hz < cutoff) {
        ++bands;
    }
    return std::min(bands, (1 << detail::max_sfb_bits(transform_length)) - 1);
}

// sequence_counter: 0 in the first frame (Part 1 Annex E.1), then 1 to 1020
// and round again from 1 (Part 1 clause 4.3.3.2.2).
[[nodiscard]] int sequence_counter(std::int64_t frame) {
    return frame == 0 ? 0 : static_cast<int>((frame - 1) % 1020) + 1;
}

}  // namespace

struct Encoder::Impl {
    EncoderConfig config{};
    detail::Analysis analysis{kFrameLength, 1};
    detail::Psychoacoustics psycho{48000, kFrameLength};
    Toc toc{};
    int fs_index = 1;
    int dialnorm_bits = 124;
    double cutoff = 20000.0;
    double bytes_per_frame = 0.0;
    double byte_carry = 0.0;

    // The input, from sample index `base` of the delayed signal on; the first
    // kDelay samples of that signal are the silence ahead of the input.
    std::vector<std::vector<double>> signal;
    std::int64_t base = 0;
    std::int64_t input_samples = 0;
    bool flushed = false;

    std::int64_t frames_out = 0;
    std::deque<FrameLayout> layouts;  // decided, for frames_out and after
    int previous_last = kFrameLength;

    [[nodiscard]] std::int64_t signal_end() const noexcept {
        return base + static_cast<std::int64_t>(signal.front().size());
    }

    [[nodiscard]] double sample(std::size_t c, std::int64_t s) const noexcept {
        if (s < base || s >= signal_end()) {
            return 0.0;
        }
        return signal[c][static_cast<std::size_t>(s - base)];
    }

    // Transient detection over the frame's centre, where its blocks are:
    // the first difference's energy per sub-block against the four before.
    [[nodiscard]] FrameLayout decide(std::int64_t frame) const {
        const std::int64_t centre = frame * kFrameLength + kFrameLength / 2;
        std::array<double, kSubBlocks + 4> energy{};
        for (int k = -4; k < kSubBlocks; ++k) {
            double e = 0.0;
            for (std::size_t c = 0; c < signal.size(); ++c) {
                const std::int64_t start = centre + static_cast<std::int64_t>(k) * kSubBlock;
                for (std::int64_t s = start; s < start + kSubBlock; ++s) {
                    const double d = sample(c, s) - sample(c, s - 1);
                    e += d * d;
                }
            }
            energy[static_cast<std::size_t>(k + 4)] = e;
        }
        std::array<int, 2> attack{-1, -1};
        const double quietest = kAttackFloor * kSubBlock * static_cast<double>(signal.size());
        for (int k = 0; k < kSubBlocks; ++k) {
            const auto i = static_cast<std::size_t>(k + 4);
            const double before = (energy[i - 1] + energy[i - 2] + energy[i - 3] + energy[i - 4]) / 4.0;
            if (energy[i] > quietest && energy[i] > kAttackRatio * std::max(before, quietest)) {
                const auto half = static_cast<std::size_t>(k / (kSubBlocks / 2));
                if (attack[half] < 0) {
                    attack[half] = k % (kSubBlocks / 2);
                }
            }
        }
        if (attack[0] < 0 && attack[1] < 0) {
            return detail::long_layout(kFrameLength);
        }
        // An attack's half splits into eight blocks; the other half stays one
        // block of half the frame.
        const std::array<int, 2> transf_length{attack[0] >= 0 ? 0 : 3, attack[1] >= 0 ? 0 : 3};
        return detail::split_layout(kFrameLength, transf_length, attack);
    }

    [[nodiscard]] std::array<int, 2> max_sfb_for(const FrameLayout& layout) const {
        const int first = layout.window_length.front();
        const int last = layout.window_length.back();
        return {bands_below(first, cutoff, config.sample_rate_hz), bands_below(last, cutoff, config.sample_rate_hz)};
    }

    [[nodiscard]] detail::FrameFields fields_for(std::int64_t frame) const {
        detail::FrameFields fields;
        fields.sequence_counter = sequence_counter(frame);
        fields.iframe = frame % config.iframe_interval == 0;
        fields.fs_index = fs_index;
        fields.frame_rate_index = 13;
        fields.stereo = config.channels == 2;
        fields.dialnorm_bits = dialnorm_bits;
        return fields;
    }

    // The channel element for a frame: the bits of audio_data_chan(), for a
    // global scale factor offset.
    struct Coded {
        std::vector<detail::CodedTrack> tracks;
        detail::StereoChoice stereo;
        std::array<int, 2> max_sfb{};
    };

    // The channel element with no bands: what a frame falls back to when no
    // step of the rate loop fits it, and what create() checks the rate holds.
    [[nodiscard]] Coded silent(const FrameLayout& layout) const {
        Coded coded;
        coded.max_sfb = {0, 0};
        for (int c = 0; c < config.channels; ++c) {
            const detail::Grouped grouped = detail::regroup({}, layout, coded.max_sfb);
            coded.tracks.push_back(
                detail::code_track(grouped, std::vector<std::vector<int>>(grouped.offset.size()), 0, layout));
        }
        return coded;
    }

    void write_element(BitWriter& w, const FrameLayout& layout, const Coded& coded) const {
        if (config.channels == 2) {
            // Part 1 Table 22, channel_pair_element() in SIMPLE mode, and
            // Table 23, stereo_data() with one sf_info() for both tracks.
            w.write(2, 0, "stereo_codec_mode");
            w.write(1, 1, "b_enable_mdct_stereo_proc");
            detail::write_sf_info(w, layout, coded.max_sfb);
            detail::write_chparam_info(w, coded.stereo);
            detail::write_sf_data(w, coded.tracks[0], layout);
            detail::write_sf_data(w, coded.tracks[1], layout);
        } else {
            // Table 20, single_channel_element() in SIMPLE mode, and Table 21,
            // mono_data(0) with the ASF.
            w.write(1, 0, "mono_codec_mode");
            w.write(1, 0, "spec_frontend");
            detail::write_sf_info(w, layout, coded.max_sfb);
            detail::write_sf_data(w, coded.tracks[0], layout);
        }
    }

    [[nodiscard]] EncodedFrame encode_frame(std::int64_t frame, const FrameLayout& layout, int next_first) {
        const std::size_t channels = signal.size();
        const std::int64_t start = frame * kFrameLength;
        Coded coded;
        coded.max_sfb = max_sfb_for(layout);

        std::vector<detail::Grouped> grouped(channels);
        std::vector<std::vector<std::vector<double>>> allowed(channels);
        std::vector<double> window(2 * kFrameLength);
        std::vector<double> spectrum;
        for (std::size_t c = 0; c < channels; ++c) {
            for (std::size_t i = 0; i < window.size(); ++i) {
                window[i] = sample(c, start + static_cast<std::int64_t>(i));
            }
            analysis.transform(window, layout, previous_last, next_first, spectrum);
            grouped[c] = detail::regroup(spectrum, layout, coded.max_sfb);
            allowed[c] = psycho.thresholds(grouped[c], layout);
        }
        if (channels == 2) {
            coded.stereo = detail::choose_stereo(grouped[0], grouped[1], allowed[0], allowed[1]);
        }
        // The frame's size, and the bits the channel element may take.
        const double exact = byte_carry + bytes_per_frame;
        const auto frame_bytes = static_cast<std::size_t>(exact);
        const detail::FrameFields fields = fields_for(frame);
        const std::size_t overhead = detail::frame_overhead_bits(fields, frame_bytes);
        std::size_t element = channels == 2 ? 3 + detail::chparam_info_bits(coded.stereo) : 2;
        element += detail::sf_info_bits(layout, coded.max_sfb);
        const std::size_t budget = 8 * frame_bytes > overhead + element ? 8 * frame_bytes - overhead - element : 0;

        // The rate loop's step p. From 0 up, every band's allowance is raised p
        // scale factor steps together, as far over its threshold as the budget
        // needs. Below 0 the thresholds are met, and the bits left lower a
        // level of noise per line a step at a time: each band whose allowance
        // per line is over the level is brought kLevelWeight of the way to it,
        // in dB, so that the bits go first where the noise is loudest.
        std::vector<std::vector<std::vector<int>>> base_sf(channels);
        double top_level = 0.0;  // the highest allowance per line
        for (std::size_t c = 0; c < channels; ++c) {
            base_sf[c] = detail::scale_factors_for(grouped[c], allowed[c]);
            for (std::size_t g = 0; g < allowed[c].size(); ++g) {
                for (std::size_t b = 0; b < allowed[c][g].size(); ++b) {
                    const auto lines = static_cast<double>(grouped[c].offset[g][b + 1] - grouped[c].offset[g][b]);
                    top_level = std::max(top_level, allowed[c][g][b] / lines);
                }
            }
        }
        std::vector<std::vector<std::vector<int>>> sf;
        std::vector<std::vector<double>> capped;
        const auto set_step = [&](int step) -> int {
            if (step >= 0) {
                sf = base_sf;
                return step;
            }
            const double level = top_level * std::pow(10.0, kStepDb * step / 10.0);
            sf.resize(channels);
            for (std::size_t c = 0; c < channels; ++c) {
                capped = allowed[c];
                for (std::size_t g = 0; g < capped.size(); ++g) {
                    for (std::size_t b = 0; b < capped[g].size(); ++b) {
                        const auto lines =
                            static_cast<double>(grouped[c].offset[g][b + 1] - grouped[c].offset[g][b]);
                        if (capped[g][b] > level * lines) {
                            capped[g][b] *= std::pow(level * lines / capped[g][b], kLevelWeight);
                        }
                    }
                }
                sf[c] = detail::scale_factors_for(grouped[c], capped);
            }
            return 0;
        };
        const auto bits_at = [&](int step) {
            const int offset = set_step(step);
            std::size_t total = 0;
            for (std::size_t c = 0; c < channels; ++c) {
                total += detail::code_track(grouped[c], sf[c], offset, layout).bits();
            }
            return total;
        };
        // The lowest step that fits: the bits fall as it rises.
        int low = kLowestStep;
        int high = kLargestOffset;
        while (low < high) {
            const int mid = low + (high - low) / 2;
            if (bits_at(mid) <= budget) {
                high = mid;
            } else {
                low = mid + 1;
            }
        }
        std::optional<std::vector<std::byte>> raw;
        for (int step = low; step <= kLargestOffset && !raw; ++step) {
            const int offset = set_step(step);
            coded.tracks.clear();
            for (std::size_t c = 0; c < channels; ++c) {
                coded.tracks.push_back(detail::code_track(grouped[c], sf[c], offset, layout));
            }
            BitWriter audio = BitWriter::buffered();
            write_element(audio, layout, coded);
            raw = detail::write_frame(fields, audio, frame_bytes, config.trace);
        }
        if (!raw) {
            // Lines so far past full scale that the coarsest step still codes
            // more than the frame holds: the frame goes out with no bands.
            coded = silent(layout);
            BitWriter audio = BitWriter::buffered();
            write_element(audio, layout, coded);
            raw = detail::write_frame(fields, audio, frame_bytes, config.trace);
        }
        byte_carry = exact - static_cast<double>(frame_bytes);
        previous_last = layout.window_length.back();
        EncodedFrame out;
        out.raw_ac4_frame = std::move(raw).value();
        out.samples = kFrameLength;
        out.iframe = fields.iframe;
        return out;
    }

    // The frames the input read so far lets through. A frame needs the input
    // to half a frame past its window, where the next frame's transients are.
    [[nodiscard]] std::vector<EncodedFrame> drain() {
        std::vector<EncodedFrame> frames;
        for (;;) {
            const std::int64_t frame = frames_out;
            if (flushed && frame * kFrameLength >= input_samples + kDelay + kDecoderDelay) {
                break;
            }
            const std::int64_t needed = (frame + 2) * kFrameLength + kFrameLength / 2;
            if (!flushed && signal_end() < needed) {
                break;
            }
            while (static_cast<std::int64_t>(layouts.size()) < 2) {
                layouts.push_back(decide(frame + static_cast<std::int64_t>(layouts.size())));
            }
            const FrameLayout layout = layouts.front();
            const int next_first = layouts[1].window_length.front();
            frames.push_back(encode_frame(frame, layout, next_first));
            layouts.pop_front();
            ++frames_out;
            // Nothing before the next frame's window is read again.
            const std::int64_t keep_from = (frames_out * kFrameLength) - kSubBlock * 5;
            if (keep_from > base) {
                const auto drop = static_cast<std::size_t>(std::min(keep_from - base, signal_end() - base));
                for (std::vector<double>& channel : signal) {
                    channel.erase(channel.begin(), channel.begin() + static_cast<std::ptrdiff_t>(drop));
                }
                base += static_cast<std::int64_t>(drop);
            }
        }
        return frames;
    }
};

std::expected<Encoder, EncodeError> Encoder::create(const EncoderConfig& config) {
    if (config.channels != 1 && config.channels != 2) {
        return std::unexpected(EncodeError::kInvalidConfig);
    }
    if (config.sample_rate_hz != 48000 && config.sample_rate_hz != 44100) {
        return std::unexpected(EncodeError::kInvalidConfig);
    }
    if (config.iframe_interval < 1 || config.bitrate_kbps < 8 || config.bitrate_kbps > 3000) {
        return std::unexpected(EncodeError::kInvalidConfig);
    }
    if (!(config.dialnorm_db <= 0.0 && config.dialnorm_db >= -31.75)) {
        return std::unexpected(EncodeError::kInvalidConfig);
    }
    auto impl = std::make_unique<Impl>();
    impl->config = config;
    impl->psycho = detail::Psychoacoustics(config.sample_rate_hz, kFrameLength);
    impl->fs_index = config.sample_rate_hz == 48000 ? 1 : 0;
    impl->dialnorm_bits = static_cast<int>(std::lround(-config.dialnorm_db * 4.0));
    impl->bytes_per_frame = static_cast<double>(config.bitrate_kbps) * 1000.0 * kFrameLength /
                            (static_cast<double>(config.sample_rate_hz) * 8.0);
    impl->cutoff = cutoff_hz(static_cast<double>(config.bitrate_kbps) / config.channels);
    impl->signal.assign(static_cast<std::size_t>(config.channels),
                        std::vector<double>(static_cast<std::size_t>(kDelay), 0.0));

    // The rate must hold a frame with no bands, in the smaller of the sizes
    // it gives frames, whatever the frame's blocks: that is what a frame falls
    // back to. The table of contents is read back from the first such frame:
    // what every frame carries but its counter and sizes.
    const detail::FrameFields fields = impl->fields_for(0);
    const auto frame_bytes = static_cast<std::size_t>(impl->bytes_per_frame);
    std::optional<std::vector<std::byte>> raw;
    for (const FrameLayout& layout :
         {detail::long_layout(kFrameLength), detail::split_layout(kFrameLength, {0, 0}, {0, 0})}) {
        BitWriter silent = BitWriter::buffered();
        impl->write_element(silent, layout, impl->silent(layout));
        auto written = detail::write_frame(fields, silent, frame_bytes, {});
        if (!written) {
            return std::unexpected(EncodeError::kInvalidConfig);  // the rate cannot hold a frame
        }
        if (!raw) {
            raw = std::move(written);
        }
    }
    const auto parsed = parse_raw_frame(*raw);
    if (!parsed) {
        return std::unexpected(EncodeError::kInvalidConfig);
    }
    impl->toc = parsed->toc;
    // What the table of contents does not carry, for build_dac4(): the
    // encoder writes no dialogue enhancement data and no immersive audio.
    for (PresentationInfoV1& presentation : impl->toc.presentations_v1) {
        presentation.de_indicator = false;
        presentation.immersive_audio_indicator = false;
    }
    return Encoder(std::move(impl));
}

Encoder::Encoder(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Encoder::~Encoder() = default;
Encoder::Encoder(Encoder&&) noexcept = default;
Encoder& Encoder::operator=(Encoder&&) noexcept = default;

std::expected<std::vector<EncodedFrame>, EncodeError> Encoder::encode(
    std::span<const std::span<const float>> channels) {
    if (impl_->flushed || channels.size() != impl_->signal.size()) {
        return std::unexpected(EncodeError::kInvalidInput);
    }
    const std::size_t count = channels.front().size();
    for (const std::span<const float> channel : channels) {
        if (channel.size() != count) {
            return std::unexpected(EncodeError::kInvalidInput);
        }
        for (const float x : channel) {
            if (!std::isfinite(x)) {
                return std::unexpected(EncodeError::kInvalidInput);
            }
        }
    }
    for (std::size_t c = 0; c < channels.size(); ++c) {
        std::vector<double>& buffer = impl_->signal[c];
        buffer.reserve(buffer.size() + count);
        for (const float x : channels[c]) {
            buffer.push_back(static_cast<double>(x));
        }
    }
    impl_->input_samples += static_cast<std::int64_t>(count);
    return impl_->drain();
}

std::expected<std::vector<EncodedFrame>, EncodeError> Encoder::flush() {
    if (impl_->flushed) {
        return std::vector<EncodedFrame>{};
    }
    impl_->flushed = true;
    return impl_->drain();
}

const Toc& Encoder::toc() const noexcept {
    return impl_->toc;
}

int Encoder::delay_samples() const noexcept {
    return kDelay;
}

}  // namespace ac4

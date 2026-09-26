#pragma once

// AC-4 workloads for the performance suite (planning/ac4.md, ROADMAP performance
// reporting). Included from ac3bench, ac3perf, ac3membench when AC3FORGE_PERF_AC4
// is defined. Uses the same real-audio fixture as the AC-3 benches.

#ifdef AC3FORGE_PERF_AC4

#include <array>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4enc/encoder.hpp"
#include "real_audio.hpp"

namespace perf::ac4 {

inline constexpr int kFrames = 200;
inline constexpr int kWarmupFrames = 8;
inline constexpr int kDecodeSourceFrames = 40;
inline constexpr double kSampleRate = 48000.0;
inline constexpr int kFrameRateIndex = 13;
inline constexpr std::size_t kInputSamplesPerChunk = 2048;

inline double real_time_budget_ms(int frames) {
    return 1000.0 * static_cast<double>(frames) * static_cast<double>(kInputSamplesPerChunk) /
           kSampleRate;
}

struct Result {
    std::string name;
    int frames = 0;
    double total_ms = 0.0;
    double ms_per_frame = 0.0;
    double p95_ms_per_frame = 0.0;
    double max_ms_per_frame = 0.0;
};

class FrameTimer {
public:
    explicit FrameTimer(int expected_frames) {
        per_frame_ms_.reserve(static_cast<std::size_t>(expected_frames));
    }

    template <typename Fn>
    void time_frame(Fn&& fn) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        per_frame_ms_.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count());
    }

    [[nodiscard]] Result result(std::string name) const {
        const auto frames = static_cast<int>(per_frame_ms_.size());
        double total = 0.0;
        for (const double ms : per_frame_ms_) {
            total += ms;
        }
        std::vector<double> sorted = per_frame_ms_;
        std::ranges::sort(sorted);
        const auto rank =
            static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(frames)));
        const auto p95_index = sorted.empty() ? 0 : std::min(sorted.size() - 1,
                                                             rank == 0 ? 0 : rank - 1);
        return {.name = std::move(name),
                .frames = frames,
                .total_ms = total,
                .ms_per_frame = frames > 0 ? total / frames : 0.0,
                .p95_ms_per_frame = sorted.empty() ? 0.0 : sorted[p95_index],
                .max_ms_per_frame = sorted.empty() ? 0.0 : sorted.back()};
    }

private:
    std::vector<double> per_frame_ms_;
};

[[noreturn]] inline void fail(const char* workload, const char* what) {
    std::fprintf(stderr, "%s: %s failed\n", workload, what);
    std::exit(1);
}

// AC-4 channel order: L, R, C, LFE, Ls, Rs — maps from the reference WAV's
// FL, FR, FC, LFE, BL, BR for 5.1; L and R from FL and FR for stereo.
class FrameSource {
public:
    FrameSource(const ac3::io::WavData& wav, std::span<const std::size_t> channel_indices) {
        ordered_.reserve(channel_indices.size());
        for (const std::size_t ch : channel_indices) {
            ordered_.push_back(&wav.channels[ch % wav.channels.size()]);
        }
        views_.resize(channel_indices.size());
        available_ = wav.frame_count() / kInputSamplesPerChunk;
        if (available_ == 0) {
            fail("ac4_frame_source", "fixture too short");
        }
    }

    [[nodiscard]] std::span<const std::span<const float>> chunk(std::size_t index) {
        const std::size_t offset = (index % available_) * kInputSamplesPerChunk;
        for (std::size_t ch = 0; ch < views_.size(); ++ch) {
            views_[ch] = std::span<const float>{*ordered_[ch]}.subspan(
                offset, kInputSamplesPerChunk);
        }
        return views_;
    }

private:
    std::vector<const std::vector<float>*> ordered_;
    std::vector<std::span<const float>> views_;
    std::size_t available_ = 0;
};

inline constexpr std::array<std::size_t, 2> kStereoChannels = {0, 1};
inline constexpr std::array<std::size_t, 6> kFiveOneChannels = {0, 1, 2, 3, 4, 5};

inline ac4::EncoderConfig stereo_encoder_config() {
    return ac4::EncoderConfig{.channels = 2, .bitrate_kbps = 192, .frame_rate_index = kFrameRateIndex};
}

inline ac4::EncoderConfig five_one_encoder_config() {
    return ac4::EncoderConfig{.channels = 6, .bitrate_kbps = 448, .frame_rate_index = kFrameRateIndex};
}

inline std::vector<std::span<const float>> to_encoder_views(
    std::span<const std::span<const float>> channels) {
    return {channels.begin(), channels.end()};
}

inline std::vector<std::byte> encode_stream(FrameSource& source, const ac4::EncoderConfig& config,
                                            int frame_count, const char* workload) {
    auto encoder = ac4::Encoder::create(config);
    if (!encoder) {
        fail(workload, "Encoder::create");
    }
    std::vector<std::byte> stream;
    stream.reserve(static_cast<std::size_t>(frame_count) * 4096);
    const int chunks = frame_count + 24;
    for (int i = 0; i < chunks; ++i) {
        const auto views = to_encoder_views(source.chunk(static_cast<std::size_t>(i)));
        auto frames = encoder->encode(views);
        if (!frames) {
            fail(workload, "encode");
        }
        for (const auto& frame : *frames) {
            stream.insert(stream.end(), frame.raw_ac4_frame.begin(), frame.raw_ac4_frame.end());
        }
    }
    auto tail = encoder->flush();
    if (!tail) {
        fail(workload, "flush");
    }
    for (const auto& frame : *tail) {
        stream.insert(stream.end(), frame.raw_ac4_frame.begin(), frame.raw_ac4_frame.end());
    }
    const auto scanned = ac4::scan(stream);
    if (scanned.frames.size() < static_cast<std::size_t>(frame_count / 2)) {
        fail(workload, "scan produced too few frames");
    }
    return stream;
}

inline Result bench_encode(FrameSource& source, const ac4::EncoderConfig& config,
                           std::string name) {
    auto encoder = ac4::Encoder::create(config);
    if (!encoder) {
        fail(name.c_str(), "Encoder::create");
    }
    int chunk = 0;
    for (; chunk < kWarmupFrames + 16; ++chunk) {
        const auto views = to_encoder_views(source.chunk(static_cast<std::size_t>(chunk)));
        (void)encoder->encode(views);
    }
    FrameTimer timer{kFrames};
    for (int i = 0; i < kFrames; ++i, ++chunk) {
        timer.time_frame([&] {
            const auto views = to_encoder_views(source.chunk(static_cast<std::size_t>(chunk)));
            const auto frames = encoder->encode(views);
            if (!frames || frames->empty()) {
                fail(name.c_str(), "encode (steady state)");
            }
        });
    }
    return timer.result(std::move(name));
}

inline Result bench_decode(std::span<const std::byte> stream, std::string name) {
    const auto scanned = ac4::scan(stream);
    if (scanned.frames.empty()) {
        fail(name.c_str(), "scan");
    }
    const auto& frames = scanned.frames;
    {
        ac4::Decoder warm;
        for (int i = 0; i < kWarmupFrames && i < static_cast<int>(frames.size()); ++i) {
            (void)warm.decode(frames[static_cast<std::size_t>(i)].raw_ac4_frame);
        }
    }

    ac4::Decoder decoder;
    FrameTimer timer{static_cast<int>(frames.size())};
    for (const auto& frame : frames) {
        timer.time_frame([&] {
            const auto result = decoder.decode(frame.raw_ac4_frame);
            if (!result) {
                fail(name.c_str(), "decode");
            }
        });
    }
    return timer.result(std::move(name));
}

}  // namespace perf::ac4

#endif  // AC3FORGE_PERF_AC4

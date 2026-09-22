#pragma once

// The real-audio fixture every member of the performance suite is fed, and
// the frame-at-a-time view the whole-frame benches read it through.
//
// This exists because the suite's three producers had drifted apart on the
// one question that decides whether their numbers mean anything: what goes
// in. kernel_bench.cpp had the rule right from the start ("every kernel is
// fed REAL audio ... this project's own validation rule - silence and frame
// 0 give false passes on correctness checks - applies just as much to timing
// here"), while ac3bench and ac3perf ran a 440 Hz sine on every channel.
//
// A single stationary tone is not a cheaper version of programme material,
// it is a different workload: its spectrum is one bin wide, so the SNR
// offset search converges against an allocation almost nothing competes
// for, coupling has near-nothing to share between channels, rematrixing
// sees a pair that is already identical, and the transient detector never
// fires, so the block-switched transform never runs at all. A regression
// confined to any of those paths could not move the number. Feeding the
// same reference_51.wav the gold-reference and kernel numbers already use
// puts all of them back in the measurement.
//
// Header-only and shared by ac3bench, ac3perf and ac3kernelbench so the
// three cannot drift again: one loader, one fixture, one set of rules about
// what a missing or too-short file means (exit, never fall back to
// synthetic audio - a bench that silently substitutes a tone is worse than
// one that does not run).

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/io/wav.hpp"

// Set by CMake to the repo root, so the fixtures resolve regardless of the
// working directory a bench binary is launched from.
#ifndef AC3FORGE_SOURCE_DIR
#define AC3FORGE_SOURCE_DIR "."
#endif

namespace perf {

inline constexpr const char* kReference51Wav =
    AC3FORGE_SOURCE_DIR "/tests/golden/audio/reference_51.wav";

// Which of the fixture's channels a workload takes, named in A/52 Table 5.8
// order (L, C, R, SL, SR, LFE) rather than the WAV's own - FrameSource does
// the permutation. Spelled out per workload rather than "the first N",
// because the first two of Table 5.8's order are L and C, and a stereo
// encoder fed a centre channel as its right is measuring a pair no 2/0
// programme would ever contain.
inline constexpr std::array<std::size_t, 6> kFiveOneChannels = {0, 1, 2, 3, 4, 5};
inline constexpr std::array<std::size_t, 2> kStereoChannels = {0, 2};  // L, R
// Objects are independent mono sources with no layout of their own, so any
// four distinct real channels serve; these are the four that carry the most
// content in this fixture.
inline constexpr std::array<std::size_t, 4> kFourObjectChannels = {0, 1, 2, 3};

// Loads a fixture, or exits. There is deliberately no synthetic fallback:
// the whole point of these files is that the numbers came from real
// programme material, and a bench that quietly substituted a tone when the
// file was missing would report that it had measured something it had not.
inline ac3::io::WavData load_real_audio(const std::string& path, std::size_t min_channels,
                                        std::size_t min_samples) {
    auto result = ac3::io::read_wav(path);
    if (!result) {
        std::fprintf(stderr,
                     "perf: failed to read real-audio fixture '%s' (%s) - bench inputs must "
                     "come from real audio, not synthetic silence, so there is no fallback "
                     "here\n",
                     path.c_str(), std::string(ac3::io::describe(result.error())).c_str());
        std::exit(1);
    }
    if (result->channels.size() < min_channels) {
        std::fprintf(stderr, "perf: '%s' has %zu channels, need >= %zu\n", path.c_str(),
                     result->channels.size(), min_channels);
        std::exit(1);
    }
    if (result->frame_count() < min_samples) {
        std::fprintf(stderr, "perf: '%s' has %zu samples/channel, need >= %zu\n", path.c_str(),
                     result->frame_count(), min_samples);
        std::exit(1);
    }
    return std::move(*result);
}

// One encoder input frame at a time, in AC-3 channel order.
//
// A WAV's channel order (FL, FR, FC, LFE, BL, BR) is not A/52 Table 5.8's
// (L, C, R, SL, SR, LFE), and feeding it through unpermuted would put a
// full-bandwidth channel where the encoder expects the LFE - which is not
// just wrong, it is wrong in a way that changes the cost: the LFE's coded
// bandwidth is 7 mantissas against a full channel's 253. ac3_layout_for()
// is the same permutation the CLI's encode path uses.
//
// The fixture is 2.5 seconds - 78 whole frames - and the benches run 200,
// so frame indices wrap. Wrapping keeps every frame real audio instead of
// padding the tail with silence (which would make the back three quarters
// of the run measure the cheapest possible workload); the seam it creates
// between the last frame and the first is one transient in 200, and it
// lands in exactly the same place on every run, so it costs the series
// nothing in comparability.
class FrameSource {
public:
    FrameSource(const ac3::io::WavData& wav, std::span<const std::size_t> ac3_channels) {
        const auto layout = ac3::io::ac3_layout_for(wav.channels.size());
        ordered_.reserve(ac3_channels.size());
        for (const std::size_t ch : ac3_channels) {
            const std::size_t source = layout && ch < layout->wav_index.size()
                                           ? layout->wav_index[ch]
                                           : ch % wav.channels.size();
            ordered_.push_back(&wav.channels[source]);
        }
        views_.resize(ac3_channels.size());
        available_ = wav.frame_count() / static_cast<std::size_t>(ac3::kSamplesPerFrame);
    }

    [[nodiscard]] std::size_t available_frames() const { return available_; }

    // Views of frame `index`'s samples, one per channel. Valid until the
    // next call.
    [[nodiscard]] std::span<const std::span<const float>> frame(std::size_t index) {
        const std::size_t offset =
            (index % available_) * static_cast<std::size_t>(ac3::kSamplesPerFrame);
        for (std::size_t ch = 0; ch < views_.size(); ++ch) {
            views_[ch] = std::span<const float>{*ordered_[ch]}.subspan(
                offset, static_cast<std::size_t>(ac3::kSamplesPerFrame));
        }
        return views_;
    }

private:
    std::vector<const std::vector<float>*> ordered_;
    std::vector<std::span<const float>> views_;
    std::size_t available_ = 0;
};

}  // namespace perf

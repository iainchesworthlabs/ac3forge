// Spike S4: does a 15-object AtmosEncoder plus the demo's per-frame work hold
// the 32 ms frame cadence on this machine, with margin?
// (docs/platforms/windows-demo.md, "Phase 0: spikes")
//
// Per frame it does what the demo engine will do: fold 16 stereo "taps" to
// mono (10 positioned applications) and into a 5-slot bed (6 bed applications,
// one of them 7.1), move the positioned objects a little, run encode_frame,
// and wrap the access unit as an IEC 61937 burst. Reports per-frame wall time
// at p50/p99/max and the ratio to the frame budget, for the normal 6-block
// frame and the 1-block low-latency frame. Synthetic signals, no audio device.
// Throwaway code: no reuse intended.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <utility>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/silent_frame.hpp"  // describe(FrameError)
#include "ac3/iec61937/iec61937.hpp"
#include "ac3/oba/atmos.hpp"

namespace {

constexpr int kObjects = 15;
constexpr int kPositioned = 10;
constexpr int kBedSlots = 5;  // L R C Ls Rs; the LFE goes by lfe_send
constexpr int kTaps = 16;

struct Tap {
    int channels;
    double hz;
    double phase = 0.0;
};

void fill_tap(Tap& tap, std::vector<float>& interleaved, std::size_t frames, double rate) {
    interleaved.resize(frames * static_cast<std::size_t>(tap.channels));
    const double inc = 2.0 * std::numbers::pi * tap.hz / rate;
    for (std::size_t i = 0; i < frames; ++i) {
        const auto v = static_cast<float>(0.2 * std::sin(tap.phase));
        tap.phase += inc;
        if (tap.phase > 2.0 * std::numbers::pi) tap.phase -= 2.0 * std::numbers::pi;
        for (int c = 0; c < tap.channels; ++c) interleaved[i * tap.channels + c] = v * (1.0f - 0.05f * c);
    }
}

struct Result {
    double p50_ms, p99_ms, max_ms, budget_ms;
    std::size_t bytes;
};

Result run(int numblkscod, unsigned bitrate_kbps, int seconds) {
    ac3::oba::AtmosConfig config;
    config.numblkscod = numblkscod;
    config.bitrate_kbps = bitrate_kbps;
    ac3::oba::AtmosEncoder encoder(config, kObjects);

    const int blocks = numblkscod == 0 ? 1 : numblkscod == 1 ? 2 : numblkscod == 2 ? 3 : 6;
    const auto frames_per = static_cast<std::size_t>(blocks * ac3::kSamplesPerBlock);
    const double rate = 48000.0;
    const double budget_ms = 1000.0 * static_cast<double>(frames_per) / rate;

    std::vector<Tap> taps;
    for (int t = 0; t < kTaps; ++t) taps.push_back({t == kTaps - 1 ? 8 : 2, 200.0 + 90.0 * t});
    std::vector<std::vector<float>> tap_pcm(kTaps);
    std::vector<std::vector<float>> objects(kObjects, std::vector<float>(frames_per, 0.0f));
    std::vector<std::span<const float>> views(kObjects);
    std::vector<ac3::oba::ObjectPlacement> placement(kObjects);

    // Bed slots: pinned to speakers, snapped.
    const ac3::oba::Position bed_pos[kBedSlots] = {
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.5, 0.0, 0.0}, {0.0, 1.0, 0.0}, {1.0, 1.0, 0.0}};
    for (int b = 0; b < kBedSlots; ++b) {
        placement[kPositioned + b].position = bed_pos[b];
        placement[kPositioned + b].snap = true;
        placement[kPositioned + b].gain = 0.7;
    }

    const int total_frames = static_cast<int>(seconds * rate / static_cast<double>(frames_per));
    std::vector<double> times;
    times.reserve(static_cast<std::size_t>(total_frames));
    std::size_t bytes = 0;
    ac3::iec61937::Eac3BurstPacker packer;
    double t = 0.0;

    for (int f = 0; f < total_frames; ++f) {
        const auto t0 = std::chrono::steady_clock::now();

        for (int i = 0; i < kTaps; ++i) fill_tap(taps[i], tap_pcm[i], frames_per, rate);

        // Positioned: taps 0..9 folded to mono, orbiting slowly.
        for (int o = 0; o < kPositioned; ++o) {
            const auto& pcm = tap_pcm[o];
            const int ch = taps[o].channels;
            auto& out = objects[o];
            for (std::size_t i = 0; i < frames_per; ++i) out[i] = 0.5f * (pcm[i * ch] + pcm[i * ch + 1]);
            const double angle = 2.0 * std::numbers::pi * (0.05 * t + o / static_cast<double>(kPositioned));
            placement[o].position = {0.5 + 0.45 * std::sin(angle), 0.5 - 0.45 * std::cos(angle),
                                     -0.5 + o / static_cast<double>(kPositioned)};
            placement[o].gain = 0.5;
        }
        // Bed: taps 10..15 summed into L R C Ls Rs; the 7.1 tap maps one-to-one
        // with side/rear folded, the stereo ones go to L and R.
        for (int b = 0; b < kBedSlots; ++b) std::fill(objects[kPositioned + b].begin(), objects[kPositioned + b].end(), 0.0f);
        for (int tp = kPositioned; tp < kTaps; ++tp) {
            const auto& pcm = tap_pcm[tp];
            const int ch = taps[tp].channels;
            for (std::size_t i = 0; i < frames_per; ++i) {
                if (ch == 2) {
                    objects[kPositioned + 0][i] += pcm[i * 2];
                    objects[kPositioned + 1][i] += pcm[i * 2 + 1];
                } else {  // 7.1: L R C LFE Lss Rss Lrs Rrs
                    objects[kPositioned + 0][i] += pcm[i * 8];
                    objects[kPositioned + 1][i] += pcm[i * 8 + 1];
                    objects[kPositioned + 2][i] += pcm[i * 8 + 2];
                    objects[kPositioned + 3][i] += 0.707f * (pcm[i * 8 + 4] + pcm[i * 8 + 6]);
                    objects[kPositioned + 4][i] += 0.707f * (pcm[i * 8 + 5] + pcm[i * 8 + 7]);
                }
            }
        }
        for (int o = 0; o < kObjects; ++o) views[o] = objects[o];

        auto unit = encoder.encode_frame(views, placement);
        if (!unit) {
            std::printf("  encode_frame refused at frame %d: FrameError %d (%s)\n", f,
                        static_cast<int>(std::to_underlying(unit.error())),
                        std::string(ac3::describe(unit.error())).c_str());
            return {0, 0, 0, budget_ms, 0};
        }
        bytes += unit->bytes.size();
        const auto burst = packer.push(std::span<const std::byte>(unit->bytes));
        (void)burst;

        const auto t1 = std::chrono::steady_clock::now();
        times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        t += static_cast<double>(frames_per) / rate;
    }

    std::sort(times.begin(), times.end());
    auto pct = [&](double p) { return times[std::min(times.size() - 1, static_cast<std::size_t>(p * times.size()))]; };
    return {pct(0.5), pct(0.99), times.back(), budget_ms, bytes};
}

}  // namespace

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 20;
    std::printf("S4: %d objects (%d positioned + %d bed), %d taps, %d s of audio per mode\n\n", kObjects, kPositioned,
                kBedSlots, kTaps, seconds);
    std::printf("%-22s %9s %9s %9s %9s %9s %10s\n", "mode", "budget ms", "p50 ms", "p99 ms", "max ms", "p99/budget",
                "kbit/s");
    struct Mode { int numblkscod; unsigned kbps; const char* label; };
    const Mode modes[] = {{3, 448, "6 blocks @448"}, {3, 640, "6 blocks @640"}, {2, 640, "3 blocks @640"},
                          {1, 1024, "2 blocks @1024"}, {0, 1536, "1 block @1536"}, {0, 2048, "1 block @2048"},
                          {0, 3072, "1 block @3072"}};
    for (const auto& m : modes) {
        const auto r = run(m.numblkscod, m.kbps, seconds);
        if (r.bytes == 0) continue;
        std::printf("%-22s %9.2f %9.3f %9.3f %9.3f %9.1f%% %10.0f\n", m.label, r.budget_ms, r.p50_ms, r.p99_ms,
                    r.max_ms, 100.0 * r.p99_ms / r.budget_ms, 8.0 * static_cast<double>(r.bytes) / seconds / 1000.0);
    }
    return 0;
}

// The sink CI uses: counts frames and returns.
//
// qemu-system-xtensa has no I2S peripheral, so the real sink's first write
// blocks on a DMA that never drains. This one lets everything else run - the
// partition reads, the access-unit framing, the decode, the interleaving, the
// player loop - on the target, under the emulator, with no board.
//
// WHAT IT DOES NOT DO, said plainly: it does not pace anything. The real sink
// blocks until the DAC has taken the samples, which is what makes the player
// run at real time and what makes its us_per_frame figures mean something. Here
// the loop runs flat out, so a run under this sink says the decode is CORRECT
// and says nothing whatever about whether it is FAST ENOUGH. See
// docs/platforms/esp32.md on why QEMU cannot answer that either way.

#include "audio_sink.hpp"

#include <cstdio>

namespace player {
namespace {

std::uint64_t g_frames = 0;
// Summed but never read back. It exists so the decode has an observable
// consumer: a sink that genuinely touched nothing would let the compiler delete
// work the run is supposed to be doing, and CI would be exercising less than it
// looks like it is.
double g_checksum = 0.0;

}  // namespace

// Any channel count, deliberately. This is the sink a 5.1 or 7.1 player would be
// developed against before a TDM DAC existed to test on, so refusing anything
// here would just move the obstacle.
bool sink_open(std::uint32_t sample_rate, int channels) {
    std::printf("sink: null %lu Hz 16-bit x%d (no peripheral, no pacing)\n",
                static_cast<unsigned long>(sample_rate), channels);
    return true;
}

void sink_write(std::span<const std::span<const float>> channels) {
    for (const auto channel : channels) {
        for (const float sample : channel) {
            g_checksum += static_cast<double>(sample);
        }
    }
    ++g_frames;
}

const char* sink_name() { return "null"; }

std::uint64_t sink_frames_written() { return g_frames; }

// Nothing to report. This sink deliberately knows nothing about the audio -
// it is the cheapest thing that can stand in for a peripheral. Use
// sink/capture/ when the question is whether the samples are right.
void sink_report() {}

}  // namespace player

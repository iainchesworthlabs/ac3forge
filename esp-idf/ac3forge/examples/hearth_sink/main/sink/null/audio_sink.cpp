// Counts blocks and returns. CI builds this sink and does not run it: its QEMU
// steps use sink/capture/, which converts the samples and checks them.
//
// qemu-system-xtensa has no I2S peripheral, so the real sink's first write
// blocks on a DMA that never drains. This one lets everything else run - the
// source reads, the access-unit framing, the decode, the player loop - on the
// target, under the emulator, with no board.
//
// WHAT IT DOES NOT DO: it does not pace the player's own plays. The real sink
// blocks until the DAC has taken the samples, which is what makes the player
// run at real time and what makes its us_per_frame figures mean something. Here
// the loop runs flat out, so a run under this sink says the decode is CORRECT
// and says nothing whatever about whether it is FAST ENOUGH. See
// docs/platforms/bare-metal/esp32-s3.md on why QEMU cannot answer that either way.
// A Sendspin stream's timed writes are the exception, below.

#include "audio_sink.hpp"

#include <algorithm>
#include <cstdio>

#include "ac3/core/tables.hpp"
#include "ac3/render/layout.hpp"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ac3forge/playout.hpp"

namespace player {
namespace {

std::uint64_t g_frames = 0;
int g_channels = 0;
// A timed write (a Sendspin stream) is paced as a DAC would take it, which
// is the one kind of pacing this sink does: a board with nothing wired to its
// pins still plays a Sendspin stream at the rate the server sends it.
constexpr std::uint32_t kVirtualDescriptors = 12;
ac3forge::VirtualDac g_dac;
// Summed but never read back. It exists so the decode has an observable
// consumer: a sink that touched nothing would let the compiler delete work the
// run is supposed to be doing.
double g_checksum = 0.0;

}  // namespace

// Any channel count, deliberately. This is the sink a 5.1 or 7.1 player would be
// developed against before a TDM DAC existed to test on, so refusing anything
// here would just move the obstacle.
bool sink_open(std::uint32_t sample_rate, int channels) {
    g_channels = channels;
    g_dac.open(kVirtualDescriptors, ac3::kSamplesPerBlock, sample_rate);
    std::printf("sink: null %lu Hz x%d (no peripheral; timed writes paced as a DAC would)\n",
                static_cast<unsigned long>(sample_rate), channels);
    return true;
}

// Any channel count, so the ceiling is the most slots a layout has: what a
// player may ask for before this sink has been opened at all.
int sink_slots() { return static_cast<int>(ac3::render::OutputLayout::kMaxSlots); }

int sink_max_slots() { return sink_slots(); }

bool sink_second_line_possible() { return false; }

// Nothing here is interleaved into slots at all, so the width is only what a
// caller asking gets told, and changing it would describe nothing.
int sink_slot_bits() { return CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS; }

bool sink_set_slot_bits(int bits) { return bits == CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS; }

void sink_write(std::span<const std::span<const float>> channels) {
    // Summed in float per channel and block, and added in double once: a
    // double addition per sample is a soft-float call on this part, which is a
    // cost the decode it stands behind would be charged for.
    for (const auto channel : channels) {
        float block = 0.0F;
        for (const float sample : channel) {
            block += sample;
        }
        g_checksum += static_cast<double>(block);
    }
    ++g_frames;
}

std::optional<ac3forge::PlayoutWrite> sink_write_timed(std::span<const std::span<const float>> channels) {
    if (g_channels == 0) {
        return std::nullopt;
    }
    sink_write(channels);
    const ac3forge::VirtualDac::Write written = g_dac.write(esp_timer_get_time());
    const std::int64_t wait_us = written.return_us - esp_timer_get_time();
    if (wait_us > 0) {
        vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS((wait_us + 999) / 1000)));
    }
    return ac3forge::PlayoutWrite{.play_us = written.play_us, .late = false, .gap = written.gap};
}

const char* sink_name() { return "null"; }

std::uint64_t sink_frames_written() { return g_frames; }

// Nothing to start again: this sink reports nothing, and sink_frames_written()
// counts from sink_open.
void sink_begin_play() {}

// Nothing to report. This sink deliberately knows nothing about the audio -
// it is the cheapest thing that can stand in for a peripheral. Use
// sink/capture/ when the question is whether the samples are right.
void sink_report() {}

}  // namespace player

// The real sink: a stereo I2S DAC.
//
// Selected by default. See ../../audio_sink.hpp for why this is a directory
// CMake picks rather than a branch in the player.
//
// Two slots, 32 bits each by default (CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS; 16
// is the other answer): 32-bit slots carry the 24-bit samples every DAC here
// accepts and a SigmaDSP requires, and cost nothing but bit clock - 3.07 MHz
// against 1.54. Master by default; CONFIG_AC3FORGE_EXAMPLE_I2S_SLAVE hands the
// clocks to the DAC, which is how an ADAU1452 or ADAU1467 wants it when the DSP
// is the house's clock: BCLK and WS become inputs on the same two pins, and
// the sample rate below is what the peripheral is told to expect rather than
// what it generates.

#include "audio_sink.hpp"

#include <array>
#include <cstdio>

#include "ac3/core/tables.hpp"
#include "ac3forge/interleave.hpp"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "../sink_common.hpp"

namespace player {
namespace {

constexpr int kSlotBits = CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS;
static_assert(kSlotBits == 16 || kSlotBits == 32,
              "CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS is 16 or 32");
constexpr bool kSlave = CONFIG_AC3FORGE_EXAMPLE_I2S_SLAVE != 0;
constexpr std::size_t kSlots = 2;
constexpr std::size_t kBytesPerSampleFrame = kSlots * (kSlotBits / 8);

i2s_chan_handle_t g_tx = nullptr;
DacQueueModel g_model;

// One block of interleaved stereo in each width, 2 KB and 1 KB. At namespace
// scope because the sink is the only thing that needs them - the player hands
// over planar float and never sees this format at all.
std::array<std::int32_t, ac3::kSamplesPerBlock * kSlots> g_wide{};
std::array<std::int16_t, ac3::kSamplesPerBlock * kSlots> g_narrow{};

// The DMA queue, from Kconfig - see main/Kconfig.projbuild for why the default
// is smaller than a frame - shaped for this slot width by dma_plan.
constexpr int kDmaDescriptors = CONFIG_AC3FORGE_EXAMPLE_I2S_DMA_DESCRIPTORS;
constexpr int kDmaFrames = CONFIG_AC3FORGE_EXAMPLE_I2S_DMA_FRAMES;

}  // namespace

bool sink_open(std::uint32_t sample_rate, int channels) {
    // Standard I2S carries exactly two slots. More than that is TDM - a
    // different driver (driver/i2s_tdm.h), a different slot configuration, and
    // a DAC that speaks it, which the common stereo breakouts (MAX98357A,
    // PCM5102) do not: sink/tdm/. One channel is fine - a mono layout goes to
    // both slots.
    if (channels != 1 && channels != 2) {
        std::printf("error: the i2s sink is stereo, asked for %d channels - wider layouts need "
                    "the tdm sink\n",
                    channels);
        return false;
    }
    const DmaPlan plan =
        dma_plan(kDmaDescriptors, kDmaFrames, kBytesPerSampleFrame, ac3::kSamplesPerBlock);
    const std::size_t dma_bytes = static_cast<std::size_t>(plan.descriptors) *
                                  static_cast<std::size_t>(plan.frames) * kBytesPerSampleFrame;
    g_model.open(sample_rate * static_cast<std::uint32_t>(kBytesPerSampleFrame), dma_bytes);

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, kSlave ? I2S_ROLE_SLAVE : I2S_ROLE_MASTER);
    // Four descriptors of 240 frames by default: 20 ms, which dma_plan reshapes
    // into eight of 128 so that every block ends on a descriptor. Small on purpose -
    // every millisecond of buffer is a millisecond of latency, and a deep
    // buffer would hide a decoder that cannot keep up, which is one of the
    // things this example exists to reveal. Kconfig, so a source that needs
    // more can ask for it without touching this file.
    chan_cfg.dma_desc_num = static_cast<uint32_t>(plan.descriptors);
    chan_cfg.dma_frame_num = static_cast<uint32_t>(plan.frames);
    chan_cfg.auto_clear = true;  // silence on underrun, not the last buffer again
    if (i2s_new_channel(&chan_cfg, &g_tx, nullptr) != ESP_OK) {
        std::printf("error: could not allocate an I2S channel\n");
        return false;
    }

    // Field by field onto a zeroed struct: C++ requires designated initialisers
    // in declaration order and IDF's layout is free to change between versions.
    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        kSlotBits == 32 ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(CONFIG_AC3FORGE_EXAMPLE_I2S_BCLK_GPIO);
    std_cfg.gpio_cfg.ws = static_cast<gpio_num_t>(CONFIG_AC3FORGE_EXAMPLE_I2S_WS_GPIO);
    std_cfg.gpio_cfg.dout = static_cast<gpio_num_t>(CONFIG_AC3FORGE_EXAMPLE_I2S_DOUT_GPIO);
    std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;

    if (i2s_channel_init_std_mode(g_tx, &std_cfg) != ESP_OK ||
        i2s_channel_enable(g_tx) != ESP_OK) {
        std::printf("error: could not start I2S\n");
        return false;
    }
    std::printf("sink: i2s %lu Hz %d-bit stereo, %s, bclk=%d ws=%d dout=%d, dma=%dx%d frames "
                "(%ld ms)\n",
                static_cast<unsigned long>(sample_rate), kSlotBits,
                kSlave ? "slave (the DAC clocks)" : "master",
                CONFIG_AC3FORGE_EXAMPLE_I2S_BCLK_GPIO, CONFIG_AC3FORGE_EXAMPLE_I2S_WS_GPIO,
                CONFIG_AC3FORGE_EXAMPLE_I2S_DOUT_GPIO, plan.descriptors, plan.frames,
                static_cast<long>(g_model.capacity_ms()));
    return true;
}

void sink_write(std::span<const std::span<const float>> channels) {
    if (channels.empty()) {
        return;
    }
    // A mono layout to both slots.
    const std::array<std::span<const float>, kSlots> pair = {
        channels[0], channels.size() > 1 ? channels[1] : channels[0]};
    std::size_t frames = pair[0].size();
    if (frames > ac3::kSamplesPerBlock) {
        frames = ac3::kSamplesPerBlock;
    }
    // Through the shared conversion, not a copy of it: sink/capture/ checks
    // this exact code on target, which it could not if each sink had its own.
    const void* data = nullptr;
    if (kSlotBits == 32) {
        ac3forge::interleave_24in32(pair, kSlots, frames,
                                    std::span<std::int32_t>{g_wide.data(), frames * kSlots});
        data = g_wide.data();
    } else {
        ac3forge::interleave_16(pair, frames, std::span<std::int16_t>{g_narrow.data(), frames * kSlots});
        data = g_narrow.data();
    }
    const std::size_t bytes = frames * kBytesPerSampleFrame;

    g_model.arriving();
    std::size_t written = 0;
    // portMAX_DELAY: block until the DMA has room. This is what paces the
    // player at real time - the DAC's clock, not a delay - and it is why the
    // timing figures mean something here and nothing under an emulator.
    (void)i2s_channel_write(g_tx, data, bytes, &written, portMAX_DELAY);
    g_model.queued(bytes);
}

const char* sink_name() { return "i2s"; }

int sink_slots() { return static_cast<int>(kSlots); }

std::uint64_t sink_frames_written() { return g_model.writes(); }

// What the DAC did with the samples is not visible from this side of the wire -
// sink/capture/ is the one that checks the conversion, and it runs the same
// interleave this does. What IS visible is whether the samples got there in
// time, and that is what this reports: see sink_common.hpp.
void sink_report() { g_model.report(); }

}  // namespace player

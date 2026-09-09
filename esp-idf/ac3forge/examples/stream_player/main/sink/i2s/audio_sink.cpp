// The real sink: an I2S DAC.
//
// Selected by default. See ../../audio_sink.hpp for why this is a directory
// CMake picks rather than a branch in the player.

#include "audio_sink.hpp"

#include <array>
#include <cstdio>

#include "ac3/core/tables.hpp"
#include "interleave.hpp"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace player {
namespace {

i2s_chan_handle_t g_tx = nullptr;
std::uint64_t g_frames = 0;

// One frame of interleaved 16-bit stereo, 6,144 bytes. At namespace scope
// because that is more than a FreeRTOS task stack has spare, and because the
// sink is the only thing that needs it - the player hands over planar float and
// never sees this format at all.
std::array<std::int16_t, ac3::kSamplesPerFrame * 2> g_interleaved{};

}  // namespace

bool sink_open(std::uint32_t sample_rate, int channels) {
    // Standard I2S carries exactly two slots. More than that is TDM - a
    // different driver (driver/i2s_tdm.h), a different slot configuration, and
    // a DAC that speaks it, which the common stereo breakouts (MAX98357A,
    // PCM5102) do not. That is a sink to add beside this one when there is
    // hardware to test it against, not a branch to bolt on here.
    if (channels != 2) {
        std::printf("error: the i2s sink is stereo only, asked for %d channels - "
                    "5.1 and 7.1 need a TDM sink\n",
                    channels);
        return false;
    }
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    // Four descriptors of 240 frames: 3,840 bytes holding 20 ms. Small on
    // purpose - every millisecond of buffer is a millisecond of latency, and a
    // deep buffer would hide a decoder that cannot keep up, which is one of the
    // things this example exists to reveal.
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear = true;  // silence on underrun, not the last buffer again
    if (i2s_new_channel(&chan_cfg, &g_tx, nullptr) != ESP_OK) {
        std::printf("error: could not allocate an I2S channel\n");
        return false;
    }

    // Field by field onto a zeroed struct: C++ requires designated initialisers
    // in declaration order and IDF's layout is free to change between versions.
    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    std_cfg.slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
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
    std::printf("sink: i2s %lu Hz 16-bit stereo, bclk=%d ws=%d dout=%d\n",
                static_cast<unsigned long>(sample_rate), CONFIG_AC3FORGE_EXAMPLE_I2S_BCLK_GPIO,
                CONFIG_AC3FORGE_EXAMPLE_I2S_WS_GPIO, CONFIG_AC3FORGE_EXAMPLE_I2S_DOUT_GPIO);
    return true;
}

void sink_write(std::span<const std::span<const float>> channels) {
    // Through the shared conversion, not a copy of it: sink/capture/ checks
    // this exact code on target, which it could not if each sink had its own.
    interleave_16(channels, ac3::kSamplesPerFrame, g_interleaved);

    std::size_t written = 0;
    // portMAX_DELAY: block until the DMA has room. This is what paces the
    // player at real time - the DAC's clock, not a delay - and it is why the
    // timing figures mean something here and nothing under an emulator.
    (void)i2s_channel_write(g_tx, g_interleaved.data(),
                            g_interleaved.size() * sizeof(std::int16_t), &written,
                            portMAX_DELAY);
    ++g_frames;
}

const char* sink_name() { return "i2s"; }

std::uint64_t sink_frames_written() { return g_frames; }

// Nothing to report: what the DAC did with the samples is not visible from
// this side of the wire. sink/capture/ is the one that checks the
// conversion, and it runs the same interleave this does.
void sink_report() {}

}  // namespace player

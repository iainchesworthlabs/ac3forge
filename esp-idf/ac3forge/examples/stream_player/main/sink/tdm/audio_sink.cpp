// Multi-channel out of one data line: I2S in TDM mode.
//
// Standard I2S carries two slots, so 5.1 and 7.1 need this. The ESP32-S3's I2S
// packs up to 16 slots onto a single data line, which means eight channels needs
// THREE pins - BCLK, WS and DATA - rather than four data lines, and eight slots
// of 32 bits at 48 kHz is a 12.3 MHz bit clock, well inside what the peripheral
// will do. The DAC has to speak TDM: a PCM3168A does, the common stereo
// breakouts (MAX98357A, PCM5102) do not.
//
// NOT TESTED ON HARDWARE. There is no TDM DAC here, and QEMU has no I2S at all,
// so what CI establishes about this file is that it compiles and links. The
// interleave is the exception and it is deliberately elsewhere:
// ac3forge/interleave.hpp is free of ESP-IDF and is unit-tested on the host
// (tests/io/test_interleave.cpp), because indexing a planar-to-interleaved
// transform with slot padding is where the bugs are, and the rest of this file
// is peripheral setup that either works on a board or does not.

#include "audio_sink.hpp"

#include <array>
#include <cstdio>

#include "ac3/core/tables.hpp"
#include "driver/i2s_tdm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ac3forge/interleave.hpp"

namespace player {
namespace {

i2s_chan_handle_t g_tx = nullptr;
std::uint64_t g_frames = 0;
std::size_t g_slots = 0;

// One frame of interleaved TDM: 1,536 samples of kMaxSlots 32-bit slots. At
// namespace scope because 49,152 bytes is far more than a FreeRTOS task stack
// has, and static because the sink is the only thing that needs it.
//
// It is .bss, which on this part is internal SRAM - so it is already where DMA
// can reach, and i2s_channel_write copies into the driver's own descriptors
// anyway. A heap_caps_malloc(MALLOC_CAP_DMA) here would add an allocation to a
// profile whose whole subject is not having any; that requirement belongs to
// the zero-copy paths (i2s_channel_preload_data and friends), which this does
// not use.
constexpr std::size_t kMaxSlots = 8;
std::array<std::int32_t, ac3::kSamplesPerFrame * kMaxSlots> g_interleaved{};

// Which slots the bus carries. Always the full width, not the channel count:
// a TDM frame is a fixed shape, and a 5.1 programme on an 8-slot bus leaves two
// slots that must still be written (with zeros - see interleave.hpp).
i2s_tdm_slot_mask_t slot_mask(std::size_t slots) {
    unsigned mask = 0;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        mask |= 1U << slot;
    }
    return static_cast<i2s_tdm_slot_mask_t>(mask);
}

}  // namespace

bool sink_open(std::uint32_t sample_rate, int channels) {
    if (channels <= 0 || static_cast<std::size_t>(channels) > kMaxSlots) {
        std::printf("error: the tdm sink carries 1 to %u channels, asked for %d\n",
                    static_cast<unsigned>(kMaxSlots), channels);
        return false;
    }
    // The BUS width, from configuration, not from the programme. A DAC is wired
    // for a fixed number of slots and does not renegotiate because this stream
    // happens to be 5.1 - so the slot count is a property of the board and the
    // channel count is a property of the audio, and they are allowed to differ.
    g_slots = static_cast<std::size_t>(CONFIG_AC3FORGE_EXAMPLE_TDM_SLOTS);
    if (g_slots > kMaxSlots || static_cast<std::size_t>(channels) > g_slots) {
        std::printf("error: %d channels do not fit %u TDM slots\n", channels,
                    static_cast<unsigned>(g_slots));
        return false;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    // Fewer, larger descriptors than the stereo sink uses: the same 20 ms of
    // audio is four times the bytes at eight 32-bit slots, and dma_frame_num is
    // counted in FRAMES rather than bytes.
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &g_tx, nullptr) != ESP_OK) {
        std::printf("error: could not allocate an I2S channel\n");
        return false;
    }

    // Field by field onto a zeroed struct: C++ requires designated initialisers
    // in declaration order and IDF's layout is free to change between versions.
    i2s_tdm_config_t tdm_cfg = {};
    tdm_cfg.clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(sample_rate);
    tdm_cfg.slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO, slot_mask(g_slots));
    tdm_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    tdm_cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(CONFIG_AC3FORGE_EXAMPLE_I2S_BCLK_GPIO);
    tdm_cfg.gpio_cfg.ws = static_cast<gpio_num_t>(CONFIG_AC3FORGE_EXAMPLE_I2S_WS_GPIO);
    tdm_cfg.gpio_cfg.dout = static_cast<gpio_num_t>(CONFIG_AC3FORGE_EXAMPLE_I2S_DOUT_GPIO);
    tdm_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;

    if (i2s_channel_init_tdm_mode(g_tx, &tdm_cfg) != ESP_OK ||
        i2s_channel_enable(g_tx) != ESP_OK) {
        std::printf("error: could not start I2S in TDM mode\n");
        return false;
    }
    std::printf("sink: tdm %lu Hz 24-in-32 x%d in %u slots, bclk=%d ws=%d dout=%d\n",
                static_cast<unsigned long>(sample_rate), channels,
                static_cast<unsigned>(g_slots), CONFIG_AC3FORGE_EXAMPLE_I2S_BCLK_GPIO,
                CONFIG_AC3FORGE_EXAMPLE_I2S_WS_GPIO, CONFIG_AC3FORGE_EXAMPLE_I2S_DOUT_GPIO);
    return true;
}

void sink_write(std::span<const std::span<const float>> channels) {
    ac3forge::interleave_24in32(channels, g_slots, ac3::kSamplesPerFrame,
                      std::span<std::int32_t>{g_interleaved.data(),
                                              ac3::kSamplesPerFrame * g_slots});

    std::size_t written = 0;
    // portMAX_DELAY: block until the DMA has room. This is what paces the
    // player at real time - the DAC's clock, not a delay.
    (void)i2s_channel_write(g_tx, g_interleaved.data(),
                            ac3::kSamplesPerFrame * g_slots * sizeof(std::int32_t), &written,
                            portMAX_DELAY);
    ++g_frames;
}

const char* sink_name() { return "tdm"; }

std::uint64_t sink_frames_written() { return g_frames; }

// Nothing to report, for the same reason the stereo sink has nothing: the
// DAC's side of the wire is not observable here. sink/capture/ with
// CONFIG_AC3FORGE_EXAMPLE_CAPTURE_TDM checks this sink's conversion.
void sink_report() {}

}  // namespace player

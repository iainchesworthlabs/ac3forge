// Multi-channel out of one data line: I2S in TDM mode.
//
// Standard I2S carries two slots, so anything wider needs this. An ESP32-S3
// TDM frame holds at most 128 bits: the peripheral's half-frame length is a
// 6-bit register field (tx_half_sample_bits), so one data line carries four
// slots of 32 bits - the width this sink sends, 24-bit samples left-justified -
// or eight of 16, and sixteen only at 8 bits. ESP-IDF v6.1 refuses anything
// larger at i2s_channel_init_tdm_mode, and so does sink_open below, first and
// with the reason. A 7.1.4 layout's twelve slots of 24-bit audio therefore do
// not fit on one line of this part: that needs two I2S controllers at 16 bits,
// or a TDM device fed by several lines (planning/esp32-714-realtime.md). Four
// slots of 32 bits at 48 kHz is a 6.1 MHz bit clock; whether the DAC follows is
// its datasheet. It has to speak TDM: a PCM3168A does, an ADAU1452 or ADAU1467
// does on its serial inputs (TDM2/4/8/16), the common stereo breakouts
// (MAX98357A, PCM5102) do not.
//
// Master by default; CONFIG_AC3FORGE_EXAMPLE_I2S_SLAVE hands the clocks to the
// other end, which is how a SigmaDSP that is the house's clock wants it.
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

#include "../sink_common.hpp"

namespace player {
namespace {

constexpr bool kSlave = CONFIG_AC3FORGE_EXAMPLE_I2S_SLAVE != 0;

i2s_chan_handle_t g_tx = nullptr;
DacQueueModel g_model;
std::size_t g_slots = 0;

// At most 128 bits a frame, so four of these 32-bit slots - see the top of
// this file. sink_open refuses more, with what to do instead, rather than
// leaving it to the driver, whose own line names the limit and nothing else.
constexpr std::size_t kFrameBitsMax = 128;
constexpr std::size_t kSlotBits = 32;
constexpr std::size_t kMaxSlots = kFrameBitsMax / kSlotBits;

// One block of interleaved TDM: 256 sample frames of up to kMaxSlots 32-bit
// slots, 4 KB. At namespace scope rather than on the decode task's stack, and
// static because the sink is the only thing that needs it.
//
// It is .bss, which on this part is internal SRAM - so it is already where DMA
// can reach, and i2s_channel_write copies into the driver's own descriptors
// anyway. A heap_caps_malloc(MALLOC_CAP_DMA) here would add an allocation to a
// profile whose whole subject is not having any; that requirement belongs to
// the zero-copy paths (i2s_channel_preload_data and friends), which this does
// not use.
std::array<std::int32_t, ac3::kSamplesPerBlock * kMaxSlots> g_interleaved{};

constexpr int kDmaDescriptors = CONFIG_AC3FORGE_EXAMPLE_I2S_DMA_DESCRIPTORS;
constexpr int kDmaFrames = CONFIG_AC3FORGE_EXAMPLE_I2S_DMA_FRAMES;

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
    // The BUS width, from configuration, not from the programme. A DAC is wired
    // for a fixed number of slots and does not renegotiate because this stream
    // happens to be 5.1 - so the slot count is a property of the board and the
    // channel count is a property of the layout, and they are allowed to differ.
    g_slots = static_cast<std::size_t>(CONFIG_AC3FORGE_EXAMPLE_TDM_SLOTS);
    if (g_slots > kMaxSlots) {
        std::printf("error: an ESP32-S3 I2S TDM frame holds at most %u bits, so %u slots of %u "
                    "bits, and CONFIG_AC3FORGE_EXAMPLE_TDM_SLOTS is %u - more channels need a "
                    "second I2S controller or a TDM device fed by several lines\n",
                    static_cast<unsigned>(kFrameBitsMax), static_cast<unsigned>(kMaxSlots),
                    static_cast<unsigned>(kSlotBits), static_cast<unsigned>(g_slots));
        return false;
    }
    if (channels <= 0 || static_cast<std::size_t>(channels) > g_slots) {
        std::printf("error: %d channels do not fit %u TDM slots\n", channels,
                    static_cast<unsigned>(g_slots));
        return false;
    }
    const std::size_t bytes_per_frame = g_slots * sizeof(std::int32_t);
    // The same depth the stereo sink has, in as many descriptors as this
    // width needs - see dma_plan.
    const DmaPlan plan =
        dma_plan(kDmaDescriptors, kDmaFrames, bytes_per_frame, ac3::kSamplesPerBlock);
    const std::size_t dma_bytes = static_cast<std::size_t>(plan.descriptors) *
                                  static_cast<std::size_t>(plan.frames) * bytes_per_frame;
    g_model.open(sample_rate * static_cast<std::uint32_t>(bytes_per_frame), dma_bytes);

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, kSlave ? I2S_ROLE_SLAVE : I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = static_cast<uint32_t>(plan.descriptors);
    chan_cfg.dma_frame_num = static_cast<uint32_t>(plan.frames);
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
    std::printf("sink: tdm %lu Hz 24-in-32 x%d in %u slots, %s, bclk=%d ws=%d dout=%d, "
                "dma=%dx%d frames (%ld ms)\n",
                static_cast<unsigned long>(sample_rate), channels,
                static_cast<unsigned>(g_slots), kSlave ? "slave (the DAC clocks)" : "master",
                CONFIG_AC3FORGE_EXAMPLE_I2S_BCLK_GPIO, CONFIG_AC3FORGE_EXAMPLE_I2S_WS_GPIO,
                CONFIG_AC3FORGE_EXAMPLE_I2S_DOUT_GPIO, plan.descriptors, plan.frames,
                static_cast<long>(g_model.capacity_ms()));
    return true;
}

void sink_write(std::span<const std::span<const float>> channels) {
    if (channels.empty()) {
        return;
    }
    std::size_t frames = channels[0].size();
    if (frames > ac3::kSamplesPerBlock) {
        frames = ac3::kSamplesPerBlock;
    }
    ac3forge::interleave_24in32(channels, g_slots, frames,
                                std::span<std::int32_t>{g_interleaved.data(), frames * g_slots});
    const std::size_t bytes = frames * g_slots * sizeof(std::int32_t);

    g_model.arriving();
    std::size_t written = 0;
    // portMAX_DELAY: block until the DMA has room. This is what paces the
    // player at real time - the DAC's clock, not a delay.
    (void)i2s_channel_write(g_tx, g_interleaved.data(), bytes, &written, portMAX_DELAY);
    g_model.queued(bytes);
}

const char* sink_name() { return "tdm"; }

int sink_slots() { return static_cast<int>(g_slots); }

std::uint64_t sink_frames_written() { return g_model.writes(); }

// As in sink/i2s/: the channel runs on between plays, so only the model starts
// again. See audio_sink.hpp.
void sink_begin_play() { g_model.restart(); }

// The DAC's side of the wire is not observable here; sink/capture/ with
// CONFIG_AC3FORGE_EXAMPLE_CAPTURE_TDM checks this sink's conversion. Whether
// the samples arrived in time is - see sink_common.hpp.
void sink_report() { g_model.report(); }

}  // namespace player

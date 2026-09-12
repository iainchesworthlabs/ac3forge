// The real sink: an I2S DAC, standard mode or TDM depending on what the
// active layout needs, on one or two of the S3's I2S lines.
//
// Selected by default. See ../../audio_sink.hpp for why this is a directory
// CMake picks rather than a branch in the player, and
// ../../../../include/ac3forge/sink_plan.hpp for the mode/slot-count
// arithmetic this file only calls, and for why a second line has to share
// line 0's bit clock and word select rather than free-run on its own.
//
// 1-2 channels: standard I2S, mono or stereo slot mode. 3 or more: TDM, up to
// one line's own ceiling (4 slots at 32-bit slots, 2 at 16 - 16-bit TDM does
// not exist in this codebase, see interleave.hpp). Past one line's ceiling,
// a second line - AC3FORGE_EXAMPLE_I2S_SECOND_LINE - takes the overflow,
// sharing line 0's BCLK/WS pins as inputs (the GPIO matrix routes a pad's
// input side per peripheral independently of who drives it as an output, so
// this needs no external jumper) and running as a slave in TDM mode at the
// same width line 0 runs, real channels filling from line 0 first.
//
// Slot width and sample rate are 32-bit-slots-at-32-bit-samples by default
// (CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS; 16 is the other answer): 32-bit
// slots carry the 24-bit samples every DAC here accepts and a SigmaDSP
// requires, and cost nothing but bit clock. Line 0 is master by default;
// CONFIG_AC3FORGE_EXAMPLE_I2S_SLAVE hands its clocks to the DAC instead, which
// is how an ADAU1452 or ADAU1467 wants it when the DSP is the house's clock -
// line 1, when it exists, is always a slave to whichever end drives those
// pins, since its own job is only to read them.
//
// RECONFIGURING BETWEEN PLAYS, NOT MID-PLAY. sink_open is callable more than
// once - stream_player.cpp's begin_play calls it again whenever the layout
// about to play needs a different slot count or mode than what is currently
// open - and each line prefers i2s_channel_reconfig_std_slot/_tdm_slot over a
// full disable+delete+recreate whenever it can stay in the same mode: no new
// DMA descriptors, no new GPIO routing, just a different slot layout. Only a
// std<->TDM crossing, or a line coming up or going down entirely, tears the
// channel down. Every channel this file ever creates asks for the same DMA
// depth regardless of how many slots it is actually carrying today - sized
// once, from this line's OWN ceiling rather than the layout in hand - which
// is what keeps a reconfigure that stays in one mode from also having to
// reallocate the DMA buffers under it; whether ESP-IDF v6.1's driver actually
// avoids reallocating them on a slot-only reconfig, rather than just avoiding
// the channel recreation around it, is the piece most worth confirming on a
// board.

#include "audio_sink.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

#include "ac3/core/tables.hpp"
#include "ac3forge/dac_queue_model.hpp"
#include "ac3forge/interleave.hpp"
#include "ac3forge/sink_plan.hpp"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "../sink_common.hpp"

namespace player {
namespace {

constexpr int kSlotBits = CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS;
static_assert(kSlotBits == 16 || kSlotBits == 32,
              "CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS is 16 or 32");
constexpr bool kSlave = CONFIG_AC3FORGE_EXAMPLE_I2S_SLAVE != 0;
constexpr bool kSecondLineEnabled = CONFIG_AC3FORGE_EXAMPLE_I2S_SECOND_LINE != 0;
constexpr std::size_t kBytesPerSlot = static_cast<std::size_t>(kSlotBits) / 8;

// This line's own ceiling at this build's slot width, by hand: 4 at 32 bits,
// 2 at 16, matching ac3forge::line_ceiling(kSlotBits).slots exactly - that
// function cannot fill in for this, despite the duplication, because it
// takes slot_bits at runtime and this needs a compile-time array bound.
constexpr std::size_t kMaxLineSlots = kSlotBits == 32 ? 4 : 2;

// The combined ceiling both lines together could ever carry - what
// sink_slots() reports, and what accept_layout() (stream_player.cpp) checks
// a requested layout against before any of this runs.
const std::size_t kCeiling = ac3forge::sink_ceiling(kSlotBits, kSecondLineEnabled);

// One line's hardware state. GPIO numbers and role are fixed for the run
// (Kconfig) and passed in rather than stored here - see kLine0Gpio/kLine1Gpio
// below - so this is only what changes across a reconfigure.
struct Line {
    i2s_chan_handle_t chan = nullptr;
    bool tdm = false;          // which mode `chan` is presently running, if it exists at all
    std::size_t slots = 0;     // its current frame width; 0 means this line is not open
    std::size_t channels = 0;  // how many of those slots carry real audio, from the plan
};

struct LineGpio {
    gpio_num_t bclk;
    gpio_num_t ws;
    gpio_num_t dout;
    bool slave;
};

const LineGpio kLine0Gpio{
    static_cast<gpio_num_t>(CONFIG_AC3FORGE_EXAMPLE_I2S_BCLK_GPIO),
    static_cast<gpio_num_t>(CONFIG_AC3FORGE_EXAMPLE_I2S_WS_GPIO),
    static_cast<gpio_num_t>(CONFIG_AC3FORGE_EXAMPLE_I2S_DOUT_GPIO),
    kSlave,
};
// BCLK/WS are line 0's own pins, read here as a second input rather than
// wired anywhere new - see the top-of-file comment. Always a slave: its job
// is to read those pins, not decide what they carry.
const LineGpio kLine1Gpio{
    kLine0Gpio.bclk,
    kLine0Gpio.ws,
    static_cast<gpio_num_t>(CONFIG_AC3FORGE_EXAMPLE_I2S_DOUT2_GPIO),
    true,
};

Line g_line0;
Line g_line1;
ac3forge::DacQueueModel g_model;

// One block of interleaved TDM per line, at that line's own ceiling width -
// 4 slots of 32 bits, 1 KB - and one block of interleaved 16-bit stereo,
// which is the only shape standard mode at 16 bits ever sends (see
// sink_write). At namespace scope because the sink is the only thing that
// needs them - the player hands over planar float and never sees this format
// at all.
std::array<std::int32_t, ac3::kSamplesPerBlock * kMaxLineSlots> g_wide0{};
std::array<std::int32_t, ac3::kSamplesPerBlock * kMaxLineSlots> g_wide1{};
std::array<std::int16_t, ac3::kSamplesPerBlock * 2> g_narrow{};

// The DMA queue, from Kconfig - see main/Kconfig.projbuild for why the
// default is smaller than a frame. Computed once, from this line's ceiling
// bytes-per-frame rather than whatever the active layout needs, so
// dma_desc_num/dma_frame_num - the values a line is actually created with -
// stay the same across every reconfigure that stays within one mode; see the
// top-of-file comment on why that matters and what is not yet confirmed
// about it.
constexpr int kDmaDescriptors = CONFIG_AC3FORGE_EXAMPLE_I2S_DMA_DESCRIPTORS;
constexpr int kDmaFrames = CONFIG_AC3FORGE_EXAMPLE_I2S_DMA_FRAMES;
const DmaPlan g_dma_plan =
    dma_plan(kDmaDescriptors, kDmaFrames, kMaxLineSlots * kBytesPerSlot, ac3::kSamplesPerBlock);

// Standard mode's slot_mode for `slots` real channels. 16-bit slots always
// run stereo - interleave_16 has no narrower or padded form, so a mono
// layout at this width is duplicated onto both slots by hand in sink_write,
// exactly as the pre-dynamic sink always sent it. 32-bit slots use
// interleave_24in32, which supports one slot natively, so a mono layout
// there is I2S_SLOT_MODE_MONO and the driver's own job to put it on the
// wire - standard for an I2S DAC, and also worth confirming on a board.
i2s_slot_mode_t std_slot_mode(std::size_t slots) {
    if (kSlotBits == 16) {
        return I2S_SLOT_MODE_STEREO;
    }
    return slots <= 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
}

// Which slots a TDM frame carries. Always the full width, not the channel
// count: a TDM frame is a fixed shape, and a 5.1 programme on an 8-slot bus
// leaves two slots that must still be written (with zeros - see
// interleave.hpp).
i2s_tdm_slot_mask_t slot_mask(std::size_t slots) {
    unsigned mask = 0;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        mask |= 1U << slot;
    }
    return static_cast<i2s_tdm_slot_mask_t>(mask);
}

// Brings `line` to exactly `plan.slots` slots in `plan.tdm` mode, allocating
// a channel only if it does not have one yet, tearing it down if `plan.slots`
// is now zero, and preferring i2s_channel_reconfig_std_slot/_tdm_slot over a
// full teardown whenever the mode does not have to cross the std/TDM
// boundary - see the top-of-file comment on why, and how far that goes
// without a board to check it against.
bool configure_line(Line& line, const ac3forge::SinkLinePlan& plan, const LineGpio& gpio,
                    std::uint32_t sample_rate) {
    line.channels = plan.channels;

    if (plan.slots == 0) {
        if (line.chan != nullptr) {
            (void)i2s_channel_disable(line.chan);
            (void)i2s_del_channel(line.chan);
            line.chan = nullptr;
        }
        line.slots = 0;
        return true;
    }

    if (line.chan != nullptr && line.tdm == plan.tdm && line.slots == plan.slots) {
        return true;  // already exactly this - nothing to reconfigure
    }

    if (line.chan != nullptr && line.tdm == plan.tdm) {
        if (i2s_channel_disable(line.chan) != ESP_OK) {
            return false;
        }
        esp_err_t err;
        if (plan.tdm) {
            i2s_tdm_slot_config_t slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO, slot_mask(plan.slots));
            err = i2s_channel_reconfig_tdm_slot(line.chan, &slot_cfg);
        } else {
            i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                kSlotBits == 32 ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT,
                std_slot_mode(plan.slots));
            err = i2s_channel_reconfig_std_slot(line.chan, &slot_cfg);
        }
        if (err != ESP_OK || i2s_channel_enable(line.chan) != ESP_OK) {
            std::printf("error: could not reconfigure an I2S line to %u slots\n",
                        static_cast<unsigned>(plan.slots));
            return false;
        }
        line.slots = plan.slots;
        return true;
    }

    // No channel yet, or the mode itself has to change:
    // i2s_channel_reconfig_*_slot cannot cross std/TDM, so this is a full
    // teardown and recreate. dma_desc_num/dma_frame_num come from
    // g_dma_plan - this line's ceiling, not plan.slots - every time, so a
    // later reconfigure that stays in the new mode never needs this path
    // again to get back to the same depth.
    if (line.chan != nullptr) {
        (void)i2s_channel_disable(line.chan);
        (void)i2s_del_channel(line.chan);
        line.chan = nullptr;
    }
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, gpio.slave ? I2S_ROLE_SLAVE : I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = static_cast<uint32_t>(g_dma_plan.descriptors);
    chan_cfg.dma_frame_num = static_cast<uint32_t>(g_dma_plan.frames);
    chan_cfg.auto_clear = true;  // silence on underrun, not the last buffer again
    if (i2s_new_channel(&chan_cfg, &line.chan, nullptr) != ESP_OK) {
        std::printf("error: could not allocate an I2S channel\n");
        return false;
    }

    // Field by field onto a zeroed struct: C++ requires designated
    // initialisers in declaration order and IDF's layout is free to change
    // between versions.
    esp_err_t init_err;
    if (plan.tdm) {
        i2s_tdm_config_t tdm_cfg = {};
        tdm_cfg.clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(sample_rate);
        tdm_cfg.slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO, slot_mask(plan.slots));
        tdm_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
        tdm_cfg.gpio_cfg.bclk = gpio.bclk;
        tdm_cfg.gpio_cfg.ws = gpio.ws;
        tdm_cfg.gpio_cfg.dout = gpio.dout;
        tdm_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
        init_err = i2s_channel_init_tdm_mode(line.chan, &tdm_cfg);
    } else {
        i2s_std_config_t std_cfg = {};
        std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
        std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            kSlotBits == 32 ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT,
            std_slot_mode(plan.slots));
        std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
        std_cfg.gpio_cfg.bclk = gpio.bclk;
        std_cfg.gpio_cfg.ws = gpio.ws;
        std_cfg.gpio_cfg.dout = gpio.dout;
        std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
        init_err = i2s_channel_init_std_mode(line.chan, &std_cfg);
    }
    if (init_err != ESP_OK || i2s_channel_enable(line.chan) != ESP_OK) {
        std::printf("error: could not start I2S%s\n", plan.tdm ? " in TDM mode" : "");
        return false;
    }
    line.tdm = plan.tdm;
    line.slots = plan.slots;
    return true;
}

}  // namespace

bool sink_open(std::uint32_t sample_rate, int channels) {
    if (channels <= 0) {
        std::printf("error: %d channels is not a sink to open\n", channels);
        return false;
    }
    const auto plan = ac3forge::plan_sink(static_cast<std::size_t>(channels), kSlotBits,
                                          kSecondLineEnabled);
    if (!plan.has_value()) {
        std::printf("error: %d channels do not fit this sink's %u-slot ceiling (%d-bit slots, "
                    "%s line)\n",
                    channels, static_cast<unsigned>(kCeiling), kSlotBits,
                    kSecondLineEnabled ? "a second" : "no second");
        return false;
    }

    // 16-bit standard mode has one physical shape - two slots, always -
    // regardless of whether the plan's logical slot count is one (a mono
    // layout) or two: interleave_16 has no narrower form, so this line
    // always opens stereo at this width and sink_write duplicates a mono
    // channel onto both by hand, as the pre-dynamic sink always did.
    ac3forge::SinkLinePlan line0_plan = plan->line0;
    if (kSlotBits == 16 && line0_plan.slots > 0) {
        line0_plan.slots = 2;
    }

    if (!configure_line(g_line0, line0_plan, kLine0Gpio, sample_rate) ||
        !configure_line(g_line1, plan->line1, kLine1Gpio, sample_rate)) {
        return false;
    }

    const std::size_t bytes_per_frame = g_line0.slots * kBytesPerSlot;
    const std::size_t dma_bytes = static_cast<std::size_t>(g_dma_plan.descriptors) *
                                  static_cast<std::size_t>(g_dma_plan.frames) * bytes_per_frame;
    g_model.open(sample_rate * static_cast<std::uint32_t>(bytes_per_frame), dma_bytes);

    std::printf("sink: i2s %lu Hz %d-bit, %d channels: line0 %u slots%s%s, %s, bclk=%d ws=%d "
                "dout=%d, dma=%dx%d frames (%ld ms)\n",
                static_cast<unsigned long>(sample_rate), kSlotBits, channels,
                static_cast<unsigned>(g_line0.slots), g_line0.tdm ? " (tdm)" : "",
                g_line1.slots > 0 ? ", line1 in use (tdm)" : "",
                kSlave ? "slave (the DAC clocks)" : "master", kLine0Gpio.bclk, kLine0Gpio.ws,
                kLine0Gpio.dout, g_dma_plan.descriptors, g_dma_plan.frames,
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

    g_model.arriving(esp_timer_get_time());
    std::size_t bytes_for_model = 0;

    if (kSlotBits == 16) {
        // A mono layout to both slots - see std_slot_mode's comment.
        const std::array<std::span<const float>, 2> pair = {
            channels[0], channels.size() > 1 ? channels[1] : channels[0]};
        ac3forge::interleave_16(pair, frames, std::span<std::int16_t>{g_narrow.data(), frames * 2});
        std::size_t written = 0;
        (void)i2s_channel_write(g_line0.chan, g_narrow.data(), frames * 2 * sizeof(std::int16_t),
                                &written, portMAX_DELAY);
        bytes_for_model = frames * 2 * sizeof(std::int16_t);
    } else {
        if (g_line0.slots > 0) {
            const std::size_t used = std::min(g_line0.channels, channels.size());
            ac3forge::interleave_24in32(
                channels.subspan(0, used), g_line0.slots, frames,
                std::span<std::int32_t>{g_wide0.data(), frames * g_line0.slots});
            std::size_t written = 0;
            (void)i2s_channel_write(g_line0.chan, g_wide0.data(),
                                    frames * g_line0.slots * sizeof(std::int32_t), &written,
                                    portMAX_DELAY);
            bytes_for_model = frames * g_line0.slots * sizeof(std::int32_t);
        }
        if (g_line1.slots > 0) {
            const std::size_t offset = g_line0.channels;
            const std::size_t available = channels.size() > offset ? channels.size() - offset : 0;
            const std::size_t used = std::min(g_line1.channels, available);
            ac3forge::interleave_24in32(
                channels.subspan(offset, used), g_line1.slots, frames,
                std::span<std::int32_t>{g_wide1.data(), frames * g_line1.slots});
            std::size_t written = 0;
            (void)i2s_channel_write(g_line1.chan, g_wide1.data(),
                                    frames * g_line1.slots * sizeof(std::int32_t), &written,
                                    portMAX_DELAY);
        }
    }

    g_model.queued(bytes_for_model, esp_timer_get_time());
}

const char* sink_name() { return "i2s"; }

int sink_slots() { return static_cast<int>(kCeiling); }

std::uint64_t sink_frames_written() { return g_model.writes(); }

// The channels stay enabled from one play to the next and play zeros once
// their queue runs dry, so a new play has nothing to set up here: only the
// model starts again. See audio_sink.hpp.
void sink_begin_play() { g_model.restart(); }

// What the DAC did with the samples is not visible from this side of the wire -
// sink/capture/ is the one that checks the conversion, and it runs the same
// interleave this does. What IS visible is whether the samples got there in
// time, and that is what this reports: see ac3forge/dac_queue_model.hpp.
void sink_report() { g_model.report(); }

}  // namespace player

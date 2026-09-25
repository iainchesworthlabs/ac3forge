// The real sink for a part with a WIDE I2S TDM frame - the ESP32-P4's one
// controller carries 512 bits a frame (I2S_LL_SLOT_FRAME_BIT_MAX), four
// times the ESP32-S3/C6's 128, which is enough on its own to reach this
// product's full sixteen-channel target (planning/esp32-sink-tiers.md's
// "best" tier: one wide controller into both ES9080 DACs, 16x32-bit) - on
// >=v3.0 silicon. See "TDM MODE IS NOT REACHABLE..." below for what this
// means on the pre-production chip revision this was brought up on.
//
// See ../../audio_sink.hpp for why this is a directory CMake picks rather
// than a branch in the player, and ../../../../include/ac3forge/sink_plan.hpp
// for the mode/slot-count arithmetic this file only calls.
//
// ONE LINE, DELIBERATELY, NOT A PARAMETERIZED COPY OF sink/i2s. That file's
// second line exists because the S3/C6 need one to reach past their own
// 128-bit ceiling, and it shares line 0's bit clock/word-select pins - both
// facts specific to a part whose one controller cannot reach the product
// target alone. Reusing that file's second-line machinery here would also be
// wrong on this part specifically: I2S_LL_INST_NUM is 3 on the ESP32-P4 (three
// physical controllers), so sink/i2s's `kSecondController = I2S_LL_INST_NUM
// > 1` would read true and a stored "second line wired" setting could bring
// up an independent second 512-bit line where the product wants one wide
// line into both DACs. This file's sink_second_line_possible() answers false
// unconditionally instead of deriving it from the instance count, and never
// asks sink_plan.hpp for a second line at all.
//
// 1-2 channels: standard I2S, mono or stereo slot mode. 3 or more: TDM, up to
// this controller's own ceiling (16 slots at 32-bit, 32 at 16: a TDM frame
// holds I2S_LL_SLOT_FRAME_BIT_MAX bits). CONFIG_AC3FORGE_EXAMPLE_I2S_FIXED_FRAME=1
// opens TDM at the full width for 1-2 channels too, for a TDM DAC set up for
// one frame shape (an ES9080) - see sink_plan.hpp's header comment for why.
//
// TDM MODE IS NOT REACHABLE ON PRE-PRODUCTION (< v3.0) P4 SILICON AT ALL,
// found on a board 2026-09-23, not specific to sixteen slots. TDM always
// opens at the line's FULL width regardless of how many of those slots a
// layout actually uses (slot_mask below is "always the full width, not the
// channel count" - see sink/i2s/audio_sink.cpp's own copy of that rule,
// because a real TDM DAC is set up for one fixed frame shape and a play
// cannot change it out from under it). That full width is
// I2S_LL_SLOT_FRAME_BIT_MAX - 512 bits, fixed by the part, not by the
// layout - and this chip revision's I2S peripheral has no PLL clock source
// at all (components/esp_hal_i2s/esp32p4/include/hal/i2s_ll.h: "No PLL
// clock source before version 3, use XTAL as default"). Of the two sources
// that remain, XTAL is 40 MHz (nowhere close) and APLL's own hardware
// ceiling is 125 MHz (CLK_LL_APLL_MAX_HZ) - enough for at most ~13 slots at
// 32-bit (~434 bits/frame) at 48 kHz, short of the 16 this sink's TDM path
// always asks for once ANY layout has 3 or more channels. A 7.1.4 (12
// slots' worth of real channels) play still failed identically to a 9.1.6
// one - same "adjust the mclk multiple to 1536" - because both open the
// same 16-slot frame. Confirmed correct on the arithmetic and clock-source
// choice below (I2S_CLK_SRC_APLL, itself needed - the DEFAULT source is
// worse, not better); this is a real ceiling of this exact silicon
// revision, not a bug here. A >=v3.0 chip's 160 MHz PLL
// (I2S_CLK_SRC_PLL_160M) clears this with room to spare - see
// docs/platforms/bare-metal/esp32-p4.md for this board's other
// pre-production-only findings.
//
// Slot width and sample rate: CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS, 32 by
// default, reused unchanged from the narrow sink - the same Kconfig options
// this file and sink/i2s share are mutually exclusive at build time via the
// sink choice, so there is no conflict in reading the same symbols.
//
// RECONFIGURING BETWEEN PLAYS, NOT MID-PLAY, exactly as sink/i2s: sink_open
// is callable more than once, and prefers i2s_channel_reconfig_std_slot/
// _tdm_slot over a full disable+delete+recreate whenever it can stay in the
// same mode.

#include "audio_sink.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

#include "ac3/core/tables.hpp"
#include "ac3forge/dac_queue_model.hpp"
#include "ac3forge/interleave.hpp"
#include "ac3forge/playout.hpp"
#include "ac3forge/sink_plan.hpp"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/i2s_ll.h"

#include "../../settings.hpp"
#include "../sink_common.hpp"

namespace player {
namespace {

// See sink/i2s/audio_sink.cpp's copy of this variable for the full reasoning
// - unchanged here.
int g_slot_bits = CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS;
static_assert(CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS == 16 ||
                  CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS == 32,
              "CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS is 16 or 32");
constexpr bool kSlave = CONFIG_AC3FORGE_EXAMPLE_I2S_SLAVE != 0;
constexpr ac3forge::SinkFrame kFrame = CONFIG_AC3FORGE_EXAMPLE_I2S_FIXED_FRAME != 0
                                           ? ac3forge::SinkFrame::fixed
                                           : ac3forge::SinkFrame::follow_layout;

[[nodiscard]] std::size_t bytes_per_slot() { return static_cast<std::size_t>(g_slot_bits) / 8; }

[[nodiscard]] i2s_data_bit_width_t data_bits() {
    return g_slot_bits == 32 ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT;
}

// This line's own ceiling at the width in force - what sink_slots() reports.
// No second line, ever: see the top-of-file comment.
[[nodiscard]] std::size_t ceiling() {
    return ac3forge::sink_ceiling(g_slot_bits, /*second_line=*/false, I2S_LL_SLOT_FRAME_BIT_MAX);
}

// One line's hardware state - see sink/i2s/audio_sink.cpp's copy.
struct Line {
    i2s_chan_handle_t chan = nullptr;
    bool tdm = false;
    std::size_t slots = 0;
    std::size_t channels = 0;
};

struct LineGpio {
    gpio_num_t bclk;
    gpio_num_t ws;
    gpio_num_t dout;
    bool slave;
};

const LineGpio kLineGpio{
    static_cast<gpio_num_t>(CONFIG_AC3FORGE_EXAMPLE_I2S_BCLK_GPIO),
    static_cast<gpio_num_t>(CONFIG_AC3FORGE_EXAMPLE_I2S_WS_GPIO),
    static_cast<gpio_num_t>(CONFIG_AC3FORGE_EXAMPLE_I2S_DOUT_GPIO),
    kSlave,
};

Line g_line;
ac3forge::DacQueueModel g_model;

ac3forge::DmaRing g_ring;
ac3forge::DmaClock g_clock;

IRAM_ATTR bool on_sent(i2s_chan_handle_t /*handle*/, i2s_event_data_t* /*event*/, void* /*context*/) {
    g_ring.sent(esp_timer_get_time());
    return false;
}

// One block of interleaved samples, in whichever width the sink is set to.
// This controller's frame is I2S_LL_SLOT_FRAME_BIT_MAX bits however it
// divides - sixteen 32-bit slots or thirty-two 16-bit ones on the ESP32-P4 -
// so I2S_LL_SLOT_FRAME_BIT_MAX/8 bytes serves both, four times sink/i2s's
// buffer at this part's 512-bit frame.
union LineBuffer {
    std::array<std::int32_t, ac3::kSamplesPerBlock * (I2S_LL_SLOT_FRAME_BIT_MAX / 32)> wide;
    std::array<std::int16_t, ac3::kSamplesPerBlock * (I2S_LL_SLOT_FRAME_BIT_MAX / 16)> narrow;
};
static_assert(sizeof(LineBuffer) == ac3::kSamplesPerBlock * (I2S_LL_SLOT_FRAME_BIT_MAX / 8),
              "a line's block is I2S_LL_SLOT_FRAME_BIT_MAX/8 bytes a frame at either slot width");
LineBuffer g_buffer{};

constexpr int kDmaDescriptors = CONFIG_AC3FORGE_EXAMPLE_I2S_DMA_DESCRIPTORS;
constexpr int kDmaFrames = CONFIG_AC3FORGE_EXAMPLE_I2S_DMA_FRAMES;
// I2S_LL_SLOT_FRAME_BIT_MAX/8 bytes a frame at either slot width, so the plan
// does not move when the width does - see sink/i2s/audio_sink.cpp's copy of
// this constant for why that matters (dma_frame_num has to keep dividing
// every write).
constexpr std::size_t kLineBytesPerFrame = I2S_LL_SLOT_FRAME_BIT_MAX / 8;
const DmaPlan g_dma_plan =
    dma_plan(kDmaDescriptors, kDmaFrames, kLineBytesPerFrame, ac3::kSamplesPerBlock);

void restart_clock(std::uint32_t sample_rate) {
    g_ring.reset();
    g_clock.open(static_cast<std::uint32_t>(g_dma_plan.descriptors), static_cast<std::uint32_t>(g_dma_plan.frames),
                 sample_rate);
}

// See sink/i2s/audio_sink.cpp's copy for the reasoning; unchanged here.
i2s_slot_mode_t std_slot_mode(std::size_t slots) {
    if (g_slot_bits == 16) {
        return I2S_SLOT_MODE_STEREO;
    }
    return slots <= 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
}

i2s_tdm_slot_mask_t slot_mask(std::size_t slots) {
    unsigned mask = 0;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        mask |= 1U << slot;
    }
    return static_cast<i2s_tdm_slot_mask_t>(mask);
}

// Brings the line to exactly `plan.slots` slots in `plan.tdm` mode - see
// sink/i2s/audio_sink.cpp's configure_line for the full reasoning on the
// reconfigure-in-place preference. Identical logic, one line instead of two.
void close_line(Line& line) {
    if (line.chan != nullptr) {
        (void)i2s_channel_disable(line.chan);
        (void)i2s_del_channel(line.chan);
        line.chan = nullptr;
    }
    line.slots = 0;
    line.channels = 0;
}

bool configure_line(Line& line, const ac3forge::SinkLinePlan& plan, const LineGpio& gpio,
                    std::uint32_t sample_rate) {
    line.channels = plan.channels;

    if (plan.slots == 0) {
        close_line(line);
        return true;
    }

    if (line.chan != nullptr && line.tdm == plan.tdm && line.slots == plan.slots) {
        return true;
    }

    if (line.chan != nullptr && line.tdm == plan.tdm) {
        if (i2s_channel_disable(line.chan) != ESP_OK) {
            return false;
        }
        restart_clock(sample_rate);
        esp_err_t err;
        if (plan.tdm) {
            i2s_tdm_slot_config_t slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
                data_bits(), I2S_SLOT_MODE_STEREO, slot_mask(plan.slots));
            err = i2s_channel_reconfig_tdm_slot(line.chan, &slot_cfg);
        } else {
            i2s_std_slot_config_t slot_cfg =
                I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(data_bits(), std_slot_mode(plan.slots));
            err = i2s_channel_reconfig_std_slot(line.chan, &slot_cfg);
        }
        if (err != ESP_OK || i2s_channel_enable(line.chan) != ESP_OK) {
            std::printf("error: could not reconfigure the I2S line to %u slots\n",
                        static_cast<unsigned>(plan.slots));
            return false;
        }
        line.slots = plan.slots;
        return true;
    }

    if (line.chan != nullptr) {
        (void)i2s_channel_disable(line.chan);
        (void)i2s_del_channel(line.chan);
        line.chan = nullptr;
    }
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, gpio.slave ? I2S_ROLE_SLAVE : I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = static_cast<uint32_t>(g_dma_plan.descriptors);
    chan_cfg.dma_frame_num = static_cast<uint32_t>(g_dma_plan.frames);
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &line.chan, nullptr) != ESP_OK) {
        std::printf("error: could not allocate the I2S channel\n");
        return false;
    }
    restart_clock(sample_rate);
    i2s_event_callbacks_t callbacks{};
    callbacks.on_sent = &on_sent;
    if (i2s_channel_register_event_callback(line.chan, &callbacks, nullptr) != ESP_OK) {
        std::printf("warning: no DMA interrupts from the I2S line; a Sendspin stream plays untimed\n");
    }

    esp_err_t init_err;
    if (plan.tdm) {
        i2s_tdm_config_t tdm_cfg = {};
        tdm_cfg.clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(sample_rate);
        // I2S_CLK_SRC_DEFAULT resolves to a fixed clock (I2S_LL_DEFAULT_CLK_SRC) rather
        // than one tuned to what is asked of it. At this controller's full sixteen
        // 32-bit slots the required bit clock is high enough that
        // I2S_TDM_CLK_DEFAULT_CONFIG's own 256x MCLK multiple is not just "not the
        // fastest option" but flatly too small - i2s_tdm_calculate_clock corrects it
        // to 1536x on its own (esp_driver_i2s/i2s_tdm.c) - and DEFAULT's fixed source
        // clock does not have the headroom that correction then needs, failing
        // "sample rate is too large" before a channel is even enabled. Found on a
        // board 2026-09-23: the 1-2 channel case never hits this (standard mode's own,
        // much narrower, clock math), which is why sink/i2s's TDM path - verified on
        // the S3 and C6 already, both a different clock tree from this part's - was no
        // guide here. APLL (SOC_I2S_SUPPORTS_APLL) is tuned to the mclk it is actually
        // asked for (i2s_set_get_apll_freq) rather than fixed, and is the better of the
        // two sources this pre-production chip revision actually has (see the
        // top-of-file comment) - but APLL's own 125 MHz ceiling still falls short of
        // what TDM's full sixteen-slot frame needs, so this line makes TDM mode
        // reachable for NOTHING wider than about 13 slots at 32-bit on this exact
        // revision, not the sixteen the arithmetic below plans for. Left in rather
        // than reverted to DEFAULT: it is still strictly better here (DEFAULT cannot
        // open TDM at ANY width past two channels on this revision, APLL can up to
        // ~13), and it is exactly correct, with full headroom, on >=v3.0 silicon.
        tdm_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
        tdm_cfg.slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(data_bits(), I2S_SLOT_MODE_STEREO,
                                                               slot_mask(plan.slots));
        tdm_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
        tdm_cfg.gpio_cfg.bclk = gpio.bclk;
        tdm_cfg.gpio_cfg.ws = gpio.ws;
        tdm_cfg.gpio_cfg.dout = gpio.dout;
        tdm_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
        init_err = i2s_channel_init_tdm_mode(line.chan, &tdm_cfg);
    } else {
        i2s_std_config_t std_cfg = {};
        std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
        std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(data_bits(), std_slot_mode(plan.slots));
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

std::size_t write_line(const Line& line, std::span<const std::span<const float>> channels,
                       std::size_t frames, LineBuffer& buffer) {
    std::size_t bytes = 0;
    const void* data = nullptr;
    if (g_slot_bits == 16 && line.tdm) {
        ac3forge::interleave_16in16(channels, line.slots, frames,
                                    std::span<std::int16_t>(buffer.narrow)
                                        .first(frames * line.slots));
        bytes = frames * line.slots * sizeof(std::int16_t);
        data = buffer.narrow.data();
    } else if (g_slot_bits == 16) {
        const std::array<std::span<const float>, 2> pair = {
            channels[0], channels.size() > 1 ? channels[1] : channels[0]};
        ac3forge::interleave_16(pair, frames, std::span<std::int16_t>(buffer.narrow).first(frames * 2));
        bytes = frames * 2 * sizeof(std::int16_t);
        data = buffer.narrow.data();
    } else {
        ac3forge::interleave_24in32(channels, line.slots, frames,
                                    std::span<std::int32_t>(buffer.wide).first(frames * line.slots));
        bytes = frames * line.slots * sizeof(std::int32_t);
        data = buffer.wide.data();
    }
    std::size_t written = 0;
    (void)i2s_channel_write(line.chan, data, bytes, &written, portMAX_DELAY);
    return bytes;
}

}  // namespace

bool sink_open(std::uint32_t sample_rate, int channels) {
    if (channels <= 0) {
        std::printf("error: %d channels is not a sink to open\n", channels);
        return false;
    }
    const auto plan = ac3forge::plan_sink(static_cast<std::size_t>(channels), g_slot_bits,
                                          /*second_line=*/false, kFrame, I2S_LL_SLOT_FRAME_BIT_MAX);
    if (!plan.has_value()) {
        std::printf("error: %d channels do not fit this sink's %u-slot ceiling (%d-bit slots)\n",
                    channels, static_cast<unsigned>(ceiling()), g_slot_bits);
        return false;
    }

    ac3forge::SinkLinePlan line_plan = plan->line0;
    if (g_slot_bits == 16 && !line_plan.tdm && line_plan.slots > 0) {
        line_plan.slots = 2;
    }

    if (!configure_line(g_line, line_plan, kLineGpio, sample_rate)) {
        return false;
    }

    const std::size_t bytes_per_frame = g_line.slots * bytes_per_slot();
    const std::size_t dma_bytes = static_cast<std::size_t>(g_dma_plan.descriptors) *
                                  static_cast<std::size_t>(g_dma_plan.frames) * bytes_per_frame;
    g_model.open(sample_rate * static_cast<std::uint32_t>(bytes_per_frame), dma_bytes);

    const bool fixed = kFrame == ac3forge::SinkFrame::fixed;
    const char* mode = "";
    if (g_line.tdm) {
        mode = fixed ? " (tdm, fixed frame)" : " (tdm)";
    }
    std::printf("sink: i2s-wide %lu Hz %d-bit, %d channels: %u slots%s, %s, bclk=%d ws=%d "
                "dout=%d, dma=%dx%d frames (%ld ms)\n",
                static_cast<unsigned long>(sample_rate), g_slot_bits, channels,
                static_cast<unsigned>(g_line.slots), mode,
                kSlave ? "slave (the DAC clocks)" : "master", kLineGpio.bclk, kLineGpio.ws,
                kLineGpio.dout, g_dma_plan.descriptors, g_dma_plan.frames,
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

    if (g_line.slots > 0) {
        const std::size_t used = std::min(g_line.channels, channels.size());
        bytes_for_model = write_line(g_line, channels.subspan(0, used), frames, g_buffer);
    }

    g_model.queued(bytes_for_model, esp_timer_get_time());
}

std::optional<ac3forge::PlayoutWrite> sink_write_timed(std::span<const std::span<const float>> channels) {
    if (g_line.slots == 0 || channels.empty()) {
        return std::nullopt;
    }
    sink_write(channels);
    const auto buffers = static_cast<std::uint32_t>(ac3::kSamplesPerBlock / static_cast<std::size_t>(g_dma_plan.frames));
    const std::optional<ac3forge::DmaClock::Taken> taken = g_clock.took(g_ring, esp_timer_get_time(), buffers);
    if (!taken) {
        return std::nullopt;
    }
    return ac3forge::PlayoutWrite{.play_us = taken->play_us, .late = taken->late, .gap = taken->skipped > 0};
}

const char* sink_name() { return "i2s_wide"; }

int sink_slots() { return static_cast<int>(ceiling()); }

int sink_max_slots() {
    return static_cast<int>(std::max(
        ac3forge::sink_ceiling(16, /*second_line=*/false, I2S_LL_SLOT_FRAME_BIT_MAX),
        ac3forge::sink_ceiling(32, /*second_line=*/false, I2S_LL_SLOT_FRAME_BIT_MAX)));
}

// sink_ceiling()'s own arithmetic (sink_plan.hpp: slots = frame_bit_max /
// slot_bits) means the max above always comes from the narrower width - see
// sink/i2s's own copy of this comment. Not the whole story on this exact
// part below chip revision v3.0 though: see ac3forge::HardwareFacts's
// revision_hard_limit_below (control.cpp) for why the wide line cannot
// reach ANY TDM channel count above 2 there regardless of what this number
// says, a fact this sink has no way to know from here (it has no chip
// revision to read).
int sink_max_slots_bit_width() { return 16; }

// Never a second line on this sink - see the top-of-file comment on why
// I2S_LL_INST_NUM alone (3 on the ESP32-P4) is not a safe proxy here, unlike
// sink/i2s's kSecondController.
bool sink_second_line_possible() { return false; }

int sink_slot_bits() { return g_slot_bits; }

bool sink_set_slot_bits(int bits) {
    if (bits != 16 && bits != 32) {
        std::printf("error: %d-bit slots is not a width this sink has (16 or 32)\n", bits);
        return false;
    }
    if (bits == g_slot_bits) {
        return true;
    }
    g_slot_bits = bits;
    close_line(g_line);
    std::printf("sink: i2s-wide slot width %d-bit, ceiling %u slots\n", g_slot_bits,
                static_cast<unsigned>(ceiling()));
    return true;
}

std::uint64_t sink_frames_written() { return g_model.writes(); }

void sink_begin_play() { g_model.restart(); }

void sink_close() { close_line(g_line); }

void sink_report() { g_model.report(); }

}  // namespace player

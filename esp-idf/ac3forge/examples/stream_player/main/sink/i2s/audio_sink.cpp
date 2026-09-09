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
#include "esp_timer.h"
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

// Stereo 16-bit is fixed by sink_open below, so the byte counts are known here.
constexpr std::size_t kBytesPerSampleFrame = 2 * sizeof(std::int16_t);
constexpr std::size_t kFrameBytes = ac3::kSamplesPerFrame * kBytesPerSampleFrame;
// The DMA queue, from Kconfig - see main/Kconfig.projbuild for why the default
// is smaller than a frame.
constexpr int kDmaDescriptors = CONFIG_AC3FORGE_EXAMPLE_I2S_DMA_DESCRIPTORS;
constexpr int kDmaFrames = CONFIG_AC3FORGE_EXAMPLE_I2S_DMA_FRAMES;
constexpr std::size_t kDmaBytes =
    static_cast<std::size_t>(kDmaDescriptors) * static_cast<std::size_t>(kDmaFrames) *
    kBytesPerSampleFrame;

// --- what the DAC heard, by the DAC's own clock -------------------------------
// The driver says nothing when the DMA runs dry: with auto_clear it plays zeros
// and carries on, and the gap is audible and otherwise unrecorded. So the queue
// is modelled here from the one fact the hardware guarantees - it drains at
// exactly the sample rate, whatever the CPU does.
//
// When a write returns, every byte of it has been queued, so the DMA holds what
// it held on arrival plus this frame, less what drained while the write waited,
// and never more than its capacity. When the NEXT frame arrives, what remains is
// that figure less the drain since - and if that is zero or negative, the DAC
// has been playing silence for the difference. That is an underrun, counted
// and timed below.
//
// It is a model, and it is out by up to one descriptor: the write returns when
// its last bytes land in a descriptor buffer, not when the queue is full to the
// byte. Five milliseconds at the default depth, which is worth knowing when
// reading min_headroom and not enough to mistake a stall for a smooth run.
std::uint32_t g_bytes_per_second = 0;
std::int64_t g_queue_bytes = 0;      // what the DMA held when the last write returned
std::int64_t g_queue_stamp_us = 0;   // and when that was
std::uint64_t g_underruns = 0;       // frames that arrived to an empty DMA
std::uint64_t g_dry_us = 0;          // how long it had been empty, summed
std::int64_t g_min_headroom_us = -1; // least audio queued as a frame arrived; -1 until measured

std::int64_t bytes_to_us(std::int64_t bytes) {
    return (bytes * 1000000) / static_cast<std::int64_t>(g_bytes_per_second);
}

std::int64_t us_to_bytes(std::int64_t us) {
    return (us * static_cast<std::int64_t>(g_bytes_per_second)) / 1000000;
}

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
    g_bytes_per_second = sample_rate * static_cast<std::uint32_t>(kBytesPerSampleFrame);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    // Four descriptors of 240 frames by default: 3,840 bytes holding 20 ms.
    // Small on purpose - every millisecond of buffer is a millisecond of
    // latency, and a deep buffer would hide a decoder that cannot keep up,
    // which is one of the things this example exists to reveal. Kconfig, so a
    // source that needs more can ask for it without touching this file.
    chan_cfg.dma_desc_num = kDmaDescriptors;
    chan_cfg.dma_frame_num = kDmaFrames;
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
    std::printf("sink: i2s %lu Hz 16-bit stereo, bclk=%d ws=%d dout=%d, dma=%dx%d frames (%ld ms)\n",
                static_cast<unsigned long>(sample_rate), CONFIG_AC3FORGE_EXAMPLE_I2S_BCLK_GPIO,
                CONFIG_AC3FORGE_EXAMPLE_I2S_WS_GPIO, CONFIG_AC3FORGE_EXAMPLE_I2S_DOUT_GPIO,
                kDmaDescriptors, kDmaFrames,
                static_cast<long>(bytes_to_us(static_cast<std::int64_t>(kDmaBytes)) / 1000));
    return true;
}

void sink_write(std::span<const std::span<const float>> channels) {
    // Through the shared conversion, not a copy of it: sink/capture/ checks
    // this exact code on target, which it could not if each sink had its own.
    interleave_16(channels, ac3::kSamplesPerFrame, g_interleaved);

    // Where the queue stands as this frame arrives: what was there when the
    // last write returned, less what the DAC has drained since. Not for the
    // first frame - the DMA has been playing zeros since sink_open and nothing
    // was owed to it yet.
    const std::int64_t arrived = esp_timer_get_time();
    if (g_frames > 0) {
        const std::int64_t headroom = g_queue_bytes - us_to_bytes(arrived - g_queue_stamp_us);
        if (headroom <= 0) {
            ++g_underruns;
            g_dry_us += static_cast<std::uint64_t>(bytes_to_us(-headroom));
        }
        const std::int64_t headroom_us = bytes_to_us(headroom < 0 ? 0 : headroom);
        if (g_min_headroom_us < 0 || headroom_us < g_min_headroom_us) {
            g_min_headroom_us = headroom_us;
        }
        g_queue_bytes = headroom < 0 ? 0 : headroom;
    }

    std::size_t written = 0;
    // portMAX_DELAY: block until the DMA has room. This is what paces the
    // player at real time - the DAC's clock, not a delay - and it is why the
    // timing figures mean something here and nothing under an emulator.
    (void)i2s_channel_write(g_tx, g_interleaved.data(),
                            g_interleaved.size() * sizeof(std::int16_t), &written,
                            portMAX_DELAY);
    const std::int64_t returned = esp_timer_get_time();

    // Every byte is queued now, so the DMA holds what it had plus this frame,
    // less what drained while the write waited, and never more than it can.
    std::int64_t queued =
        g_queue_bytes + static_cast<std::int64_t>(kFrameBytes) - us_to_bytes(returned - arrived);
    if (queued > static_cast<std::int64_t>(kDmaBytes)) {
        queued = static_cast<std::int64_t>(kDmaBytes);
    }
    if (queued < 0) {
        queued = 0;
    }
    g_queue_bytes = queued;
    g_queue_stamp_us = returned;
    ++g_frames;
}

const char* sink_name() { return "i2s"; }

std::uint64_t sink_frames_written() { return g_frames; }

// What the DAC did with the samples is not visible from this side of the wire -
// sink/capture/ is the one that checks the conversion, and it runs the same
// interleave this does. What IS visible is whether the samples got there in
// time, and that is what this reports: see the model above.
void sink_report() {
    std::printf("sink.writes=%lu sink.underruns=%lu sink.dry_ms=%lu sink.min_headroom_ms=%ld "
                "sink.dma_ms=%ld\n",
                static_cast<unsigned long>(g_frames), static_cast<unsigned long>(g_underruns),
                static_cast<unsigned long>(g_dry_us / 1000),
                static_cast<long>(g_min_headroom_us < 0 ? -1 : g_min_headroom_us / 1000),
                static_cast<long>(bytes_to_us(static_cast<std::int64_t>(kDmaBytes)) / 1000));
}

}  // namespace player

// Decode AC-3 and play it out of an I2S DAC.
//
// The smallest thing that is actually a player rather than a measurement: it
// takes the 5.1 fixture apps/baremetal/fixture.hpp already carries, folds it to
// stereo with the decoder's own §7.8 output stage, and writes 16-bit frames to
// an I2S peripheral in a loop. Flash it at a MAX98357A, a PCM5102 or any other
// I2S DAC and it makes a noise.
//
// WHY THIS IS AN EXAMPLE AND NOT PART OF THE PROBE. apps/baremetal/probe.cpp is
// a measuring instrument whose numbers CI holds to ceilings; adding a
// peripheral, a DMA buffer and a playback loop to it would move every one of
// those numbers and measure something nobody asked about. This is the other
// half - what the decoder is FOR - and it is kept where an integrator can copy
// it.
//
// IT ALSO ANSWERS THE OPEN QUESTION. Whether this part decodes in real time is
// unmeasured, and QEMU cannot say: it is not cycle-accurate and reports a CPU
// clock that disagrees with its own boot log (docs/platforms/esp32.md). On
// hardware, I2S is a clock - the DMA drains at exactly 48,000 frames a second
// whatever the CPU is doing - so a decode that cannot keep up underruns
// audibly, and the numbers this prints are from silicon rather than from an
// emulator. That is why it counts and reports the margin rather than only
// playing.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"

#include "fixture.hpp"

namespace {

constexpr std::uint32_t kSampleRate = 48000;
// §5.3.2: 1,536 samples a frame at 48 kHz, so 32 ms of audio. The budget every
// per-frame figure below is measured against.
constexpr std::uint64_t kFrameDurationUs = 32000;
constexpr std::size_t kOutputChannels = 2;

// --- caller-owned PCM ------------------------------------------------------
// The same shape the probe uses and the same reason: decode_frame_into writes
// through spans the caller owns, which is what an embedded integrator has - one
// block, sized once, reused every frame.
//
// Six channels because the fixture is 5.1. The decoder folds to stereo before
// it returns (see the OutputConfig below), so only the first two hold anything
// meaningful afterwards - but the fold happens IN this storage, so it has to be
// wide enough for the coded programme going in, not for the two coming out.
constexpr std::size_t kCodedChannels = 6;
std::array<std::array<float, ac3::kSamplesPerFrame>, kCodedChannels> g_pcm{};
std::array<std::span<float>, kCodedChannels> g_pcm_spans{};

// One frame of interleaved 16-bit stereo, which is what the I2S peripheral
// consumes. At namespace scope rather than in a function's frame: 6,144 bytes
// is more than a default FreeRTOS task stack has spare, and putting it on the
// stack is how you get a crash somewhere unrelated a long way from the cause.
std::array<std::int16_t, ac3::kSamplesPerFrame * kOutputChannels> g_interleaved{};

i2s_chan_handle_t g_tx = nullptr;

// float to 16-bit, with clipping rather than wraparound.
//
// The decoder emits floats nominally in [-1, 1), and §7.8's normalisation
// bounds the Lo/Ro fold by the loudest coded sample - so in this configuration
// an out-of-range sample should not arrive. Clamped anyway, because the cost is
// two comparisons per sample and the failure mode without them is not a quiet
// bit of distortion: an int16 that wraps turns a peak into full-scale noise of
// the opposite sign, which is the loudest sound the system can make. That is
// worth two comparisons.
//
// 32767 rather than 32768 as the scale, so +1.0 maps to full scale and does not
// need the clamp to catch it.
std::int16_t to_pcm16(float sample) {
    constexpr float kScale = 32767.0F;
    const float scaled = sample * kScale;
    if (scaled >= kScale) {
        return 32767;
    }
    if (scaled <= -kScale) {
        return -32767;
    }
    return static_cast<std::int16_t>(scaled);
}

// Four descriptors of 256 frames: see start_i2s for why so few, and why 256.
constexpr std::uint32_t kDmaDescriptors = 4;
constexpr std::uint32_t kDmaFrames = 256;

// What the driver saw, counted in the I2S interrupt: every DMA buffer the
// peripheral finished sending, and every one it came to with nothing new
// written into it - the send queue overflowed, which is an underrun, played as
// zeros because of auto_clear. The first gives the clock the DAC is actually
// driven at; the second, the silence the loop let in. Neither depends on the
// loop's own idea of time.
std::atomic<std::uint32_t> g_buffers_sent{0};
std::atomic<std::uint32_t> g_buffers_starved{0};

bool on_sent(i2s_chan_handle_t /*handle*/, i2s_event_data_t* /*event*/, void* /*ctx*/) {
    g_buffers_sent.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool on_send_q_ovf(i2s_chan_handle_t /*handle*/, i2s_event_data_t* /*event*/, void* /*ctx*/) {
    g_buffers_starved.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool start_i2s() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    // Four descriptors of 256 frames each: 4,096 bytes of DMA buffer holding
    // 21 ms of audio. Small on purpose. The decode peaks at 233,546 bytes of a
    // part with 277,400 free (docs/platforms/esp32.md), so what is left for
    // buffering is about 43,000 - and every millisecond of I2S buffer is also a
    // millisecond of latency. 21 ms rides out the jitter between one frame's
    // decode and the next without hiding a decoder that is genuinely too slow,
    // which is the thing this example exists to reveal.
    //
    // 256 because it divides the 1,536-frame write. ESP-IDF v6.1's
    // i2s_channel_write starts a fresh buffer whenever two or more sent ones
    // are waiting for it, and the rest of the buffer in hand goes out
    // unwritten - zeros, with auto_clear. After a 10 ms decode that is every
    // frame, so with 240-frame buffers each frame ended part-way through one
    // and 144 frames of silence went out with it: measured on a DevKitC-1, the
    // loop paced at 35 ms a frame instead of 32, with the clock at exactly
    // 48 kHz and not one buffer starved.
    chan_cfg.dma_desc_num = kDmaDescriptors;
    chan_cfg.dma_frame_num = kDmaFrames;
    chan_cfg.auto_clear = true;  // send zeros on underrun, not the last buffer again
    if (i2s_new_channel(&chan_cfg, &g_tx, nullptr) != ESP_OK) {
        std::printf("error: could not allocate an I2S channel\n");
        return false;
    }

    // Field-by-field onto a zeroed struct rather than one braced initialiser.
    // C++ requires designated initialisers to appear in declaration order and
    // IDF's layout is free to change between versions; this compiles whatever
    // order the fields are in, and the zeroing means a field added upstream
    // starts at zero rather than at whatever was on the stack.
    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    std_cfg.slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(CONFIG_AC3FORGE_EXAMPLE_I2S_BCLK_GPIO);
    std_cfg.gpio_cfg.ws = static_cast<gpio_num_t>(CONFIG_AC3FORGE_EXAMPLE_I2S_WS_GPIO);
    std_cfg.gpio_cfg.dout = static_cast<gpio_num_t>(CONFIG_AC3FORGE_EXAMPLE_I2S_DOUT_GPIO);
    std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;

    if (i2s_channel_init_std_mode(g_tx, &std_cfg) != ESP_OK) {
        std::printf("error: could not configure I2S in standard mode\n");
        return false;
    }
    // Before enabling: the driver refuses DMA callbacks on a running channel.
    i2s_event_callbacks_t callbacks = {};
    callbacks.on_sent = on_sent;
    callbacks.on_send_q_ovf = on_send_q_ovf;
    if (i2s_channel_register_event_callback(g_tx, &callbacks, nullptr) != ESP_OK) {
        std::printf("error: could not register the I2S callbacks\n");
        return false;
    }
    if (i2s_channel_enable(g_tx) != ESP_OK) {
        std::printf("error: could not enable the I2S channel\n");
        return false;
    }
    std::printf("i2s: 48000 Hz, 16-bit stereo, bclk=%d ws=%d dout=%d\n",
                CONFIG_AC3FORGE_EXAMPLE_I2S_BCLK_GPIO, CONFIG_AC3FORGE_EXAMPLE_I2S_WS_GPIO,
                CONFIG_AC3FORGE_EXAMPLE_I2S_DOUT_GPIO);
    return true;
}

}  // namespace

extern "C" void app_main() {
    std::printf("ac3forge i2s_player: AC-3 5.1 folded to stereo\n");

    for (std::size_t ch = 0; ch < kCodedChannels; ++ch) {
        g_pcm_spans[ch] = std::span<float>(g_pcm[ch]);
    }

    if (!start_i2s()) {
        return;
    }

    const std::span<const std::byte> stream{
        reinterpret_cast<const std::byte*>(ac3probe::kAc3Stream.data()),
        ac3probe::kAc3Stream.size()};
    const auto frames = ac3::split_frames(stream);
    if (!frames) {
        std::printf("error: the fixture did not split into frames (%d)\n",
                    static_cast<int>(frames.error()));
        return;
    }
    // split_frames succeeding with nothing in it is not a case this fixture
    // can reach, but the loop below divides by the number of frames played -
    // so the thing that cannot happen is checked here rather than trapped as
    // a divide by zero a few hundred milliseconds later.
    if (frames->empty()) {
        std::printf("error: the fixture contains no frames\n");
        return;
    }

    // The decoder does the fold itself. §7.8's Lo/Ro against ac3::OutputStage
    // in this file would be the same arithmetic written twice, and the copy
    // here would be the one without tests.
    //
    // kLine is the §7.7.1 operating mode - dialnorm normalisation plus the full
    // transmitted dynamic range - which is what a decoder feeding an amplifier
    // does. Without it, playback level follows whatever the encoder's dialnorm
    // happened to be and two different streams play back at two different
    // loudnesses, which on a DAC with a fixed analogue gain is the difference
    // between quiet and painful.
    ac3::FrameDecoder decoder{{.output = {.target = ac3::DownmixTarget::kLoRo,
                                          .mode = ac3::OperatingMode::kLine}}};

    std::uint64_t decode_us = 0;
    std::uint64_t worst_frame_us = 0;
    std::uint64_t played = 0;
    std::uint32_t laps = 0;
    std::int64_t first_lap_us = 0;
    std::uint32_t first_lap_sent = 0;
    std::uint64_t first_lap_played = 0;

    // Loops forever. The I2S write below blocks until the DMA has room, so the
    // loop is paced by the DAC's own clock rather than by a delay - which is
    // both how a player should be written and what makes the timing figures
    // below mean something.
    while (true) {
        for (const auto frame : *frames) {
            const std::int64_t started = esp_timer_get_time();
            const auto decoded = decoder.decode_frame_into(frame, g_pcm_spans);
            const std::uint64_t elapsed =
                static_cast<std::uint64_t>(esp_timer_get_time() - started);
            if (!decoded) {
                std::printf("error: decode failed (%d)\n", static_cast<int>(decoded.error()));
                return;
            }
            decode_us += elapsed;
            if (elapsed > worst_frame_us) {
                worst_frame_us = elapsed;
            }

            for (std::size_t n = 0; n < ac3::kSamplesPerFrame; ++n) {
                g_interleaved[n * 2] = to_pcm16(g_pcm[0][n]);
                g_interleaved[(n * 2) + 1] = to_pcm16(g_pcm[1][n]);
            }

            std::size_t written = 0;
            // portMAX_DELAY: block until the DMA has taken it. This is the
            // back-pressure that keeps the loop at real time, and a decode too
            // slow to keep up shows up as a gap in the audio rather than as a
            // return value - which is why the margin is reported instead.
            if (i2s_channel_write(g_tx, g_interleaved.data(), sizeof(g_interleaved), &written,
                                  portMAX_DELAY) != ESP_OK) {
                std::printf("error: I2S write failed\n");
                return;
            }
            ++played;
        }

        ++laps;
        // Once per lap of the fixture - 192 ms of audio - rather than per frame,
        // so the console does not become the thing that misses the deadline.
        //
        // realtime_permille is decode time against wall-clock audio time: 1000
        // is exactly real time and anything at or above it cannot play without
        // gaps. The worst SINGLE frame matters as much as the average, because
        // 20 ms of DMA buffer only absorbs a spike that small.
        //
        // i2s_hz is the clock the peripheral is really running at: buffers the
        // driver reported sent since the first lap, times their length, over
        // the time between. starved_buffers is how many of those the DMA came
        // to with nothing new in them, each one kDmaFrames of silence.
        // wall_us_per_frame is the loop's own pace over the same stretch, by
        // the part's clock: 32,000 when every frame went out whole and on
        // time. Read it rather than the arrival times of these lines, which
        // USB-Serial-JTAG delivers in bursts.
        const std::uint64_t permille = (decode_us * 1000) / (kFrameDurationUs * played);
        const std::int64_t now_us = esp_timer_get_time();
        const std::uint32_t sent = g_buffers_sent.load(std::memory_order_relaxed);
        if (laps == 1) {
            first_lap_us = now_us;
            first_lap_sent = sent;
            first_lap_played = played;
        }
        const std::uint64_t wall_us_per_frame =
            played > first_lap_played
                ? static_cast<std::uint64_t>(now_us - first_lap_us) / (played - first_lap_played)
                : 0;
        const std::uint64_t i2s_hz =
            now_us > first_lap_us
                ? (static_cast<std::uint64_t>(sent - first_lap_sent) * kDmaFrames * 1000000ULL) /
                      static_cast<std::uint64_t>(now_us - first_lap_us)
                : 0;
        std::printf("lap=%lu frames=%lu us_per_frame=%lu worst_frame_us=%lu "
                    "realtime_permille=%lu heap_free=%lu i2s_hz=%lu starved_buffers=%lu "
                    "wall_us_per_frame=%lu\n",
                    static_cast<unsigned long>(laps), static_cast<unsigned long>(played),
                    static_cast<unsigned long>(decode_us / played),
                    static_cast<unsigned long>(worst_frame_us),
                    static_cast<unsigned long>(permille),
                    static_cast<unsigned long>(
                        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                    static_cast<unsigned long>(i2s_hz),
                    static_cast<unsigned long>(g_buffers_starved.load(std::memory_order_relaxed)),
                    static_cast<unsigned long>(wall_us_per_frame));
    }
}

#pragma once

#include <cstddef>

// What the two I2S sinks (sink/i2s/, sink/tdm/) share and the others do not
// need: the shape of the DMA queue. The model of what the DAC heard, which they
// share as well, is in the component - ac3forge/dac_queue_model.hpp - free of
// ESP-IDF, so that the host tests can run it.

namespace player {

// --- the DMA queue's shape ------------------------------------------------------
// Kconfig gives a depth as descriptors x frames (main/Kconfig.projbuild), sized
// for stereo. The driver caps a descriptor at 4,092 bytes and quietly shortens
// one that asks for more, so eight 32-bit slots at the stereo default of 240
// frames would silently get 127 and a queue a quarter as deep as configured.
// This keeps the DEPTH - the product - and splits it into as many descriptors
// as the bus width needs.
//
// And each descriptor DIVIDES a write, so no write ever ends part-way through
// one. ESP-IDF v6.1's i2s_channel_write starts a fresh buffer whenever two or
// more sent buffers are already waiting for the writer, and whatever it had
// not yet written of the one in hand goes out as it stands - zeros, with
// auto_clear. A write that ends mid-descriptor therefore turns into silence
// whenever the next one is a little late: measured on a DevKitC-1, 1,536-frame
// writes into 240-frame descriptors paced at 35 ms a frame instead of 32, the
// extra three milliseconds being the unwritten 144 frames of every seventh
// buffer. The player writes one 256-frame block per call, so a descriptor of
// 128 frames (stereo) or 64 (12 TDM slots) always ends exactly where a write
// does.
struct DmaPlan {
    int descriptors;
    int frames;  // per descriptor
};

inline DmaPlan dma_plan(int budget_descriptors, int budget_frames, std::size_t bytes_per_frame,
                        std::size_t write_frames) {
    constexpr std::size_t kMaxDescriptorBytes = 4092;  // I2S_DMA_BUFFER_MAX_SIZE
    const std::size_t total =
        static_cast<std::size_t>(budget_descriptors) * static_cast<std::size_t>(budget_frames);
    std::size_t cap = bytes_per_frame == 0 ? 1 : kMaxDescriptorBytes / bytes_per_frame;
    if (cap > static_cast<std::size_t>(budget_frames)) {
        cap = static_cast<std::size_t>(budget_frames);
    }
    if (write_frames > 0 && cap > write_frames) {
        cap = write_frames;
    }
    // The largest descriptor under the cap that a write divides into exactly.
    std::size_t per = 1;
    for (std::size_t frames = cap; frames > 1; --frames) {
        if (write_frames == 0 || write_frames % frames == 0) {
            per = frames;
            break;
        }
    }
    std::size_t descriptors = (total + per - 1) / per;
    if (descriptors < 2) {
        descriptors = 2;
    }
    return {static_cast<int>(descriptors), static_cast<int>(per)};
}

}  // namespace player

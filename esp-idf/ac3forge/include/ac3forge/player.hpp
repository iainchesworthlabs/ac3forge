#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "freertos/FreeRTOS.h"

#include "ac3/decoder/decoder.hpp"

// The player: bytes in, sound out, on two cores.
//
// This is the ESP-IDF-specific layer of ac3forge - the part that cannot live in
// the library because it is made of FreeRTOS tasks, a ring buffer between them
// and a pair of seams for the two things that differ per product: where the
// bitstream comes from and where the audio goes. planning/esp32-player.md says
// why it is here and not in the library or in an example.
//
// The shape, and the reason for it. A fetch task on one core reads the source
// into a ring; a decode task on the other drains the ring through
// ac3::io::AccessUnitAccumulator, decodes each access unit into caller-owned
// storage, and writes the folded frame to the sink. The sink blocks until the
// DAC has taken the frame, which is what paces the player at real time. A
// source that blocks - a socket waiting on the network - blocks the fetch task
// and nothing else: the decode keeps draining the ring, and the ring's depth is
// how long a stall the DAC never hears. The single loop this replaced had 20 ms
// of I2S DMA between a slow read and silence.
//
// WiFi and TCP/IP run on core 0, so the fetch task goes beside them and the
// decode task has core 1 to itself. Both are PlayerConfig fields, because a
// build under QEMU or on a single-core part wants them elsewhere.
//
// Both decoders, chosen per access unit. Eac3Decoder reads Annex E and accepts
// a plain AC-3 syncframe as one access unit of one substream, but it does not
// survive a fold (its inner FrameDecoder is built with the fold applied, and
// the §E3.8.2 assembly then refuses the two-channel core - recorded as a
// hand-over in planning/esp32-player.md), so a unit that is exactly one AC-3
// syncframe goes to FrameDecoder itself.

namespace ac3forge {

// Where the bitstream comes from. One implementation per transport; the player
// never learns which. Called from the fetch task only.
class ByteSource {
   public:
    virtual ~ByteSource() = default;
    // Fills as much of `dst` as it has, blocking for more if it must. 0 means
    // end of stream - "there will never be more", not "wait".
    [[nodiscard]] virtual std::size_t read(std::span<std::byte> dst) = 0;
    // Back to the beginning, for a player that loops. False if this source
    // cannot; a socket generally cannot, and saying so beats pretending.
    [[nodiscard]] virtual bool rewind() = 0;
};

// Where decoded audio goes. One frame per call: `channels` planar spans of
// kSamplesPerFrame floats, nominally in [-1, 1), in the decoder's own order
// after the configured fold. Called from the decode task only.
//
// Planar float rather than interleaved integers because the sample format is
// the sink's business: standard I2S wants 16-bit stereo, a TDM bus wants 24 bits
// in 32-bit slots with the unused slots zeroed, and the conversion is one pass
// either way - ac3forge/interleave.hpp has both.
class PcmSink {
   public:
    virtual ~PcmSink() = default;
    // Blocks until the sink has taken the frame. For a DAC that is the
    // back-pressure that paces the whole player; a sink with no peripheral
    // returns at once and the player runs flat out.
    virtual void write(std::span<const std::span<const float>> channels) = 0;
};

struct PlayerConfig {
    // The fold and operating mode, and whether to reconstruct objects. A
    // stereo sink wants kLoRo or kLtRt with objects skipped: an Atmos stream's
    // bed is the complete mix, and reconstruction costs this part about 10 ms
    // of every 32 ms frame for objects a stereo DAC cannot place.
    ac3::DecoderConfig decoder{.output = {.target = ac3::DownmixTarget::kLoRo,
                                         .mode = ac3::OperatingMode::kLine},
                               .skip_object_reconstruction = true};
    // How many channels the fold leaves, which is how many spans the sink is
    // handed. 2 for the folds; the coded count for kAsCoded.
    std::size_t output_channels = 2;

    // The ring between fetch and decode, in bytes of bitstream. 32 KB is
    // 0.57 s at 448 kbit/s. Preferably in PSRAM where the part has it - a
    // buffer this size is exactly what external RAM is for - and in internal
    // SRAM otherwise, since a build with PSRAM off still wants to work.
    std::size_t ring_bytes = 32768;
    bool ring_in_psram = true;
    // How much the fetch task asks the source for at a time. Deliberately
    // smaller than the framing buffer, so the accumulator's "need more input"
    // path runs on a real device and not only in its unit tests.
    std::size_t fetch_bytes = 2048;

    // Cores and priorities. tskNO_AFFINITY for either core lets the scheduler
    // choose, which is what a QEMU run or a single-core part wants.
    BaseType_t fetch_core = 0;
    BaseType_t decode_core = 1;
    UBaseType_t fetch_priority = 5;
    UBaseType_t decode_priority = 6;
    // The decode task's stack. 32 KB is what the probe measured a decode
    // needing about 21 KB of, with an overflow that surfaced as a panic on the
    // other core when it was smaller (apps/baremetal/platform/esp32s3/
    // sdkconfig.defaults).
    std::uint32_t decode_stack_bytes = 32768;
    std::uint32_t fetch_stack_bytes = 8192;

    // Passes through the stream before stopping. 0 plays until the source
    // cannot rewind. A source that cannot rewind ends the run after one pass
    // whatever this says.
    std::uint32_t max_passes = 0;

    // A linear gain applied to the folded frame before it reaches the sink,
    // 0.0 to 1.0. Changeable while playing through Player::set_volume().
    float volume = 1.0F;
};

// What the first decoded access unit said the stream is.
struct StreamInfo {
    bool eac3 = false;
    int acmod = 0;
    int channels = 0;
    int substreams = 0;
    int dialnorm = 0;
    bool objects = false;
};

struct PlayerStats {
    std::uint64_t frames_played = 0;    // access units that produced audio
    std::uint64_t frames_held = 0;      // released one call late (§3.7)
    std::uint64_t decode_us = 0;        // inside the decoder, summed
    std::uint64_t worst_frame_us = 0;
    std::uint64_t fetched_bytes = 0;    // taken from the source
    std::uint64_t resync_bytes = 0;     // skipped looking for a sync word
    std::uint32_t passes = 0;           // completed passes through the stream
    // The least the ring ever held when the decoder came for more, in bytes,
    // measured while the source was still delivering. Zero means the decoder
    // waited on the source at least once; how far above zero it stays is the
    // margin the ring's depth is buying. `ring_low_valid` is false until a
    // measurement has been taken - a stream shorter than the ring, or one that
    // has just started, has nothing to say yet.
    std::size_t ring_low_water = 0;
    bool ring_low_valid = false;
    // The least stack the decode task has had spare, in bytes, sampled at each
    // pass boundary and when the run ends: PlayerConfig::decode_stack_bytes
    // minus this is what the decode actually used. Zero until sampled.
    std::size_t decode_stack_free = 0;
    bool finished = false;
    bool failed = false;
    // Why the run ended, once `finished`: "passes" (max_passes reached), "end
    // of stream" (the source could not rewind), or with `failed` set,
    // "framing" or "decode" with the library's own error code in `error`.
    const char* failure = "";
    int error = 0;
};

class Player {
   public:
    // The source and sink outlive the player. Nothing runs until start().
    Player(const PlayerConfig& config, ByteSource& source, PcmSink& sink);
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Allocates the ring and the framing buffer and starts both tasks. False
    // means it could not, having said why on the console.
    [[nodiscard]] bool start();
    // Stops both tasks and waits for them. Safe to call twice, or never: the
    // destructor calls it.
    void stop();

    // A snapshot, safe from any task at any time.
    [[nodiscard]] PlayerStats stats() const;
    // The snapshot the decode task took as the most recent pass through the
    // stream completed - the figures a per-pass line should carry, rather than
    // whatever stats() says by the time a reporting task wakes up to print it.
    // `passes` in the result says which pass it describes.
    [[nodiscard]] PlayerStats last_pass() const;
    // Set once the first access unit has decoded.
    [[nodiscard]] std::optional<StreamInfo> stream() const;
    // True once the run has ended: the last pass played, the source could not
    // rewind, or something failed. stats() says which.
    [[nodiscard]] bool finished() const;
    // Blocks until finished(), or for `ticks`. Returns finished().
    bool wait(TickType_t ticks = portMAX_DELAY);

    // The gain the decode task applies to the next frame onwards; clamped to
    // 0.0 to 1.0. Safe from any task.
    void set_volume(float volume);
    [[nodiscard]] float volume() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3forge

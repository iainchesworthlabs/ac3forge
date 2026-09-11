#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "freertos/FreeRTOS.h"

#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"

#include "ac3forge/layout.hpp"

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
// ac3::io::AccessUnitAccumulator, decodes each access unit a block at a time
// (decode_access_unit_by_block: 256 samples of every channel, and the objects
// beside them when there are any), renders each block onto the configured
// speaker layout (ac3forge/render.hpp) and writes it to the sink. The sink
// blocks until the DAC has taken the block, which is what paces the player at
// real time. A source that blocks - a socket waiting on the network - blocks
// the fetch task and nothing else: the decode keeps draining the ring, and the
// ring's depth is how long a stall the DAC never hears. The single loop this
// replaced had 20 ms of I2S DMA between a slow read and silence.
//
// A block at a time rather than a frame, because the storage is the
// difference between a height layout fitting on this part or not: one block of
// sixteen slots is 16 KB where a frame of them is 96 KB, and the decoder's own
// block form copies nothing on the way.
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

// Where decoded audio goes. One BLOCK per call: one planar span of float per
// slot of the configured OutputLayout, in slot order, each ac3::kSamplesPerBlock
// samples long or fewer, nominally in [-1, 1). Called from one task only, six
// times per frame at 48 kHz: the decode task, or the output task when
// PlayerConfig::output_blocks gives the player one.
//
// Planar float rather than interleaved integers because the sample format is
// the sink's business: standard I2S wants two slots of 16 or 32 bits, a TDM bus
// wants 24 bits in 32-bit slots with the unused slots zeroed, and the
// conversion is one pass either way - ac3forge/interleave.hpp has both.
class PcmSink {
   public:
    virtual ~PcmSink() = default;
    // Blocks until the sink has taken the block. For a DAC that is the
    // back-pressure that paces the whole player; a sink with no peripheral
    // returns at once and the player runs flat out.
    virtual void write(std::span<const std::span<const float>> slots) = 0;
};

struct PlayerConfig {
    // The speakers, one per output slot - ac3forge/layout.hpp. What the sink is
    // handed is one span per slot of this, whatever the stream was coded as.
    OutputLayout layout = OutputLayout::stereo();
    // Which §7.8 fold a two-speaker layout gets: kLoRo, or kLtRt for a Dolby
    // Surround decoder downstream. A one-speaker layout folds to mono; every
    // other layout is rendered as coded (see `objects`) and this is unused.
    ac3::DownmixTarget stereo_fold = ac3::DownmixTarget::kLoRo;
    // Whether to reconstruct a stream's object layer and place the objects on
    // the speakers by their own positions, or play the bed (the objects' 5.1
    // fold, which is the complete mix for a stereo or 5.1 room). Costs this
    // part about 10 ms of every 32 ms frame and, under the QMF domain, about
    // 233 KB of heap - PSRAM territory. kAuto reconstructs exactly when the
    // layout has height speakers, which is the case the bed cannot serve;
    // kAlways does so for any rendered layout; kNever plays the bed. A layout
    // that folds never reconstructs.
    enum class Objects : std::uint8_t { kAuto, kNever, kAlways };
    Objects objects = Objects::kAuto;

    // The decoder's own knobs: operating mode, DRC, the JOC domain, the
    // programme. Its `output.target` and `skip_object_reconstruction` are
    // decided by the player from `layout` and `objects` above, whatever is set
    // here.
    ac3::DecoderConfig decoder{.output = {.mode = ac3::OperatingMode::kLine}};

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
    // sdkconfig.defaults). PlayerStats::decode_stack_free says what a run used.
    std::uint32_t decode_stack_bytes = 32768;
    std::uint32_t fetch_stack_bytes = 8192;

    // The output task: rendering onto the layout and writing to the sink on a
    // core of its own, fed by a ring of this many decoded blocks. 0 keeps both
    // inside the decode task, as the player did before the task existed.
    //
    // Why it exists. The render, the level meter a sink may run and the sink's
    // conversion all used to run on the decode's core, after the decode, while
    // the other core - WiFi's - sat three quarters idle (planning/
    // esp32-714-realtime.md). Moved there, they stop adding to the decode's
    // time, and the ring, not the DMA queue, rides out a slow frame: sixteen
    // blocks is 85 ms of audio, and the sink's DMA queue can shrink to what
    // covers the output task being held off by WiFi.
    //
    // A slot holds the decoder's own block - the coded channels, or the two a
    // fold leaves, and the objects when they are placed - so a slot is 1 KB a
    // channel and sixteen slots of sixteen channels are 256 KB: PSRAM, where
    // the part has it (`output_in_psram`), is where a ring that size belongs.
    std::size_t output_blocks = 0;
    bool output_in_psram = true;
    BaseType_t output_core = 0;
    // Above the fetch task: a block late to the sink is heard, a read late to
    // the ring is not.
    UBaseType_t output_priority = 7;
    std::uint32_t output_stack_bytes = 6144;

    // Passes through the stream before stopping. 0 plays until the source
    // cannot rewind. A source that cannot rewind ends the run after one pass
    // whatever this says.
    std::uint32_t max_passes = 0;

    // A linear gain applied to every slot before it reaches the sink, 0.0 to
    // 1.0. Changeable while playing through Player::set_volume().
    float volume = 1.0F;

    // The rate the sink runs at. A stream at any other is refused - the play
    // fails with the reason "sample rate" and the stream's rate, in Hz, as its
    // error - rather than played at the wrong speed.
    std::uint32_t sample_rate_hz = 48000;
};

// What the first decoded access unit said the stream is, and what the player
// is doing with it.
struct StreamInfo {
    bool eac3 = false;
    int acmod = 0;
    int channels = 0;    // as coded, every substream unioned
    int substreams = 0;
    int dialnorm = 0;
    bool objects = false;           // the stream carries an object layer
    bool objects_rendered = false;  // and this player is placing them
    int slots = 0;                  // what the sink is handed: the layout's

    // How this play serves its layout, for a report such as the web page's
    // (planning/esp32-device-ui.md, "The output layout"). `layout` is the
    // layout's text; `render` is "loro", "ltrt" or "mono" for the decoder's
    // fold, "channels" for the coded channels placed, "objects" for the
    // objects placed; `coded` names the stream's channels by Table E2.5
    // location, comma-separated in the decoder's order ("Ch1,Ch2" for dual
    // mono); `silent` names the layout's speakers this play has sent nothing
    // to so far, and is empty when every one has had something.
    std::array<char, OutputLayout::kTextBytes> layout{};
    const char* render = "";
    std::array<char, 96> coded{};
    std::array<char, 160> silent{};
};

struct PlayerStats {
    std::uint64_t frames_played = 0;    // access units that produced audio
    std::uint64_t frames_held = 0;      // released one call late (§3.7)
    // The decode call, summed. Without an output task the blocks reach the
    // renderer and the sink from inside it, so it includes both: render_us and
    // sink_us are those two parts on their own, and decode_us minus both is the
    // decoder's. With one, render_us and sink_us are the output task's, on its
    // own core, and decode_us is the decoder's plus copying each block into
    // the ring and any wait for a free slot. Either way, on a paced sink the
    // wait for the DAC's clock lands in it: in sink_us directly, or as the
    // decode waiting on a ring the sink is draining at the DAC's rate.
    std::uint64_t decode_us = 0;
    std::uint64_t render_us = 0;
    std::uint64_t sink_us = 0;
    std::uint64_t worst_frame_us = 0;
    std::uint64_t fetched_bytes = 0;    // taken from the source
    std::uint64_t resync_bytes = 0;     // skipped looking for a sync word
    std::uint32_t passes = 0;           // completed passes through the stream
    // Access units whose decoded layout was not the one their headers
    // announced. The bed's placement is set up from the headers before the
    // unit decodes (the block form hands the samples over before the layout),
    // so a mismatch means one unit was placed by the previous layout; the
    // next is placed by the decoded one. Zero for every stream met so far.
    std::uint32_t layout_mismatches = 0;
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
    // With an output task (PlayerConfig::output_blocks): the least number of
    // blocks the ring held as the output task came for the next one, once a
    // ring's worth had played - zero means the task waited on the decode at
    // least once, with only the sink's own queue left playing; and the least
    // stack that task has had spare. `output_low_valid` is false without an
    // output task or before the first measurement.
    std::size_t output_low_blocks = 0;
    bool output_low_valid = false;
    std::size_t output_stack_free = 0;
    bool finished = false;
    bool failed = false;
    // Why the run ended, once `finished`: "passes" (max_passes reached), "end
    // of stream" (the source could not rewind), or with `failed` set,
    // "framing" or "decode" with the library's own error code in `error`,
    // or "sample rate" with the stream's rate in Hz (PlayerConfig::sample_rate_hz).
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

    // The gain the decode task applies to the next block onwards; clamped to
    // 0.0 to 1.0. Safe from any task.
    void set_volume(float volume);
    [[nodiscard]] float volume() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3forge

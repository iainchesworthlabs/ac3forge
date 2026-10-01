#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"

#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/decoder/output.hpp"
#include "iclforge/ac3/decoder/serving.hpp"
#include "iclforge/render/layout.hpp"

// The player: bytes in, sound out, on two cores.
//
// This is the ESP-IDF-specific layer of iclforge - the part that cannot live in
// the library because it is made of FreeRTOS tasks, a ring buffer between them
// and a pair of seams for the two things that differ per product: where the
// bitstream comes from and where the audio goes. planning/esp32-player.md says
// why it is here and not in the library or in an example.
//
// The shape, and the reason for it. A fetch task on one core reads the source
// into a ring; a decode task on the other drains the ring through
// iclforge::ac3::io::AccessUnitAccumulator, decodes each access unit a block at a time
// (decode_access_unit_by_block: 256 samples of every channel, and the objects
// beside them when there are any), renders each block onto the configured
// speaker layout (ac3/render/render.hpp) and writes it to the sink. The sink
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
//
// And AC-4, when the component is built with CONFIG_ICLFORGE_AC4 (its Kconfig):
// a stream that opens with an AC-4 sync word is read by iclforge::ac4::SyncFrameSplitter
// and iclforge::ac4::Decoder in place of the accumulator and the two above, once for the
// whole play, and its blocks go through the same renderer and sink. Everything
// that is AC-4's alone is behind that switch, in this header and in player.cpp
// (src/ac4_bridge.hpp has the rest), so a build without it is what it was.

namespace iclforge {

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
// slot of the configured iclforge::render::OutputLayout, in slot order, each
// iclforge::ac3::kSamplesPerBlock samples long or fewer, nominally in [-1, 1). Called from the
// decode task only, six times per frame at 48 kHz.
//
// Planar float rather than interleaved integers because the sample format is
// the sink's business: standard I2S wants two slots of 16 or 32 bits, a TDM bus
// wants 24 bits in 32-bit slots with the unused slots zeroed, and the
// conversion is one pass either way - iclforge/interleave.hpp has both.
class PcmSink {
   public:
    virtual ~PcmSink() = default;
    // Blocks until the sink has taken the block. For a DAC that is the
    // back-pressure that paces the whole player; a sink with no peripheral
    // returns at once and the player runs flat out.
    virtual void write(std::span<const std::span<const float>> slots) = 0;
};

struct PlayerConfig {
    // The speakers, one per output slot - ac3/render/layout.hpp. What the sink is
    // handed is one span per slot of this, whatever the stream was coded as.
    iclforge::render::OutputLayout layout = iclforge::render::OutputLayout::stereo();
    // Which §7.8 fold a two-speaker layout gets: kLoRo, or kLtRt for a Dolby
    // Surround decoder downstream. A one-speaker layout folds to mono; every
    // other layout is rendered as coded (see `objects`) and this is unused.
    iclforge::ac3::DownmixTarget stereo_fold = iclforge::ac3::DownmixTarget::kLoRo;
    // Whether to reconstruct a stream's object layer and place the objects on
    // the speakers by their own positions, or play the bed (the objects' 5.1
    // fold, which is the complete mix for a stereo or 5.1 room). Costs this
    // part about 10 ms of every 32 ms frame and, under the QMF domain, about
    // 233 KB of heap - PSRAM territory. The policy is the library's
    // (ac3/decoder/serving.hpp): kAuto reconstructs exactly when the layout has
    // height speakers, kAlways for any rendered layout, kNever plays the bed,
    // and a layout that folds never reconstructs.
    using Objects = iclforge::ac3::render::ObjectsPolicy;
    Objects objects = Objects::kAuto;

    // The decoder's own knobs: operating mode, DRC, the JOC domain, the
    // programme. Its `output.target` and `skip_object_reconstruction` are
    // decided by the player from `layout` and `objects` above, whatever is set
    // here.
    iclforge::ac3::DecoderConfig decoder{.output = {.mode = iclforge::ac3::OperatingMode::kLine}};

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

    // Holds each play's first access unit until the second has decoded, so
    // the sink starts with two frames queued rather than one. A play's first
    // frames decode more slowly than the rest, and with one frame queued an
    // ESP32-S3 playing 7.1.4 over WiFi ran its DAC dry in each play's first
    // ten frames and never after (planning/esp32-714-realtime.md, decision
    // 14). Costs a frame more before a play is heard - 32 ms at 48 kHz - and
    // a copy of the unit while it is held, 1 KB a channel for each of its six
    // blocks, in PSRAM where the part has it and freed once the unit plays.
    bool hold_first_unit = false;

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

#if CONFIG_ICLFORGE_AC4
    // How a stream that opens with an AC-4 sync word is decoded
    // (CONFIG_ICLFORGE_AC4; planning/ac4.md, D14b). Its output layout is served
    // as an AC-3 stream's is: the decoder's own fold for a stereo or mono
    // layout (`stereo_fold` above), the renderer placing the decoded bed for
    // any other.
    struct Ac4Options {
        // Core decoding (ETSI TS 103 190-2 clause 4.7): an immersive
        // element's 5.X.2 core, with A-CPL and A-JCC replaced or reduced, in
        // place of full decoding. The Part 1 channel elements decode alike in
        // both.
        bool core = false;
        // FNV-1a over the bit pattern of every sample the decoder hands over,
        // in delivery order (PlayerStats::ac4_pcm_hash), which is what says
        // whether the float output is the same on the host and on this part
        // (decision 26). It takes time, which is measured, so that
        // PlayerStats::ac4_hash_us can be taken back out of decode_us.
        bool pcm_hash = false;
    };
    Ac4Options ac4;
#endif
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
    std::array<char, iclforge::render::OutputLayout::kTextBytes> layout{};
    const char* render = "";
    std::array<char, 96> coded{};
    std::array<char, 160> silent{};

#if CONFIG_ICLFORGE_AC4
    // The stream is AC-4, and `eac3`, `acmod` and `substreams` mean nothing
    // for it: `channels` is what the decoder handed over, and `coded` names
    // them in its order.
    bool ac4 = false;
#endif
};

struct PlayerStats {
    std::uint64_t frames_played = 0;    // access units that produced audio
    std::uint64_t frames_held = 0;      // released one call late (§3.7)
    // The decode call, summed. The blocks reach the renderer and the sink
    // from inside it, so it includes both: render_us and sink_us are those two
    // parts on their own, and decode_us minus both is the decoder's. On a
    // paced sink, sink_us is mostly the wait for the DAC's clock.
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
    bool finished = false;
    bool failed = false;
#if CONFIG_ICLFORGE_AC4
    // What an AC-4 play adds (all zero for any other). `ac4_samples` is the audio
    // the play has decoded, in samples of each channel at the sink's rate: the
    // 2,048 samples of a frame at 23.44 fps, 1,920 at 25 fps and 1,601 or 1,602
    // at 29.97 are not `frames_played` times one figure, so an AC-4 play's
    // real-time ratio is its time against this and not against a frame length.
    // `ac4_hash_us` is the time PlayerConfig::Ac4Options::pcm_hash spent, inside
    // `decode_us`. `ac4_pcm_hash` is that hash of the last completed pass, and of
    // the play so far until one completes.
    std::uint64_t ac4_samples = 0;
    std::uint64_t ac4_hash_us = 0;
    std::uint64_t ac4_pcm_hash = 0;
#endif
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

}  // namespace iclforge

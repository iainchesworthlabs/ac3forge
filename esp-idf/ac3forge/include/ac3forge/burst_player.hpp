#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "freertos/FreeRTOS.h"

#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/render/serving.hpp"
#include "ac3/sendspin/ac3forge_player.hpp"
#include "ac3/sendspin/chunks.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3forge/playout.hpp"

// The Sendspin player's audio (planning/hearth-reference-player.md, B3): what
// the server sends arrives here with the time it should play, and leaves for
// the sink at that time.
//
//   _ac3forge_player@v1's bursts (planning/hearth-sendspin-extension.md, Burst
//   chunks) are decoded, AC-3 or E-AC-3 with any object layer, and rendered
//   onto the board's layout by ac3::render;
//   player@v1's PCM, which is what Music Assistant sends, is rendered onto the
//   same layout as a stereo or mono bed.
//
// Either way each block is then routed to the sink's outputs, trimmed and
// delayed per output (ac3::render::Routing and TrimDelay), replaced by the
// identify tone while one plays, metered, scaled by the player's volume, and
// handed to a Playout, which writes it so that its first frame leaves the
// audio port when the server said.
//
// ONE TASK DOES THE WORK. The Sendspin session's task only copies each chunk,
// with its local time, into a ring - PSRAM where the part has it - and the
// decode task takes them in order, so a slow decode never holds up the
// network. Stream starts, clears and ends travel through the same ring, so
// they take effect exactly between the chunks they came between; a clear or
// an end also drops whatever the ring still holds from before it.
//
// A chunk that could not play in time is dropped before it is decoded
// (late_chunks), and so the next is decoded as the start of a stream. What
// the decoder hands over for a unit it held back (§3.7) is placed by the
// unit's own time: every decoded frame is counted from the stream's start,
// and each burst's time says where its frames go.
//
// Settings from the server - layout, routing, trims, delays, the crossover
// and the decoder's own - are checked when they arrive and applied by the
// decode task at the next burst boundary.

namespace ac3forge {

// Where a burst player writes: a PlayoutSink that can also be opened for a
// number of outputs. The i2s sink is one; the capture sink is the other.
class ScheduledSink : public PlayoutSink {
   public:
    // Opens for `outputs` outputs at `sample_rate`. False when this sink
    // cannot carry that many.
    [[nodiscard]] virtual bool open(std::uint32_t sample_rate, std::size_t outputs) = 0;
    // The most outputs the sink could be opened for now.
    [[nodiscard]] virtual std::size_t max_outputs() = 0;
    // A stream is beginning: the sink's own figures start again.
    virtual void begin_stream() = 0;
};

struct BurstPlayerConfig {
    std::uint32_t sample_rate = 48000;
    // The ring between the session and the decode task, in bytes, chunks and
    // their headers together. In PSRAM where the part has some. What a
    // server may send ahead is this less a reserve kept for stream starts,
    // clears and ends (buffer_capacity()).
    std::size_t ring_bytes = 256 * 1024;
    // The largest chunk taken, and the decode task's buffer for one: an
    // E-AC-3 burst at the bitstream's ceiling is 24,585 bytes, and 150 ms of
    // 24-bit stereo PCM 21,600.
    std::size_t max_chunk_bytes = 25 * 1024;
    // The most outputs the player will ever route to, for its buffers.
    std::size_t max_outputs = Playout::kMaxOutputs;
    // The longest delay a speaker can be given; delay lines for every output,
    // in PSRAM where the part has some. 0 offers no delay.
    double max_delay_ms = 20.0;
    // A syncframe coding more full-bandwidth channels plus LFE than this
    // (FrameHeader::coded_channels()) is refused before a decoder is ever
    // opened for it, rather than decoded: the decoder's own scratch scales
    // with the channel count, and on a part with no PSRAM and a Sendspin
    // ring resident, running out partway through can fail an allocation the
    // heap has no room left to recover from. 0 refuses nothing - the
    // default, and every board with room to decode whatever `outputs.count`
    // does not already turn away.
    std::size_t max_coded_channels = 0;

    BaseType_t core = 1;
    UBaseType_t priority = 6;
    // The decode task's stack, which the decoder needs most of
    // (PlayerConfig::decode_stack_bytes has the measurements).
    std::uint32_t stack_bytes = 32768;
    // A sendspin.progress line on the console every this many chunks while a
    // stream plays, as well as the closing line; 0 for the closing line only.
    std::uint32_t report_every_chunks = 0;
    // The local time a server time plays at by the playing connection's clock
    // as it stands, or nothing; called from any task. A chunk comes with its
    // local time worked out when it arrived, which may be seconds before it
    // plays, and the player moves it by as much as this has moved since. With
    // none, a clock update reaches the playout only with the chunks sent
    // after it.
    std::function<std::optional<std::int64_t>(std::int64_t)> local_time;

    // The layout until a server sends one, and the decoder settings under
    // it: the server's settings replace these whole, as the extension page
    // says, and a stream that sends none plays with these.
    ac3::render::OutputLayout layout = ac3::render::OutputLayout::stereo();
    ac3::DecoderConfig decoder{.output = {.mode = ac3::OperatingMode::kLine}};
    ac3::DownmixTarget stereo_fold = ac3::DownmixTarget::kLoRo;
    ac3::render::ObjectsPolicy objects = ac3::render::ObjectsPolicy::kAuto;

    Playout::Tuning tuning;
};

// What the player reports, for the role's client/state and the board's page.
// Fixed-size so a copy never allocates.
struct BurstPlayerStatus {
    // "idle", "bursts" or "pcm".
    const char* stream = "idle";
    // What the decoder found in the current stream, once a unit has decoded.
    bool have_decoder = false;
    ac3::sendspin::ac3forge::DecoderReport decoder;

    // Per output: the last 100 ms, in dB of full scale (kSilenceDb for
    // silence), and the whole stream's RMS scaled by a million, as the
    // console's stream.rms lines print it.
    std::size_t outputs = 0;
    std::array<float, Playout::kMaxOutputs> peak_db{};
    std::array<float, Playout::kMaxOutputs> rms_db{};
    std::array<std::uint32_t, Playout::kMaxOutputs> stream_rms{};
    // The window the levels were measured over has closed at least once.
    bool have_levels = false;
    std::uint32_t levels_serial = 0;

    ac3::sendspin::ac3forge::Counters counters;
    Playout::Stats playout;

    // The newest stream frame with a known play time: its position from the
    // stream's start, and when it plays, locally.
    std::uint64_t play_frame = 0;
    std::int64_t play_local_us = 0;
    bool have_play = false;

    // Decode, render and the rest per burst, averaged over the stream, and
    // the worst.
    std::uint32_t burst_us = 0;
    std::uint32_t worst_burst_us = 0;
    // The least the decode task's stack has had spare.
    std::size_t stack_free = 0;

    std::int64_t settings_revision = 0;
    // The layout in force, as text.
    std::array<char, ac3::render::OutputLayout::kTextBytes> layout{};
    bool identifying = false;
};

class BurstPlayer {
   public:
    BurstPlayer(const BurstPlayerConfig& config, ScheduledSink& sink);
    ~BurstPlayer();
    BurstPlayer(const BurstPlayer&) = delete;
    BurstPlayer& operator=(const BurstPlayer&) = delete;

    // Allocates the ring and the buffers and starts the task. False, having
    // said why on the console, when it cannot.
    [[nodiscard]] bool start();
    void stop();

    // What a server may send ahead, for the support objects' buffer_capacity.
    [[nodiscard]] std::size_t buffer_capacity() const;

    // From the Sendspin session's task. Each goes into the ring in order;
    // chunks that do not fit are dropped and counted.
    void start_bursts(const ac3::sendspin::ac3forge::StreamStart& stream);
    void start_pcm(const ac3::sendspin::messages::AudioFormat& format);
    void clear();
    void end();
    void burst(const ac3::sendspin::BurstChunk& chunk, std::int64_t local_us);
    void pcm(std::span<const std::uint8_t> frame, std::int64_t server_us, std::int64_t local_us);
    void invalid_chunk();

    // The role's settings: checked against `support` now, and applied at the
    // next burst boundary. The reason they are refused, or nothing.
    [[nodiscard]] std::optional<std::string> settings(const ac3::sendspin::ac3forge::Settings& settings,
                                                      const ac3::sendspin::ac3forge::Support& support);
    // The layout a board's own page sets, for streams with no server settings:
    // applied at the next burst boundary, and replaced by the next settings a
    // server sends. False for a layout the sink cannot carry.
    [[nodiscard]] bool set_layout(const ac3::render::OutputLayout& layout);
    // The identify tone on one output at `level_db`, or none.
    void identify(std::optional<ac3::sendspin::ac3forge::Identify> tone);
    // The player's volume, 0 to 100, and mute: (volume / 100)^1.5, ramped over
    // a block.
    void set_volume(int volume, bool muted);

    // Something else is about to use the sink: the player drops what it
    // holds and stops writing, and this returns once it has (or after two
    // seconds, false). Released, the player goes on with the chunks that
    // arrive after it, opening the sink again its own way. A stream keeps
    // running across a hold, as far as its server knows.
    bool hold(bool held);

    // Safe from any task.
    [[nodiscard]] BurstPlayerStatus status() const;
    // Whether a stream is running.
    [[nodiscard]] bool active() const;

    struct Impl;

   private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3forge

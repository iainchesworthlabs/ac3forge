#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/decoder/serving.hpp"
#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/render.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/sendspin/chunks.hpp"
#include "iclforge/ac4dec/decoder.hpp"

// A test sink's output for _iclforge_player@v1 (planning/hearth-sendspin-extension.md): each
// stream's bursts decoded, AC-3 or E-AC-3 with any object layer, or AC-4, and rendered to the
// sink's speaker layout by iclforge::render, to a float WAV file with one channel per slot and a
// play-time log beside it. The log's lines are `local_time_us,first_frame,frames` for each burst,
// first_frame being the burst's first decoded sample counted from the stream's start, and
// `clear,<frames>` where stream/clear dropped what was buffered, as WavOutput's are for player@v1.
// With no directory it decodes and counts.
//
// AC-4 (planning/ac4.md, D11): a burst is one AC-4 sync frame (IEC 61937-14 Annex A), zero-padded
// to a whole 8-byte unit in an HBR16 burst, which iclforge::ac4::Decoder decodes to its own
// channels; those are rendered by the bed ac4_bed() makes of the decoder's speakers, in blocks of
// 256 samples, a two-speaker layout included, since the AC-4 decoder has no fold of its own yet. A
// burst's duration on the log is its frame's at the base sampling frequency, which IEC 61937-14
// Tables 5 and 6 give as the AC-4 data-burst's repetition period. The decoder on main decodes 48
// kHz at frame_rate_index 13 only, and refuses the other rates, which count as undecodable.
//
// How it decodes, which a local reference decode repeats to compare: DecoderConfig's defaults in
// line mode, served by render::serve(layout, Lo/Ro, ObjectsPolicy::kAuto) and
// render::configure_decoder, so objects are placed when the layout has heights and a two-speaker
// layout is folded by the decoder. An E-AC-3 stream plays the programme of its first access unit.
// The E-AC-3 decoder releases a unit it holds back (§3.7) during the next unit's call; blocks are
// placed and written in the order they arrive, and a unit still held when the stream ends is not
// written. A burst that does not decode resets the decoders, and the stream carries on from the
// next.
//
// Not thread-safe: the sink calls it from the session's callbacks, under the session's lock.

namespace iclforge::hearth::testsink {

// The coded layout an AC-4 decoder's channels make, for the renderer: each speaker at the E-AC-3
// channel map's location of the same place. Ls and Rs stay the surround pair, which a 7.X mode
// keeps at the sides; Lb and Rb, Lw and Rw, and Tfl and Tfr are the rear, wide and front height
// pairs (ETSI TS 103 190-1 clause D.1; A/52 Table E2.5).
[[nodiscard]] ac3::eac3::chanmap::Layout ac4_bed(std::span<const iclforge::ac4::Speaker> speakers);

class BurstOutput {
   public:
    // `prefix` starts every file's name.
    BurstOutput(std::filesystem::path directory, std::string prefix, const render::OutputLayout& layout);
    ~BurstOutput();
    BurstOutput(const BurstOutput&) = delete;
    BurstOutput& operator=(const BurstOutput&) = delete;
    BurstOutput(BurstOutput&&) = delete;
    BurstOutput& operator=(BurstOutput&&) = delete;

    // A stream began, or changed in place: new decoders, and a new file.
    bool start(const sendspin::player::StreamStart& stream);
    void clear();
    void end();
    // One burst to be played from `local_time`.
    void write(const sendspin::BurstChunk& chunk, std::int64_t local_time);

    [[nodiscard]] std::uint64_t bursts() const { return bursts_; }
    [[nodiscard]] std::uint64_t frames() const { return frames_; }
    [[nodiscard]] std::uint64_t undecodable() const { return undecodable_; }
    [[nodiscard]] std::uint32_t streams() const { return streams_; }
    // The file the current or last stream went to; empty without a directory.
    [[nodiscard]] const std::filesystem::path& file() const { return file_; }
    // What the decoder found in the current stream, once a unit of it has decoded; updated once
    // more by the first unit to carry objects, if the first did not.
    [[nodiscard]] const std::optional<sendspin::player::DecoderReport>& decoder() const { return decoder_; }

   private:
    void reset_decoding();
    void decode_unit(std::span<const std::byte> unit);
    void place(const ac3::PcmBlock& block);
    void write_ac4(const sendspin::BurstChunk& chunk, std::int64_t local_time);
    void render_ac4(const iclforge::ac4::DecodedFrame& frame);
    // Writes the first `n` samples of every slot of block_.
    void emit(std::size_t n);

    std::filesystem::path directory_;
    std::string prefix_;
    render::OutputLayout layout_;
    ac3::render::Serving serving_;
    ac3::DecoderConfig config_;
    render::LayoutRenderer renderer_;
    std::optional<sendspin::player::StreamStart> stream_;
    std::optional<ac3::FrameDecoder> ac3_decoder_;
    std::optional<ac3::Eac3Decoder> eac3_decoder_;
    std::optional<iclforge::ac4::Decoder> ac4_decoder_;
    // One span per decoded AC-4 channel, a block's worth of it at a time.
    std::vector<std::span<const float>> ac4_block_;
    std::optional<int> programme_;
    // The bed each unit given to the decoder is placed by, oldest first: a unit's first block takes
    // the oldest, whichever call delivers it.
    std::deque<ac3::eac3::chanmap::Layout> beds_;
    std::optional<ac3::eac3::chanmap::Layout> renderer_bed_;
    std::optional<sendspin::player::DecoderReport> decoder_;

    std::array<std::array<float, ac3::kSamplesPerBlock>, render::OutputLayout::kMaxSlots> block_{};
    std::vector<float> interleaved_;
    ac3::io::WavStreamWriter writer_;
    std::ofstream log_;
    std::filesystem::path file_;
    std::uint64_t bursts_ = 0;
    std::uint64_t frames_ = 0;
    std::uint64_t undecodable_ = 0;
    std::uint64_t stream_frames_ = 0;
    std::uint32_t streams_ = 0;
};

}  // namespace iclforge::hearth::testsink

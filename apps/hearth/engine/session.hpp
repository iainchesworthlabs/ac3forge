#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ac3/io/elementary.hpp"
#include "container_input.hpp"
#include "queue.hpp"
#include "stream_decoder.hpp"

// One queue item, opened (planning/hearth-reference-player.md, A3: "a session
// per item: container input, access units, decoder ...").
//
// The session owns the item's bytes and what a scan found in them - its
// access units, how many samples each codes, its rate and width - and walks
// them through a StreamDecoder on request. It owns no decoder and no output:
// the player keeps one StreamDecoder for the output layout and hands it to
// whichever session is current, which is what lets one item end and the next
// begin without anything being reopened.
//
// Splitting and timing are io::scan()'s, not a second reading of the same
// headers: a scan already reports an AC-3 stream, an E-AC-3 stream and an AC-3
// core carrying E-AC-3 dependents as the right units, and records each unit's
// own length - 1536 samples for AC-3, and 256, 512, 768 or 1536 for an E-AC-3
// unit, which a player that assumes 1536 gets wrong in both its duration and
// its seeks.
//
// An item plays only the part of its stream its loader says to: an MP4 edit
// list's priming at the start and padding at the end are decoded - the
// decoder needs them - but never delivered, so two such items join with
// nothing of either encoder's making between them (the plan's Gapless
// playback section). Positions and durations count from the start of that
// part.
//
// A decoder that starts part-way through a stream - after a seek, or when the
// settings change and a new one takes over - has none of the overlap the unit
// before would have left it, and its first block would differ from an
// unbroken decode's. So it starts one unit early, and that unit is decoded
// and dropped: from the first frame delivered on, the audio is what an
// unbroken decode gives.
//
// No file I/O: the path is turned into bytes by an ItemLoader, which the
// application supplies (reading the file and demuxing Matroska, MP4 or
// MPEG-TS through apps/common/container_input.hpp, whose functions the engine
// does not compile) and a test supplies over memory.

namespace ac3::hearth {

// What an ItemLoader hands over: the item's elementary stream, and the part
// of it to play.
struct LoadedItem {
    std::vector<std::byte> bytes{};
    // Samples at the stream's rate to decode but not play, from the start.
    std::uint64_t skip_samples = 0;
    // Samples to play after those, or to the end when unset.
    std::optional<std::uint64_t> play_samples = std::nullopt;
    // Anything to show beside the item: an edit list that could not be
    // applied, say.
    std::string note{};
    // What the file's container said about the stream, for the media
    // information; empty for a bare elementary stream.
    apps::ContainerFacts container{};
};

using ItemLoader =
    std::function<std::expected<LoadedItem, std::string>(const std::string& path)>;

class Session {
public:
    // A unit's report, with how many of the frames the item plays came from
    // it; units the item plays nothing of are not reported.
    using ReportFn = std::function<void(const UnitReport& report, std::size_t frames)>;
    // An access unit the item plays, as the stream carries it, and the
    // samples it codes: what a bitstream output sends.
    using SentFn = std::function<void(std::span<const std::byte> unit, std::uint32_t samples)>;

    // Loads `path` and scans it. The error is a sentence for the queue list.
    // `programme` picks one programme of a multi-programme E-AC-3 stream by
    // its independent substream id; unset, or naming one the stream does not
    // carry (which the item's note then says), the first plays.
    [[nodiscard]] static std::expected<Session, std::string> open(
        const std::string& path, const ItemLoader& loader,
        std::optional<int> programme = std::nullopt);

    // Movable: the access units are views of the item's own buffer, and a
    // moved vector keeps its buffer, so the views move with it.
    Session(Session&&) noexcept = default;
    Session& operator=(Session&&) noexcept = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() = default;

    [[nodiscard]] const ItemFacts& facts() const { return facts_; }
    [[nodiscard]] std::size_t unit_count() const { return units_.size(); }
    // The programme playing: its independent substream id (0 for AC-3).
    [[nodiscard]] int programme() const { return programme_; }
    // Whether that is the stream's first programme, which is the one a
    // receiver decodes when the stream is sent to it whole.
    [[nodiscard]] bool first_programme() const { return first_programme_; }
    // The samples the unit covering `position` codes, counted from the start
    // of what the item plays.
    [[nodiscard]] std::uint32_t unit_samples_at(std::uint64_t position) const;
    // Every sample the item plays: the part of the stream its loader named,
    // counted from the units' own lengths.
    [[nodiscard]] std::uint64_t total_samples() const { return window_end_ - window_start_; }
    // Everything the item plays has been delivered, and the decoder has let
    // go of the stream.
    [[nodiscard]] bool finished() const { return finished_; }

    // Decodes units until at least `wanted` frames have been delivered, and
    // releases the end of the stream once the last frame the item plays has
    // gone - so an item that has finished has delivered every frame it will
    // ever deliver. Each unit's report follows its frames. `sent`, when
    // given, is handed each unit the item plays as it goes into the decoder:
    // the priming units before the item's part, and the unit a decoder
    // starting part-way through is primed with, are decoded and not sent.
    // Returns the frames this call delivered; an undecodable unit ends the
    // call with the reason, having been sent, and the session carries on from
    // the next unit if asked again.
    [[nodiscard]] std::expected<std::size_t, std::string> render(StreamDecoder& decoder,
                                                                 const StreamDecoder::BlockFn& deliver,
                                                                 std::size_t wanted,
                                                                 const ReportFn& reported = {},
                                                                 const SentFn& sent = {});

    // For a bitstream output, which can only send whole units: the part the
    // item plays grows to the whole units it touches, so what is decoded and
    // delivered is what is sent. A receiver decodes a whole frame, so an edit
    // list's priming or padding inside the first or last unit is heard.
    // Called before anything is rendered.
    void play_whole_units();
    [[nodiscard]] bool whole_units() const { return whole_units_; }

    // The next frame delivered is the first of the unit covering `to`,
    // counted from the start of what the item plays and clamped to it. The
    // decoder is reset - what it holds belongs to the old place - and primed
    // with the unit before.
    void seek(std::chrono::milliseconds to, StreamDecoder& decoder);

    // For a new decoder taking over from `current`: everything `current` has
    // held back comes out now through `deliver`, and the session carries on
    // from the unit it would have decoded next, primed with the unit before -
    // so nothing is lost, nothing repeats, and the handover cannot be heard
    // beyond what the new decoder's own settings change. The caller replaces
    // `current` with the new decoder before the next render().
    void hand_over(StreamDecoder& current, const StreamDecoder::BlockFn& deliver,
                   const ReportFn& reported = {});

    // Where the next frame render() delivers sits, in samples from the start
    // of what the item plays.
    [[nodiscard]] std::uint64_t position_samples() const;

private:
    Session() = default;

    // Where the frames and reports a render() call is delivering go, for the
    // call's window callbacks, and the frame count the current decoder call
    // started at.
    struct Target {
        const StreamDecoder::BlockFn* deliver = nullptr;
        std::size_t* frames = nullptr;
        const ReportFn* reported = nullptr;
        std::size_t unit_start = 0;
    };

    // Hands on the part of a decoded block the item plays, if any.
    void deliver_window(const Target& target, std::span<const std::span<const float>> slots,
                        std::size_t n);
    // Hands on a unit's report if the item played any of its frames.
    static void report_window(const Target& target, const UnitReport& report);
    // The decoder's report callback for `target`, or none when it has no
    // caller to go to.
    static StreamDecoder::UnitFn unit_reports(Target& target);
    // The next frame delivered is the first of `unit`, with the unit before it
    // decoded first and dropped.
    void start_at(std::size_t unit, StreamDecoder& decoder);

    std::vector<std::byte> bytes_;
    io::ScannedStream scanned_{};
    // The programme's access units, and the stream sample each starts at -
    // one more entry than there are units, the last being the stream's end.
    std::vector<std::span<const std::byte>> units_;
    std::vector<std::uint64_t> starts_;
    int programme_ = 0;
    bool first_programme_ = true;
    ItemFacts facts_{};
    // The part of the stream the item plays, in stream samples.
    std::uint64_t window_start_ = 0;
    std::uint64_t window_end_ = 0;
    // The next unit to decode, and the stream sample the next frame the
    // decoder hands over sits at.
    std::size_t next_ = 0;
    std::uint64_t next_frame_ = 0;
    // Frames before this are a priming unit's, decoded and not delivered.
    std::uint64_t skip_until_ = 0;
    bool finished_ = false;
    bool whole_units_ = false;
};

}  // namespace ac3::hearth

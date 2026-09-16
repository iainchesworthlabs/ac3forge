#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <vector>

#include "ac3/io/elementary.hpp"
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
// No file I/O: the path is turned into bytes by an ItemLoader, which the
// application supplies (reading the file and demuxing Matroska, MP4 or
// MPEG-TS through apps/common/container_input.hpp, which the engine does not
// link) and a test supplies over memory.

namespace ac3::hearth {

using ItemLoader =
    std::function<std::expected<std::vector<std::byte>, std::string>(const std::string& path)>;

class Session {
public:
    // Loads `path` and scans it. The error is a sentence for the queue list.
    [[nodiscard]] static std::expected<Session, std::string> open(const std::string& path,
                                                                  const ItemLoader& loader);

    // Movable: the access units are views of the item's own buffer, and a
    // moved vector keeps its buffer, so the views move with it.
    Session(Session&&) noexcept = default;
    Session& operator=(Session&&) noexcept = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() = default;

    [[nodiscard]] const ItemFacts& facts() const { return facts_; }
    [[nodiscard]] std::size_t unit_count() const { return scanned_.access_units.size(); }
    // Every sample the stream codes, from the units' own lengths.
    [[nodiscard]] std::uint64_t total_samples() const { return total_samples_; }
    // Every unit decoded and the end of the stream released.
    [[nodiscard]] bool finished() const { return finished_; }

    // Decodes units until at least `wanted` frames have been delivered, and
    // releases the end of the stream once the last unit has gone - so an item
    // that has finished has delivered every frame it will ever deliver.
    // Returns the frames this call delivered; an undecodable unit ends the
    // call with the reason, and the session carries on from the next unit if
    // asked again.
    [[nodiscard]] std::expected<std::size_t, std::string> render(StreamDecoder& decoder,
                                                                 const StreamDecoder::BlockFn& deliver,
                                                                 std::size_t wanted);

    // The next render() starts at the unit covering `to`, clamped to the
    // stream. The decoder is reset: what it holds belongs to the old place.
    void seek(std::chrono::milliseconds to, StreamDecoder& decoder);

    // Where the next render() starts, in samples from the stream's start.
    [[nodiscard]] std::uint64_t position_samples() const;

private:
    Session() = default;

    std::vector<std::byte> bytes_;
    io::ScannedStream scanned_{};
    ItemFacts facts_{};
    std::uint64_t total_samples_ = 0;
    std::size_t next_ = 0;
    bool finished_ = false;
};

}  // namespace ac3::hearth

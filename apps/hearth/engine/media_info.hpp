#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/probe.hpp"
#include "ac3/meta/bsi.hpp"
#include "ac3/meta/mixing.hpp"
#include "container_input.hpp"
#include "probe_json.hpp"
#include "session.hpp"

// The media information (planning/hearth-reference-player.md, Media
// information): what a queue item's file says about itself, read once for the
// whole file.
//
// - The container's facts, from the item's loader: the track, its codec
//   configuration box, an edit list and what it trims.
// - For AC-3 and E-AC-3: the programmes and associated services io::scan
//   finds, and the channel map; the whole-stream report io::probe makes, as
//   ac3cli probe makes it (rates, extent, metadata ranges, EMDF payloads,
//   objects, authenticity tags, CRCs, coding tools); and what the lead
//   programme's first access unit says in its bitstream information, with the
//   fold levels that follows from it.
// - For AC-4, which this build cannot play: the sync frames, and the first
//   frame's table of contents - presentations, substream groups, channel
//   modes, bitrates and A-JOC.
//
// Reading a whole stream takes a noticeable part of a second for a long item,
// so describe_media() runs on MediaInspector's thread (media_inspector.hpp),
// never the engine's. What changes frame by frame while an item plays - its
// metadata, and the positions of its objects - is the player's to report, at
// play time.
//
// media_info_json() writes the description as the document "Export JSON"
// saves:
//
//   {
//     "schema": "ac3forge.hearth.media/1",
//     "generator": the ac3forge version,
//     "file": the item's path,
//     "codec": "ac3", "eac3", "ac3+eac3" (an AC-3 core with E-AC-3
//              dependents), "ac4", or null when nothing could be read,
//     "error": null, or why the item could not be described,
//     "container": null for an elementary stream, else {format, codec_id,
//                  track, language, samples, sample_rate_hz, channels,
//                  mp4: null or {timescale, movie_timescale, edits,
//                  codec_box: null or {type, bytes, fscod, bsid, bsmod,
//                  acmod, lfeon, bit_rate_code, data_rate_kbps,
//                  independent_substreams, num_dep_sub, chan_loc, asvc,
//                  complexity_index}},
//                  mpegts: null or {program_number, pmt_pid, stream_type,
//                  signalling, packet_size}},
//     "playback": {sample_rate_hz, stream_samples, skip_samples,
//                  play_samples (null: to the end), played_samples,
//                  duration_seconds, note},
//     "programmes": [{substream_id, acmod, lfeon, layout_label, channels,
//                     bsid, bsmod, bsmod_label, substreams_per_access_unit,
//                     complexity_index, access_units}],
//     "associated_services": [{substream_id, bsmod, bsmod_present,
//                              bsmod_label, acmod, lfeon, mix_metadata}],
//     "channel_map": the Table E2.5 word, or null,
//     "bitstream": null, or {info, alternate_bsi, cmixlev, surmixlev,
//                  mixing, fold_levels} - see write_bitstream() for each,
//     "probe": null, or {schema: "ac3forge.probe/1", stream}: the stream
//              object ac3cli probe json=1 writes (docs/forge/cli/commands.md),
//              the AC-4 one for an AC-4 item
//   }
//
// Members are only ever added within a version, and one that does not apply
// is present and null, as ac3forge.probe/1 promises for its own.

namespace ac3::hearth {

enum class MediaCodec : std::uint8_t {
    kAc3,
    kEac3,
    // An AC-3 core carrying E-AC-3 dependent substreams
    // (io::StreamKind::kAc3CoreEac3Extension).
    kAc3WithEac3,
    kAc4,
};

// "ac3", "eac3", "ac3+eac3" or "ac4".
[[nodiscard]] std::string_view codec_token(MediaCodec codec);

// One programme (§E2.3.1.2): an independent substream and its dependents. An
// AC-3 stream has one.
struct MediaProgramme {
    int substreamid = 0;
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    int channels = 0;
    int bsid = 0;
    int bsmod = 0;
    std::size_t substreams_per_unit = 0;
    std::optional<int> complexity_index = std::nullopt;
    std::size_t access_units = 0;
};

// What the lead programme's first access unit says in its bitstream
// information, read with the transform left out. What a stream does not
// carry stays unset.
struct MediaBitstream {
    // The unit's acmod, which names bsmod 7.
    Acmod acmod = Acmod::k2_0;
    // AC-3's bsi, or E-AC-3's informational metadata when it sends it.
    std::optional<meta::BsiInfo> info = std::nullopt;
    // Annex D's alternate syntax: AC-3 bsid 6, or a legacy core's.
    std::optional<meta::AlternateBsi> alternate_bsi = std::nullopt;
    // §5.4.2.4-5, which only AC-3 codes.
    std::optional<meta::CentreMixLevel> cmixlev = std::nullopt;
    std::optional<meta::SurroundMixLevel> surmixlev = std::nullopt;
    // Annex E's mixing metadata, from the independent substream.
    std::optional<meta::MixMetadata> mixing = std::nullopt;
    // The fold levels the output stage takes from all of the above, with the
    // defaults for what the stream leaves out.
    MixLevels levels{};
};

struct MediaInfo {
    std::string path{};
    // Unset when nothing in the item could be read.
    std::optional<MediaCodec> codec = std::nullopt;
    // Why the item could not be described; the rest is then what was read
    // before that.
    std::string error{};
    apps::ContainerFacts container{};
    // The stream's rate and length in samples, and the part of it the item
    // plays.
    std::uint32_t sample_rate = 0;
    std::uint64_t stream_samples = 0;
    std::uint64_t skip_samples = 0;
    std::optional<std::uint64_t> play_samples = std::nullopt;
    std::string note{};
    // AC-3 and E-AC-3.
    std::vector<MediaProgramme> programmes{};
    // Associated services, substream ids 1-3; `present` says which exist.
    std::array<io::SubstreamService, 3> associated_services{};
    std::optional<std::uint16_t> channel_map = std::nullopt;
    std::optional<io::ProbeReport> probe = std::nullopt;
    std::optional<MediaBitstream> bitstream = std::nullopt;
    // AC-4.
    std::optional<apps::probe_json::Ac4Summary> ac4 = std::nullopt;

    // The samples the item plays: what is left after the skip, cut to
    // play_samples.
    [[nodiscard]] std::uint64_t played_samples() const;
};

// Describes the item `loaded` holds, read from `path`. Walks the whole stream.
[[nodiscard]] MediaInfo describe_media(const std::string& path, const LoadedItem& loaded);

// The description as a JSON document; see the top of this file.
[[nodiscard]] std::string media_info_json(const MediaInfo& info);

}  // namespace ac3::hearth

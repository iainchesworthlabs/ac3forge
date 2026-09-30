#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/ac3/io/probe.hpp"
#include "iclforge/objects/oamd.hpp"
#include "iclforge/ac4/ac4.hpp"
#include "iclforge/ac4dec/decoder.hpp"
#include "container_input.hpp"
#include "json_sink.hpp"

// The `stream` object of the iclforge.probe/1 document (docs/forge/cli/
// commands.md), for ac3cli probe and for Hearth's media information, which
// carries the same object so one stream is never described two ways. Also the
// fixed names both of ac3cli probe's forms use, and the AC-4 walk.
//
// Compiled into each application that uses it, like the rest of apps/common:
// it needs iclforge::ac3, iclforge::ac4 and iclforge::ac4dec, which both applications
// link.

namespace iclforge::apps::probe_json {

// The document's vocabulary: fixed text for transmitted values.
[[nodiscard]] std::string_view codec_token(io::StreamKind kind);
[[nodiscard]] std::string_view codec_label(io::StreamKind kind);
[[nodiscard]] std::string_view strmtyp_token(eac3::StreamType type);
// A/52 Table 5.7, where bsmod 7 is voice over at acmod 1/0 and karaoke above
// it.
[[nodiscard]] std::string_view bsmod_label(int bsmod, Acmod acmod);
// ETSI TS 102 366 Annex F §F.6's dec3 asvc bit, in the same two words a
// receiver's own choice between them comes down to.
[[nodiscard]] std::string_view asvc_label(bool asvc);
[[nodiscard]] std::string_view exp_strategy_token(ExpStrategy strategy);
// TS 103 420 Table 55's names for two EMDF payload ids, and empty for the rest.
[[nodiscard]] std::string_view emdf_payload_label(int id);
// The bed a §5.5 program describes: a channel count, "LFE only" or "none".
[[nodiscard]] std::string bed_label(const oba::Program& program);
// dialnorm's 1..31 code as the -1..-31 dB it means (§5.4.2.8).
[[nodiscard]] int dialnorm_db(int code);

// A dialnorm, compr or dynrng range as {present, min, max}, with dialnorm's
// codes turned into dB when `negate` is set.
void write_range(JsonSink& json, std::string_view name, const io::MinMax& range, bool negate);

// The "stream" member for an AC-3 or E-AC-3 stream.
void write_stream(JsonSink& json, const io::ProbeReport& report);

// An AC-4 stream, walked sync frame by sync frame: the counts, and the first
// frame's table of contents, which stands for the stream's structure; then
// planning/ac4.md's "Media information" over the whole stream.
struct Ac4Summary {
    std::size_t sync_frames = 0;
    std::size_t bytes = 0;
    std::size_t crc_failures = 0;
    std::optional<iclforge::ac4::Error> parse_error = std::nullopt;  // the first one seen
    std::optional<iclforge::ac4::RawFrame> first_frame = std::nullopt;
    // The first frame's frame rate and rates (iclforge::ac4::frame_rate()), and the bit
    // rate over whole raw_ac4_frame()s at that rate.
    std::optional<iclforge::ac4::FrameRate> frame_rate = std::nullopt;
    std::optional<double> bitrate_kbps = std::nullopt;
    // Frames with b_iframe_global, and the fewest and most frames from one to
    // the next.
    std::size_t iframes = 0;
    std::optional<std::size_t> min_iframe_interval = std::nullopt;
    std::optional<std::size_t> max_iframe_interval = std::nullopt;
    // Changes of source: frames whose sequence_counter does not continue the
    // stream (ETSI TS 103 190-1 clause 4.3.3.2.2), a splice among them.
    std::size_t splices = 0;
    // What iclforge::ac4::Decoder reads of every frame: the presentations of the last
    // frame whose table of contents reads, with their names, and the
    // metadata of the presentation it selects without preferences.
    std::vector<iclforge::ac4::PresentationInfo> presentations{};
    std::optional<iclforge::ac4::PresentationMetadata> metadata = std::nullopt;
    // The oamd_common_data() of each OAMD substream (Part 2 clause 6.2.2.4) that
    // sent one, by the substream's index, from the first frame that did: what
    // iclforge::ac4::SubstreamReport reports of it.
    std::map<int, iclforge::ac4::OamdCommonData> oamd_common_data{};
};

[[nodiscard]] Ac4Summary summarize_ac4(std::span<const std::byte> data);
[[nodiscard]] std::string_view ac4_error_token(iclforge::ac4::Error error);
[[nodiscard]] std::string_view object_kind_token(iclforge::ac4::ObjectKind kind);
// One line for a §6.2.1.6 substream, for a human-readable listing.
[[nodiscard]] std::string describe_group_substream(const iclforge::ac4::GroupSubstream& sub);

// The "stream" member for an AC-4 stream: codec "ac4", the counts, integrity,
// and an "ac4" object in place of the AC-3/E-AC-3 fields.
void write_ac4_stream(JsonSink& json, const Ac4Summary& summary);

// The "container" member: what a Matroska, MP4 or MPEG-TS file said of the
// track whose stream the document describes, in the members Hearth's media
// information writes; null for a bare elementary stream.
void write_container(JsonSink& json, const ContainerFacts& facts);

}  // namespace iclforge::apps::probe_json

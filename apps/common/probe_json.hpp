#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/probe.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"
#include "json_sink.hpp"

// The `stream` object of the ac3forge.probe/1 document (docs/forge/cli/
// commands.md), for ac3cli probe and for Hearth's media information, which
// carries the same object so one stream is never described two ways. Also the
// fixed names both of ac3cli probe's forms use, and the AC-4 walk.
//
// Compiled into each application that uses it, like the rest of apps/common:
// it needs ac3::forge, ac4::ac4 and ac4::decoder, which both applications
// link.

namespace ac3::apps::probe_json {

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
    std::optional<ac4::Error> parse_error = std::nullopt;  // the first one seen
    std::optional<ac4::RawFrame> first_frame = std::nullopt;
    // The first frame's frame rate and rates (ac4::frame_rate()), and the bit
    // rate over whole raw_ac4_frame()s at that rate.
    std::optional<ac4::FrameRate> frame_rate = std::nullopt;
    std::optional<double> bitrate_kbps = std::nullopt;
    // Frames with b_iframe_global, and the fewest and most frames from one to
    // the next.
    std::size_t iframes = 0;
    std::optional<std::size_t> min_iframe_interval = std::nullopt;
    std::optional<std::size_t> max_iframe_interval = std::nullopt;
    // Changes of source: frames whose sequence_counter does not continue the
    // stream (ETSI TS 103 190-1 clause 4.3.3.2.2), a splice among them.
    std::size_t splices = 0;
    // What ac4::Decoder reads of every frame: the presentations of the last
    // frame whose table of contents reads, with their names, and the
    // metadata of the presentation it selects without preferences.
    std::vector<ac4::PresentationInfo> presentations{};
    std::optional<ac4::PresentationMetadata> metadata = std::nullopt;
};

[[nodiscard]] Ac4Summary summarize_ac4(std::span<const std::byte> data);
[[nodiscard]] std::string_view ac4_error_token(ac4::Error error);
[[nodiscard]] std::string_view object_kind_token(ac4::ObjectKind kind);
// One line for a §6.2.1.6 substream, for a human-readable listing.
[[nodiscard]] std::string describe_group_substream(const ac4::GroupSubstream& sub);

// The "stream" member for an AC-4 stream: codec "ac4", the counts, integrity,
// and an "ac4" object in place of the AC-3/E-AC-3 fields.
void write_ac4_stream(JsonSink& json, const Ac4Summary& summary);

}  // namespace ac3::apps::probe_json

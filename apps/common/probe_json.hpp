#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/probe.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac4/ac4.hpp"
#include "json_sink.hpp"

// The `stream` object of the ac3forge.probe/1 document (docs/forge/cli/
// commands.md), for ac3cli probe and for Hearth's media information, which
// carries the same object so one stream is never described two ways. Also the
// fixed names both of ac3cli probe's forms use, and the AC-4 walk.
//
// Compiled into each application that uses it, like the rest of apps/common:
// it needs ac3::forge and ac4::ac4, which both applications link.

namespace ac3::apps::probe_json {

// The document's vocabulary: fixed text for transmitted values.
[[nodiscard]] std::string_view codec_token(io::StreamKind kind);
[[nodiscard]] std::string_view codec_label(io::StreamKind kind);
[[nodiscard]] std::string_view strmtyp_token(eac3::StreamType type);
// A/52 Table 5.7, where bsmod 7 is voice over at acmod 1/0 and karaoke above
// it.
[[nodiscard]] std::string_view bsmod_label(int bsmod, Acmod acmod);
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
// frame's table of contents, which stands for the stream's structure.
struct Ac4Summary {
    std::size_t sync_frames = 0;
    std::size_t bytes = 0;
    std::size_t crc_failures = 0;
    std::optional<ac4::Error> parse_error = std::nullopt;  // the first one seen
    std::optional<ac4::RawFrame> first_frame = std::nullopt;
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

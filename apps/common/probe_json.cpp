#include "probe_json.hpp"

#include <algorithm>
#include <cstdint>
#include <fmt/format.h>
#include <utility>

#include "ac3/analysis/levels.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/emdf/emdf.hpp"
#include "ac3/meta/bsi.hpp"
#include "ac3/meta/mixing.hpp"

// See probe_json.hpp. Moved here from apps/cli/commands/probe.cpp, which
// writes the same document it always did through these.

namespace ac3::apps::probe_json {

// --- naming ----------------------------------------------------------------
// Every label here is fixed text keyed off a transmitted value. They are the
// JSON document's vocabulary as much as the table's, so both forms use these
// same functions - a consumer and a reader can never be told two different
// names for one stream.

std::string_view codec_token(io::StreamKind kind) {
    return kind == io::StreamKind::kAc3 ? "ac3" : "eac3";
}

std::string_view codec_label(io::StreamKind kind) {
    return kind == io::StreamKind::kAc3 ? "AC-3" : "E-AC-3";
}

std::string_view strmtyp_token(eac3::StreamType type) {
    switch (type) {
        case eac3::StreamType::kIndependent: return "independent";
        case eac3::StreamType::kDependent: return "dependent";
        case eac3::StreamType::kConvertible: return "convertible";
        case eac3::StreamType::kReserved: break;
    }
    return "reserved";
}

// A/52 Table 5.7. bsmod's meaning additionally depends on acmod for one
// value - 0x7 is an associated "voice over" service at acmod 1/0 and a main
// "karaoke" service at anything wider - so the pair is what names it, not
// bsmod alone.
std::string_view bsmod_label(int bsmod, Acmod acmod) {
    switch (bsmod) {
        case 0: return "complete main";
        case 1: return "music and effects";
        case 2: return "visually impaired";
        case 3: return "hearing impaired";
        case 4: return "dialogue";
        case 5: return "commentary";
        case 6: return "emergency";
        case 7: return acmod == Acmod::k1_0 ? "voice over" : "karaoke";
        default: break;
    }
    return "reserved";
}

// ETSI TS 102 366 Annex F §F.6's dec3 asvc bit - "is this programme's own
// bsmod an associated one", per the same Table 5.7 split bsmod_label above
// names in full.
std::string_view asvc_label(bool asvc) { return asvc ? "associated service" : "main service"; }

std::string_view exp_strategy_token(ExpStrategy strategy) {
    switch (strategy) {
        case ExpStrategy::kReuse: return "reuse";
        case ExpStrategy::kD15: return "D15";
        case ExpStrategy::kD25: return "D25";
        case ExpStrategy::kD45: return "D45";
    }
    return "reuse";
}

// TS 103 420 Table 55 names two of Table H.2.3's reserved payload ids; the
// rest are EMDF's own and are reported as bare numbers.
std::string_view emdf_payload_label(int id) {
    if (id == emdf::kPayloadIdOamd) {
        return "OAMD";
    }
    if (id == emdf::kPayloadIdJoc) {
        return "JOC";
    }
    return "";
}

// The bed a §5.5 program describes, as the channel names its assignment bits
// stand for - "5.1", "5.1.4", or "none" for a program that is dynamic objects
// alone. Counted off the Table 12 mask rather than looked up in a table of
// layout names, because a bed instance is a bit mask, not one of a fixed set:
// the full-range, LFE and height groups are each a known set of bits, so any
// mask - including one no layout has a name for - still describes itself.
//
// This used to answer "{n} channel(s)", which is not what either caller wants
// to read: both the Decoder and the Media page paste it into a sentence, and
// "reconstructed from the 6 channel(s)" is not a sentence. The shapes that
// have names now get them.
std::string bed_label(const oba::Program& program) {
    if (program.dynamic_only) {
        return program.lfe ? "LFE only" : "none";
    }
    const auto group = [mask = program.bed](std::uint16_t bit, int channels) {
        return (mask & bit) != 0 ? channels : 0;
    };
    const int full = group(oba::bed::kLR, 2) + group(oba::bed::kC, 1)
                     + group(oba::bed::kLsRs, 2) + group(oba::bed::kLbRb, 2)
                     + group(oba::bed::kLwRw, 2);
    const int lfe = group(oba::bed::kLfe, 1) + group(oba::bed::kLfe2, 1);
    const int height = group(oba::bed::kTflTfr, 2) + group(oba::bed::kTslTsr, 2)
                       + group(oba::bed::kTblTbr, 2);
    if (full == 0 && lfe == 0 && height == 0) {
        // A non-standard assignment, which Table 12's bits cannot describe -
        // the count is all there is to say, said grammatically.
        const int count = oba::bed::channel_count(program.bed);
        return count == 1 ? std::string{"1-channel bed"} : fmt::format("{}-channel bed", count);
    }
    // "5.1 bed", not "5.1": both callers paste this straight into a
    // sentence ("reconstructed by JOC from the ..."), and the design's own
    // wording is the noun phrase, not the bare layout.
    return height > 0 ? fmt::format("{}.{}.{} bed", full, lfe, height)
                      : fmt::format("{}.{} bed", full, lfe);
}

// dialnorm is transmitted as 1..31 meaning -1..-31 dB LKFS (§5.4.2.8); 0 is
// reserved. Reporting the dB is what every other tool shows and what a
// delivery spec is written in, so that is what both output forms carry - see
// docs/forge/cli/commands.md, which documents the JSON field as dB for exactly this
// reason.
int dialnorm_db(int code) { return -code; }

// --- JSON ------------------------------------------------------------------

void write_range(JsonSink& json, std::string_view name, const io::MinMax& range, bool negate) {
    json.key(name);
    json.begin_object();
    json.member("present", range.seen);
    if (range.seen) {
        // Negating swaps the ends: the largest dialnorm CODE is the quietest
        // programme, hence the smallest dB.
        json.member("min", static_cast<std::int64_t>(negate ? -range.max : range.min));
        json.member("max", static_cast<std::int64_t>(negate ? -range.min : range.max));
    } else {
        json.member_null("min");
        json.member_null("max");
    }
    json.end_object();
}

void write_stream(JsonSink& json, const io::ProbeReport& report) {
    json.key("stream");
    json.begin_object();
    json.member("codec", codec_token(report.kind));
    json.member("bsid", static_cast<std::int64_t>(report.bsid));
    json.member("bsmod", static_cast<std::int64_t>(report.bsmod));
    json.member("bsmod_label", bsmod_label(report.bsmod, report.acmod));
    json.member("sample_rate_hz", static_cast<std::int64_t>(sample_rate_hz(report.sample_rate)));
    json.member("reduced_rate", report.reduced_rate);
    json.member("acmod", static_cast<std::int64_t>(report.acmod));
    json.member("lfeon", report.lfe);
    json.member("layout_label", analysis::layout_name(report.acmod, report.lfe));
    json.member("numblkscod", static_cast<std::int64_t>(report.numblkscod));
    json.member("blocks_per_syncframe",
                static_cast<std::int64_t>(report.kind == io::StreamKind::kAc3
                                              ? kBlocksPerFrame
                                              : eac3::blocks_per_syncframe(report.numblkscod)));
    json.member("coded_channels", static_cast<std::int64_t>(report.coded_channels));
    json.member("rendered_channels", static_cast<std::int64_t>(report.rendered_channels));
    json.key("layout");
    json.begin_array();
    for (const auto location : report.layout) {
        json.value(eac3::chanmap::name(location));
    }
    json.end_array();

    json.key("substreams");
    json.begin_array();
    for (const auto& sub : report.substreams) {
        json.begin_object();
        json.member("stream_type", strmtyp_token(sub.strmtyp));
        json.member("substream_id", static_cast<std::int64_t>(sub.substreamid));
        json.member("bsid", static_cast<std::int64_t>(sub.bsid));
        json.member("bsmod", static_cast<std::int64_t>(sub.bsmod));
        json.member("acmod", static_cast<std::int64_t>(sub.acmod));
        json.member("lfeon", sub.lfe);
        json.member("numblkscod", static_cast<std::int64_t>(sub.numblkscod));
        if (sub.chanmap.has_value()) {
            json.member("chanmap", static_cast<std::int64_t>(*sub.chanmap));
        } else {
            json.member_null("chanmap");
        }
        json.member("syncframes", static_cast<std::uint64_t>(sub.syncframes));
        json.end_object();
    }
    json.end_array();
    json.member("substreams_per_access_unit", static_cast<std::uint64_t>(report.substreams_per_unit));

    json.member("access_units", static_cast<std::uint64_t>(report.access_units));
    json.member("syncframes", static_cast<std::uint64_t>(report.syncframes));
    json.member("bytes", static_cast<std::uint64_t>(report.bytes));
    json.member("duration_seconds", report.duration_seconds, 6);
    json.member("bitrate_kbps", report.bitrate_kbps, 3);
    if (report.nominal_bitrate_kbps.has_value()) {
        json.member("nominal_bitrate_kbps", static_cast<std::int64_t>(*report.nominal_bitrate_kbps));
    } else {
        json.member_null("nominal_bitrate_kbps");
    }
    json.member("variable_bitrate", report.variable_bitrate);
    json.key("access_unit_bytes");
    json.begin_object();
    json.member("min", static_cast<std::uint64_t>(report.min_access_unit_bytes));
    json.member("max", static_cast<std::uint64_t>(report.max_access_unit_bytes));
    json.end_object();

    json.key("metadata");
    json.begin_object();
    write_range(json, "dialnorm_db", report.dialnorm, true);
    write_range(json, "dialnorm2_db", report.dialnorm2, true);
    write_range(json, "compr", report.compr, false);
    write_range(json, "compr2", report.compr2, false);
    write_range(json, "dynrng", report.dynrng, false);
    write_range(json, "dynrng2", report.dynrng2, false);
    // A code and its Table D2.2 name rather than a range - see
    // ProbeReport::dmixmod - with both null when the field was never sent.
    json.key("dmixmod");
    json.begin_object();
    json.member("present", report.dmixmod.has_value());
    if (report.dmixmod.has_value()) {
        json.member("code", static_cast<std::int64_t>(*report.dmixmod));
        json.member("label", meta::describe(*report.dmixmod));
    } else {
        json.member_null("code");
        json.member_null("label");
    }
    json.end_object();
    json.end_object();

    json.key("objects");
    json.begin_object();
    if (report.oba_complexity_index.has_value()) {
        json.member("complexity_index", static_cast<std::int64_t>(*report.oba_complexity_index));
    } else {
        json.member_null("complexity_index");
    }
    json.member("oamd", report.oamd);
    json.member("joc", report.joc);
    json.key("emdf_payload_ids");
    json.begin_array();
    for (const int id : report.emdf_payload_ids) {
        json.value(static_cast<std::int64_t>(id));
    }
    json.end_array();
    if (report.program.has_value()) {
        json.member("total", static_cast<std::int64_t>(oba::object_count(*report.program)));
        json.member("dynamic", static_cast<std::int64_t>(report.program->dynamic_objects));
        json.member("bed", bed_label(*report.program));
        // §5.5.2's bed instance exists only in the non-dynamic_only branch;
        // reporting its zero for a dynamic-object-only program would read as
        // "a bed with no channels" rather than "no bed field at all".
        if (report.program->dynamic_only) {
            json.member_null("bed_mask");
        } else {
            json.member("bed_mask", static_cast<std::int64_t>(report.program->bed));
        }
        json.member("lfe", oba::has_lfe(*report.program));
    } else {
        json.member_null("total");
        json.member_null("dynamic");
        json.member_null("bed");
        json.member_null("bed_mask");
        json.member_null("lfe");
    }
    json.member("frames", static_cast<std::uint64_t>(report.object_frames));
    json.end_object();

    json.key("authenticity");
    json.begin_object();
    json.member("tagged_syncframes", static_cast<std::uint64_t>(report.authenticity_tagged_frames));
    json.member("present", report.authenticity_tagged_frames > 0);
    json.end_object();

    json.key("integrity");
    json.begin_object();
    json.member("crc_valid", static_cast<std::uint64_t>(report.syncframes - report.crc_failures));
    json.member("crc_failures", static_cast<std::uint64_t>(report.crc_failures));
    json.member("parse_failures", static_cast<std::uint64_t>(report.parse_failures));
    if (report.first_parse_error.has_value()) {
        json.member("first_parse_error", describe(*report.first_parse_error));
    } else {
        json.member_null("first_parse_error");
    }
    json.end_object();

    const auto& tools = report.tools;
    json.key("tools");
    json.begin_object();
    json.member("blocks", static_cast<std::uint64_t>(tools.blocks));
    json.member("coupling", static_cast<std::uint64_t>(tools.coupling));
    json.member("enhanced_coupling", static_cast<std::uint64_t>(tools.enhanced_coupling));
    json.member("spectral_extension", static_cast<std::uint64_t>(tools.spectral_extension));
    json.member("block_switch", static_cast<std::uint64_t>(tools.block_switch));
    json.member("dither", static_cast<std::uint64_t>(tools.dither));
    json.member("rematrixing", static_cast<std::uint64_t>(tools.rematrixing));
    json.member("delta_bit_alloc", static_cast<std::uint64_t>(tools.delta_bit_alloc));
    json.member("skip_field", static_cast<std::uint64_t>(tools.skip_field));
    json.member("aht_syncframes", static_cast<std::uint64_t>(tools.aht_frames));
    json.member("transient_prenoise_syncframes",
                static_cast<std::uint64_t>(tools.transient_prenoise_frames));
    json.key("exponent_strategy");
    json.begin_object();
    json.member("reuse", static_cast<std::uint64_t>(tools.exp_strategy[0]));
    json.member("D15", static_cast<std::uint64_t>(tools.exp_strategy[1]));
    json.member("D25", static_cast<std::uint64_t>(tools.exp_strategy[2]));
    json.member("D45", static_cast<std::uint64_t>(tools.exp_strategy[3]));
    json.end_object();
    json.end_object();

    json.end_object();
}

// --- AC-4 ---------------------------------------------------------------
//
// A separate walk from everything above, over ac4::scan()/parse_raw_frame()
// rather than ac3::io::Prober - AC-4 is a different codec with a different
// bitstream (see src/ac4/include/ac4/ac4.hpp's own scope note: TOC/
// presentation/substream-group framing, not audio decode), so none of the
// AC-3/E-AC-3-specific fields above (acmod, bsmod, chanmap, dialnorm,
// exponent_strategy, ...) apply to it. Rather than writing thirty `null`
// members onto every AC-4 response for fields that belong to a different
// codec family entirely, `stream.codec == "ac4"` responses carry only the
// fields meaningful across any codec plus a dedicated `stream.ac4` object -
// additive to the documented schema, not a violation of its "never
// omitted" rule, which is about a stream's own optional fields within one
// codec family. See docs/forge/cli/commands.md.
//
// The first sync frame's TOC stands for the whole file's structure (real
// AC-4 streams do not change presentation/substream-group layout frame to
// frame), alongside file-wide CRC and parse-failure counts.

std::string_view ac4_error_token(ac4::Error error) {
    switch (error) {
        case ac4::Error::kTruncated:
            return "truncated";
        case ac4::Error::kLostSync:
            return "lost_sync";
        case ac4::Error::kUnsupportedBitstreamVersion:
            return "unsupported_bitstream_version";
    }
    return "unknown";
}

Ac4Summary summarize_ac4(std::span<const std::byte> data) {
    Ac4Summary summary;
    const auto scanned = ac4::scan(data);
    summary.sync_frames = scanned.frames.size();
    // The decoder reads every substream of every frame, for the names that
    // arrive in chunks and the metadata the I-frames send.
    ac4::Decoder decoder;
    std::uint64_t raw_bytes = 0;
    std::optional<int> previous_counter;
    std::optional<std::size_t> last_iframe;
    for (std::size_t f = 0; f < scanned.frames.size(); ++f) {
        const ac4::SyncFrame& frame = scanned.frames[f];
        summary.bytes += frame.raw_ac4_frame.size() + (frame.crc_ok ? 6 : 4);
        raw_bytes += frame.raw_ac4_frame.size();
        if (frame.crc_ok.has_value() && !*frame.crc_ok) {
            ++summary.crc_failures;
        }
        auto parsed = ac4::parse_raw_frame(frame.raw_ac4_frame);
        if (!parsed && !summary.parse_error.has_value()) {
            summary.parse_error = parsed.error();
        }
        if (parsed) {
            const ac4::Toc& toc = parsed->toc;
            // ETSI TS 103 190-1 clause 4.3.3.2.2: the counter continues from
            // the last, wraps from 1020 to 1, or follows a splice mark of 0.
            if (previous_counter) {
                const int previous = *previous_counter;
                const int counter = toc.sequence_counter;
                const bool continues = counter == previous + 1 ||
                                       (counter == 1 && previous == 1020) ||
                                       (counter != 0 && previous == 0);
                summary.splices += continues ? 0U : 1U;
            }
            previous_counter = toc.sequence_counter;
            if (toc.b_iframe_global) {
                ++summary.iframes;
                if (last_iframe) {
                    const std::size_t interval = f - *last_iframe;
                    summary.min_iframe_interval =
                        std::min(summary.min_iframe_interval.value_or(interval), interval);
                    summary.max_iframe_interval =
                        std::max(summary.max_iframe_interval.value_or(interval), interval);
                }
                last_iframe = f;
            }
        }
        if (parsed && !summary.first_frame.has_value()) {
            summary.first_frame = std::move(*parsed);
        }
        (void)decoder.parse(frame.raw_ac4_frame);
    }
    if (scanned.stopped_at.has_value() && !summary.parse_error.has_value()) {
        summary.parse_error = scanned.stopped_at;
    }
    if (summary.first_frame.has_value()) {
        summary.frame_rate = ac4::frame_rate(summary.first_frame->toc);
        if (summary.frame_rate && summary.sync_frames > 0) {
            const double seconds =
                static_cast<double>(summary.sync_frames) / summary.frame_rate->frames_per_second;
            summary.bitrate_kbps = static_cast<double>(raw_bytes) * 8.0 / seconds / 1000.0;
        }
    }
    const std::span<const ac4::PresentationInfo> presentations = decoder.presentations();
    summary.presentations.assign(presentations.begin(), presentations.end());
    if (decoder.metadata().presentation.has_value()) {
        summary.metadata = decoder.metadata();
    }
    return summary;
}

std::string_view object_kind_token(ac4::ObjectKind kind) {
    switch (kind) {
        case ac4::ObjectKind::kBed: return "bed";
        case ac4::ObjectKind::kDyn: return "dyn";
        case ac4::ObjectKind::kIsf: return "isf";
    }
    return "unknown";
}

// One line per §6.2.1.6 substream, whichever of chan/ajoc/obj it is - the
// human-readable counterpart to write_ac4_group_substream()'s JSON.
std::string describe_group_substream(const ac4::GroupSubstream& sub) {
    switch (sub.kind) {
        case ac4::GroupSubstream::Kind::kChan:
            if (!sub.chan.has_value()) {
                break;
            }
            return fmt::format(
                "{}{}", sub.chan->channel_mode_name,
                sub.chan->bitrate_kbps ? fmt::format(", {} kbit/s", *sub.chan->bitrate_kbps) : "");
        case ac4::GroupSubstream::Kind::kAjoc:
            if (!sub.ajoc.has_value()) {
                break;
            }
            return fmt::format(
                "A-JOC, {} dmx + {} upmix signal(s){}", sub.ajoc->n_fullband_dmx_signals,
                sub.ajoc->n_fullband_upmix_signals,
                sub.ajoc->bitrate_kbps ? fmt::format(", {} kbit/s", *sub.ajoc->bitrate_kbps) : "");
        case ac4::GroupSubstream::Kind::kObj:
            if (!sub.obj.has_value()) {
                break;
            }
            return fmt::format(
                "object, {} object(s){}{}", sub.obj->objects.size(),
                sub.obj->b_dynamic_objects ? " (dynamic)" : "",
                sub.obj->bitrate_kbps ? fmt::format(", {} kbit/s", *sub.obj->bitrate_kbps) : "");
    }
    return "(refused)";
}

namespace {

// A channel-coded substream's members, for the caller's own object.
void write_ac4_substream_members(JsonSink& json, const ac4::ChannelSubstreamInfo& sub) {
    json.member("channel_mode", static_cast<std::int64_t>(sub.channel_mode));
    json.member("channel_mode_name", sub.channel_mode_name);
    if (sub.ch_mode.has_value()) {
        json.member("ch_mode", static_cast<std::int64_t>(*sub.ch_mode));
    } else {
        json.member_null("ch_mode");
    }
    if (sub.bitrate_kbps.has_value()) {
        json.member("bitrate_kbps", static_cast<std::int64_t>(*sub.bitrate_kbps));
    } else {
        json.member_null("bitrate_kbps");
    }
    if (sub.substream_index.has_value()) {
        json.member("substream_index", static_cast<std::int64_t>(*sub.substream_index));
    } else {
        json.member_null("substream_index");
    }
    // §6.3.2.7.3-.5: whether channels channel_mode implies exist in the
    // original content or carry encoded silence - e.g. a 5.1.4 source
    // carried in a 7.1.4-coded substream has b_4_back_channels_present ==
    // false, and channel_mode_name alone would say "7.1.4" either way.
    if (sub.original_content.has_value()) {
        json.key("original_content");
        json.begin_object();
        json.member("b_4_back_channels_present", sub.original_content->b_4_back_channels_present);
        json.member("b_centre_present", sub.original_content->b_centre_present);
        json.member("top_channels_present",
                    static_cast<std::int64_t>(sub.original_content->top_channels_present));
        json.end_object();
    } else {
        json.member_null("original_content");
    }
}

void write_ac4_substream_info(JsonSink& json, const ac4::ChannelSubstreamInfo& sub) {
    json.begin_object();
    write_ac4_substream_members(json, sub);
    json.end_object();
}

void write_ac4_object_entries(JsonSink& json, const std::vector<ac4::ObjectEntry>& objects) {
    json.begin_array();
    for (const auto& obj : objects) {
        json.begin_object();
        json.member("kind", object_kind_token(obj.kind));
        json.member("lfe", obj.lfe);
        json.member("ajoc_coded", obj.ajoc_coded);
        json.end_object();
    }
    json.end_array();
}

void write_ac4_ajoc_substream_info(JsonSink& json, const ac4::AjocSubstreamInfo& sub) {
    json.begin_object();
    json.member("b_lfe", sub.b_lfe);
    json.member("b_static_dmx", sub.b_static_dmx);
    json.member("n_fullband_dmx_signals", static_cast<std::int64_t>(sub.n_fullband_dmx_signals));
    json.key("static_objects");
    write_ac4_object_entries(json, sub.static_objects);
    json.member("n_fullband_upmix_signals", static_cast<std::int64_t>(sub.n_fullband_upmix_signals));
    json.key("upmix_objects");
    write_ac4_object_entries(json, sub.upmix_objects);
    if (sub.sf_multiplier.has_value()) {
        json.member("sf_multiplier", static_cast<std::int64_t>(*sub.sf_multiplier));
    } else {
        json.member_null("sf_multiplier");
    }
    if (sub.bitrate_kbps.has_value()) {
        json.member("bitrate_kbps", static_cast<std::int64_t>(*sub.bitrate_kbps));
    } else {
        json.member_null("bitrate_kbps");
    }
    if (sub.substream_index.has_value()) {
        json.member("substream_index", static_cast<std::int64_t>(*sub.substream_index));
    } else {
        json.member_null("substream_index");
    }
    json.end_object();
}

void write_ac4_obj_substream_info(JsonSink& json, const ac4::ObjSubstreamInfo& sub) {
    json.begin_object();
    json.key("objects");
    write_ac4_object_entries(json, sub.objects);
    json.member("b_dynamic_objects", sub.b_dynamic_objects);
    if (sub.sf_multiplier.has_value()) {
        json.member("sf_multiplier", static_cast<std::int64_t>(*sub.sf_multiplier));
    } else {
        json.member_null("sf_multiplier");
    }
    if (sub.bitrate_kbps.has_value()) {
        json.member("bitrate_kbps", static_cast<std::int64_t>(*sub.bitrate_kbps));
    } else {
        json.member_null("bitrate_kbps");
    }
    if (sub.substream_index.has_value()) {
        json.member("substream_index", static_cast<std::int64_t>(*sub.substream_index));
    } else {
        json.member_null("substream_index");
    }
    json.end_object();
}

// One entry of a substream group's own substream list (§6.2.1.6) - a tagged
// union in JSON the same way ac4::GroupSubstream is in C++: "kind" says
// which of "chan"/"ajoc"/"obj" is non-null.
void write_ac4_group_substream(JsonSink& json, const ac4::GroupSubstream& sub) {
    json.begin_object();
    switch (sub.kind) {
        case ac4::GroupSubstream::Kind::kChan:
            json.member("kind", "chan");
            break;
        case ac4::GroupSubstream::Kind::kAjoc:
            json.member("kind", "ajoc");
            break;
        case ac4::GroupSubstream::Kind::kObj:
            json.member("kind", "obj");
            break;
    }
    json.key("chan");
    if (sub.chan.has_value()) {
        write_ac4_substream_info(json, *sub.chan);
    } else {
        json.value_null();
    }
    json.key("ajoc");
    if (sub.ajoc.has_value()) {
        write_ac4_ajoc_substream_info(json, *sub.ajoc);
    } else {
        json.value_null();
    }
    json.key("obj");
    if (sub.obj.has_value()) {
        write_ac4_obj_substream_info(json, *sub.obj);
    } else {
        json.value_null();
    }
    json.end_object();
}

// --- What the decoder reports (planning/ac4.md, "Media information") ------

void write_speakers(JsonSink& json, std::string_view name, std::span<const ac4::Speaker> speakers) {
    json.key(name);
    json.begin_array();
    for (const ac4::Speaker speaker : speakers) {
        json.value(ac4::describe(speaker));
    }
    json.end_array();
}

void write_optional(JsonSink& json, std::string_view name, const std::optional<int>& value) {
    if (value) {
        json.member(name, static_cast<std::int64_t>(*value));
    } else {
        json.member_null(name);
    }
}

void write_optional(JsonSink& json, std::string_view name, const std::optional<double>& value,
                    int decimals) {
    if (value) {
        json.member(name, *value, decimals);
    } else {
        json.member_null(name);
    }
}

std::string_view role_token(ac4::SubstreamRole role) {
    switch (role) {
        case ac4::SubstreamRole::kMain:
            return "main";
        case ac4::SubstreamRole::kMusicAndEffects:
            return "music_and_effects";
        case ac4::SubstreamRole::kDialogue:
            return "dialogue";
        case ac4::SubstreamRole::kDialogueEnhancement:
            return "dialogue_enhancement";
        case ac4::SubstreamRole::kAssociated:
            return "associated";
    }
    return "unknown";
}

// What the decoder read of a presentation.
void write_ac4_presentation_members(JsonSink& json, const ac4::PresentationInfo& info) {
    json.member("index", static_cast<std::uint64_t>(info.index));
    write_optional(json, "presentation_id", info.presentation_id);
    json.member("presentation_version", static_cast<std::int64_t>(info.presentation_version));
    write_optional(json, "presentation_config", info.presentation_config);
    write_optional(json, "md_compat", info.md_compat);
    json.member("enabled", info.enabled);
    json.member("alternative", info.alternative);
    json.member("pre_virtualized", info.pre_virtualized);
    json.member("name", std::string_view{info.name});
    json.member("language", std::string_view{info.language});
    write_speakers(json, "channels", info.speakers);
    json.key("substream_groups");
    json.begin_array();
    for (const int group : info.substream_groups) {
        json.value(static_cast<std::int64_t>(group));
    }
    json.end_array();
    json.member("decodable", info.decodable);
    json.member("selectable", info.selectable);
    json.key("members");
    json.begin_array();
    for (const ac4::PresentationMember& member : info.members) {
        json.begin_object();
        json.member("substream", static_cast<std::int64_t>(member.substream));
        json.member("role", role_token(member.role));
        json.member("group", static_cast<std::int64_t>(member.group));
        write_optional(json, "content_classifier", member.content_classifier);
        json.member("language", std::string_view{member.language});
        write_speakers(json, "channels", member.speakers);
        json.end_object();
    }
    json.end_array();
}

std::string_view compression_token(ac4::DrcModeInfo::Compression compression) {
    switch (compression) {
        case ac4::DrcModeInfo::Compression::kDefaultProfile:
            return "default_profile";
        case ac4::DrcModeInfo::Compression::kCurve:
            return "curve";
        case ac4::DrcModeInfo::Compression::kGains:
            return "gains";
    }
    return "unknown";
}

std::string_view preferred_token(ac4::DownmixInfo::Preferred preferred) {
    switch (preferred) {
        case ac4::DownmixInfo::Preferred::kNotIndicated:
            return "not_indicated";
        case ac4::DownmixInfo::Preferred::kLoRo:
            return "lo_ro";
        case ac4::DownmixInfo::Preferred::kLtRt:
            return "lt_rt";
        case ac4::DownmixInfo::Preferred::kLtRtProLogicII:
            return "lt_rt_pro_logic_ii";
    }
    return "unknown";
}

// The selected presentation's metadata. A gain of 0, -infinity dB, is null,
// as JSON writes no infinity.
void write_ac4_metadata(JsonSink& json, const ac4::PresentationMetadata& metadata) {
    json.begin_object();
    if (metadata.presentation) {
        json.member("presentation", static_cast<std::uint64_t>(*metadata.presentation));
    } else {
        json.member_null("presentation");
    }
    const ac4::LoudnessInfo& l = metadata.loudness;
    json.key("loudness");
    json.begin_object();
    write_optional(json, "dialnorm_dbfs", l.dialnorm_dbfs, 2);
    write_optional(json, "practice", l.practice);
    write_optional(json, "correction_gating", l.correction_gating);
    json.member("corrected_in_real_time", l.corrected_in_real_time);
    write_optional(json, "integrated_lkfs", l.integrated_lkfs, 1);
    write_optional(json, "speech_gated_lkfs", l.speech_gated_lkfs, 1);
    write_optional(json, "speech_gating", l.speech_gating);
    write_optional(json, "short_term_lufs", l.short_term_lufs, 1);
    write_optional(json, "max_short_term_lufs", l.max_short_term_lufs, 1);
    write_optional(json, "true_peak_dbtp", l.true_peak_dbtp, 1);
    write_optional(json, "max_true_peak_dbtp", l.max_true_peak_dbtp, 1);
    write_optional(json, "loudness_range_lu", l.loudness_range_lu, 1);
    write_optional(json, "loudness_range_practice", l.loudness_range_practice);
    write_optional(json, "momentary_lufs", l.momentary_lufs, 1);
    write_optional(json, "max_momentary_lufs", l.max_momentary_lufs, 1);
    json.end_object();
    json.key("drc");
    if (metadata.drc) {
        json.begin_object();
        json.member("eac3_profile", static_cast<std::int64_t>(metadata.drc->eac3_profile));
        json.key("modes");
        json.begin_array();
        for (const ac4::DrcModeInfo& mode : metadata.drc->modes) {
            json.begin_object();
            json.member("id", static_cast<std::int64_t>(mode.id));
            write_optional(json, "output_level_from_db", mode.output_level_from_db);
            write_optional(json, "output_level_to_db", mode.output_level_to_db);
            json.member("compression", compression_token(mode.compression));
            write_optional(json, "repeat_of", mode.repeat_of);
            write_optional(json, "gains_config", mode.gains_config);
            json.end_object();
        }
        json.end_array();
        write_optional(json, "applied_mode", metadata.drc->applied_mode);
        json.end_object();
    } else {
        json.value_null();
    }
    json.key("dialogue_enhancement");
    if (metadata.dialogue_enhancement) {
        const ac4::DialogueEnhancementInfo& de = *metadata.dialogue_enhancement;
        json.begin_object();
        json.member("method", static_cast<std::int64_t>(de.method));
        json.member("left", de.left);
        json.member("right", de.right);
        json.member("centre", de.centre);
        json.member("max_gain_db", de.max_gain_db, 1);
        json.end_object();
    } else {
        json.value_null();
    }
    json.key("downmix");
    if (metadata.downmix) {
        const ac4::DownmixInfo& d = *metadata.downmix;
        json.begin_object();
        json.member("loro_centre_db", d.loro_centre_db, 1);
        json.member("loro_surround_db", d.loro_surround_db, 1);
        json.member("ltrt_centre_db", d.ltrt_centre_db, 1);
        json.member("ltrt_surround_db", d.ltrt_surround_db, 1);
        write_optional(json, "lfe_db", d.lfe_db, 1);
        json.member("preferred", preferred_token(d.preferred));
        write_optional(json, "loro_correction_db2", d.loro_correction_db2, 1);
        write_optional(json, "ltrt_correction_db2", d.ltrt_correction_db2, 1);
        json.end_object();
    } else {
        json.value_null();
    }
    json.end_object();
}

}  // namespace

void write_ac4_stream(JsonSink& json, const Ac4Summary& summary) {
    json.key("stream");
    json.begin_object();
    json.member("codec", "ac4");
    json.member("access_units", static_cast<std::uint64_t>(summary.sync_frames));
    json.member("syncframes", static_cast<std::uint64_t>(summary.sync_frames));
    json.member("bytes", static_cast<std::uint64_t>(summary.bytes));
    json.key("integrity");
    json.begin_object();
    json.member("crc_valid", summary.crc_failures == 0 && summary.sync_frames > 0);
    json.member("crc_failures", static_cast<std::uint64_t>(summary.crc_failures));
    json.member("parse_failures", summary.parse_error ? std::uint64_t{1} : std::uint64_t{0});
    if (summary.parse_error.has_value()) {
        json.member("first_parse_error", ac4_error_token(*summary.parse_error));
    } else {
        json.member_null("first_parse_error");
    }
    json.end_object();

    json.key("ac4");
    if (!summary.first_frame.has_value()) {
        json.value_null();
        json.end_object();
        return;
    }
    const auto& toc = summary.first_frame->toc;
    json.begin_object();
    json.member("bitstream_version", static_cast<std::int64_t>(toc.bitstream_version));
    json.member("sample_rate_hz", static_cast<std::int64_t>(toc.sample_rate_hz));
    json.member("frame_rate_index", static_cast<std::int64_t>(toc.frame_rate_index));
    json.member("n_presentations", static_cast<std::int64_t>(toc.n_presentations));
    json.key("frame_rate");
    if (summary.frame_rate) {
        json.begin_object();
        json.member("fps", summary.frame_rate->frames_per_second, 3);
        json.member("frame_length", static_cast<std::int64_t>(summary.frame_rate->frame_length));
        json.member("internal_sample_rate_hz", summary.frame_rate->internal_rate_hz, 2);
        json.end_object();
    } else {
        json.value_null();
    }
    write_optional(json, "bitrate_kbps", summary.bitrate_kbps, 1);
    json.member("iframes", static_cast<std::uint64_t>(summary.iframes));
    json.key("iframe_interval_frames");
    if (summary.min_iframe_interval && summary.max_iframe_interval) {
        json.begin_object();
        json.member("min", static_cast<std::uint64_t>(*summary.min_iframe_interval));
        json.member("max", static_cast<std::uint64_t>(*summary.max_iframe_interval));
        json.end_object();
    } else {
        json.value_null();
    }
    json.member("splices", static_cast<std::uint64_t>(summary.splices));

    json.key("substream_groups");
    json.begin_array();
    for (const auto& group : toc.substream_groups) {
        json.begin_object();
        json.member("b_substreams_present", group.b_substreams_present);
        json.member("b_channel_coded", group.b_channel_coded);
        json.key("oamd");
        if (group.oamd.has_value()) {
            json.begin_object();
            json.member("b_oamd_ndot", group.oamd->b_oamd_ndot);
            if (group.oamd->substream_index.has_value()) {
                json.member("substream_index", static_cast<std::int64_t>(*group.oamd->substream_index));
            } else {
                json.member_null("substream_index");
            }
            json.end_object();
        } else {
            json.value_null();
        }
        json.key("substreams");
        json.begin_array();
        for (const auto& sub : group.substreams) {
            write_ac4_group_substream(json, sub);
        }
        json.end_array();
        json.end_object();
    }
    json.end_array();

    json.key("presentations_v0");
    json.begin_array();
    for (std::size_t p = 0; p < toc.presentations_v0.size(); ++p) {
        const auto& pres = toc.presentations_v0[p];
        json.begin_object();
        json.member("presentation_version", static_cast<std::int64_t>(pres.presentation_version));
        // What the decoder read of it.
        if (p < summary.presentations.size()) {
            json.key("decoded");
            json.begin_object();
            write_ac4_presentation_members(json, summary.presentations[p]);
            json.end_object();
        }
        json.key("substreams");
        json.begin_array();
        // Each entry is the substream's own members, with its role beside
        // them.
        for (const auto& [role, sub] : pres.substreams) {
            json.begin_object();
            json.member("role", role);
            write_ac4_substream_members(json, sub);
            json.end_object();
        }
        json.end_array();
        json.end_object();
    }
    json.end_array();

    // Version 1 presentations, as the decoder reads them.
    json.key("presentations_v1");
    json.begin_array();
    if (toc.bitstream_version >= 2) {
        for (const ac4::PresentationInfo& info : summary.presentations) {
            json.begin_object();
            write_ac4_presentation_members(json, info);
            json.end_object();
        }
    }
    json.end_array();
    json.key("selected_presentation");
    if (summary.metadata && summary.metadata->presentation) {
        json.value(static_cast<std::uint64_t>(*summary.metadata->presentation));
    } else {
        json.value_null();
    }
    json.key("metadata");
    if (summary.metadata) {
        write_ac4_metadata(json, *summary.metadata);
    } else {
        json.value_null();
    }
    json.end_object();  // ac4

    json.end_object();  // stream
}

}  // namespace ac3::apps::probe_json

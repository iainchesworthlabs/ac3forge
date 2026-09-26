#include "probe.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fmt/base.h>
#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../json.hpp"
#include "../platform/stdio_binary.hpp"
#include "../support.hpp"
#include "ac3/analysis/levels.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/syntax_trace.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/probe.hpp"
#include "ac3/meta/bsi.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/signing/emdf_atmos_signer.hpp"
#include "ac3/version.hpp"
#include "ac4/ac4.hpp"
#include "container_input.hpp"
#include "probe_json.hpp"

namespace ac3cli::commands {

namespace {

namespace io = ac3::io;

// How much per-frame detail was asked for. The stream summary always comes
// out; these add to it.
enum class Detail : std::uint8_t {
    kNone,
    kFrames,  // one entry per access unit: offsets, sizes, CRC, headers
    kBlocks,  // ...plus every block's coding tools and exponent strategies
};

// --- naming ----------------------------------------------------------------
// The document's vocabulary - fixed text keyed off transmitted values - and
// its stream summary are shared with Hearth's media information, so that no
// two tools here name one stream two ways (apps/common/probe_json.hpp). The
// table uses the same names.

using ac3::apps::probe_json::Ac4Summary;
using ac3::apps::probe_json::bed_label;
using ac3::apps::probe_json::bsmod_label;
using ac3::apps::probe_json::codec_label;
using ac3::apps::probe_json::describe_group_substream;
using ac3::apps::probe_json::dialnorm_db;
using ac3::apps::probe_json::emdf_payload_label;
using ac3::apps::probe_json::exp_strategy_token;
using ac3::apps::probe_json::strmtyp_token;
using ac3::apps::probe_json::summarize_ac4;
using ac3::apps::probe_json::write_ac4_stream;
using ac3::apps::probe_json::write_container;
using ac3::apps::probe_json::write_stream;

// --- human-readable table --------------------------------------------------

void print_range(std::string_view label, const io::MinMax& range, std::string_view unit) {
    if (!range.seen) {
        fmt::println("{:<16}absent", label);
        return;
    }
    if (range.constant()) {
        fmt::println("{:<16}{}{}", label, range.min, unit);
        return;
    }
    fmt::println("{:<16}{}{} .. {}{}", label, range.min, unit, range.max, unit);
}

void print_container(const ac3::apps::ContainerFacts& facts);

void print_table(std::string_view path, const io::ProbeReport& report,
                 const ac3::apps::ContainerFacts& container) {
    fmt::println("{:<16}{}", "file", path);
    print_container(container);
    fmt::println("{:<16}{} (bsid {})", "codec", codec_label(report.kind), report.bsid);
    fmt::println("{:<16}{} Hz{}", "sample rate", ac3::sample_rate_hz(report.sample_rate),
                 report.reduced_rate ? " (fscod2 reduced rate)" : "");
    fmt::println("{:<16}{} ({})", "bsmod", report.bsmod,
                 bsmod_label(report.bsmod, report.acmod));
    fmt::println("{:<16}{} (acmod {}, lfeon {})", "layout",
                 ac3::analysis::layout_name(report.acmod, report.lfe),
                 static_cast<int>(report.acmod), report.lfe ? 1 : 0);
    if (report.layout.count > 0) {
        std::string locations;
        for (const auto location : report.layout) {
            locations += locations.empty() ? "" : " ";
            locations += ac3::eac3::chanmap::name(location);
        }
        fmt::println("{:<16}{} channel(s): {}", "renders", report.rendered_channels, locations);
    } else {
        fmt::println("{:<16}{} channel(s), no Table E2.5 layout (dual mono)", "renders",
                     report.rendered_channels);
    }
    fmt::println("{:<16}{} per syncframe (numblkscod {})", "blocks",
                 report.kind == io::StreamKind::kAc3
                     ? ac3::kBlocksPerFrame
                     : ac3::eac3::blocks_per_syncframe(report.numblkscod),
                 report.numblkscod);

    fmt::println("{:<16}{} per access unit", "substreams", report.substreams_per_unit);
    for (const auto& sub : report.substreams) {
        std::string chanmap = "-";
        if (sub.chanmap.has_value()) {
            chanmap = fmt::format("chanmap 0x{:04x}", *sub.chanmap);
        }
        fmt::println("  {:<14}{} id {}, {}, {} syncframe(s), {}", "",
                     strmtyp_token(sub.strmtyp), sub.substreamid,
                     ac3::analysis::layout_name(sub.acmod, sub.lfe), sub.syncframes, chanmap);
    }

    fmt::println("{:<16}{} ({} syncframe(s)), {} bytes", "access units", report.access_units,
                 report.syncframes, report.bytes);
    fmt::println("{:<16}{:.3f} s", "duration", report.duration_seconds);
    if (report.nominal_bitrate_kbps.has_value()) {
        fmt::println("{:<16}{:.1f} kbit/s measured, {} kbit/s declared (frmsizecod)", "bit rate",
                     report.bitrate_kbps, *report.nominal_bitrate_kbps);
    } else {
        fmt::println("{:<16}{:.1f} kbit/s measured", "bit rate", report.bitrate_kbps);
    }
    fmt::println("{:<16}{} ({} .. {} bytes per access unit)", "rate control",
                 report.variable_bitrate ? "variable" : "constant",
                 report.min_access_unit_bytes, report.max_access_unit_bytes);

    // dialnorm is negated into dB here rather than in the MinMax itself, so
    // the range's own min/max stay the transmitted codes; -1 is the LOUDEST
    // dialnorm and -31 the quietest, so the code range inverts.
    if (report.dialnorm.seen) {
        const io::MinMax db{.seen = true,
                            .min = dialnorm_db(report.dialnorm.max),
                            .max = dialnorm_db(report.dialnorm.min)};
        print_range("dialnorm", db, " dB");
    } else {
        print_range("dialnorm", report.dialnorm, " dB");
    }
    if (report.dialnorm2.seen) {
        const io::MinMax db{.seen = true,
                            .min = dialnorm_db(report.dialnorm2.max),
                            .max = dialnorm_db(report.dialnorm2.min)};
        print_range("dialnorm2", db, " dB");
    }
    print_range("compr", report.compr, "");
    if (report.compr2.seen) {
        print_range("compr2", report.compr2, "");
    }
    print_range("dynrng", report.dynrng, "");
    if (report.dynrng2.seen) {
        print_range("dynrng2", report.dynrng2, "");
    }
    // Table D2.2's code and its name, in the same "code (name)" shape as bsmod.
    if (report.dmixmod.has_value()) {
        fmt::println("{:<16}{} ({})", "dmixmod", static_cast<int>(*report.dmixmod),
                     ac3::meta::describe(*report.dmixmod));
    } else {
        fmt::println("{:<16}absent", "dmixmod");
    }

    if (report.emdf_payload_ids.empty()) {
        fmt::println("{:<16}none", "EMDF");
    } else {
        std::string ids;
        for (const int id : report.emdf_payload_ids) {
            ids += ids.empty() ? "" : ", ";
            const auto label = emdf_payload_label(id);
            ids += label.empty() ? fmt::format("{}", id) : fmt::format("{} ({})", id, label);
        }
        fmt::println("{:<16}payload id(s) {}", "EMDF", ids);
    }
    if (report.program.has_value()) {
        fmt::println("{:<16}{} object(s): bed {}, {} dynamic, in {} frame(s)", "object audio",
                     ac3::oba::object_count(*report.program), bed_label(*report.program),
                     report.program->dynamic_objects, report.object_frames);
    } else if (report.oba_complexity_index.has_value()) {
        fmt::println("{:<16}addbsi marker only, no OAMD payload parsed", "object audio");
    } else {
        fmt::println("{:<16}none", "object audio");
    }
    if (report.oba_complexity_index.has_value()) {
        fmt::println("{:<16}{}", "complexity", *report.oba_complexity_index);
    }
    fmt::println("{:<16}{}", "JOC", report.joc ? "present" : "absent");
    fmt::println("{:<16}{}", "authenticity",
                 report.authenticity_tagged_frames > 0
                     ? fmt::format("tag in {} of {} syncframe(s)",
                                   report.authenticity_tagged_frames, report.syncframes)
                     : std::string{"no tag"});

    fmt::println("{:<16}{} of {} syncframe(s) valid", "CRC",
                 report.syncframes - report.crc_failures, report.syncframes);
    if (report.parse_failures > 0) {
        fmt::println("{:<16}{} syncframe(s) refused by the parser{}", "parse errors",
                     report.parse_failures,
                     report.first_parse_error
                         ? fmt::format(" (first: {})", ac3::describe(*report.first_parse_error))
                         : std::string{});
    }

    const auto& tools = report.tools;
    if (tools.blocks == 0) {
        fmt::println("{:<16}no block was parsed", "tools");
        return;
    }
    const auto usage = [&](std::string_view name, std::uint64_t count) {
        if (count > 0) {
            fmt::println("  {:<14}{} of {} block(s)", name, count, tools.blocks);
        }
    };
    fmt::println("{:<16}{} block(s) parsed", "tools", tools.blocks);
    usage("coupling", tools.coupling);
    usage("enh coupling", tools.enhanced_coupling);
    usage("spx", tools.spectral_extension);
    usage("block switch", tools.block_switch);
    usage("dither", tools.dither);
    usage("rematrix", tools.rematrixing);
    usage("delta ba", tools.delta_bit_alloc);
    usage("skip field", tools.skip_field);
    if (tools.aht_frames > 0) {
        fmt::println("  {:<14}{} of {} syncframe(s)", "aht", tools.aht_frames,
                     report.syncframes);
    }
    if (tools.transient_prenoise_frames > 0) {
        fmt::println("  {:<14}{} of {} syncframe(s)", "tpnp", tools.transient_prenoise_frames,
                     report.syncframes);
    }
    fmt::println("  {:<14}reuse {}, D15 {}, D25 {}, D45 {}", "exponents", tools.exp_strategy[0],
                 tools.exp_strategy[1], tools.exp_strategy[2], tools.exp_strategy[3]);
}

// --- per-frame dump, human-readable ----------------------------------------

void print_access_unit(const io::ProbeAccessUnit& unit, Detail detail) {
    fmt::println("");
    fmt::println("access unit {} @ {} ({} bytes, t={:.4f}s)", unit.index, unit.byte_offset,
                 unit.bytes, unit.start_seconds);
    for (const auto& frame : unit.syncframes) {
        fmt::println("  {} id {} @ {}: {} bytes, {}, {}, dialnorm {} dB{}{}",
                     strmtyp_token(frame.header.strmtyp), frame.header.substreamid,
                     frame.byte_offset, frame.header.bytes,
                     ac3::analysis::layout_name(frame.header.acmod, frame.header.lfe),
                     frame.crc_valid ? "crc ok" : "CRC BAD",
                     dialnorm_db(frame.header.dialnorm),
                     frame.header.compr ? fmt::format(", compr {}", *frame.header.compr)
                                        : std::string{},
                     frame.authenticity_tag ? ", signed" : "");
        if (frame.parse_error.has_value()) {
            fmt::println("    parse error: {}", ac3::describe(*frame.parse_error));
        }
        if (frame.objects.has_value()) {
            fmt::println("    objects: {} total, {} dynamic, bed {}",
                         ac3::oba::object_count(frame.objects->program),
                         frame.objects->program.dynamic_objects,
                         bed_label(frame.objects->program));
        }
        if (detail != Detail::kBlocks || !frame.syntax.valid) {
            continue;
        }
        const auto& syntax = frame.syntax;
        for (int index = 0; index < syntax.block_count; ++index) {
            const auto& block = syntax.blocks[static_cast<std::size_t>(index)];
            if (!block.entered) {
                fmt::println("    blk {}: not reached", index);
                continue;
            }
            std::string tools;
            const auto add = [&tools](std::string_view name, bool on) {
                if (on) {
                    tools += tools.empty() ? "" : "+";
                    tools += name;
                }
            };
            add("cpl", block.coupling && !block.enhanced_coupling);
            add("ecpl", block.enhanced_coupling);
            add("spx", block.spectral_extension);
            add("blksw", block.block_switch != 0);
            add("dither", block.dither != 0);
            add("remat", block.rematrixing);
            add("dba", block.delta_bit_alloc);
            add("skip", block.skip_field);
            std::string strategies;
            const int coded = syntax.fbw_channels + (syntax.lfe ? 1 : 0);
            for (int stream = 0; stream < coded; ++stream) {
                strategies += strategies.empty() ? "" : " ";
                strategies += exp_strategy_token(
                    block.exp_strategy[static_cast<std::size_t>(stream)]);
            }
            if (block.coupling) {
                strategies += fmt::format(
                    " cpl:{}", exp_strategy_token(block.exp_strategy[ac3::kCouplingSyntaxStream]));
            }
            fmt::println("    blk {}: {:<28} exp [{}]", index,
                         tools.empty() ? "-" : tools, strategies);
        }
    }
}

// --- JSON ------------------------------------------------------------------
// The per-frame dump. The stream summary after it is probe_json's.

void write_syncframe(JsonWriter& json, const io::ProbeSyncframe& frame, Detail detail) {
    json.begin_object();
    json.member("byte_offset", static_cast<std::uint64_t>(frame.byte_offset));
    json.member("bytes", static_cast<std::uint64_t>(frame.header.bytes));
    json.member("stream_type", strmtyp_token(frame.header.strmtyp));
    json.member("substream_id", static_cast<std::int64_t>(frame.header.substreamid));
    json.member("bsid", static_cast<std::int64_t>(frame.header.bsid));
    json.member("bsmod", static_cast<std::int64_t>(frame.header.bsmod));
    json.member("acmod", static_cast<std::int64_t>(frame.header.acmod));
    json.member("lfeon", frame.header.lfe);
    json.member("numblkscod", static_cast<std::int64_t>(frame.header.numblkscod));
    json.member("dialnorm_db", static_cast<std::int64_t>(dialnorm_db(frame.header.dialnorm)));
    if (frame.header.compr.has_value()) {
        json.member("compr", static_cast<std::int64_t>(*frame.header.compr));
    } else {
        json.member_null("compr");
    }
    if (frame.header.dmixmod.has_value()) {
        json.member("dmixmod", static_cast<std::int64_t>(*frame.header.dmixmod));
    } else {
        json.member_null("dmixmod");
    }
    if (frame.header.chanmap.has_value()) {
        json.member("chanmap", static_cast<std::int64_t>(*frame.header.chanmap));
    } else {
        json.member_null("chanmap");
    }
    json.member("crc_valid", frame.crc_valid);
    json.member("authenticity_tag", frame.authenticity_tag);
    if (frame.parse_error.has_value()) {
        json.member("parse_error", ac3::describe(*frame.parse_error));
    } else {
        json.member_null("parse_error");
    }
    if (frame.objects.has_value()) {
        json.key("objects");
        json.begin_object();
        json.member("total", static_cast<std::int64_t>(
                                 ac3::oba::object_count(frame.objects->program)));
        json.member("dynamic",
                    static_cast<std::int64_t>(frame.objects->program.dynamic_objects));
        json.member("bed", bed_label(frame.objects->program));
        json.end_object();
    } else {
        json.member_null("objects");
    }
    if (detail != Detail::kBlocks || !frame.syntax.valid) {
        json.end_object();
        return;
    }

    const auto& syntax = frame.syntax;
    json.key("frame_tools");
    json.begin_object();
    json.member("block_switch_enabled", syntax.block_switch_enabled);
    json.member("dither_enabled", syntax.dither_enabled);
    json.member("bamode", syntax.bamode);
    json.member("delta_bit_alloc_enabled", syntax.delta_bit_alloc_enabled);
    json.member("skip_enabled", syntax.skip_enabled);
    json.member("spx_attenuation_enabled", syntax.spx_attenuation_enabled);
    json.member("transient_prenoise", syntax.transient_prenoise);
    json.member("per_block_exp_strategy", syntax.per_block_exp_strategy);
    json.member("snroffststr", static_cast<std::int64_t>(syntax.snroffststr));
    json.key("aht_streams");
    json.begin_array();
    for (int stream = 0; stream < ac3::kMaxSyntaxStreams; ++stream) {
        if (syntax.aht_stream[static_cast<std::size_t>(stream)]) {
            json.value(static_cast<std::int64_t>(stream));
        }
    }
    json.end_array();
    json.end_object();

    json.key("blocks");
    json.begin_array();
    for (int index = 0; index < syntax.block_count; ++index) {
        const auto& block = syntax.blocks[static_cast<std::size_t>(index)];
        json.begin_object();
        json.member("index", static_cast<std::int64_t>(index));
        json.member("parsed", block.entered);
        json.member("coupling", block.coupling);
        json.member("enhanced_coupling", block.enhanced_coupling);
        json.member("spectral_extension", block.spectral_extension);
        json.member("block_switch", static_cast<std::int64_t>(block.block_switch));
        json.member("dither", static_cast<std::int64_t>(block.dither));
        json.member("rematrixing", block.rematrixing);
        json.member("delta_bit_alloc", block.delta_bit_alloc);
        json.member("skip_field", block.skip_field);
        json.member("skip_bytes", static_cast<std::int64_t>(block.skip_bytes));
        json.key("exponent_strategy");
        json.begin_array();
        const int coded = syntax.fbw_channels + (syntax.lfe ? 1 : 0);
        for (int stream = 0; stream < coded; ++stream) {
            json.value(exp_strategy_token(block.exp_strategy[static_cast<std::size_t>(stream)]));
        }
        json.end_array();
        if (block.coupling) {
            json.member("coupling_exponent_strategy",
                        exp_strategy_token(block.exp_strategy[ac3::kCouplingSyntaxStream]));
        } else {
            json.member_null("coupling_exponent_strategy");
        }
        json.end_object();
    }
    json.end_array();
    json.end_object();
}

void write_access_unit(JsonWriter& json, const io::ProbeAccessUnit& unit, Detail detail) {
    json.begin_object();
    json.member("index", static_cast<std::uint64_t>(unit.index));
    json.member("byte_offset", static_cast<std::uint64_t>(unit.byte_offset));
    json.member("bytes", static_cast<std::uint64_t>(unit.bytes));
    json.member("start_seconds", unit.start_seconds, 6);
    json.key("syncframes");
    json.begin_array();
    for (const auto& frame : unit.syncframes) {
        write_syncframe(json, frame, detail);
    }
    json.end_array();
    json.end_object();
}

// --- AC-4 ---------------------------------------------------------------
//
// A separate walk from everything above, over ac4::scan()/parse_raw_frame()
// rather than ac3::io::Prober: probe_json's summarize_ac4() and
// write_ac4_stream(), whose comments say why the document's AC-4 stream has
// its own shape.
//
// No detail=frames/detail=blocks equivalent exists here: there is no
// per-block audio-layer walk to show, by scope.

// What a Matroska, MP4 or MPEG-TS file said of the track the table describes,
// on the line after the file's name; nothing for a bare stream.
void print_container(const ac3::apps::ContainerFacts& facts) {
    using ac3::apps::ContainerKind;
    switch (facts.kind) {
        case ContainerKind::kUnknown:
            return;
        case ContainerKind::kMpegTs:
            fmt::println("{:<16}MPEG-TS, PID {}, program {}, stream_type 0x{:02X} ({}), {} PES "
                         "payloads, {}-byte packets",
                         "container", facts.track, facts.program_number, facts.stream_type,
                         facts.signalling, facts.samples, facts.packet_size);
            return;
        case ContainerKind::kMp4:
        case ContainerKind::kMatroska:
            break;
    }
    const std::string box =
        facts.codec_box ? fmt::format(", {} of {} bytes", facts.codec_box->type,
                                      facts.codec_box->bytes)
                        : std::string{};
    const std::string timescale =
        facts.kind == ContainerKind::kMp4 && facts.timescale != facts.sample_rate
            ? fmt::format(", timescale {}", facts.timescale)
            : std::string{};
    fmt::println("{:<16}{}, track {}, '{}', {} {}, {} Hz{}, {} channel(s){}", "container",
                 facts.kind == ContainerKind::kMp4 ? "MP4" : "Matroska", facts.track,
                 facts.codec_id, facts.samples,
                 facts.kind == ContainerKind::kMp4 ? "samples" : "frames", facts.sample_rate,
                 timescale, facts.channels, box);
}

void print_ac4_table(std::string_view path, const Ac4Summary& summary,
                     const ac3::apps::ContainerFacts& container) {
    fmt::println("{:<16}{}", "file", path);
    print_container(container);
    fmt::println("{:<16}AC-4", "codec");
    fmt::println("{:<16}{} ({} sync frame(s)), {} bytes", "access units", summary.sync_frames,
                 summary.sync_frames, summary.bytes);
    fmt::println("{:<16}{} of {} valid", "CRC", summary.sync_frames - summary.crc_failures,
                 summary.sync_frames);
    if (summary.parse_error.has_value()) {
        fmt::println("{:<16}{}", "parse error", ac4::describe(*summary.parse_error));
    }
    if (!summary.first_frame.has_value()) {
        return;
    }
    const auto& toc = summary.first_frame->toc;
    fmt::println("{:<16}{}", "bs version", toc.bitstream_version);
    fmt::println("{:<16}{} Hz", "sample rate", toc.sample_rate_hz);
    if (summary.frame_rate) {
        fmt::println("{:<16}{:.3f} fps, {} samples a frame at {:.2f} Hz", "frame rate",
                     summary.frame_rate->frames_per_second, summary.frame_rate->frame_length,
                     summary.frame_rate->internal_rate_hz);
    }
    if (summary.bitrate_kbps) {
        fmt::println("{:<16}{:.1f} kbps", "bit rate", *summary.bitrate_kbps);
    }
    if (summary.min_iframe_interval && summary.max_iframe_interval) {
        fmt::println("{:<16}{}, every {} frames", "I-frames", summary.iframes,
                     *summary.min_iframe_interval == *summary.max_iframe_interval
                         ? fmt::format("{}", *summary.min_iframe_interval)
                         : fmt::format("{} to {}", *summary.min_iframe_interval,
                                       *summary.max_iframe_interval));
    } else {
        fmt::println("{:<16}{}", "I-frames", summary.iframes);
    }
    fmt::println("{:<16}{}", "splices", summary.splices);
    fmt::println("{:<16}{}", "presentations", toc.n_presentations);
    for (const auto& group : toc.substream_groups) {
        for (const auto& sub : group.substreams) {
            fmt::println("  {:<14}{}", "", describe_group_substream(sub));
        }
    }
    for (const auto& pres : toc.presentations_v0) {
        for (const auto& [role, sub] : pres.substreams) {
            fmt::println("  {:<14}{}: {}", "", role, sub.channel_mode_name);
        }
    }
    // What the decoder read: each presentation, and the one it selects.
    const std::optional<std::size_t> selected =
        summary.metadata ? summary.metadata->presentation : std::optional<std::size_t>{};
    for (const ac4::PresentationInfo& p : summary.presentations) {
        std::string channels;
        for (const ac4::Speaker s : p.speakers) {
            channels += channels.empty() ? "" : " ";
            channels += ac4::describe(s);
        }
        std::string roles;
        for (const ac4::PresentationMember& m : p.members) {
            roles += roles.empty() ? "" : ", ";
            roles += ac4::describe(m.role);
            if (!m.language.empty()) {
                roles += " (" + m.language + ")";
            }
        }
        fmt::println(
            "{:<16}{}{}{}{}; {}; {}{}{}", fmt::format("presentation {}", p.index),
            p.presentation_id ? fmt::format("id {}, ", *p.presentation_id) : std::string{},
            p.name.empty() ? std::string{} : fmt::format("\"{}\", ", p.name),
            p.md_compat ? fmt::format("md_compat {}, ", *p.md_compat) : std::string{},
            channels.empty() ? "not decoded" : channels, roles, p.enabled ? "" : "disabled, ",
            p.pre_virtualized ? "pre-virtualized, " : "",
            selected == p.index ? "selected" : (p.selectable ? "selectable" : "not selectable"));
    }
    if (!summary.metadata) {
        return;
    }
    const ac4::PresentationMetadata& m = *summary.metadata;
    if (m.loudness.dialnorm_dbfs) {
        fmt::println("{:<16}{:g} dBFS", "dialnorm", *m.loudness.dialnorm_dbfs);
    }
    if (m.loudness.integrated_lkfs) {
        fmt::println("{:<16}{:.1f} LKFS integrated", "loudness", *m.loudness.integrated_lkfs);
    }
    if (m.loudness.max_true_peak_dbtp) {
        fmt::println("{:<16}{:.1f} dBTP", "true peak", *m.loudness.max_true_peak_dbtp);
    }
    if (m.drc) {
        std::string modes;
        for (const ac4::DrcModeInfo& mode : m.drc->modes) {
            modes += modes.empty() ? "" : ", ";
            modes += fmt::format(
                "{} {}", mode.id,
                mode.repeat_of ? fmt::format("as {}", *mode.repeat_of)
                : mode.compression == ac4::DrcModeInfo::Compression::kCurve ? "curve"
                : mode.compression == ac4::DrcModeInfo::Compression::kGains ? "gains"
                                                                            : "default profile");
        }
        fmt::println("{:<16}profile {}, modes {}", "DRC", m.drc->eac3_profile, modes);
    }
    if (m.dialogue_enhancement) {
        const ac4::DialogueEnhancementInfo& de = *m.dialogue_enhancement;
        fmt::println("{:<16}method {}, {}{}{}up to {:g} dB", "dialogue enh.", de.method,
                     de.left ? "L " : "", de.right ? "R " : "", de.centre ? "C " : "",
                     de.max_gain_db);
    }
    if (m.downmix) {
        const ac4::DownmixInfo& d = *m.downmix;
        fmt::println(
            "{:<16}Lo/Ro centre {:g} dB, surround {:g} dB; Lt/Rt centre {:g} dB, surround {:g} "
            "dB{}",
            "downmix", d.loro_centre_db, d.loro_surround_db, d.ltrt_centre_db, d.ltrt_surround_db,
            d.lfe_db ? fmt::format("; LFE {:g} dB", *d.lfe_db) : std::string{});
    }
}

int run_probe_ac4(std::string_view in_path, std::istream& in, const Options& meta,
                  const ac3::apps::ContainerFacts& container) {
    // Reads the whole input into memory - unlike the AC-3/E-AC-3 path above,
    // which pulls forward through a fixed window (see docs/forge/cli/commands.md's
    // "Memory is flat" claim, which is specific to that path and not
    // extended here). ac4::scan()/parse_raw_frame() operate on a
    // std::span - a deliberate parse-and-inspect design, not a streaming
    // decoder - and this command's own scope never walks per-block audio
    // detail the way detail=frames/blocks does for AC-3/E-AC-3, so an AC-4
    // file large enough for that to matter is not the case this exists for.
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> data(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        data[i] = static_cast<std::byte>(raw[i]);
    }
    const auto summary = summarize_ac4(data);
    if (summary.sync_frames == 0) {
        fmt::println(stderr, "error: {}",
                     summary.parse_error ? ac4::describe(*summary.parse_error)
                                          : "no AC-4 sync frame found");
        return 1;
    }
    if (meta.json) {
        JsonWriter json{stdout};
        json.begin_object();
        json.member("schema", "ac3forge.probe/1");
        json.member("generator", ac3::version_full);
        json.member("file", in_path);
        // null for a bare stream: the schema's members are never omitted.
        write_container(json, container);
        write_ac4_stream(json, summary);
        json.end_object();
        json.finish();
    } else {
        print_ac4_table(in_path, summary, container);
    }
    return summary.crc_failures > 0 || summary.parse_error ? 1 : 0;
}

}  // namespace

int run_probe(std::string_view in_path, const Options& meta) {
    Detail detail = Detail::kNone;
    if (meta.detail.has_value()) {
        detail = *meta.detail == "blocks" ? Detail::kBlocks : Detail::kFrames;
    }

    // "-" reads the stream from stdin, the same convention every other input
    // path here follows. It works for probe specifically because nothing in
    // this command ever seeks: AccessUnitReader pulls forward through a fixed
    // window, which is exactly what a pipe can give it.
    std::ifstream file;
    if (is_stdio_path(in_path)) {
        ac3::cli::platform::set_stdio_binary();
    } else {
        file.open(std::string{in_path}, std::ios::binary);
        if (!file) {
            fmt::println(stderr, "error: cannot open {}", in_path);
            return 1;
        }
    }
    // A container - Matroska, MP4 or MPEG-2 TS - is read whole and its audio
    // track's elementary stream probed, with what the container says of the
    // track beside it: the demux decode and the other commands read a
    // container through (container_input.hpp). A bare stream, from a file or
    // from stdin, is read as it arrives, as before.
    ac3::apps::ContainerFacts container;
    std::istringstream demuxed;
    std::istream* source = is_stdio_path(in_path) ? static_cast<std::istream*>(&std::cin) : &file;
    if (!is_stdio_path(in_path)) {
        // The sniff decides within a few hundred bytes (sniff_container).
        std::array<char, 4096> head{};
        file.read(head.data(), static_cast<std::streamsize>(head.size()));
        const auto got = static_cast<std::size_t>(file.gcount());
        file.clear();
        file.seekg(0, std::ios::beg);
        const auto kind = ac3::apps::sniff_container(std::as_bytes(std::span{head}).first(got));
        if (kind != ac3::apps::ContainerKind::kUnknown) {
            const auto bytes = read_all(in_path);
            auto result = ac3::apps::elementary_stream_from_bytes(bytes);
            if (!result.error.empty()) {
                fmt::println(stderr, "error: {} is a {}", in_path, result.error);
                return 1;
            }
            container = std::move(result.container);
            demuxed.str(std::string{reinterpret_cast<const char*>(result.bytes.data()),
                                    result.bytes.size()});
            source = &demuxed;
        }
    }
    std::istream& in = *source;

    // json=1's document is byte-exact text a consumer (or, here, a test's own
    // substring check) can reasonably expect to round-trip: on Windows, an
    // untouched stdout is in the same CRT text mode set_stdio_binary()'s own
    // header comment describes for raw audio bytes, so every 0x0A this writer
    // emits would otherwise arrive as 0x0D 0x0A. Idempotent alongside the
    // is_stdio_path() call above when both apply to the same run.
    if (meta.json) {
        ac3::cli::platform::set_stdio_binary();
    }

    // A single peek() disambiguates without disturbing the stream position
    // for either downstream reader: AC-3/E-AC-3's syncword starts 0x0B77,
    // AC-4's Annex G sync_word starts 0xAC40/0xAC41 - the first byte alone
    // (0x0B vs 0xAC) already decides it, and peek() works the same way on a
    // real file and on a pipe (std::cin), unlike a seek. ac3::io::
    // AccessUnitReader below is unaffected either way - this dispatch has
    // to happen before it, since it is hardwired to AC-3/E-AC-3 framing.
    if (in.peek() == 0xAC) {
        return run_probe_ac4(in_path, in, meta, container);
    }

    // The JSON document is written as the walk produces it - the frames array
    // first, streamed, then the stream summary, which is only complete once
    // every unit has been seen. Object member order carries no meaning in
    // JSON, so this costs a consumer nothing and is what lets a per-frame
    // dump of an arbitrarily long stream run in constant memory. See
    // docs/forge/cli/commands.md, which states the ordering as part of the
    // contract so nothing comes to depend on the opposite.
    JsonWriter json{stdout};
    if (meta.json) {
        json.begin_object();
        json.member("schema", "ac3forge.probe/1");
        json.member("generator", ac3::version_full);
        json.member("file", in_path);
        // null for a bare stream: the schema's members are never omitted.
        write_container(json, container);
        if (detail != Detail::kNone) {
            json.key("access_units");
            json.begin_array();
        }
    }

    io::ProbeOptions options;
    options.detail = detail != Detail::kNone;
    // ac3::signing lives in its own library and ac3::forge neither links nor
    // should link it, so the question is passed in rather than asked there -
    // see ProbeOptions::authenticity. No key is involved: whether a frame
    // carries a tag is answerable without one, and only whether that tag is
    // VALID is not (that is 'decode verify-objects').
    options.authenticity = [](std::span<const std::byte> frame) {
        return ac3::signing::has_authenticity_tag(frame);
    };
    if (detail != Detail::kNone) {
        options.on_access_unit = [&meta, &json, &detail](const io::ProbeAccessUnit& unit) {
            if (meta.json) {
                write_access_unit(json, unit, detail);
            } else {
                print_access_unit(unit, detail);
            }
        };
    }

    io::Prober prober{std::move(options)};
    io::AccessUnitReader reader{in};
    while (true) {
        const auto unit = reader.next();
        if (!unit) {
            fmt::println(stderr, "error: {} at byte {}", ac3::io::describe(unit.error()),
                         reader.byte_offset());
            return 1;
        }
        if (unit->empty()) {
            break;
        }
        if (const auto pushed = prober.push(*unit); !pushed) {
            fmt::println(stderr, "error: {} at byte {}", ac3::io::describe(pushed.error()),
                         reader.byte_offset());
            return 1;
        }
    }

    const auto report = prober.report();
    if (report.access_units == 0) {
        fmt::println(stderr, "error: {}", ac3::io::describe(io::ScanError::kEmpty));
        return 1;
    }
    if (meta.json) {
        if (detail != Detail::kNone) {
            json.end_array();
        }
        write_stream(json, report);
        json.end_object();
        json.finish();
    } else if (detail == Detail::kNone) {
        print_table(in_path, report, container);
    } else {
        // The dump has already gone out unit by unit; the summary follows it,
        // in the same order the JSON form puts them.
        fmt::println("");
        print_table(in_path, report, container);
    }

    // A stream that fails its own CRCs, or that this decoder cannot parse, is
    // still fully described above - but the exit code says so, the same way
    // 'qc' reports a measurement and gates on it separately. A CI step can
    // therefore use `ac3cli probe` as a check without reading its output.
    return report.crc_failures > 0 || report.parse_failures > 0 ? 1 : 0;
}

}  // namespace ac3cli::commands

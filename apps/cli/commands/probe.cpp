#include "probe.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <print>
#include <span>
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
#include "ac3/emdf/emdf.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/probe.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/signing/emdf_atmos_signer.hpp"
#include "ac3/version.hpp"

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

std::string_view strmtyp_token(ac3::eac3::StreamType type) {
    switch (type) {
        case ac3::eac3::StreamType::kIndependent: return "independent";
        case ac3::eac3::StreamType::kDependent: return "dependent";
        case ac3::eac3::StreamType::kConvertible: return "convertible";
        case ac3::eac3::StreamType::kReserved: break;
    }
    return "reserved";
}

// A/52 Table 5.7. bsmod's meaning additionally depends on acmod for one
// value - 0x7 is "voice over" everywhere except acmod 1/0, where it is
// "karaoke" - so the pair is what names it, not bsmod alone.
std::string_view bsmod_label(int bsmod, ac3::Acmod acmod) {
    switch (bsmod) {
        case 0: return "complete main";
        case 1: return "music and effects";
        case 2: return "visually impaired";
        case 3: return "hearing impaired";
        case 4: return "dialogue";
        case 5: return "commentary";
        case 6: return "emergency";
        case 7: return acmod == ac3::Acmod::k1_0 ? "karaoke" : "voice over";
        default: break;
    }
    return "reserved";
}

std::string_view exp_strategy_token(ac3::ExpStrategy strategy) {
    switch (strategy) {
        case ac3::ExpStrategy::kReuse: return "reuse";
        case ac3::ExpStrategy::kD15: return "D15";
        case ac3::ExpStrategy::kD25: return "D25";
        case ac3::ExpStrategy::kD45: return "D45";
    }
    return "reuse";
}

// TS 103 420 Table 55 names two of Table H.2.3's reserved payload ids; the
// rest are EMDF's own and are reported as bare numbers.
std::string_view emdf_payload_label(int id) {
    if (id == ac3::emdf::kPayloadIdOamd) {
        return "OAMD";
    }
    if (id == ac3::emdf::kPayloadIdJoc) {
        return "JOC";
    }
    return "";
}

// The bed a §5.5 program describes, as the channel names its assignment bits
// stand for - "5.1", "5.1.4", or "none" for a program that is dynamic objects
// alone. Built from the object count rather than from a table of layout names
// because a bed instance is a bit mask, not one of a fixed set.
std::string bed_label(const ac3::oba::Program& program) {
    if (program.dynamic_only) {
        return program.lfe ? "LFE only" : "none";
    }
    return std::format("{} channel(s)", ac3::oba::bed::channel_count(program.bed));
}

// dialnorm is transmitted as 1..31 meaning -1..-31 dB LKFS (§5.4.2.8); 0 is
// reserved. Reporting the dB is what every other tool shows and what a
// delivery spec is written in, so that is what both output forms carry - see
// docs/cli/commands.md, which documents the JSON field as dB for exactly this
// reason.
int dialnorm_db(int code) { return -code; }

// --- human-readable table --------------------------------------------------

void print_range(std::string_view label, const io::MinMax& range, std::string_view unit) {
    if (!range.seen) {
        std::println("{:<16}absent", label);
        return;
    }
    if (range.constant()) {
        std::println("{:<16}{}{}", label, range.min, unit);
        return;
    }
    std::println("{:<16}{}{} .. {}{}", label, range.min, unit, range.max, unit);
}

void print_table(std::string_view path, const io::ProbeReport& report) {
    std::println("{:<16}{}", "file", path);
    std::println("{:<16}{} (bsid {})", "codec", codec_label(report.kind), report.bsid);
    std::println("{:<16}{} Hz{}", "sample rate", ac3::sample_rate_hz(report.sample_rate),
                 report.reduced_rate ? " (fscod2 reduced rate)" : "");
    std::println("{:<16}{} ({})", "bsmod", report.bsmod,
                 bsmod_label(report.bsmod, report.acmod));
    std::println("{:<16}{} (acmod {}, lfeon {})", "layout",
                 ac3::analysis::layout_name(report.acmod, report.lfe),
                 static_cast<int>(report.acmod), report.lfe ? 1 : 0);
    if (report.layout.count > 0) {
        std::string locations;
        for (const auto location : report.layout) {
            locations += locations.empty() ? "" : " ";
            locations += ac3::eac3::chanmap::name(location);
        }
        std::println("{:<16}{} channel(s): {}", "renders", report.rendered_channels, locations);
    } else {
        std::println("{:<16}{} channel(s), no Table E2.5 layout (dual mono)", "renders",
                     report.rendered_channels);
    }
    std::println("{:<16}{} per syncframe (numblkscod {})", "blocks",
                 report.kind == io::StreamKind::kAc3
                     ? ac3::kBlocksPerFrame
                     : ac3::eac3::blocks_per_syncframe(report.numblkscod),
                 report.numblkscod);

    std::println("{:<16}{} per access unit", "substreams", report.substreams_per_unit);
    for (const auto& sub : report.substreams) {
        std::string chanmap = "-";
        if (sub.chanmap) {
            chanmap = std::format("chanmap 0x{:04x}", *sub.chanmap);
        }
        std::println("  {:<14}{} id {}, {}, {} syncframe(s), {}", "",
                     strmtyp_token(sub.strmtyp), sub.substreamid,
                     ac3::analysis::layout_name(sub.acmod, sub.lfe), sub.syncframes, chanmap);
    }

    std::println("{:<16}{} ({} syncframe(s)), {} bytes", "access units", report.access_units,
                 report.syncframes, report.bytes);
    std::println("{:<16}{:.3f} s", "duration", report.duration_seconds);
    if (report.nominal_bitrate_kbps) {
        std::println("{:<16}{:.1f} kbit/s measured, {} kbit/s declared (frmsizecod)", "bit rate",
                     report.bitrate_kbps, *report.nominal_bitrate_kbps);
    } else {
        std::println("{:<16}{:.1f} kbit/s measured", "bit rate", report.bitrate_kbps);
    }
    std::println("{:<16}{} ({} .. {} bytes per access unit)", "rate control",
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

    if (report.emdf_payload_ids.empty()) {
        std::println("{:<16}none", "EMDF");
    } else {
        std::string ids;
        for (const int id : report.emdf_payload_ids) {
            ids += ids.empty() ? "" : ", ";
            const auto label = emdf_payload_label(id);
            ids += label.empty() ? std::format("{}", id) : std::format("{} ({})", id, label);
        }
        std::println("{:<16}payload id(s) {}", "EMDF", ids);
    }
    if (report.program) {
        std::println("{:<16}{} object(s): bed {}, {} dynamic, in {} frame(s)", "object audio",
                     ac3::oba::object_count(*report.program), bed_label(*report.program),
                     report.program->dynamic_objects, report.object_frames);
    } else if (report.oba_complexity_index) {
        std::println("{:<16}addbsi marker only, no OAMD payload parsed", "object audio");
    } else {
        std::println("{:<16}none", "object audio");
    }
    if (report.oba_complexity_index) {
        std::println("{:<16}{}", "complexity", *report.oba_complexity_index);
    }
    std::println("{:<16}{}", "JOC", report.joc ? "present" : "absent");
    std::println("{:<16}{}", "authenticity",
                 report.authenticity_tagged_frames > 0
                     ? std::format("tag in {} of {} syncframe(s)",
                                   report.authenticity_tagged_frames, report.syncframes)
                     : std::string{"no tag"});

    std::println("{:<16}{} of {} syncframe(s) valid", "CRC",
                 report.syncframes - report.crc_failures, report.syncframes);
    if (report.parse_failures > 0) {
        std::println("{:<16}{} syncframe(s) refused by the parser{}", "parse errors",
                     report.parse_failures,
                     report.first_parse_error
                         ? std::format(" (first: {})", ac3::describe(*report.first_parse_error))
                         : std::string{});
    }

    const auto& tools = report.tools;
    if (tools.blocks == 0) {
        std::println("{:<16}no block was parsed", "tools");
        return;
    }
    const auto usage = [&](std::string_view name, std::uint64_t count) {
        if (count > 0) {
            std::println("  {:<14}{} of {} block(s)", name, count, tools.blocks);
        }
    };
    std::println("{:<16}{} block(s) parsed", "tools", tools.blocks);
    usage("coupling", tools.coupling);
    usage("enh coupling", tools.enhanced_coupling);
    usage("spx", tools.spectral_extension);
    usage("block switch", tools.block_switch);
    usage("dither", tools.dither);
    usage("rematrix", tools.rematrixing);
    usage("delta ba", tools.delta_bit_alloc);
    usage("skip field", tools.skip_field);
    if (tools.aht_frames > 0) {
        std::println("  {:<14}{} of {} syncframe(s)", "aht", tools.aht_frames,
                     report.syncframes);
    }
    if (tools.transient_prenoise_frames > 0) {
        std::println("  {:<14}{} of {} syncframe(s)", "tpnp", tools.transient_prenoise_frames,
                     report.syncframes);
    }
    std::println("  {:<14}reuse {}, D15 {}, D25 {}, D45 {}", "exponents", tools.exp_strategy[0],
                 tools.exp_strategy[1], tools.exp_strategy[2], tools.exp_strategy[3]);
}

// --- per-frame dump, human-readable ----------------------------------------

void print_access_unit(const io::ProbeAccessUnit& unit, Detail detail) {
    std::println("");
    std::println("access unit {} @ {} ({} bytes, t={:.4f}s)", unit.index, unit.byte_offset,
                 unit.bytes, unit.start_seconds);
    for (const auto& frame : unit.syncframes) {
        std::println("  {} id {} @ {}: {} bytes, {}, {}, dialnorm {} dB{}{}",
                     strmtyp_token(frame.header.strmtyp), frame.header.substreamid,
                     frame.byte_offset, frame.header.bytes,
                     ac3::analysis::layout_name(frame.header.acmod, frame.header.lfe),
                     frame.crc_valid ? "crc ok" : "CRC BAD",
                     dialnorm_db(frame.header.dialnorm),
                     frame.header.compr ? std::format(", compr {}", *frame.header.compr)
                                        : std::string{},
                     frame.authenticity_tag ? ", signed" : "");
        if (frame.parse_error) {
            std::println("    parse error: {}", ac3::describe(*frame.parse_error));
        }
        if (frame.objects) {
            std::println("    objects: {} total, {} dynamic, bed {}",
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
                std::println("    blk {}: not reached", index);
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
                strategies += std::format(
                    " cpl:{}", exp_strategy_token(block.exp_strategy[ac3::kCouplingSyntaxStream]));
            }
            std::println("    blk {}: {:<28} exp [{}]", index,
                         tools.empty() ? "-" : tools, strategies);
        }
    }
}

// --- JSON ------------------------------------------------------------------

void write_range(JsonWriter& json, std::string_view name, const io::MinMax& range,
                 bool negate) {
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
    if (frame.header.compr) {
        json.member("compr", static_cast<std::int64_t>(*frame.header.compr));
    } else {
        json.member_null("compr");
    }
    if (frame.header.chanmap) {
        json.member("chanmap", static_cast<std::int64_t>(*frame.header.chanmap));
    } else {
        json.member_null("chanmap");
    }
    json.member("crc_valid", frame.crc_valid);
    json.member("authenticity_tag", frame.authenticity_tag);
    if (frame.parse_error) {
        json.member("parse_error", ac3::describe(*frame.parse_error));
    } else {
        json.member_null("parse_error");
    }
    if (frame.objects) {
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

void write_stream(JsonWriter& json, const io::ProbeReport& report) {
    json.key("stream");
    json.begin_object();
    json.member("codec", codec_token(report.kind));
    json.member("bsid", static_cast<std::int64_t>(report.bsid));
    json.member("bsmod", static_cast<std::int64_t>(report.bsmod));
    json.member("bsmod_label", bsmod_label(report.bsmod, report.acmod));
    json.member("sample_rate_hz",
                static_cast<std::int64_t>(ac3::sample_rate_hz(report.sample_rate)));
    json.member("reduced_rate", report.reduced_rate);
    json.member("acmod", static_cast<std::int64_t>(report.acmod));
    json.member("lfeon", report.lfe);
    json.member("layout_label", ac3::analysis::layout_name(report.acmod, report.lfe));
    json.member("numblkscod", static_cast<std::int64_t>(report.numblkscod));
    json.member("blocks_per_syncframe",
                static_cast<std::int64_t>(report.kind == io::StreamKind::kAc3
                                              ? ac3::kBlocksPerFrame
                                              : ac3::eac3::blocks_per_syncframe(
                                                    report.numblkscod)));
    json.member("coded_channels", static_cast<std::int64_t>(report.coded_channels));
    json.member("rendered_channels", static_cast<std::int64_t>(report.rendered_channels));
    json.key("layout");
    json.begin_array();
    for (const auto location : report.layout) {
        json.value(ac3::eac3::chanmap::name(location));
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
        if (sub.chanmap) {
            json.member("chanmap", static_cast<std::int64_t>(*sub.chanmap));
        } else {
            json.member_null("chanmap");
        }
        json.member("syncframes", static_cast<std::uint64_t>(sub.syncframes));
        json.end_object();
    }
    json.end_array();
    json.member("substreams_per_access_unit",
                static_cast<std::uint64_t>(report.substreams_per_unit));

    json.member("access_units", static_cast<std::uint64_t>(report.access_units));
    json.member("syncframes", static_cast<std::uint64_t>(report.syncframes));
    json.member("bytes", static_cast<std::uint64_t>(report.bytes));
    json.member("duration_seconds", report.duration_seconds, 6);
    json.member("bitrate_kbps", report.bitrate_kbps, 3);
    if (report.nominal_bitrate_kbps) {
        json.member("nominal_bitrate_kbps",
                    static_cast<std::int64_t>(*report.nominal_bitrate_kbps));
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
    json.end_object();

    json.key("objects");
    json.begin_object();
    if (report.oba_complexity_index) {
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
    if (report.program) {
        json.member("total", static_cast<std::int64_t>(ac3::oba::object_count(*report.program)));
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
        json.member("lfe", ac3::oba::has_lfe(*report.program));
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
    json.member("tagged_syncframes",
                static_cast<std::uint64_t>(report.authenticity_tagged_frames));
    json.member("present", report.authenticity_tagged_frames > 0);
    json.end_object();

    json.key("integrity");
    json.begin_object();
    json.member("crc_valid",
                static_cast<std::uint64_t>(report.syncframes - report.crc_failures));
    json.member("crc_failures", static_cast<std::uint64_t>(report.crc_failures));
    json.member("parse_failures", static_cast<std::uint64_t>(report.parse_failures));
    if (report.first_parse_error) {
        json.member("first_parse_error", ac3::describe(*report.first_parse_error));
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

}  // namespace

int run_probe(std::string_view in_path, const Options& meta) {
    Detail detail = Detail::kNone;
    if (meta.detail) {
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
            std::println(stderr, "error: cannot open {}", in_path);
            return 1;
        }
    }
    std::istream& in = is_stdio_path(in_path) ? std::cin : file;

    // The JSON document is written as the walk produces it - the frames array
    // first, streamed, then the stream summary, which is only complete once
    // every unit has been seen. Object member order carries no meaning in
    // JSON, so this costs a consumer nothing and is what lets a per-frame
    // dump of an arbitrarily long stream run in constant memory. See
    // docs/cli/commands.md, which states the ordering as part of the
    // contract so nothing comes to depend on the opposite.
    JsonWriter json{stdout};
    if (meta.json) {
        json.begin_object();
        json.member("schema", "ac3forge.probe/1");
        json.member("generator", ac3::version_full);
        json.member("file", in_path);
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
        options.on_access_unit = [&](const io::ProbeAccessUnit& unit) {
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
            std::println(stderr, "error: {} at byte {}", ac3::io::describe(unit.error()),
                         reader.byte_offset());
            return 1;
        }
        if (unit->empty()) {
            break;
        }
        if (const auto pushed = prober.push(*unit); !pushed) {
            std::println(stderr, "error: {} at byte {}", ac3::io::describe(pushed.error()),
                         reader.byte_offset());
            return 1;
        }
    }

    const auto report = prober.report();
    if (report.access_units == 0) {
        std::println(stderr, "error: {}", ac3::io::describe(io::ScanError::kEmpty));
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
        print_table(in_path, report);
    } else {
        // The dump has already gone out unit by unit; the summary follows it,
        // in the same order the JSON form puts them.
        std::println("");
        print_table(in_path, report);
    }

    // A stream that fails its own CRCs, or that this decoder cannot parse, is
    // still fully described above - but the exit code says so, the same way
    // 'qc' reports a measurement and gates on it separately. A CI step can
    // therefore use `ac3cli probe` as a check without reading its output.
    return report.crc_failures > 0 || report.parse_failures > 0 ? 1 : 0;
}

}  // namespace ac3cli::commands

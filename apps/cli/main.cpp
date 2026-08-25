#include <array>
#include <charconv>
#include <cstddef>
#include <exception>
#include <cstdint>
#include <cstdio>
#include <fmt/base.h>
#include <fmt/format.h>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/meta/qc.hpp"
#include "ac3/audio/audio_backend.hpp"
#include "ac3/version.hpp"
#include "adm/atmos_adm.hpp"
#include "commands/analysis.hpp"
#include "commands/atmos.hpp"
#include "commands/audio_io.hpp"
#include "commands/containers.hpp"
#include "commands/decode.hpp"
#include "commands/encode.hpp"
#include "commands/live_audio.hpp"
#include "commands/probe.hpp"
#include "commands/stream_tools.hpp"
#include "commands/synth.hpp"
#include "exit_codes.hpp"
#include "support.hpp"
#include "usage.hpp"

namespace {

namespace plan = ac3::plan;

using namespace ac3cli;
using namespace ac3cli::commands;

// ---------------------------------------------------------------------------
// The command table. Every command is one row: its name, how many positional
// arguments it needs, the argument spec the usage text prints, and the code
// that runs it.
//
// It replaces a chain of `if (command == ...)` comparisons, each of which
// repeated `args.size() > N ? parse(args[N]) : default` for every parameter.
// That repetition is where this file kept going wrong: consolidating six
// parallel branches turned up SIX argv faults of exactly one shape - an entry
// counting from the wrong base, or reading a slot it had not checked. Two
// would have written output to a file named after the duration. None was
// visible in a build or a unit test, because the indices are only wrong
// relative to a convention nothing states in one place.
//
// Here the convention is stated once: args[0] is the command, so args[1] is
// the first parameter, and min_args is checked before any handler runs.
// usage.hpp's print_usage()/print_command_help() are generated from the
// same rows, so the help cannot drift from what dispatch accepts - it
// already had, with eac3-silence and eac3-sine missing from it entirely.
// ---------------------------------------------------------------------------

struct Args {
    std::span<char* const> a;
    const Options& meta;
    bool couple;

    [[nodiscard]] std::string_view str(std::size_t i, std::string_view fallback = {}) const {
        return i < a.size() ? std::string_view{a[i]} : fallback;
    }
    [[nodiscard]] std::uint32_t u32(std::size_t i, std::uint32_t fallback) const {
        return i < a.size() ? parse_u32_or(a[i], fallback) : fallback;
    }
    // Signed, unlike u32: routing a negative token through parse_u32_or (which
    // parses unsigned) always fails and silently returns 0 rather than
    // `fallback` - the wrong answer for the sentinel values several commands
    // read as "unset" or "default" (e.g. play's device index). from_chars
    // for a signed int accepts the leading '-' directly, so this parses
    // the token itself instead of bouncing through the unsigned path.
    [[nodiscard]] int i32(std::size_t i, int fallback) const {
        if (i >= a.size()) {
            return fallback;
        }
        const std::string_view text{a[i]};
        int value = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        return ec == std::errc{} && ptr == text.data() + text.size() ? value : fallback;
    }
    // Every positional argument from index i onward, for the one command
    // whose argument list is variadic ('cat' joins as many inputs as it is
    // given). Returned as string_views over argv, which outlives the call.
    [[nodiscard]] std::vector<std::string_view> tail(std::size_t i) const {
        std::vector<std::string_view> out;
        for (; i < a.size(); ++i) {
            out.emplace_back(a[i]);
        }
        return out;
    }
};

// What a command needs beyond plain file I/O to run at all in THIS build. Most commands need
// nothing. Several need the machine's audio hardware; one (atmos-adm) needs a library that is not
// part of every build - either way, unmet() below answers with the same {available, reason} shape
// (ac3::audio::Capability), so dispatch and the usage listing treat both kinds of "not here"
// identically.
//
// This is a column in the table rather than a check inside each handler for
// the same reason min_args is: stated once, beside the command it describes,
// and read by both dispatch and the usage text so the two cannot disagree
// about which commands exist here.
//
// 'live' needs kCapture, not a new combined category: capture is the one
// hard requirement (no capture endpoint, no session at all), while its
// monitor/passthrough legs are soft - unavailable or refused there degrades
// to a warning and a file-only session, exactly like plugging into a
// receiver that says no. Only 'monitor' (which does nothing BUT play back)
// needs kMonitor as a hard gate, the same way 'play'/'outputs' need
// kPassthrough.
//
// kAdm ('atmos-adm', roadmap B1 phase 3): unlike the three audio ones, this is not a hardware
// question - it is whether ac3adm::ac3adm/ac3::admbridge were linked into this build at all
// (AC3FORGE_BUILD_ADM, default OFF - see the root CMakeLists.txt's own option()). Answered the
// same way regardless: adm/atmos_adm.hpp's ac3cli::adm_capability(), backed by exactly one of
// adm/enabled/atmos_adm.cpp or adm/disabled/atmos_adm.cpp (see run_atmos_adm's own comment for
// why a CMake-selected file, not a preprocessor conditional, decides this).
enum class Needs : std::uint8_t { kNothing, kCapture, kPassthrough, kMonitor, kAdm };

// The unmet requirement, or nullptr when this build/platform can satisfy it.
//
// Note what kCapture/kPassthrough/kMonitor are not: an OS test. main.cpp never asks whether it is
// on Windows - it asks the one translation unit CMake compiled from
// src/audio/src/backend/<os>/ what that backend can do, and prints the answer
// that unit supplied. The day a Unix capture backend lands, capture flips to
// available in that file alone and 'devices' and 'record' start working here
// with no change to this file. kAdm asks the analogous question of
// adm/{enabled,disabled}/atmos_adm.cpp instead - a library-linked-or-not fact rather than an
// OS one, answered by the identical "ask the compiled-in file" shape.
const ac3::audio::Capability* unmet(Needs needs) {
    const auto& backend = ac3::audio::audio_backend();
    switch (needs) {
        case Needs::kNothing: return nullptr;
        case Needs::kCapture: return backend.capture.available ? nullptr : &backend.capture;
        case Needs::kPassthrough:
            return backend.passthrough.available ? nullptr : &backend.passthrough;
        case Needs::kMonitor: return backend.monitor.available ? nullptr : &backend.monitor;
        case Needs::kAdm: {
            const auto& adm = ac3cli::adm_capability();
            return adm.available ? nullptr : &adm;
        }
    }
    return nullptr;
}

struct Command {
    std::string_view name;
    std::size_t min_args;  // positional count INCLUDING the command itself
    std::string_view spec;
    std::string_view note;
    // Which usage.hpp grammar sections this command's own arguments and
    // options actually reach - the column that turns the old "print all ~130
    // lines of prose on any argument error" into `ac3cli help <command>`
    // printing just this row and just those paragraphs. Stated beside the
    // command it describes, for the same reason min_args and needs are.
    std::uint32_t topics;
    Needs needs;
    int (*run)(const Args&);
};

// The three commands that print rather than encode: the help itself, the
// generated man page and the generated shell completions. Declared here and
// defined below kCommands, because all three read that table - a forward
// declaration is cheaper than a self-referential constexpr initializer and
// makes the dependency obvious in the direction it actually runs.
int run_help(const Args& x);
int run_man();
int run_completions(std::string_view shell);

// 38 commands, always - including atmos-adm, whether or not AC3FORGE_BUILD_ADM linked
// ac3adm::ac3adm/ac3::admbridge into this particular build (see Needs::kAdm/unmet() above and
// run_atmos_adm's own comment): a command this build cannot run is listed with Needs gating it,
// never sized out of the table entirely - the identical "listed, not hidden" treatment
// kCapture/kPassthrough/kMonitor commands already get (see print_usage()'s own comment below on
// why hiding would be a lie about a command that exists and would work elsewhere).
constexpr std::array<Command, 38> kCommands{{
    {"silence", 2, "<out.ac3> [seconds] [bitrate_kbps]", "", topic::kNone,
     Needs::kNothing,
     [](const Args& x) { return run_silence(x.str(1), x.u32(2, 5), x.u32(3, 192)); }},
    {"sine", 2, "<out.ac3> [seconds] [bitrate_kbps] [freq_hz] [amp_pct] [layout]", "",
     topic::kLayout | topic::kMeta,
     Needs::kNothing,
     [](const Args& x) {
         return run_sine(x.str(1), x.u32(2, 5), x.u32(3, 192), x.u32(4, 1000), x.u32(5, 50),
                         x.str(6, "stereo"), x.couple, x.meta);
     }},
    {"orbit", 2, "<out.ac3> [seconds] [bitrate_kbps] [orbit_seconds]", "", topic::kMeta,
     Needs::kNothing,
     [](const Args& x) {
         return run_orbit(x.str(1), x.u32(2, 8), x.u32(3, 448), x.u32(4, 4), x.meta);
     }},
    {"atmos", 2, "<out.ec3> [seconds] [bitrate_kbps] [objects] [orbit_seconds] [mode]", "",
     topic::kAtmos | topic::kMeta | topic::kObjects,
     Needs::kNothing,
     [](const Args& x) {
         return run_atmos(x.str(1), x.u32(2, 8), x.u32(3, 448), x.u32(4, 4), x.u32(5, 6),
                          x.str(6, "objects"), x.meta);
     }},
    {"atmos-path", 3, "<out.ec3> <paths.txt> [seconds] [bitrate_kbps] [objects]",
     "objects driven by an authored scene file instead of the built-in orbit",
     topic::kAtmos | topic::kPaths | topic::kMeta | topic::kObjects,
     Needs::kNothing,
     [](const Args& x) {
         return run_atmos_path(x.str(1), x.str(2), x.u32(3, 8), x.u32(4, 448), x.u32(5, 0),
                               x.meta);
     }},
    {"atmos-encode", 3, "<in.wav> <out.ec3> [bitrate_kbps] [objects] [paths.txt]",
     "every source channel as an object; optional: authored per-object motion from a scene "
     "file (same formats as atmos-path), objects it doesn't mention keep their default placement",
     topic::kStdio | topic::kAtmos | topic::kPaths | topic::kMulti | topic::kMeta | topic::kObjects,
     Needs::kNothing,
     [](const Args& x) {
         return run_atmos_encode(x.str(1), x.str(2), x.u32(3, 448), x.u32(4, 0), x.meta,
                                 x.str(5));
     }},
    {"atmos-adm", 3, "<in.adm.wav> <out.ec3> [bitrate_kbps] [programme_id]",
     "a real ADM BWF master (BS.2076-2 ADM XML + BW64/RF64, roadmap B1) straight to DD+ JOC "
     "E-AC-3; every bed/object channel the resolved audioProgramme names becomes an AtmosEncoder "
     "object, driven by the file's own authored automation - no scene file needed. Only in "
     "builds with -DAC3FORGE_BUILD_ADM=ON",
     topic::kAtmos | topic::kMeta | topic::kObjects,
     Needs::kAdm,
     [](const Args& x) {
         return run_atmos_adm(x.str(1), x.str(2), x.u32(3, 448), x.meta, x.str(4));
     }},
    {"strip-objects", 3, "<in.ec3> <out.ec3>",
     "remove the JOC/OAMD object layer from a DD+ stream, leaving a bit-identical 5.1 bed",
     topic::kStdio | topic::kMeta,
     Needs::kNothing,
     [](const Args& x) { return run_strip_objects(x.str(1), x.str(2), x.meta); }},
    {"record", 2, "<out.ac3|out.ec3> [seconds] [bitrate_kbps] [device_index]",
     "capture straight to a file; layout=/codec=/container= decide its shape",
     topic::kTake | topic::kLayout | topic::kMeta,
     Needs::kCapture,
     [](const Args& x) {
         return run_record(x.str(1), x.u32(2, 5), x.u32(3, 192), x.i32(4, 0), x.meta);
     }},
    {"live", 3,
     "<out.ac3|out.ec3> <capture_device> [seconds] [bitrate_kbps] [monitor_device] "
     "[passthrough_device] [mode]",
     "capture -> encode -> live monitor and/or passthrough", topic::kLive | topic::kTake | topic::kLayout | topic::kAtmos | topic::kMulti | topic::kMeta,
     Needs::kCapture,
     [](const Args& x) {
         return run_live(x.str(1), x.i32(2, 0), x.u32(3, 10), x.u32(4, 192), x.i32(5, -2),
                         x.i32(6, -2), x.str(7, "channels"), x.meta);
     }},
    {"encode", 3, "<in.wav> <out.ac3> [bitrate_kbps] [layout] [in2.wav]",
     "in2.wav: layout 1+1's Ch2, when Ch1 is a separate mono file; or use src=/map= "
     "for more than one source",
     topic::kStdio | topic::kLayout | topic::kMulti | topic::kMeta,
     Needs::kNothing,
     [](const Args& x) {
         return run_encode(x.str(1), x.str(2), x.u32(3, 192), x.couple, x.str(4), x.meta,
                           x.str(5));
     }},
    {"eac3-silence", 2, "<out.ec3> [seconds] [bitrate_kbps] [layout]", "", topic::kLayout | topic::kMeta,
     Needs::kNothing,
     [](const Args& x) {
         return run_eac3_silence(x.str(1), x.u32(2, 5), x.u32(3, 192), x.str(4, "stereo"),
                                 x.meta);
     }},
    {"eac3-sine", 2,
     "<out.ec3> [seconds] [bitrate_kbps] [freq_hz] [amp_pct] [layout]", "", topic::kLayout | topic::kMeta,
     Needs::kNothing,
     [](const Args& x) {
         return run_eac3_sine(x.str(1), x.u32(2, 5), x.u32(3, 192), x.u32(4, 1000),
                              x.u32(5, 50), x.str(6, "stereo"), x.meta);
     }},
    {"eac3-encode", 3,
     "<in.wav> <out.ec3> [bitrate_kbps] [tools] [layout] [vbr] [in2.wav]",
     "in2.wav: layout 1+1's Ch2, when Ch1 is a separate mono file; or use src=/map= "
     "for more than one source. programme2= is a different thing entirely - a second, "
     "independent E-AC-3 substream (its own layout/bitrate/dialnorm via "
     "programme2-layout=/-bitrate=/-dialnorm=), not another channel of this one",
     topic::kStdio | topic::kLayout | topic::kTools | topic::kVbr | topic::kMulti | topic::kMeta,
     Needs::kNothing,
     [](const Args& x) {
         return run_eac3_encode(x.str(1), x.str(2), x.u32(3, 192), x.str(4, "none"), x.str(5),
                                x.str(6, "off"), x.meta, x.str(7));
     }},
    {"decode", 3, "<in.ac3|in.ec3> <out.wav> [objects_dir]",
     "AC-3 or E-AC-3; bsid decides. objects_dir (E-AC-3 Atmos only): export each "
     "JOC-reconstructed object as its own object_NN.wav there",
     topic::kStdio | topic::kDecode | topic::kObjects,
     Needs::kNothing,
     [](const Args& x) { return run_decode(x.str(1), x.str(2), x.meta, x.str(3)); }},
    {"probe", 2, "<in.ac3|in.ec3> [json=1] [detail=frames|blocks]",
     "what the stream declares: layout, substreams, rates, metadata ranges, object layer, "
     "tool usage and per-frame CRC - as a table, or as a documented JSON contract",
     topic::kStdio | topic::kProbe,
     Needs::kNothing, [](const Args& x) { return run_probe(x.str(1), x.meta); }},
    {"transcode", 3, "<in.ac3|in.ec3> <out.ac3|out.ec3> [bitrate_kbps] [layout]",
     "decode and re-encode, carrying dialnorm, compr and the mix metadata across - the "
     "DD+-to-DD path for optical and AC-3-only HDMI sinks. The output codec comes from the "
     "output name's suffix, or from codec=",
     topic::kLayout | topic::kStreamTools,
     Needs::kNothing,
     [](const Args& x) {
         return run_transcode(x.str(1), x.str(2), x.u32(3, 448), x.str(4), x.meta);
     }},
    {"metadata", 3, "<in.ac3|in.ec3> <out.ac3|out.ec3>",
     "rewrite dialnorm/compr/bsmod/dsurmod on an existing stream and re-stamp its CRCs; the "
     "audio is copied through untouched, not re-encoded",
     topic::kStreamTools,
     Needs::kNothing,
     [](const Args& x) { return run_metadata(x.str(1), x.str(2), x.meta); }},
    {"normalize", 3, "<in.ac3|in.ec3> <out.ac3|out.ec3>",
     "measure BS.1770-4 loudness and write the dialnorm it implies (ATSC A/85 §8), audio "
     "untouched",
     topic::kStreamTools,
     Needs::kNothing,
     [](const Args& x) { return run_normalize(x.str(1), x.str(2), x.meta); }},
    {"cut", 3, "<in.ac3|in.ec3> <out.ac3|out.ec3> [start_seconds] [duration_seconds]",
     "extract on access-unit boundaries; nothing is re-encoded", topic::kStreamTools,
     Needs::kNothing,
     [](const Args& x) { return run_cut(x.str(1), x.str(2), x.str(3), x.str(4)); }},
    {"cat", 4, "<out.ac3|out.ec3> <in1> <in2> [in3...]",
     "join streams end to end (output FIRST, since the input list is variadic); refuses "
     "inputs whose codec, rate, layout or substream shape differ",
     topic::kStreamTools,
     Needs::kNothing,
     [](const Args& x) {
         const auto inputs = x.tail(2);
         return run_cat(x.str(1), inputs);
     }},
    {"levels", 2, "<in.wav|in.ac3|in.ec3>", "per-channel peak/RMS report",
     topic::kProgramme,
     Needs::kNothing,
     [](const Args& x) { return run_levels(x.str(1), x.meta.programme); }},
    {"loudness", 2, "<in.wav>", "BS.1770-4 loudness -> dialnorm", topic::kNone,
     Needs::kNothing,
     [](const Args& x) { return run_loudness(x.str(1)); }},
    {"qc", 2, "<in.ac3|in.ec3> [preset=<name>|all] [layout=bed|rendered]",
     "bitstream-aware loudness QC: measured loudness vs. embedded dialnorm/compr, optional "
     "preset gate",
     topic::kQc | topic::kProgramme,
     Needs::kNothing, [](const Args& x) {
         return run_qc(x.str(1), x.meta.qc_preset, x.meta.qc_rendered_layout, x.meta.programme);
     }},
    {"spdif", 3, "<in.ac3> <out.wav>", "IEC 61937 wrap as playable PCM16 WAV", topic::kNone,
     Needs::kNothing,
     [](const Args& x) { return run_spdif(x.str(1), x.str(2)); }},
    {"unspdif", 3, "<in.wav|in.raw|-> <out.ac3|out.ec3|->",
     "the inverse: recover the elementary stream from IEC 61937 bursts, as captured from "
     "an S/PDIF or HDMI input or written by 'spdif'. '-' pipes either end",
     topic::kStdio,
     Needs::kNothing,
     [](const Args& x) { return run_unspdif(x.str(1), x.str(2), x.meta.keep_partial); }},
    {"mkv", 3, "<in.ac3|in.ec3> <out.mkv>", "wrap as a playable Matroska file", topic::kMkv,
     Needs::kNothing,
     [](const Args& x) { return run_mkv(x.str(1), x.str(2)); }},
    {"mp4", 3, "<in.ac3|in.ec3> <out.mp4>",
     "wrap as a playable MP4 with a spec-correct dac3/dec3 box", topic::kNone,
     Needs::kNothing,
     [](const Args& x) { return run_mp4(x.str(1), x.str(2)); }},
    {"fmp4", 3, "<in.ac3|in.ec3> <out_dir> [frames_per_fragment]",
     "fragmented MP4/CMAF + HLS/DASH manifests, ready for a packager; fallback-51 also writes "
     "an object-stripped 5.1 companion rendition",
     topic::kFmp4 | topic::kMeta,
     Needs::kNothing,
     [](const Args& x) { return run_fmp4(x.str(1), x.str(2), x.u32(3, 48), x.meta); }},
    {"ts", 3, "<in.ac3|in.ec3> <out.ts> [dvb|atsc]",
     "wrap as an MPEG-2 Transport Stream, DVB profile by default",
     topic::kTs | topic::kMeta,
     Needs::kNothing, [](const Args& x) { return run_ts(x.str(1), x.str(2), x.str(3, "dvb"), x.meta); }},
    {"demux", 3, "<in.mkv|in.mp4|in.ts> <out.ac3|out.ec3>",
     "the inverse of 'mkv': unwrap the elementary stream a container carries. The container is "
     "identified by its own magic bytes, not by the file name",
     topic::kNone,
     Needs::kNothing, [](const Args& x) { return run_demux(x.str(1), x.str(2)); }},
    {"devices", 1, "", "input and loopback capture endpoints", topic::kNone,
     Needs::kCapture,
     [](const Args&) { return run_devices(); }},
    {"outputs", 1, "", "render endpoints + AC-3/E-AC-3 passthrough support", topic::kNone,
     Needs::kPassthrough,
     [](const Args&) { return run_outputs(); }},
    {"play", 2, "<in.ac3|in.ec3> [device_index]",
     "exclusive-mode IEC 61937 passthrough; bsid decides AC-3 vs E-AC-3", topic::kNone,
     Needs::kPassthrough,
     // -1, not 0: run_play reads a negative index as "the default endpoint",
     // where 0 names the first one 'outputs' lists and demands passthrough of it.
     [](const Args& x) { return run_play(x.str(1), x.i32(2, -1)); }},
    {"monitor", 2, "<in.ac3|in.ec3> [device_index]",
     "decode and play on an ordinary (non-bitstreamed) output", topic::kDecode | topic::kObjects,
     Needs::kMonitor,
     [](const Args& x) { return run_monitor(x.str(1), x.i32(2, -1), x.meta); }},
    {"help", 1, "[<command>|exit-codes]",
     "one command's own arguments and grammars, not the whole manual", topic::kNone,
     Needs::kNothing, run_help},
    {"man", 1, "", "the generated groff man page, on stdout", topic::kNone, Needs::kNothing,
     [](const Args&) { return run_man(); }},
    {"completions", 2, "<bash|zsh|fish|powershell>",
     "the generated completion script for that shell, on stdout", topic::kNone, Needs::kNothing,
     [](const Args& x) { return run_completions(x.str(1)); }},
}};

// kCommands as usage.hpp sees it: no handler, no Needs, and this build's own
// answer to "can it run here" already resolved. Rebuilt on every call - 33
// rows of string_view, so there is nothing worth caching and nothing that can
// go stale between the table and what gets printed.
std::vector<CommandInfo> command_infos() {
    std::vector<CommandInfo> infos;
    infos.reserve(kCommands.size());
    for (const auto& c : kCommands) {
        const auto* missing = unmet(c.needs);
        infos.push_back(CommandInfo{.name = c.name,
                                    .spec = c.spec,
                                    .note = c.note,
                                    .topics = c.topics,
                                    .available = missing == nullptr,
                                    .unavailable_reason =
                                        missing != nullptr ? missing->reason : std::string_view{}});
    }
    return infos;
}

const Command* find_command(std::string_view name) {
    for (const auto& c : kCommands) {
        if (c.name == name) {
            return &c;
        }
    }
    return nullptr;
}

// `ac3cli help`, `ac3cli help <command>`, `ac3cli help exit-codes`.
int run_help(const Args& x) {
    const auto topic_name = x.str(1);
    if (topic_name.empty()) {
        print_usage(command_infos());
        return kExitOk;
    }
    if (topic_name == "exit-codes") {
        print_exit_codes();
        return kExitOk;
    }
    const auto infos = command_infos();
    for (const auto& info : infos) {
        if (info.name == topic_name) {
            print_command_help(info);
            return kExitOk;
        }
    }
    fmt::println(stderr, "error: unknown command '{}'", topic_name);
    print_command_index(infos);
    return kExitUsage;
}

int run_man() {
    print_man_page(command_infos());
    return kExitOk;
}

int run_completions(std::string_view shell) {
    return print_completions(shell, command_infos());
}

}  // namespace

int run_main(int argc, char** argv) {
    const std::span<char*> raw{argv, static_cast<std::size_t>(argc)};
    if (raw.size() > 1 &&
        (std::string_view{raw[1]} == "--version" || std::string_view{raw[1]} == "-v")) {
        fmt::println("{}", ac3::version_details());
        return kExitOk;
    }
    // Split the command line into positional arguments and metadata options. An
    // option is a key=value token or one of the bare flags, so the positional
    // arguments keep their places whether options are present or not, and
    // options may appear in any order.
    //
    // --help/-h are neither: they carry no '=' and are not option words, so
    // they would otherwise land among the positionals and be read as a file
    // name. They are lifted out here instead and answered before anything
    // else runs, so `ac3cli encode --help` works whether or not the rest of
    // the command line would have satisfied `encode`.
    std::vector<char*> args{};      // args[0] is the command
    std::vector<char*> options{};
    bool couple_flag = false;
    bool help_flag = false;
    for (std::size_t i = 1; i < raw.size(); ++i) {
        const std::string_view token{raw[i]};
        if (token == "--help" || token == "-h") {
            help_flag = true;
            continue;
        }
        const bool is_option = token.find('=') != std::string_view::npos ||
                               token == "couple" || token == "heavy" || token == "heavy2" ||
                               token == "mixmeta" || token == "sign-objects" ||
                               token == "verify-objects" || token == "verify" ||
                               token == "keep-partial" || token == "fast-mdct" ||
                               token == "fast-imdct" || token == "mix-lfe" ||
                               token == "fallback-51" || token == "annexd" ||
                               token == "infomdat" || token == "encinfo" ||
                               token == "langcod" || token == "langcod2" ||
                               token == "copyright" || token == "sourcefscod" ||
                               token == "quiet" || token == "verbose";
        if (token == "couple") {
            couple_flag = true;
        }
        (is_option ? options : args).push_back(raw[i]);
    }
    if (help_flag) {
        const auto infos = command_infos();
        for (const auto& info : infos) {
            if (!args.empty() && info.name == std::string_view{args[0]}) {
                print_command_help(info);
                return kExitOk;
            }
        }
        print_usage(infos);
        return kExitOk;
    }
    Options meta;
    if (!parse_options(options, meta, args.empty() ? std::string_view{} : std::string_view{args[0]})) {
        return kExitUsage;
    }
    // Before the first handler runs, so every status printer downstream sees
    // the same answer - including the ones inside support.cpp that have no
    // Options in scope at all.
    set_verbosity(meta.quiet, meta.verbose);
    if (args.empty()) {
        print_usage(command_infos());
        return kExitOk;
    }

    const std::string_view command{args[0]};
    if (const auto* c = find_command(command)) {
        if (args.size() < c->min_args) {
            // This command's own row and grammars, not the whole manual: the
            // one line the operator needs is the one they typed wrong, and
            // burying it under ~130 lines of prose about every other command
            // is what made the old behaviour worth replacing.
            fmt::println(stderr, "error: {} needs {}", c->name, c->spec);
            fmt::println(stderr, "  see 'ac3cli help {}'", c->name);
            return kExitUsage;
        }
        // Refuse before the handler runs, so a command that cannot work here
        // says why once, in the platform's own words, instead of failing
        // partway through with whatever error code the no-backend stub
        // happened to return. Nothing silently does nothing.
        if (const auto* missing = unmet(c->needs)) {
            fmt::println(stderr, "error: '{}' is unavailable on this platform: {}", c->name,
                         missing->reason);
            if (c->needs == Needs::kPassthrough) {
                // The one live-audio capability with a portable substitute:
                // same bursts, written to a file instead of an endpoint.
                fmt::println(stderr,
                             "  'ac3cli spdif <in.ac3> <out.wav>' wraps the same IEC 61937 "
                             "bursts into a WAV that any player will pass through untouched.");
            }
            return kExitUnavailable;
        }
        return c->run(Args{args, meta, couple_flag});
    }
    fmt::println(stderr, "error: unknown command '{}'", command);
    print_command_index(command_infos());
    return kExitUsage;
}

// run_main is std::expected-clean throughout; the one realistic exception
// source left is fmt::format/fmt::println itself (fmt::format_error), which
// nothing here catches internally. Left uncaught, that unwinds out of main
// and terminates - a crash with no exit code a script could act on rather
// than the ordinary "error: ..." this CLI otherwise always prints on
// failure. This is the one place that catches it. clang-tidy still flags
// main() itself: it cannot see past this try/catch to know the escape is
// caught, and reports the one path it cannot fully close by construction -
// the catch block's own fmt::println, whose fixed one-argument format string
// has no realistic way to throw. NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv) {
    try {
        return run_main(argc, argv);
    } catch (const std::exception& e) {
        fmt::println(stderr, "error: unhandled exception: {}", e.what());
        return kExitInternal;
    } catch (...) {
        fmt::println(stderr, "error: unhandled exception of unknown type");
        return kExitInternal;
    }
}

#include "usage.hpp"

#include <array>
#include <cstdint>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/encoder/assignment.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/meta/qc.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/version.hpp"
#include "exit_codes.hpp"

namespace ac3cli {

namespace plan = ac3::plan;

namespace {

// Every option token parse_options() accepts, in the order the help prints
// them, with the value grammar the completion scripts suggest. This is the
// list the man page's OPTIONS section and all four completion scripts are
// generated from.
//
// It is a second statement of what parse_options() accepts (that function is
// one long if-chain over `key`, with no list to read), so it can drift. Two
// things hold it: parse_options' own unknown-option path prints
// print_meta_usage(), which is generated from the same prose the entries here
// describe, and tests/cli's completion test asserts every bare (valueless)
// token below is actually accepted by a real invocation. A key= token cannot
// be checked that cheaply - its value grammar differs per option - so those
// are checked by eye against parse_options when either changes.
struct OptionToken {
    std::string_view spelling;  // "drc=" for a key=value option, "couple" for a bare flag
    std::string_view summary;
};

constexpr std::array<OptionToken, 30> kOptionTokens{{
    {"couple", "enable channel coupling wherever this command encodes"},
    {"heavy", "§7.7.2 heavy compression"},
    {"heavy2", "Ch2's own heavy compression (layout 1+1)"},
    {"mixmeta", "E-AC-3: emit the mixmdate group (Table E1.2)"},
    {"keep-partial", "keep a failed run's already-encoded frames as <name>.partial.<ext>"},
    {"sign-objects", "write a keyed EMDF object signature (needs signing-key=)"},
    {"verify-objects", "check each frame's EMDF object signature instead of just decoding"},
    {"fast-mdct", "names the default forward MDCT (the fast §7.9.4 path)"},
    {"fast-imdct", "names the default inverse MDCT (the fast §7.9.4 step 3)"},
    {"quiet", "no status output at all - errors and the payload only"},
    {"verbose", "print the stderr progress line whatever the run's length"},
    {"drc=", "§7.7.1 DRC profile (encode) or partial-compression scale (decode)"},
    {"drc2=", "Ch2's own DRC profile (layout 1+1)"},
    {"ceiling=", "heavy compression's peak ceiling, dBFS"},
    {"ceiling2=", "Ch2's heavy-compression peak ceiling, dBFS"},
    {"dialogue=", "where heavy compression puts dialogue, dBFS"},
    {"dialogue2=", "Ch2's heavy-compression dialogue target, dBFS"},
    {"dialnorm=", "auto, or 1..31 (§5.4.2.8)"},
    {"dialnorm2=", "Ch2's own dialnorm, auto or 1..31 (§5.4.2.16)"},
    {"cmixlev=", "-3, -4.5 or -6 (Table 5.9)"},
    {"surmixlev=", "-3, -6 or off (Table 5.10)"},
    {"lfemix=", "0..31 or off (§E2.3.1.11)"},
    {"dmixmod=", "ltrt, loro or none (Table D2.2)"},
    {"mode=", "performance (default) or reference - both transforms at once"},
    {"src=", "an additional input source; repeat for more than one"},
    {"map=", "where each source channel goes"},
    {"offset=", "<sourceIndex>:<seconds> leading silence for that source"},
    {"capture2=", "live: a second capture device, clock-conformed to the first"},
    {"container=", "record/live: raw, mkv, ts or spdif"},
    {"preset=", "qc: gate the measurement against a named delivery spec"},
}};

// The note column of the usage listing starts here; a row whose spec already
// runs past it gets one hand-placed space instead (see print_row).
constexpr std::size_t kNoteColumn = 62;

void print_row(const CommandInfo& c) {
    std::string line = std::format("  ac3cli {:<13}{}", c.name, c.spec);
    // A command the platform cannot run is listed, not hidden: hiding it
    // makes 'ac3cli play' answer "unknown command", which is a lie about a
    // command that exists and would work elsewhere. The note slot says so
    // instead, and the reasons follow once below rather than being repeated
    // on every affected row.
    const std::string_view note = c.available ? c.note : std::string_view{"UNAVAILABLE HERE"};
    if (!note.empty()) {
        // 'record' has a spec longer than the note column and no padding is
        // applied to a line already past the stop - so guarantee the
        // separating space by hand rather than letting the note run into the
        // last argument.
        if (line.size() >= kNoteColumn) {
            line += ' ';
        }
        line = std::format("{:<{}}({})", line, kNoteColumn, note);
    }
    std::println("{}", line);
}

void print_unavailable_reasons(std::span<const CommandInfo> commands) {
    std::vector<std::string_view> seen;
    for (const auto& c : commands) {
        if (c.available || c.unavailable_reason.empty()) {
            continue;
        }
        bool already = false;
        for (const auto& reason : seen) {
            already = already || reason == c.unavailable_reason;
        }
        if (!already) {
            seen.push_back(c.unavailable_reason);
        }
    }
    if (seen.empty()) {
        return;
    }
    std::println("");
    for (const auto& reason : seen) {
        std::println("UNAVAILABLE HERE — {}.", reason);
    }
    std::println("Everything else is file I/O and behaves identically on every platform;");
    std::println("'spdif' in particular reaches a receiver without any audio backend at all.");
}

void print_stdio_topic() {
    std::println("");
    std::println("'-' in place of <in.wav>, <out.ac3>, <out.ec3>, <in.ac3|in.ec3> or <out.wav>");
    std::println("       means stdin (an input path) or stdout (an output path) - encode,");
    std::println("       eac3-encode, atmos-encode and decode only. e.g.:");
    std::println("       ac3cli encode - - 448 couple < in.wav > out.ac3");
}

void print_live_topic() {
    std::println("");
    std::println("live monitor_device/passthrough_device: -2 (default) leaves that leg off,");
    std::println("       -1 is the default render endpoint, N picks one from 'outputs'.");
    std::println("       Either or both may run alongside the file this always writes.");
    std::println("live mode: 'channels' (default) encodes the captured channels as they are,");
    std::println("       onto layout= (stereo by default, anything up to 7.1.4); 'atmos' pans");
    std::println("       every captured channel into a 5.1 bed as its own object, moving it");
    std::println("       every frame the same way 'atmos' orbits its synthetic ones — the hook");
    std::println("       a real live position source drops into once one exists.");
    std::println("live capture2=<index>: the capture_device positional stays the session's");
    std::println("       clock master, paced exactly as it always has been; capture2= adds a");
    std::println("       second, independently-clocked device whose stream is resampled to");
    std::println("       track the master, with the measured drift printed at session end.");
    std::println("live downmix: when the layout or object mode needs E-AC-3 but the chosen");
    std::println("       passthrough endpoint only bitstreams AC-3, a parallel 5.1 AC-3 leg is");
    std::println("       encoded alongside the main stream and sent there, so the receiver");
    std::println("       hears a capped downmix instead of a refusal. The file always carries");
    std::println("       the full stream. downmix=off refuses instead, as it used to.");
    std::println("monitor/live --monitor play the 5.1 BED of an Atmos-mode stream: the decoder");
    std::println("       reads TS 103 420's object layer (OAMD/JOC) and reports an object count,");
    std::println("       but this path does not render or export objects, so this is what a");
    std::println("       legacy decoder hears, not unmixed objects.");
}

void print_take_topic() {
    std::println("");
    std::println("record/live container=: 'raw' (the default) writes the bare elementary");
    std::println("       stream; 'mkv' writes Matroska, 'ts' an MPEG-2 Transport Stream and");
    std::println("       'spdif' an IEC 61937 WAV carrier - all four written incrementally as");
    std::println("       the session runs, so a take survives a crash and its memory cost does");
    std::println("       not grow with its length. 'mkv'/'ts'/'spdif' remain the way to wrap an");
    std::println("       ALREADY-encoded file after the fact.");
    std::println("record/live layout=: the encoded layout, default stereo. Anything wider than");
    std::println("       AC-3 can carry promotes the stream to E-AC-3 on its own; codec=eac3");
    std::println("       forces E-AC-3 for a narrow layout too. A capture device with fewer");
    std::println("       channels than the layout leaves the rest silent.");
    std::println("record/live watchdog=<seconds>: how long the capture device may deliver");
    std::println("       nothing before the session stops as a failure rather than sitting");
    std::println("       there reading 'running' (default 3, 0 disables). Whatever was already");
    std::println("       written stays on disk.");
    std::println("live objects=<N>: the object-slot budget for mode=atmos - allocated once at");
    std::println("       session start (1..15; the bed's LFE is the 16th, TS 103 420 §8.3.2.2");
    std::println("       caps the total at 16), so a slot that is bound later does not change");
    std::println("       the stream's object count mid-session. Default: one slot per captured");
    std::println("       channel. map= binds capture channels to slots; an unbound slot is");
    std::println("       carried silent.");
}

void print_layout_topic() {
    std::println("");
    std::println("layout: {}", plan::layout_names(plan::Codec::kEac3));
    std::println("        AC-3 carries only {} — everything wider needs the dependent",
                 plan::layout_names(plan::Codec::kAc3));
    std::println("        substreams that only E-AC-3 has.");
    for (const auto& info : plan::kLayouts) {
        if (info.transmitted == info.rendered) {
            continue;
        }
        // Where the two differ, say so: a dependent that REPLACES a bed
        // channel spends coded channels a listener never counts.
        std::println("        {} renders {} speakers from {} coded channels", info.name,
                     info.rendered, info.transmitted);
    }
    std::println("        For 'sine' and 'eac3-sine' each speaker gets its own tone; append");
    std::println("        'c' to a 'sine' layout (stereoc, 51c) to enable channel coupling.");
    std::println("        For 'encode' and 'eac3-encode' it names the OUTPUT layout: a");
    std::println("        source narrower than it leaves the channels it lacks silent, and");
    std::println("        a wider one folds down per §7.8 using cmixlev/surmixlev.");
    std::println("");
    std::println("        [layout] also takes a comma-separated Table E2.5 location list");
    std::println("        instead of one of the names above, for anything Annex E allows");
    std::println("        that has no preset: e.g. L,C,R,LFE,Vhl,Vhr or L,C,R,LFE,LFE2,Vhc.");
    std::println("        AC-3 accepts one too, as long as it needs no dependent substream");
    std::println("        (e.g. L,R,Cs or L,C,R,Cs - Table 5.8 modes no preset names).");
    std::println("        Locations: L C R Ls Rs Lc Rc Lrs Rrs Cs Ts Lsd Rsd Lw Rw Vhl Vhr");
    std::println("        Vhc Lts Rts LFE2 LFE - a paired location (Lc/Rc, Lrs/Rrs, Lsd/Rsd,");
    std::println("        Lw/Rw, Vhl/Vhr, Lts/Rts) must be given both halves.");
}

void print_tools_topic() {
    std::println("");
    std::println("tools:  Annex E coding tools, '+'-joined — {}", plan::kToolsSyntax);
    std::println("        cpl:N / spx:N pin that tool's band edge (e.g. cpl:4+spx:5);");
    std::println("        aht:N pins the GAQ mode — aht:0 is AHT with GAQ switched off;");
    std::println("        atten:N pins the SPX notch depth, noatten removes it");
}

void print_vbr_topic() {
    std::println("");
    std::println("vbr (eac3-encode only): {}", plan::kVbrSyntax);
    std::println("        quality is encoder-relative, not a fixed target — bit cost rises");
    std::println("        steeply above roughly half the range, so a high quality with no");
    std::println("        max bound will often refuse real programme material outright;");
    std::println("        bitrate_kbps still matters in vbr mode — it feeds the same");
    std::println("        coupling/spx frequency defaults it always has, not a target rate");
}

void print_atmos_topic() {
    std::println("");
    std::println("atmos: objects orbit the room at different heights and rates;");
    std::println("       atmos-encode makes each channel of a real file an object instead.");
    std::println("       Both emit a 5.1 E-AC-3 bed with JOC + OAMD side data (TS 103 420).");
    std::println("       FFmpeg reports \"Dolby Digital Plus + Dolby Atmos\".");
    std::println("atmos mode: objects (default) writes the JOC+OAMD container; bed51 omits");
    std::println("       it so the 5.1 bed still plays on a decoder that refuses an object");
    std::println("       container it cannot validate instead of falling back to the bed.");
}

void print_paths_topic() {
    std::println("");
    std::println("paths.txt: authored per-object motion - one keyframe per line, addressed by");
    std::println("       object index (atmos-path) or WAV channel index (atmos-encode). An");
    std::println("       object the file does not mention keeps that command's own default");
    std::println("       placement rather than being silenced.");
}

void print_decode_topic() {
    std::println("");
    std::println("For decode, drc=<scale> applies §7.7.1 partial compression (0 = ignore,");
    std::println("1 = as encoded) and 'heavy' prefers compr where the stream carries it.");
    std::println("decode objects_dir (E-AC-3 Atmos only): exports each JOC-reconstructed object");
    std::println("       as its own object_NN.wav, alongside the usual 5.1 bed WAV.");
}

void print_qc_topic() {
    std::println("");
    std::println("qc measures a stream's real BS.1770-4/EBU Tech 3342 loudness and compares it");
    std::println("       against the dialnorm/compr it embeds - preset=<name> also gates that");
    std::println("       measurement against a named delivery spec ({}),", ac3::meta::kQcPresetNames);
    std::println("       or preset=all checks every one; omitting preset= just measures and");
    std::println("       reports, with no pass/fail verdict. Exit code is 0 only when every");
    std::println("       requested gate passes (or none was requested and decode succeeded),");
    std::println("       {} when a gate fails and {} when the stream could not be read at all.",
                 kExitQcGate, kExitInput);
}

void print_mkv_topic() {
    std::println("");
    std::println("mkv wraps an AC-3 or E-AC-3 elementary stream in Matroska, taking the");
    std::println("format, packet boundaries, sample rate and channel count from the bitstream");
    std::println("itself — so it cannot be told the wrong ones. E-AC-3 dependent substreams");
    std::println("are grouped into their access unit and counted as the channels they render.");
}

void print_fmp4_topic() {
    std::println("");
    std::println("fmp4 writes a fragmented MP4/CMAF init segment plus one media segment per");
    std::println("fragment (frames_per_fragment access units each, default 48 - about 1.5s at");
    std::println("48 kHz), alongside an HLS media+master playlist pair and a DASH MPD, all");
    std::println("pointing at the same segments (CMAF's whole point) — ready for a real HLS/");
    std::println("DASH origin or packager. Dolby Atmos content signals CHANNELS=\"<N>/JOC\" in");
    std::println("the HLS playlists automatically, per Apple's HLS Authoring Specification.");
}

void print_ts_topic() {
    std::println("");
    std::println("ts wraps the same elementary stream as an MPEG-2 Transport Stream (PAT + PMT");
    std::println("+ one PES-wrapped audio PID), identified per the DVB profile — stream_type");
    std::println("0x06 plus the AC3_descriptor or Enhanced_AC3_descriptor ETSI EN 300 468 Annex D");
    std::println("defines, not ATSC's — with PCR stamped on the audio PID every access unit.");
}

void print_objects_topic() {
    std::println("");
    std::println("sign-objects/verify-objects carry a keyed signature over the EMDF object");
    std::println("       container: the encode side writes one, a decode or monitor checks it");
    std::println("       and refuses the whole command on a mismatch. Both need a key -");
    std::println("       signing-key=<path>, or AC3FORGE_SIGNING_KEY_FILE / AC3FORGE_SIGNING_KEY");
    std::println("       - which this tool never stores. See docs/concepts/object-signing.md.");
}

// The per-topic sections, in the order both the full listing and a single
// command's help print them. One table, so the two orders cannot diverge.
struct TopicSection {
    std::uint32_t bit;
    void (*print)();
};

constexpr std::array<TopicSection, 13> kTopicSections{{
    {topic::kStdio, print_stdio_topic},
    {topic::kLive, print_live_topic},
    {topic::kTake, print_take_topic},
    {topic::kAtmos, print_atmos_topic},
    {topic::kPaths, print_paths_topic},
    {topic::kTools, print_tools_topic},
    {topic::kVbr, print_vbr_topic},
    {topic::kLayout, print_layout_topic},
    {topic::kMkv, print_mkv_topic},
    {topic::kFmp4, print_fmp4_topic},
    {topic::kTs, print_ts_topic},
    {topic::kDecode, print_decode_topic},
    {topic::kQc, print_qc_topic},
}};

void print_topic_sections(std::uint32_t mask) {
    for (const auto& section : kTopicSections) {
        if ((mask & section.bit) != 0) {
            section.print();
        }
    }
    if ((mask & topic::kObjects) != 0) {
        print_objects_topic();
    }
}

// The option blocks a topic mask selects, printed after the prose sections
// because they are reference tables rather than explanation.
void print_option_blocks(std::uint32_t mask) {
    if ((mask & topic::kMeta) != 0) {
        std::println("");
        std::println("metadata options (any order, after the positional arguments):");
        std::println("  drc=<profile>     §7.7.1 dynamic range control per block");
        std::println("                    {}", ac3::meta::kProfileNames);
        std::println("  heavy             §7.7.2 heavy compression: a peak ceiling in the");
        std::println("                    mono downmix, at syncframe resolution");
        std::println("  ceiling=<dBFS>    that ceiling (default -0.5)");
        std::println("  dialogue=<dBFS>   where heavy compression puts dialogue (default -20)");
        std::println("  drc2=<profile>    Ch2's own DRC profile, layout 1+1 only (§7.7.1) - not "
                     "inherited from drc=, set both to compress both programmes alike");
        std::println("  heavy2            Ch2's own heavy compression, layout 1+1 only (§7.7.2.2)");
        std::println("  ceiling2=<dBFS>   that ceiling for Ch2 (default -0.5)");
        std::println("  dialogue2=<dBFS>  where Ch2's heavy compression puts dialogue (default -20)");
        std::println("  dialnorm=auto     measure BS.1770 loudness and derive dialnorm (§5.4.2.8)");
        std::println("  dialnorm=<1..31>  set it directly (default 31)");
        std::println("  dialnorm2=auto | <1..31>   Ch2's own dialnorm, layout 1+1 only "
                     "(§5.4.2.16, default 31)");
        std::println("  cmixlev=-3|-4.5|-6      centre downmix level (Table 5.9)");
        std::println("  surmixlev=-3|-6|off     surround downmix level (Table 5.10)");
        std::println("  mixmeta           E-AC-3 only: emit the mixmdate group (Table E1.2)");
        std::println("  lfemix=<0..31>|off      E-AC-3 LFE mix level, 10-code dB (§E2.3.1.11)");
        std::println("  dmixmod=ltrt|loro|none  preferred stereo downmix (Table D2.2)");
        std::println("  keep-partial      encode/eac3-encode/atmos-encode: if the run fails partway, "
                     "keep whatever frames were already encoded (named beside the intended output as "
                     "<name>.partial.<ext>) instead of discarding them - off by default, matching the "
                     "GUI's own keep-partial-output preference");
        std::println("  fast-mdct=off     force the direct §8.2.3.2 forward MDCT instead of the "
                     "default §7.9.4 fast path (identical streams to within ~1e-12 coefficient "
                     "error; the direct form is the validation oracle) - applies wherever this "
                     "command encodes, incl. atmos/record/live/eac3-sine; eac3-encode alone has a "
                     "[tools] positional argument whose bare nofastmdct token reaches the same "
                     "field instead; bare fast-mdct (the old opt-in) is a no-op");
        std::println("  mode=reference    force BOTH transforms onto the spec's own direct "
                     "evaluations (the forms every fast-path test validates against): the §8.2.3.2 "
                     "forward MDCT wherever this command encodes, and §7.9.4's step-3 inverse in "
                     "'decode' - for runs where bit-for-bit agreement with the spec's stated "
                     "arithmetic matters more than speed. mode=performance (the default) keeps "
                     "both fast paths: 215-285 dB SNR against reference on 180 s programmes, "
                     "4.5-4.7x faster decodes. Tokens apply in order, so a later fast-mdct=off / "
                     "fast-imdct=off still adjusts one half on its own");
        std::println("  fast-imdct=off    decode: force just the direct §7.9.4 step-3 inverse "
                     "(mode=reference's decode half); bare fast-imdct names the default");
        std::println("  sign-objects      atmos/atmos-path/atmos-encode: write a keyed EMDF object "
                     "signature (needs signing-key=); see docs/concepts/object-signing.md");
        std::println("  verify-objects    decode/monitor: check each frame's EMDF object signature "
                     "against signing-key= instead of just playing it - a mismatch refuses the "
                     "command; omitted (the default) decodes signed and unsigned streams alike, "
                     "unchecked");
        std::println("  signing-key=<path>      the key file sign-objects/verify-objects use "
                     "(or AC3FORGE_SIGNING_KEY_FILE / AC3FORGE_SIGNING_KEY)");
    }
    if ((mask & topic::kMulti) != 0) {
        std::println("");
        std::println("source options (encode/eac3-encode/atmos-encode/live; any order, after "
                     "the positional arguments):");
        std::println("  src=<path>        an additional input source; repeat for more than one");
        std::println("  map=<spec>        {}", plan::kAssignmentSyntax);
        std::println("                    once given, every loaded channel must appear - explicit "
                     "'none' silences the goes-nowhere warning without giving it anywhere to go");
        std::println("                    obj/objm are real destinations on atmos-encode and on "
                     "live mode=atmos: each obj channel becomes its own object, a contiguous objm "
                     "range folds to one mono object, and the objects appear in map= order");
        std::println("  offset=<sourceIndex>:<seconds>   leading silence ahead of that source's own "
                     "channels (seconds >= 0), same 0-based numbering as src=");
        std::println("                    the programme is still as long as the longest one once "
                     "every offset is applied");
    }
    if ((mask & topic::kTake) != 0) {
        std::println("");
        std::println("record/live options (record, live; any order, after the positional "
                     "arguments):");
        std::println("  container=raw     the bare elementary stream (the default)");
        std::println("  container=mkv     Matroska, written incrementally as the take runs");
        std::println("  container=ts      an MPEG-2 Transport Stream, same DVB profile as 'ts'");
        std::println("  container=spdif   an IEC 61937 WAV carrier, same bursts as 'spdif'");
        std::println("  layout=<name>     the encoded layout (default stereo); anything wider");
        std::println("                    than AC-3 carries promotes the stream to E-AC-3");
        std::println("  codec=ac3|eac3    force the codec instead of deriving it from layout=");
        std::println("  watchdog=<sec>    stop the session if capture delivers nothing for this "
                     "long (default 3, 0 disables)");
    }
    if ((mask & topic::kLive) != 0) {
        std::println("");
        std::println("live options (live; any order, after the positional arguments):");
        std::println("  capture2=<index>  a second capture device, clock-conformed to the first "
                     "(see 'devices')");
        std::println("  objects=<N>       the object-slot budget for mode=atmos (1..15)");
        std::println("  downmix=off       refuse an AC-3-only passthrough endpoint instead of "
                     "running the parallel 5.1 AC-3 leg");
    }
    if ((mask & topic::kQc) != 0) {
        std::println("");
        std::println("qc options (qc; any order, after the positional arguments):");
        std::println("  preset=<name>     gate the measurement against a named delivery spec");
        std::println("                    {}", ac3::meta::kQcPresetNames);
        std::println("  preset=all        gate against every preset above");
        std::println("                    omitted: measure and report only, no gate");
    }
}

// fish's -d description is a single-quoted word, and several command notes
// and option summaries here contain an apostrophe ("objects it doesn't
// mention", "Ch2's own DRC profile"). fish accepts a backslash-escaped quote
// inside single quotes, so that is what this produces - the alternative,
// rewording every string that has one, would make the help worse to read in
// order to make one generator simpler.
std::string fish_quote(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 4);
    for (const char ch : text) {
        if (ch == '\\' || ch == '\'') {
            out += '\\';
        }
        out += ch;
    }
    return out;
}

// One man-page line, with groff's own escapes applied. Only two characters
// actually need it: a leading '.' or '\'' would start a request, and a
// backslash starts an escape sequence.
std::string roff_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        if (ch == '\\') {
            out += "\\e";
            continue;
        }
        if (ch == '-') {
            out += "\\-";
            continue;
        }
        out += ch;
    }
    if (!out.empty() && (out.front() == '.' || out.front() == '\'')) {
        out.insert(out.begin(), '\\');
        out.insert(out.begin() + 1, '&');
    }
    return out;
}

}  // namespace

void print_common_options() {
    std::println("");
    std::println("common options (every command; any order, after the positional arguments):");
    std::println("  quiet             no status output at all - errors on stderr and, for a '-'");
    std::println("                    output, the payload on stdout. Nothing else is printed.");
    std::println("  verbose           print the stderr progress line whatever the run's length,");
    std::println("                    and name every source/routing decision as it is made.");
    std::println("                    Without either token a run longer than a few seconds");
    std::println("                    prints that progress line on stderr and nothing more.");
    std::println("  --help, -h        this command's own help; 'ac3cli help <command>' is the");
    std::println("                    same thing spelled the other way round.");
    std::println("Exit codes: 0 success, {} usage, {} input, {} output, {} unavailable here,",
                 kExitUsage, kExitInput, kExitOutput, kExitUnavailable);
    std::println("            {} runtime, {} QC gate failed, {} internal. 'ac3cli help "
                 "exit-codes' explains each.",
                 kExitRuntime, kExitQcGate, kExitInternal);
}

void print_exit_codes() {
    std::println("ac3cli exit codes");
    std::println("");
    std::println("  {}  success.", kExitOk);
    std::println("  {}  usage: a bad or missing argument, an unknown command or option, or a",
                 kExitUsage);
    std::println("     configuration the encoder cannot express (an illegal bitrate for a");
    std::println("     layout, more objects than a stream can carry). Retrying the same command");
    std::println("     line cannot help.");
    std::println("  {}  input: the input could not be read, or is not a valid AC-3/E-AC-3/WAV/",
                 kExitInput);
    std::println("     ADM file, or stopped decoding part-way.");
    std::println("  {}  output: the destination could not be created, written or finalized.",
                 kExitOutput);
    std::println("  {}  unavailable here: this build or this machine cannot run the command at",
                 kExitUnavailable);
    std::println("     all - no audio backend, no capture/render endpoint, an endpoint that");
    std::println("     refuses the format, or a library this build was not configured with.");
    std::println("     The same command line may well succeed elsewhere.");
    std::println("  {}  runtime: the run started and then failed for none of the above reasons",
                 kExitRuntime);
    std::println("     - a capture device that stopped delivering audio (the record/live");
    std::println("     watchdog), a loudness measurement with nothing above the gate, a signing");
    std::println("     pass that could not complete.");
    std::println("  {}  a QC gate failed. Distinct from {} so a CI step can tell 'the stream is",
                 kExitQcGate, kExitInput);
    std::println("     out of spec' (a result) from 'qc could not read the file' (a fault).");
    std::println("  {}  internal: an exception escaped a command. Never expected.", kExitInternal);
}

void print_meta_usage() {
    print_option_blocks(topic::kMeta | topic::kMulti | topic::kTake | topic::kLive | topic::kQc);
    print_common_options();
}

void print_command_index(std::span<const CommandInfo> commands) {
    std::println("Usage:");
    std::println("  ac3cli --version    print version and git provenance, then exit");
    std::println("  ac3cli help [<command>|exit-codes]   this list, or one command's own help");
    for (const auto& c : commands) {
        print_row(c);
    }
}

void print_usage(std::span<const CommandInfo> commands) {
    std::println("ac3forge — clean-room AC-3 / E-AC-3 (ATSC A/52) encoder/decoder");
    std::println("");
    print_command_index(commands);
    print_unavailable_reasons(commands);
    print_topic_sections(topic::kAll);
    std::println("");
    std::println("Without a layout, encode and eac3-encode both follow the source: 1 -> mono,");
    std::println("2 -> stereo, 3 to 6 -> 5.1; eac3-encode alone extends that to 8 -> 7.1,");
    std::println("10 -> 5.1.4, 12 -> 7.1.4 (encode refuses anything wider than 3/2 + LFE).");
    std::println("Commands that carry PCM report per-channel levels when they finish; 'record'");
    std::println("meters live. 'couple' turns on channel coupling wherever a command encodes.");
    print_option_blocks(topic::kAll);
    print_common_options();
}

void print_command_help(const CommandInfo& command) {
    print_row(command);
    if (!command.available && !command.unavailable_reason.empty()) {
        std::println("");
        std::println("UNAVAILABLE HERE — {}.", command.unavailable_reason);
    }
    print_topic_sections(command.topics);
    print_option_blocks(command.topics);
    print_common_options();
}

void print_man_page(std::span<const CommandInfo> commands) {
    // Generated, never hand-edited: apps/cli/CMakeLists.txt runs this at
    // build time into ac3cli.1. The .TH date is deliberately the project
    // version rather than a build date - a date would make the file differ
    // between two builds of the same source, which is exactly what a
    // packaging diff should not see.
    std::println(R"(.\" Generated by `ac3cli man` - do not edit.)");
    std::println(".TH AC3CLI 1 \"ac3forge {}\" \"ac3forge\" \"User Commands\"",
                 roff_escape(ac3::version_full));
    std::println(".SH NAME");
    std::println("ac3cli \\- clean\\-room AC\\-3 / E\\-AC\\-3 (ATSC A/52) encoder, decoder and "
                 "Atmos object tool");
    std::println(".SH SYNOPSIS");
    std::println(".B ac3cli");
    std::println(".I command");
    std::println("[\\fIarguments\\fR]... [\\fIoption\\fR=\\fIvalue\\fR]...");
    std::println(".SH DESCRIPTION");
    std::println("ac3cli encodes, decodes, wraps, measures and plays AC\\-3 and E\\-AC\\-3");
    std::println("(Dolby Digital and Dolby Digital Plus, including the Atmos object layer).");
    std::println("Positional arguments come first and options follow in any order; an option");
    std::println("is either a bare word or a");
    std::println(".IR key = value");
    std::println("token, so the positionals keep their places whether options are present or");
    std::println("not.");
    std::println(".PP");
    std::println("A lone");
    std::println(".B \\-");
    std::println("in place of an input or output path means standard input or standard output");
    std::println("respectively, for");
    std::println(".BR encode ,");
    std::println(".BR eac3\\-encode ,");
    std::println(".B atmos\\-encode");
    std::println("and");
    std::println(".BR decode .");
    std::println(".SH COMMANDS");
    for (const auto& c : commands) {
        std::println(".TP");
        std::println(".B ac3cli {} {}", roff_escape(c.name), roff_escape(c.spec));
        if (!c.available) {
            std::println("Unavailable in this build/on this platform: {}.",
                         roff_escape(c.unavailable_reason));
            continue;
        }
        std::println("{}", c.note.empty() ? std::string{"See "} +
                                                std::string{"\\fBac3cli help "} +
                                                std::string{c.name} + "\\fR."
                                          : roff_escape(c.note));
    }
    std::println(".SH OPTIONS");
    for (const auto& option : kOptionTokens) {
        std::println(".TP");
        std::println(".B {}", roff_escape(option.spelling));
        std::println("{}", roff_escape(option.summary));
    }
    std::println(".SH EXIT STATUS");
    std::println(".TP");
    std::println(".B {}", kExitOk);
    std::println("Success.");
    std::println(".TP");
    std::println(".B {}", kExitUsage);
    std::println("Usage: a bad or missing argument, an unknown command or option, or a "
                 "configuration the encoder cannot express.");
    std::println(".TP");
    std::println(".B {}", kExitInput);
    std::println("Input: unreadable, absent, or not a valid stream.");
    std::println(".TP");
    std::println(".B {}", kExitOutput);
    std::println("Output: the destination could not be created, written or finalized.");
    std::println(".TP");
    std::println(".B {}", kExitUnavailable);
    std::println("Unavailable: this build or machine cannot run the command at all.");
    std::println(".TP");
    std::println(".B {}", kExitRuntime);
    std::println("Runtime: the run started and then failed - a capture dropout, a measurement "
                 "with nothing to measure, a signing pass that could not complete.");
    std::println(".TP");
    std::println(".B {}", kExitQcGate);
    std::println("A QC gate failed.");
    std::println(".TP");
    std::println(".B {}", kExitInternal);
    std::println("Internal: an exception escaped a command.");
    std::println(".SH ENVIRONMENT");
    std::println(".TP");
    std::println(".B AC3FORGE_SIGNING_KEY_FILE");
    std::println("Path to the object\\-signing key used by");
    std::println(".B sign\\-objects");
    std::println("and");
    std::println(".BR verify\\-objects ,");
    std::println("when no");
    std::println(".B signing\\-key=");
    std::println("was given.");
    std::println(".TP");
    std::println(".B AC3FORGE_SIGNING_KEY");
    std::println("The same key inline, for environments with no file to point at.");
    std::println(".SH SEE ALSO");
    std::println("Full documentation at");
    std::println(".UR https://github.com/iainchesworthlabs/ac3forge");
    std::println(".UE");
}

int print_completions(std::string_view shell, std::span<const CommandInfo> commands) {
    // Every command name, and every option spelling, as one space-separated
    // word list each - all four scripts below are the same two lists wearing
    // that shell's own syntax, so a new command or option reaches all four at
    // once.
    std::string names;
    for (const auto& c : commands) {
        if (!names.empty()) {
            names += ' ';
        }
        names += std::string{c.name};
    }
    std::string options;
    for (const auto& option : kOptionTokens) {
        if (!options.empty()) {
            options += ' ';
        }
        options += std::string{option.spelling};
    }

    if (shell == "bash") {
        std::println("# ac3cli bash completion - generated by `ac3cli completions bash`.");
        std::println("# Install as /usr/share/bash-completion/completions/ac3cli, or source it.");
        std::println("_ac3cli() {{");
        std::println("    local cur prev");
        std::println("    COMPREPLY=()");
        std::println("    cur=\"${{COMP_WORDS[COMP_CWORD]}}\"");
        std::println("    if [ \"$COMP_CWORD\" -eq 1 ]; then");
        std::println("        COMPREPLY=( $(compgen -W \"{} --version --help\" -- "
                     "\"$cur\") )", names);
        std::println("        return 0");
        std::println("    fi");
        std::println("    if [[ \"$cur\" == *=* || \"$cur\" == -* ]]; then");
        std::println("        COMPREPLY=( $(compgen -W \"{} --help\" -- \"$cur\") )", options);
        std::println("        compopt -o nospace 2>/dev/null");
        std::println("        return 0");
        std::println("    fi");
        std::println("    COMPREPLY=( $(compgen -f -- \"$cur\") $(compgen -W \"{}\" -- "
                     "\"$cur\") )", options);
        std::println("    return 0");
        std::println("}}");
        std::println("complete -F _ac3cli ac3cli");
        return kExitOk;
    }

    if (shell == "zsh") {
        std::println("#compdef ac3cli");
        std::println("# Generated by `ac3cli completions zsh`. Install as _ac3cli on $fpath.");
        std::println("_ac3cli() {{");
        std::println("    local -a _ac3cli_commands _ac3cli_options");
        std::println("    _ac3cli_commands=({})", names);
        std::println("    _ac3cli_options=({} --help)", options);
        std::println("    if (( CURRENT == 2 )); then");
        std::println("        _describe -t commands 'ac3cli command' _ac3cli_commands");
        std::println("        return");
        std::println("    fi");
        std::println("    _alternative \\");
        std::println("        'files:file:_files' \\");
        std::println("        'options:option:compadd -S \"\" -a _ac3cli_options'");
        std::println("}}");
        std::println("_ac3cli \"$@\"");
        return kExitOk;
    }

    if (shell == "fish") {
        std::println("# ac3cli fish completion - generated by `ac3cli completions fish`.");
        std::println("# Install as ~/.config/fish/completions/ac3cli.fish.");
        std::println("complete -c ac3cli -f");
        for (const auto& c : commands) {
            const std::string_view note = c.note.empty() ? c.spec : c.note;
            std::println("complete -c ac3cli -n '__fish_use_subcommand' -a '{}' -d '{}'", c.name,
                         fish_quote(note));
        }
        for (const auto& option : kOptionTokens) {
            std::println("complete -c ac3cli -n 'not __fish_use_subcommand' -a '{}' -d '{}'",
                         option.spelling, fish_quote(option.summary));
        }
        std::println("complete -c ac3cli -n 'not __fish_use_subcommand' -F");
        return kExitOk;
    }

    if (shell == "powershell") {
        std::println("# ac3cli PowerShell completion - generated by "
                     "`ac3cli completions powershell`.");
        std::println("# Add to $PROFILE, or dot-source it from there.");
        std::println("Register-ArgumentCompleter -Native -CommandName ac3cli -ScriptBlock {{");
        std::println("    param($wordToComplete, $commandAst, $cursorPosition)");
        std::println("    $commands = @('{}')", names);
        std::println("    $options  = @('{}', '--help')", options);
        std::println("    $words = $commandAst.CommandElements.Count");
        std::println("    $pool = if ($words -le 2 -and -not $wordToComplete.Contains('=')) "
                     "{{ $commands + $options }} else {{ $options }}");
        std::println("    $pool | Where-Object {{ $_ -like \"$wordToComplete*\" }} | "
                     "ForEach-Object {{");
        std::println("        [System.Management.Automation.CompletionResult]::new("
                     "$_, $_, 'ParameterValue', $_)");
        std::println("    }}");
        std::println("}}");
        return kExitOk;
    }

    std::println(stderr, "error: unknown shell '{}' ({})", shell, kCompletionShells);
    return kExitUsage;
}

}  // namespace ac3cli

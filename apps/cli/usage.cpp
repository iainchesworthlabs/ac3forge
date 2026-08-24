#include "usage.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fmt/base.h>
#include <fmt/format.h>
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

constexpr std::array<OptionToken, 46> kOptionTokens{{
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
    {"dither=", "off pins §7.3.4 dithflag at 0 wherever this command encodes"},
    {"joc-domain=", "atmos*/decode: mdct estimates JOC over 256 MDCT bins, not §7.1's QMF"},
    {"search=", "AC-3 encode: distortion or perceptual bit-allocation search, off (default)"},
    {"verify", "eac3-encode: decode every access unit as it's encoded and diff against it"},
    {"src=", "an additional input source; repeat for more than one"},
    {"map=", "where each source channel goes"},
    {"offset=", "<sourceIndex>:<seconds> leading silence for that source"},
    {"capture2=", "live: a second capture device, clock-conformed to the first"},
    {"container=", "record/live: raw, mkv, ts, spdif or fmp4"},
    {"fmp4-window=", "record/live container=fmp4: rolling segment-list window, 0 keeps all"},
    {"layout=", "record/live: the encoded layout (default stereo)"},
    {"codec=", "record/live: ac3 or eac3, instead of deriving it from layout="},
    {"watchdog=", "record/live: capture-silence timeout in seconds (0 disables)"},
    {"objects=", "live mode=atmos: the object-slot budget, 1..15"},
    {"downmix=", "live: off refuses an AC-3-only receiver instead of capping to 5.1"},
    {"preset=", "qc: gate the measurement against a named delivery spec"},
    {"json=", "probe: emit the JSON document instead of the human table"},
    {"detail=", "probe: frames or blocks - add per-access-unit/per-block detail"},
    {"fallback-51", "fmp4: also write the object-stripped 5.1 companion rendition"},
    {"mainid=", "ts: this service's A/52 Annex A main-service number"},
    {"asvc=", "ts: the main service this one is associated with (A/52 Annex A)"},
    {"programme=", "decode/qc/levels: which independent substream (0..7) of a multi-programme "
                   "stream"},
}};

// The note column of the usage listing starts here; a row whose spec already
// runs past it gets one hand-placed space instead (see print_row).
constexpr std::size_t kNoteColumn = 62;

void print_row(const CommandInfo& c) {
    // Wide enough that the LONGEST command name still gets a separating
    // space: 'strip-objects' is 13 characters, so a 13-wide field padded
    // nothing at all and ran the name straight into its own spec.
    std::string line = fmt::format("  ac3cli {:<14}{}", c.name, c.spec);
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
        line = fmt::format("{:<{}}({})", line, kNoteColumn, note);
    }
    fmt::println("{}", line);
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
    fmt::println("");
    for (const auto& reason : seen) {
        fmt::println("UNAVAILABLE HERE — {}.", reason);
    }
    fmt::println("Everything else is file I/O and behaves identically on every platform;");
    fmt::println("'spdif' in particular reaches a receiver without any audio backend at all.");
}

void print_stdio_topic() {
    fmt::println("");
    fmt::println("'-' in place of <in.wav>, <out.ac3>, <out.ec3>, <in.ac3|in.ec3> or <out.wav>");
    fmt::println("       means stdin (an input path) or stdout (an output path) - encode,");
    fmt::println("       eac3-encode, atmos-encode, decode and probe only. e.g.:");
    fmt::println("       ac3cli encode - - 448 couple < in.wav > out.ac3");
}

void print_live_topic() {
    fmt::println("");
    fmt::println("live monitor_device/passthrough_device: -2 (default) leaves that leg off,");
    fmt::println("       -1 is the default render endpoint, N picks one from 'outputs'.");
    fmt::println("       Either or both may run alongside the file this always writes.");
    fmt::println("live mode: 'channels' (default) encodes the captured channels as they are,");
    fmt::println("       onto layout= (stereo by default, anything up to 7.1.4); 'atmos' pans");
    fmt::println("       every captured channel into a 5.1 bed as its own object, moving it");
    fmt::println("       every frame the same way 'atmos' orbits its synthetic ones — the hook");
    fmt::println("       a real live position source drops into once one exists.");
    fmt::println("live capture2=<index>: the capture_device positional stays the session's");
    fmt::println("       clock master, paced exactly as it always has been; capture2= adds a");
    fmt::println("       second, independently-clocked device whose stream is resampled to");
    fmt::println("       track the master, with the measured drift printed at session end.");
    fmt::println("live downmix: when the layout or object mode needs E-AC-3 but the chosen");
    fmt::println("       passthrough endpoint only bitstreams AC-3, a parallel 5.1 AC-3 leg is");
    fmt::println("       encoded alongside the main stream and sent there, so the receiver");
    fmt::println("       hears a capped downmix instead of a refusal. The file always carries");
    fmt::println("       the full stream. downmix=off refuses instead, as it used to.");
    fmt::println("monitor/live --monitor play the 5.1 BED of an Atmos-mode stream: the decoder");
    fmt::println("       reads TS 103 420's object layer (OAMD/JOC) and reports an object count,");
    fmt::println("       but this path does not render or export objects, so this is what a");
    fmt::println("       legacy decoder hears, not unmixed objects.");
}

void print_take_topic() {
    fmt::println("");
    fmt::println("record/live container=: 'raw' (the default) writes the bare elementary");
    fmt::println("       stream; 'mkv' writes Matroska, 'ts' an MPEG-2 Transport Stream, 'spdif'");
    fmt::println("       an IEC 61937 WAV carrier and 'fmp4' a DIRECTORY of fragmented MP4/CMAF");
    fmt::println("       segments plus live HLS playlists and a dynamic DASH MPD - all five");
    fmt::println("       written incrementally as the session runs, so a take survives a crash");
    fmt::println("       and its memory cost does not grow with its length. fmp4-window=<n>");
    fmt::println("       (container=fmp4 only) keeps only the last <n> segments listed (a");
    fmt::println("       rolling live window); 0, the default, keeps every segment.");
    fmt::println("       'mkv'/'ts'/'spdif'/'fmp4' remain the way to wrap an ALREADY-encoded");
    fmt::println("       file after the fact.");
    fmt::println("record/live layout=: the encoded layout, default stereo. Anything wider than");
    fmt::println("       AC-3 can carry promotes the stream to E-AC-3 on its own; codec=eac3");
    fmt::println("       forces E-AC-3 for a narrow layout too. A capture device with fewer");
    fmt::println("       channels than the layout leaves the rest silent.");
    fmt::println("record/live watchdog=<seconds>: how long the capture device may deliver");
    fmt::println("       nothing before the session stops as a failure rather than sitting");
    fmt::println("       there reading 'running' (default 3, 0 disables). Whatever was already");
    fmt::println("       written stays on disk.");
    fmt::println("live objects=<N>: the object-slot budget for mode=atmos - allocated once at");
    fmt::println("       session start (1..15; the bed's LFE is the 16th, TS 103 420 §8.3.2.2");
    fmt::println("       caps the total at 16), so a slot that is bound later does not change");
    fmt::println("       the stream's object count mid-session. Default: one slot per captured");
    fmt::println("       channel. map= binds capture channels to slots; an unbound slot is");
    fmt::println("       carried silent.");
}

void print_layout_topic() {
    fmt::println("");
    fmt::println("layout: {}", plan::layout_names(plan::Codec::kEac3));
    fmt::println("        AC-3 carries only {} — everything wider needs the dependent",
                 plan::layout_names(plan::Codec::kAc3));
    fmt::println("        substreams that only E-AC-3 has.");
    for (const auto& info : plan::kLayouts) {
        if (info.transmitted == info.rendered) {
            continue;
        }
        // Where the two differ, say so: a dependent that REPLACES a bed
        // channel spends coded channels a listener never counts.
        fmt::println("        {} renders {} speakers from {} coded channels", info.name,
                     info.rendered, info.transmitted);
    }
    fmt::println("        For 'sine' and 'eac3-sine' each speaker gets its own tone; append");
    fmt::println("        'c' to a 'sine' layout (stereoc, 51c) to enable channel coupling.");
    fmt::println("        For 'encode' and 'eac3-encode' it names the OUTPUT layout: a");
    fmt::println("        source narrower than it leaves the channels it lacks silent, and");
    fmt::println("        a wider one folds down per §7.8 using cmixlev/surmixlev.");
    fmt::println("");
    fmt::println("        [layout] also takes a comma-separated Table E2.5 location list");
    fmt::println("        instead of one of the names above, for anything Annex E allows");
    fmt::println("        that has no preset: e.g. L,C,R,LFE,Vhl,Vhr or L,C,R,LFE,LFE2,Vhc.");
    fmt::println("        AC-3 accepts one too, as long as it needs no dependent substream");
    fmt::println("        (e.g. L,R,Cs or L,C,R,Cs - Table 5.8 modes no preset names).");
    fmt::println("        Locations: L C R Ls Rs Lc Rc Lrs Rrs Cs Ts Lsd Rsd Lw Rw Vhl Vhr");
    fmt::println("        Vhc Lts Rts LFE2 LFE - a paired location (Lc/Rc, Lrs/Rrs, Lsd/Rsd,");
    fmt::println("        Lw/Rw, Vhl/Vhr, Lts/Rts) must be given both halves.");
}

void print_tools_topic() {
    fmt::println("");
    fmt::println("tools:  Annex E coding tools, '+'-joined — {}", plan::kToolsSyntax);
    fmt::println("        cpl:N / spx:N pin that tool's band edge (e.g. cpl:4+spx:5);");
    fmt::println("        aht:N pins the GAQ mode — aht:0 is AHT with GAQ switched off;");
    fmt::println("        atten:N pins the SPX notch depth, noatten removes it");
}

void print_vbr_topic() {
    fmt::println("");
    fmt::println("vbr (eac3-encode only): {}", plan::kVbrSyntax);
    fmt::println("        quality is encoder-relative, not a fixed target — bit cost rises");
    fmt::println("        steeply above roughly half the range, so a high quality with no");
    fmt::println("        max bound will often refuse real programme material outright;");
    fmt::println("        bitrate_kbps still matters in vbr mode — it feeds the same");
    fmt::println("        coupling/spx frequency defaults it always has, not a target rate");
}

void print_atmos_topic() {
    fmt::println("");
    fmt::println("atmos: objects orbit the room at different heights and rates;");
    fmt::println("       atmos-encode makes each channel of a real file an object instead.");
    fmt::println("       Both emit a 5.1 E-AC-3 bed with JOC + OAMD side data (TS 103 420).");
    fmt::println("       FFmpeg reports \"Dolby Digital Plus + Dolby Atmos\".");
    fmt::println("atmos mode: objects (default) writes the JOC+OAMD container; bed51 omits");
    fmt::println("       it so the 5.1 bed still plays on a decoder that refuses an object");
    fmt::println("       container it cannot validate instead of falling back to the bed.");
}

void print_paths_topic() {
    fmt::println("");
    fmt::println("atmos-path/atmos-encode scene files come in two forms, told apart by their");
    fmt::println("       first character, not their suffix: the keyframe columns");
    fmt::println("       'object_index time_s x y z gain lfe_send' per line ('#' comments,");
    fmt::println("       addressed by object index for atmos-path or WAV channel index for");
    fmt::println("       atmos-encode), or an object scene in JSON (named objects, per-segment");
    // "{{" is the literal-brace escape - the character a JSON scene file
    // starts with.
    fmt::println("       interpolation, a scene orientation) starting with '{{'. The GUI writes");
    fmt::println("       either. An object the file does not mention keeps that command's own");
    fmt::println("       default placement rather than being silenced.");
}

void print_decode_topic() {
    fmt::println("");
    fmt::println("For decode, drc=<scale> applies §7.7.1 partial compression (0 = ignore,");
    fmt::println("1 = as encoded) and 'heavy' prefers compr where the stream carries it.");
    fmt::println("decode objects_dir (E-AC-3 Atmos only): exports each JOC-reconstructed object");
    fmt::println("       as its own object_NN.wav, alongside the usual 5.1 bed WAV.");
}

void print_qc_topic() {
    fmt::println("");
    fmt::println("qc measures a stream's real BS.1770-4/EBU Tech 3342 loudness and compares it");
    fmt::println("       against the dialnorm/compr it embeds - preset=<name> also gates that");
    fmt::println("       measurement against a named delivery spec ({}),", ac3::meta::kQcPresetNames);
    fmt::println("       or preset=all checks every one; omitting preset= just measures and");
    fmt::println("       reports, with no pass/fail verdict. Exit code is 0 only when every");
    fmt::println("       requested gate passes (or none was requested and decode succeeded),");
    fmt::println("       {} when a gate fails and {} when the stream could not be read at all.",
                 kExitQcGate, kExitInput);
    fmt::println("       layout=bed (default) meters the independent substream's own Table 5.8");
    fmt::println("       bed (BS.1770 Annex 1); layout=rendered meters the whole assembled");
    fmt::println("       program, every dependent substream's height/wide/rear channels");
    fmt::println("       included (BS.1770-5 Annex 3's extended algorithm).");
}

void print_probe_topic() {
    fmt::println("");
    fmt::println("probe reports what a stream DECLARES, without decoding its audio: bsid, rate");
    fmt::println("       (fscod2 half rates included), acmod/lfeon and the resolved layout,");
    fmt::println("       bsmod, chanmap, the substream map, frame and access-unit counts,");
    fmt::println("       duration, measured bit rate and VBR spread, dialnorm/compr/dynrng");
    fmt::println("       ranges, EMDF payload ids, OAMD/JOC with complexity_index and the");
    fmt::println("       object/bed configuration, whether an authenticity tag is present,");
    fmt::println("       per-frame CRC validity and how often each coding tool was used.");
    fmt::println("       json=1 emits the ac3forge.probe/1 document instead (docs/cli/");
    fmt::println("       commands.md documents it as a stable contract); detail=frames adds");
    fmt::println("       a per-access-unit dump and detail=blocks adds each block's Annex E");
    fmt::println("       tools and exponent strategies. Exit code is non-zero if any frame");
    fmt::println("       failed its CRC or the parser refused it, so this works as a gate.");
}

void print_mkv_topic() {
    fmt::println("");
    fmt::println("mkv wraps an AC-3 or E-AC-3 elementary stream in Matroska, taking the");
    fmt::println("format, packet boundaries, sample rate and channel count from the bitstream");
    fmt::println("itself — so it cannot be told the wrong ones. E-AC-3 dependent substreams");
    fmt::println("are grouped into their access unit and counted as the channels they render.");
}

void print_fmp4_topic() {
    fmt::println("");
    fmt::println("fmp4 writes a fragmented MP4/CMAF init segment plus one media segment per");
    fmt::println("fragment (frames_per_fragment access units each, default 48 - about 1.5s at");
    fmt::println("48 kHz), alongside an HLS media+master playlist pair and a DASH MPD, all");
    fmt::println("pointing at the same segments (CMAF's whole point) — ready for a real HLS/");
    fmt::println("DASH origin or packager. Dolby Atmos content signals CHANNELS=\"<N>/JOC\" in");
    fmt::println("the HLS playlists automatically, per Apple's HLS Authoring Specification.");
    fmt::println("fallback-51 also writes an object-stripped 5.1 companion rendition beside");
    fmt::println("the Atmos one (see 'strip-objects'), which is what Apple's HLS authoring");
    fmt::println("requirements want as the non-Atmos alternative in the same group.");
}

void print_ts_topic() {
    fmt::println("");
    fmt::println("ts wraps the same elementary stream as an MPEG-2 Transport Stream (PAT + PMT");
    fmt::println("+ one PES-wrapped audio PID), with PCR stamped on the audio PID every access");
    fmt::println("unit. [dvb|atsc] picks the broadcast profile: dvb (the default) writes");
    fmt::println("stream_type 0x06 plus the AC3_descriptor/Enhanced_AC3_descriptor ETSI EN 300");
    fmt::println("468 Annex D defines; atsc writes stream_type 0x81/0x87 plus A/52 Annex A's");
    fmt::println("AC-3_audio_stream_descriptor or Annex G's E-AC-3_audio_descriptor. Either");
    fmt::println("way the descriptor's fields come off the bitstream itself, except mainid=/");
    fmt::println("asvc= for the service associations no single elementary stream can know.");
}

void print_stream_tools_topic() {
    fmt::println("");
    fmt::println("transcode/metadata/normalize/cut/cat work on an ALREADY-encoded stream. Only");
    fmt::println("transcode re-encodes - it exists because DD+ and DD are different codecs and");
    fmt::println("nothing else bridges them; it carries dialnorm, compr and the mix metadata");
    fmt::println("across rather than resetting them, and folds a layout AC-3 cannot code down");
    fmt::println("to 5.1 per §7.8. The other four never touch a coded coefficient:");
    fmt::println("metadata/normalize rewrite bsi fields in place and re-stamp the CRCs, cut/cat");
    fmt::println("move whole access units. Convertible substreams (strmtyp 2) are out of scope");
    fmt::println("for all five, the same way 'validate' already refuses them.");
}

void print_objects_topic() {
    fmt::println("");
    fmt::println("sign-objects/verify-objects carry a keyed signature over the EMDF object");
    fmt::println("       container: the encode side writes one, a decode or monitor checks it");
    fmt::println("       and refuses the whole command on a mismatch. Both need a key -");
    fmt::println("       signing-key=<path>, or AC3FORGE_SIGNING_KEY_FILE / AC3FORGE_SIGNING_KEY");
    fmt::println("       - which this tool never stores. See docs/concepts/object-signing.md.");
}

// The per-topic sections, in the order both the full listing and a single
// command's help print them. One table, so the two orders cannot diverge.
struct TopicSection {
    std::uint32_t bit;
    void (*print)();
};

constexpr std::array<TopicSection, 15> kTopicSections{{
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
    {topic::kProbe, print_probe_topic},
    {topic::kStreamTools, print_stream_tools_topic},
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
        fmt::println("");
        fmt::println("metadata options (any order, after the positional arguments):");
        fmt::println("  drc=<profile>     §7.7.1 dynamic range control per block");
        fmt::println("                    {}", ac3::meta::kProfileNames);
        fmt::println("  heavy             §7.7.2 heavy compression: a peak ceiling in the");
        fmt::println("                    mono downmix, at syncframe resolution");
        fmt::println("  ceiling=<dBFS>    that ceiling (default -0.5)");
        fmt::println("  dialogue=<dBFS>   where heavy compression puts dialogue (default -20)");
        fmt::println("  drc2=<profile>    Ch2's own DRC profile, layout 1+1 only (§7.7.1) - not "
                     "inherited from drc=, set both to compress both programmes alike");
        fmt::println("  heavy2            Ch2's own heavy compression, layout 1+1 only (§7.7.2.2)");
        fmt::println("  ceiling2=<dBFS>   that ceiling for Ch2 (default -0.5)");
        fmt::println("  dialogue2=<dBFS>  where Ch2's heavy compression puts dialogue (default -20)");
        fmt::println("  dialnorm=auto     measure BS.1770 loudness and derive dialnorm (§5.4.2.8)");
        fmt::println("  dialnorm=<1..31>  set it directly (default 31)");
        fmt::println("  dialnorm2=auto | <1..31>   Ch2's own dialnorm, layout 1+1 only "
                     "(§5.4.2.16, default 31)");
        fmt::println("  cmixlev=-3|-4.5|-6      centre downmix level (Table 5.9)");
        fmt::println("  surmixlev=-3|-6|off     surround downmix level (Table 5.10)");
        fmt::println("  mixmeta           E-AC-3 only: emit the mixmdate group (Table E1.2)");
        fmt::println("  lfemix=<0..31>|off      E-AC-3 LFE mix level, 10-code dB (§E2.3.1.11)");
        fmt::println("  dmixmod=ltrt|loro|none  preferred stereo downmix (Table D2.2)");
        fmt::println("  keep-partial      encode/eac3-encode/atmos-encode: if the run fails partway, "
                     "keep whatever frames were already encoded (named beside the intended output as "
                     "<name>.partial.<ext>) instead of discarding them - off by default, matching the "
                     "GUI's own keep-partial-output preference");
        fmt::println("  fast-mdct=off     force the direct §8.2.3.2 forward MDCT instead of the "
                     "default §7.9.4 fast path (identical streams to within ~1e-12 coefficient "
                     "error; the direct form is the validation oracle) - applies wherever this "
                     "command encodes, incl. atmos/record/live/eac3-sine; eac3-encode alone has a "
                     "[tools] positional argument whose bare nofastmdct token reaches the same "
                     "field instead; bare fast-mdct (the old opt-in) is a no-op");
        fmt::println("  mode=reference    force BOTH transforms onto the spec's own direct "
                     "evaluations (the forms every fast-path test validates against): the §8.2.3.2 "
                     "forward MDCT wherever this command encodes, and §7.9.4's step-3 inverse in "
                     "'decode' - for runs where bit-for-bit agreement with the spec's stated "
                     "arithmetic matters more than speed. mode=performance (the default) keeps "
                     "both fast paths: 215-285 dB SNR against reference on 180 s programmes, "
                     "4.5-4.7x faster decodes. Tokens apply in order, so a later fast-mdct=off / "
                     "fast-imdct=off still adjusts one half on its own");
        fmt::println("  fast-imdct=off    decode: force just the direct §7.9.4 step-3 inverse "
                     "(mode=reference's decode half); bare fast-imdct names the default");
        fmt::println("  joc-domain=mdct   atmos*/decode: estimate and apply the JOC reconstruction "
                     "matrix over 256 MDCT bins instead of the default §7.1 64-band complex QMF - "
                     "cheaper, and what this project did before it had a filterbank, but ~5 dB worse "
                     "per object and not the domain a licensed decoder reconstructs in. Not "
                     "part of mode= either way: unlike the two transform switches, these are "
                     "different answers rather than the same one at different speed, and the "
                     "default is already the domain the clause states");
        fmt::println("  search=<what>     AC-3 encode only: choose §7.2.2's transmitted bit "
                     "allocation parameters per frame from the reconstruction error a decoder "
                     "will produce, instead of the rate-derived defaults. distortion minimises "
                     "that error; perceptual weights it by a tonality/masking model first. off "
                     "(the default) keeps every release before this one's fixed values - costs "
                     "encode time, see docs/library/quality.md for the measured figures");
        fmt::println("  dither=off        pin §7.3.4 dithflag at 0 instead of deciding it per "
                     "channel per block from content - applies wherever this command encodes, "
                     "the same reach as fast-mdct=off; eac3-encode's [tools] positional argument "
                     "has the equivalent bare nodither token instead. Real dither values are "
                     "decoder-defined, so this is for a run that needs bit-for-bit agreement "
                     "with another decoder more than it needs dither's own perceptual benefit "
                     "(tools/checks/verify_gold_reference.sh is the one that does)");
        fmt::println("  sign-objects      atmos/atmos-path/atmos-encode: write a keyed EMDF object "
                     "signature (needs signing-key=); see docs/concepts/object-signing.md");
        fmt::println("  verify-objects    decode/monitor: check each frame's EMDF object signature "
                     "against signing-key= instead of just playing it - a mismatch refuses the "
                     "command; omitted (the default) decodes signed and unsigned streams alike, "
                     "unchecked");
        fmt::println("  signing-key=<path>      the key file sign-objects/verify-objects use "
                     "(or AC3FORGE_SIGNING_KEY_FILE / AC3FORGE_SIGNING_KEY)");
    }
    if ((mask & topic::kMulti) != 0) {
        fmt::println("");
        fmt::println("source options (encode/eac3-encode/atmos-encode/live; any order, after "
                     "the positional arguments):");
        fmt::println("  src=<path>        an additional input source; repeat for more than one");
        fmt::println("  map=<spec>        {}", plan::kAssignmentSyntax);
        fmt::println("                    once given, every loaded channel must appear - explicit "
                     "'none' silences the goes-nowhere warning without giving it anywhere to go");
        fmt::println("                    obj/objm are real destinations on atmos-encode and on "
                     "live mode=atmos: each obj channel becomes its own object, a contiguous objm "
                     "range folds to one mono object, and the objects appear in map= order");
        fmt::println("  offset=<sourceIndex>:<seconds>   leading silence ahead of that source's own "
                     "channels (seconds >= 0), same 0-based numbering as src=");
        fmt::println("                    the programme is still as long as the longest one once "
                     "every offset is applied");
    }
    if ((mask & topic::kTake) != 0) {
        fmt::println("");
        fmt::println("record/live options (record, live; any order, after the positional "
                     "arguments):");
        fmt::println("  container=raw     the bare elementary stream (the default)");
        fmt::println("  container=mkv     Matroska, written incrementally as the take runs");
        fmt::println("  container=ts      an MPEG-2 Transport Stream, same DVB profile as 'ts'");
        fmt::println("  container=spdif   an IEC 61937 WAV carrier, same bursts as 'spdif'");
        fmt::println("  container=fmp4    a DIRECTORY of fragmented MP4/CMAF segments plus live");
        fmt::println("                    HLS playlists and a dynamic DASH MPD - the output path");
        fmt::println("                    names the folder");
        fmt::println("  fmp4-window=<n>   container=fmp4 only: keep only the last <n> segments in");
        fmt::println("                    the playlist/MPD (a rolling live window); 0, the "
                     "default, keeps every segment");
        fmt::println("  layout=<name>     the encoded layout (default stereo); anything wider");
        fmt::println("                    than AC-3 carries promotes the stream to E-AC-3");
        fmt::println("  codec=ac3|eac3    force the codec instead of deriving it from layout=");
        fmt::println("  watchdog=<sec>    stop the session if capture delivers nothing for this "
                     "long (default 3, 0 disables)");
    }
    if ((mask & topic::kLive) != 0) {
        fmt::println("");
        fmt::println("live options (live; any order, after the positional arguments):");
        fmt::println("  capture2=<index>  a second capture device, clock-conformed to the first "
                     "(see 'devices')");
        fmt::println("  objects=<N>       the object-slot budget for mode=atmos (1..15)");
        fmt::println("  downmix=off       refuse an AC-3-only passthrough endpoint instead of "
                     "running the parallel 5.1 AC-3 leg");
    }
    if ((mask & topic::kQc) != 0) {
        fmt::println("");
        fmt::println("qc options (qc; any order, after the positional arguments):");
        fmt::println("  preset=<name>     gate the measurement against a named delivery spec");
        fmt::println("                    {}", ac3::meta::kQcPresetNames);
        fmt::println("  preset=all        gate against every preset above");
        fmt::println("                    omitted: measure and report only, no gate");
        fmt::println("  layout=bed        the default - meter the independent substream's own");
        fmt::println("                    Table 5.8 bed (BS.1770 Annex 1's basic algorithm)");
        fmt::println("  layout=rendered   meter the whole assembled program instead, every");
        fmt::println("                    dependent substream's height/wide/rear channels");
        fmt::println("                    included (BS.1770-5 Annex 3's extended algorithm)");
    }
    if ((mask & topic::kProbe) != 0) {
        fmt::println("");
        fmt::println("probe options (probe; any order, after the positional arguments):");
        fmt::println("  json=1            emit the JSON document instead of the human table");
        fmt::println("                    (schema ac3forge.probe/1 - docs/cli/commands.md)");
        fmt::println("  detail=frames     add a per-access-unit dump: offsets, sizes, CRC,");
        fmt::println("                    substream headers and each frame's object layer");
        fmt::println("  detail=blocks     the same, plus every block's coding tools and");
        fmt::println("                    exponent strategies - what a codec bug report needs");
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
    fmt::println("");
    fmt::println("common options (every command; any order, after the positional arguments):");
    fmt::println("  quiet             no status output at all - errors on stderr and, for a '-'");
    fmt::println("                    output, the payload on stdout. Nothing else is printed.");
    fmt::println("  verbose           print the stderr progress line whatever the run's length,");
    fmt::println("                    and name every source/routing decision as it is made.");
    fmt::println("                    Without either token a run longer than a few seconds");
    fmt::println("                    prints that progress line on stderr and nothing more.");
    fmt::println("  --help, -h        this command's own help; 'ac3cli help <command>' is the");
    fmt::println("                    same thing spelled the other way round.");
    fmt::println("Exit codes: 0 success, {} usage, {} input, {} output, {} unavailable here,",
                 kExitUsage, kExitInput, kExitOutput, kExitUnavailable);
    fmt::println("            {} runtime, {} QC gate failed, {} internal. 'ac3cli help "
                 "exit-codes' explains each.",
                 kExitRuntime, kExitQcGate, kExitInternal);
}

void print_exit_codes() {
    fmt::println("ac3cli exit codes");
    fmt::println("");
    fmt::println("  {}  success.", kExitOk);
    fmt::println("  {}  usage: a bad or missing argument, an unknown command or option, or a",
                 kExitUsage);
    fmt::println("     configuration the encoder cannot express (an illegal bitrate for a");
    fmt::println("     layout, more objects than a stream can carry). Retrying the same command");
    fmt::println("     line cannot help.");
    fmt::println("  {}  input: the input could not be read, or is not a valid AC-3/E-AC-3/WAV/",
                 kExitInput);
    fmt::println("     ADM file, or stopped decoding part-way.");
    fmt::println("  {}  output: the destination could not be created, written or finalized.",
                 kExitOutput);
    fmt::println("  {}  unavailable here: this build or this machine cannot run the command at",
                 kExitUnavailable);
    fmt::println("     all - no audio backend, no capture/render endpoint, an endpoint that");
    fmt::println("     refuses the format, or a library this build was not configured with.");
    fmt::println("     The same command line may well succeed elsewhere.");
    fmt::println("  {}  runtime: the run started and then failed for none of the above reasons",
                 kExitRuntime);
    fmt::println("     - a capture device that stopped delivering audio (the record/live");
    fmt::println("     watchdog), a loudness measurement with nothing above the gate, a signing");
    fmt::println("     pass that could not complete.");
    fmt::println("  {}  a QC gate failed. Distinct from {} so a CI step can tell 'the stream is",
                 kExitQcGate, kExitInput);
    fmt::println("     out of spec' (a result) from 'qc could not read the file' (a fault).");
    fmt::println("  {}  internal: an exception escaped a command. Never expected.", kExitInternal);
}

void print_meta_usage() {
    print_option_blocks(topic::kMeta | topic::kMulti | topic::kTake | topic::kLive | topic::kQc |
                        topic::kProbe);
    print_common_options();
}

void print_command_index(std::span<const CommandInfo> commands) {
    fmt::println("Usage:");
    fmt::println("  ac3cli --version    print version and git provenance, then exit");
    fmt::println("  ac3cli help [<command>|exit-codes]   this list, or one command's own help");
    for (const auto& c : commands) {
        print_row(c);
    }
}

void print_usage(std::span<const CommandInfo> commands) {
    fmt::println("ac3forge — clean-room AC-3 / E-AC-3 (ATSC A/52) encoder/decoder");
    fmt::println("");
    print_command_index(commands);
    print_unavailable_reasons(commands);
    print_topic_sections(topic::kAll);
    fmt::println("");
    fmt::println("Without a layout, encode and eac3-encode both follow the source: 1 -> mono,");
    fmt::println("2 -> stereo, 3 to 6 -> 5.1; eac3-encode alone extends that to 8 -> 7.1,");
    fmt::println("10 -> 5.1.4, 12 -> 7.1.4 (encode refuses anything wider than 3/2 + LFE).");
    fmt::println("Commands that carry PCM report per-channel levels when they finish; 'record'");
    fmt::println("meters live. 'couple' turns on channel coupling wherever a command encodes.");
    print_option_blocks(topic::kAll);
    print_common_options();
}

void print_command_help(const CommandInfo& command) {
    print_row(command);
    if (!command.available && !command.unavailable_reason.empty()) {
        fmt::println("");
        fmt::println("UNAVAILABLE HERE — {}.", command.unavailable_reason);
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
    fmt::println(R"(.\" Generated by `ac3cli man` - do not edit.)");
    fmt::println(".TH AC3CLI 1 \"ac3forge {}\" \"ac3forge\" \"User Commands\"",
                 roff_escape(ac3::version_full));
    fmt::println(".SH NAME");
    fmt::println("ac3cli \\- clean\\-room AC\\-3 / E\\-AC\\-3 (ATSC A/52) encoder, decoder and "
                 "Atmos object tool");
    fmt::println(".SH SYNOPSIS");
    fmt::println(".B ac3cli");
    fmt::println(".I command");
    fmt::println("[\\fIarguments\\fR]... [\\fIoption\\fR=\\fIvalue\\fR]...");
    fmt::println(".SH DESCRIPTION");
    fmt::println("ac3cli encodes, decodes, wraps, measures and plays AC\\-3 and E\\-AC\\-3");
    fmt::println("(Dolby Digital and Dolby Digital Plus, including the Atmos object layer).");
    fmt::println("Positional arguments come first and options follow in any order; an option");
    fmt::println("is either a bare word or a");
    fmt::println(".IR key = value");
    fmt::println("token, so the positionals keep their places whether options are present or");
    fmt::println("not.");
    fmt::println(".PP");
    fmt::println("A lone");
    fmt::println(".B \\-");
    fmt::println("in place of an input or output path means standard input or standard output");
    fmt::println("respectively, for");
    fmt::println(".BR encode ,");
    fmt::println(".BR eac3\\-encode ,");
    fmt::println(".B atmos\\-encode");
    fmt::println("and");
    fmt::println(".BR decode .");
    fmt::println(".SH COMMANDS");
    for (const auto& c : commands) {
        fmt::println(".TP");
        fmt::println(".B ac3cli {} {}", roff_escape(c.name), roff_escape(c.spec));
        if (!c.available) {
            fmt::println("Unavailable in this build/on this platform: {}.",
                         roff_escape(c.unavailable_reason));
            continue;
        }
        fmt::println("{}", c.note.empty() ? std::string{"See "} +
                                                std::string{"\\fBac3cli help "} +
                                                std::string{c.name} + "\\fR."
                                          : roff_escape(c.note));
    }
    fmt::println(".SH OPTIONS");
    for (const auto& option : kOptionTokens) {
        fmt::println(".TP");
        fmt::println(".B {}", roff_escape(option.spelling));
        fmt::println("{}", roff_escape(option.summary));
    }
    fmt::println(".SH EXIT STATUS");
    fmt::println(".TP");
    fmt::println(".B {}", kExitOk);
    fmt::println("Success.");
    fmt::println(".TP");
    fmt::println(".B {}", kExitUsage);
    fmt::println("Usage: a bad or missing argument, an unknown command or option, or a "
                 "configuration the encoder cannot express.");
    fmt::println(".TP");
    fmt::println(".B {}", kExitInput);
    fmt::println("Input: unreadable, absent, or not a valid stream.");
    fmt::println(".TP");
    fmt::println(".B {}", kExitOutput);
    fmt::println("Output: the destination could not be created, written or finalized.");
    fmt::println(".TP");
    fmt::println(".B {}", kExitUnavailable);
    fmt::println("Unavailable: this build or machine cannot run the command at all.");
    fmt::println(".TP");
    fmt::println(".B {}", kExitRuntime);
    fmt::println("Runtime: the run started and then failed - a capture dropout, a measurement "
                 "with nothing to measure, a signing pass that could not complete.");
    fmt::println(".TP");
    fmt::println(".B {}", kExitQcGate);
    fmt::println("A QC gate failed.");
    fmt::println(".TP");
    fmt::println(".B {}", kExitInternal);
    fmt::println("Internal: an exception escaped a command.");
    fmt::println(".SH ENVIRONMENT");
    fmt::println(".TP");
    fmt::println(".B AC3FORGE_SIGNING_KEY_FILE");
    fmt::println("Path to the object\\-signing key used by");
    fmt::println(".B sign\\-objects");
    fmt::println("and");
    fmt::println(".BR verify\\-objects ,");
    fmt::println("when no");
    fmt::println(".B signing\\-key=");
    fmt::println("was given.");
    fmt::println(".TP");
    fmt::println(".B AC3FORGE_SIGNING_KEY");
    fmt::println("The same key inline, for environments with no file to point at.");
    fmt::println(".SH SEE ALSO");
    fmt::println("Full documentation at");
    fmt::println(".UR https://github.com/iainchesworthlabs/ac3forge");
    fmt::println(".UE");
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
        fmt::println("# ac3cli bash completion - generated by `ac3cli completions bash`.");
        fmt::println("# Install as /usr/share/bash-completion/completions/ac3cli, or source it.");
        fmt::println("_ac3cli() {{");
        fmt::println("    local cur prev");
        fmt::println("    COMPREPLY=()");
        fmt::println("    cur=\"${{COMP_WORDS[COMP_CWORD]}}\"");
        fmt::println("    if [ \"$COMP_CWORD\" -eq 1 ]; then");
        fmt::println("        COMPREPLY=( $(compgen -W \"{} --version --help\" -- "
                     "\"$cur\") )", names);
        fmt::println("        return 0");
        fmt::println("    fi");
        fmt::println("    if [[ \"$cur\" == *=* || \"$cur\" == -* ]]; then");
        fmt::println("        COMPREPLY=( $(compgen -W \"{} --help\" -- \"$cur\") )", options);
        fmt::println("        compopt -o nospace 2>/dev/null");
        fmt::println("        return 0");
        fmt::println("    fi");
        fmt::println("    COMPREPLY=( $(compgen -f -- \"$cur\") $(compgen -W \"{}\" -- "
                     "\"$cur\") )", options);
        fmt::println("    return 0");
        fmt::println("}}");
        fmt::println("complete -F _ac3cli ac3cli");
        return kExitOk;
    }

    if (shell == "zsh") {
        fmt::println("#compdef ac3cli");
        fmt::println("# Generated by `ac3cli completions zsh`. Install as _ac3cli on $fpath.");
        fmt::println("_ac3cli() {{");
        fmt::println("    local -a _ac3cli_commands _ac3cli_options");
        fmt::println("    _ac3cli_commands=({})", names);
        fmt::println("    _ac3cli_options=({} --help)", options);
        fmt::println("    if (( CURRENT == 2 )); then");
        fmt::println("        _describe -t commands 'ac3cli command' _ac3cli_commands");
        fmt::println("        return");
        fmt::println("    fi");
        fmt::println("    _alternative \\");
        fmt::println("        'files:file:_files' \\");
        fmt::println("        'options:option:compadd -S \"\" -a _ac3cli_options'");
        fmt::println("}}");
        fmt::println("_ac3cli \"$@\"");
        return kExitOk;
    }

    if (shell == "fish") {
        fmt::println("# ac3cli fish completion - generated by `ac3cli completions fish`.");
        fmt::println("# Install as ~/.config/fish/completions/ac3cli.fish.");
        fmt::println("complete -c ac3cli -f");
        for (const auto& c : commands) {
            const std::string_view note = c.note.empty() ? c.spec : c.note;
            fmt::println("complete -c ac3cli -n '__fish_use_subcommand' -a '{}' -d '{}'", c.name,
                         fish_quote(note));
        }
        for (const auto& option : kOptionTokens) {
            fmt::println("complete -c ac3cli -n 'not __fish_use_subcommand' -a '{}' -d '{}'",
                         option.spelling, fish_quote(option.summary));
        }
        fmt::println("complete -c ac3cli -n 'not __fish_use_subcommand' -F");
        return kExitOk;
    }

    if (shell == "powershell") {
        fmt::println("# ac3cli PowerShell completion - generated by "
                     "`ac3cli completions powershell`.");
        fmt::println("# Add to $PROFILE, or dot-source it from there.");
        fmt::println("Register-ArgumentCompleter -Native -CommandName ac3cli -ScriptBlock {{");
        fmt::println("    param($wordToComplete, $commandAst, $cursorPosition)");
        fmt::println("    $commands = @('{}')", names);
        fmt::println("    $options  = @('{}', '--help')", options);
        fmt::println("    $words = $commandAst.CommandElements.Count");
        fmt::println("    $pool = if ($words -le 2 -and -not $wordToComplete.Contains('=')) "
                     "{{ $commands + $options }} else {{ $options }}");
        fmt::println("    $pool | Where-Object {{ $_ -like \"$wordToComplete*\" }} | "
                     "ForEach-Object {{");
        fmt::println("        [System.Management.Automation.CompletionResult]::new("
                     "$_, $_, 'ParameterValue', $_)");
        fmt::println("    }}");
        fmt::println("}}");
        return kExitOk;
    }

    fmt::println(stderr, "error: unknown shell '{}' ({})", shell, kCompletionShells);
    return kExitUsage;
}

}  // namespace ac3cli

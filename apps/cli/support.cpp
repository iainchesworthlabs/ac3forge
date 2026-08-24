#include "support.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fmt/base.h>
#include <fmt/chrono.h>  // IWYU pragma: keep - fmt::formatter<time_point> for "{:%FT%TZ}" below
#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ac3/analysis/levels.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/encoder/assignment.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/io/dec3.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/meta/mixing.hpp"
#include "ac3/meta/qc.hpp"
#include "ac3/signing/emdf_atmos_signer.hpp"
#include "ac3/signing/signing_key.hpp"
#include "matroska/matroska.hpp"
#include "mp4/dash.hpp"
#include "mp4/hls.hpp"
#include "mp4/mp4.hpp"
#include "platform/stdio_binary.hpp"

namespace ac3cli {

namespace plan = ac3::plan;

namespace {

bool parse_double(std::string_view text, double& out) {
    // from_chars for floating point needs the locale-independent form, which
    // is what a command line gives.
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

std::vector<std::byte> to_bytes(std::span<const char> raw) {
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    }
    return bytes;
}

// Wraps ac3::io::write_wav_f32 to honor the "-" stdout convention: "-"
// writes the WAV to stdout, binary mode set first, instead of opening a file
// with that literal name. ac3::io::write_wav_f32(std::ostream&, ...) never
// seeks (see its own comment), so this is exactly as safe on the unseekable
// pipe stdout usually is as the path overload is on a plain file.
std::expected<void, ac3::io::WavError> write_wav_f32_arg(
        std::string_view path, std::span<const std::vector<float>> channels,
        std::uint32_t sample_rate, std::span<const std::size_t> channel_order = {}) {
    if (is_stdio_path(path)) {
        ac3::cli::platform::set_stdio_binary();
        auto result = ac3::io::write_wav_f32(std::cout, channels, sample_rate, channel_order);
        std::cout.flush();
        return result;
    }
    return ac3::io::write_wav_f32(std::string{path}, channels, sample_rate, channel_order);
}

}  // namespace

std::uint32_t parse_u32_or(std::string_view text, std::uint32_t fallback) {
    std::uint32_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && ptr == text.data() + text.size() ? value : fallback;
}

void print_meta_usage() {
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
    fmt::println("  dither=off        pin §7.3.4 dithflag at 0 instead of deciding it per "
                 "channel per block from content - applies wherever this command encodes, "
                 "the same reach as fast-mdct=off; eac3-encode's [tools] positional argument "
                 "has the equivalent bare nodither token instead. Real dither values are "
                 "decoder-defined, so this is for a run that needs bit-for-bit agreement "
                 "with another decoder more than it needs dither's own perceptual benefit "
                 "(tools/checks/verify_gold_reference.sh is the one that does)");
    fmt::println("  channels=2|1      decode/monitor: apply the §7.8 output stage and leave "
                 "that many channels - 2 is a stereo fold, 1 is mono. channels=as-coded (the "
                 "default) does nothing at all. The stream's own cmixlev/surmixlev (AC-3) or "
                 "mixmdate levels (E-AC-3) drive the matrix; §7.8.1's normalisation keeps it "
                 "from overloading");
    fmt::println("  downmix=loro|ltrt|mono  which fold channels= produces: §7.8.1's plain "
                 "stereo (the default), §7.8.2's Dolby Surround compatible Lt/Rt, or mono. "
                 "Naming one implies the width, so downmix=ltrt on its own is enough");
    fmt::println("  ltrt-phase=off    take Lt/Rt's sign-only matrix instead of §7.8.2's real "
                 "90-degree surround phase shift, which costs 63 samples of output delay");
    fmt::println("  mix-lfe           fold the LFE into the downmix too (§7.8 makes it "
                 "optional and this decoder drops it by default), at the stream's own "
                 "lfemixlevcod where it has one and §7.8's +10 dB ideal where it does not");
    fmt::println("  drcmode=line|rf   decode/monitor: §7.7's two named consumer modes. line "
                 "normalises dialnorm and applies the transmitted dynrng in full; rf uses "
                 "compr instead (falling back on dynrng per §7.7.2.1) and protects the "
                 "downmix from overload. Both set dialnorm normalisation, unlike drc=/heavy, "
                 "which are the individual switches. Default: neither");
    fmt::println("  conceal=repeat|mute     decode/monitor: §7.10 error concealment. A frame "
                 "that will not decode is reconstructed from the previous block's overlap - "
                 "repeated and faded, or muted through the codec's own window - instead of "
                 "failing the command. Off by default");
    fmt::println("  sign-objects      atmos/atmos-path/atmos-encode: write a keyed EMDF object "
                 "signature (needs signing-key=); see docs/concepts/object-signing.md");
    fmt::println("  verify-objects    decode/monitor: check each frame's EMDF object signature "
                 "against signing-key= instead of just playing it - a mismatch refuses the "
                 "command; omitted (the default) decodes signed and unsigned streams alike, "
                 "unchecked");
    fmt::println("  signing-key=<path>      the key file sign-objects/verify-objects use "
                 "(or AC3FORGE_SIGNING_KEY_FILE / AC3FORGE_SIGNING_KEY)");
    fmt::println("");
    fmt::println("source options (encode/eac3-encode; any order, after the positional "
                 "arguments):");
    fmt::println("  src=<path>        an additional input source; repeat for more than one");
    fmt::println("  map=<spec>        {}", plan::kAssignmentSyntax);
    fmt::println("                    once given, every loaded channel must appear - explicit "
                 "'none' silences the goes-nowhere warning without giving it anywhere to go");
    fmt::println("  offset=<sourceIndex>:<seconds>   leading silence ahead of that source's own "
                 "channels (seconds >= 0), same 0-based numbering as src=");
    fmt::println("                    the programme is still as long as the longest one once "
                 "every offset is applied");
    fmt::println("");
    fmt::println("record/live options (record, live; any order, after the positional "
                 "arguments):");
    fmt::println("  container=mkv     write straight to Matroska instead of the bare elementary");
    fmt::println("                    stream this writes by default - same shape of choice as");
    fmt::println("                    the GUI's own Container setting");
    fmt::println("  container=fmp4    write a DIRECTORY of fragmented MP4/CMAF segments plus live");
    fmt::println("                    HLS playlists and a dynamic DASH MPD, updated as the");
    fmt::println("                    session runs - the output path names the folder");
    fmt::println("  fmp4-window=<n>   container=fmp4 only: keep only the last <n> segments in the");
    fmt::println("                    playlist/MPD (a rolling live window); 0, the default, keeps");
    fmt::println("                    every segment");
    fmt::println("  container=raw     the default, spelled out");
    fmt::println("");
    fmt::println("live options (live; any order, after the positional arguments):");
    fmt::println("  capture2=<index>  a second capture device, clock-conformed to the first "
                 "(see 'devices')");
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
    fmt::println("");
    fmt::println("probe options (probe; any order, after the positional arguments):");
    fmt::println("  json=1            emit the JSON document instead of the human table");
    fmt::println("                    (schema ac3forge.probe/1 - docs/cli/commands.md)");
    fmt::println("  detail=frames     add a per-access-unit dump: offsets, sizes, CRC,");
    fmt::println("                    substream headers and each frame's object layer");
    fmt::println("  detail=blocks     the same, plus every block's coding tools and");
    fmt::println("                    exponent strategies - what a codec bug report needs");
}

bool parse_options(std::span<char*> tokens, Options& out) {
    for (char* raw : tokens) {
        const std::string_view token{raw};
        const auto eq = token.find('=');
        const std::string_view key = token.substr(0, eq);
        const std::string_view value =
            eq == std::string_view::npos ? std::string_view{} : token.substr(eq + 1);

        if (token == "couple" || token == "heavy" || token == "heavy2" || token == "mixmeta" ||
            token == "keep-partial" || token == "fast-mdct") {
            if (token == "heavy") {
                out.p.heavy.emplace();
            } else if (token == "heavy2") {
                out.p.heavy2.emplace();
            } else if (token == "mixmeta") {
                out.p.mixmeta = true;
            } else if (token == "keep-partial") {
                out.keep_partial = true;
            } else if (token == "fast-mdct") {
                out.fast_mdct = true;
            }
            continue;
        }
        if (token == "fast-imdct") {
            out.fast_imdct = true;
            continue;
        }
        if (token == "sign-objects") {
            out.sign_objects = true;
            continue;
        }
        if (token == "verify-objects") {
            out.verify_objects = true;
            continue;
        }
        if (key == "fast-mdct") {
            // The bare word (handled above) is the historical opt-in; with
            // the fast path now the default, the value form exists for the
            // direction that still needs saying.
            if (value == "off") {
                out.fast_mdct = false;
                continue;
            }
            fmt::println(stderr,
                         "error: the fast MDCT is the default; 'fast-mdct=off' forces the "
                         "direct §8.2.3.2 transform (got '{}')",
                         token);
            return false;
        }
        if (key == "fast-imdct") {
            // Same shape as fast-mdct above, decode side: the fast inverse
            // is the default since its evidence was accepted, so the value
            // form exists for the direction that still needs saying. The
            // bare word (handled above) now just names what already happens.
            if (value == "off") {
                out.fast_imdct = false;
                continue;
            }
            fmt::println(stderr,
                         "error: the fast IMDCT is the default; 'fast-imdct=off' forces the "
                         "direct §7.9.4 step-3 evaluation (got '{}')",
                         token);
            return false;
        }
        if (key == "search") {
            // The per-frame bit-allocation-parameter search (EQ13). Off by
            // default; the two values name what it minimises rather than an
            // effort level, because they are different questions and not
            // two points on one scale - one is waveform error, the other is
            // that error weighted by what the signal can hide.
            if (value == "off") {
                out.search = ac3::quality::Criterion::kNone;
                continue;
            }
            if (value == "distortion") {
                out.search = ac3::quality::Criterion::kDistortion;
                continue;
            }
            if (value == "perceptual") {
                out.search = ac3::quality::Criterion::kPerceptual;
                continue;
            }
            fmt::println(stderr,
                         "error: search is 'off' (the default), 'distortion' or 'perceptual' "
                         "(got '{}')",
                         token);
            return false;
        }
        if (key == "dither") {
            // No bare-word form: unlike fast-mdct, dither has no prior
            // opt-in spelling to keep parsing, so only the value form -
            // the direction that still needs saying - exists at all.
            if (value == "off") {
                out.dither = false;
                continue;
            }
            fmt::println(stderr,
                         "error: dither is content-decided by default; 'dither=off' pins "
                         "dithflag at 0 unconditionally (got '{}')",
                         token);
            return false;
        }
        if (key == "mode") {
            // The two transform switches as one intent-level toggle:
            // performance (the default state - both fast paths) for normal
            // runs, reference (both spec-direct evaluations, the forms
            // every fast-path test validates against) for runs where
            // bit-for-bit agreement with the spec's stated arithmetic
            // matters more than speed. Tokens apply in order, so a later
            // fast-mdct=off / fast-imdct=off can still adjust one half.
            if (value == "performance") {
                out.fast_mdct = true;
                out.fast_imdct = true;
                continue;
            }
            if (value == "reference") {
                out.fast_mdct = false;
                out.fast_imdct = false;
                continue;
            }
            fmt::println(stderr,
                         "error: mode is 'performance' (the default) or 'reference' (got '{}')",
                         token);
            return false;
        }
        if (token == "mix-lfe") {
            out.output.mix_lfe = true;
            continue;
        }
        if (key == "channels") {
            // How many channels to LEAVE, which is the question an operator
            // actually has ("this has to play on a stereo device"). Which
            // stereo matrix is downmix='s question, and it has a default, so
            // channels= alone is enough to get a usable fold.
            if (value == "as-coded") {
                out.output.target = ac3::DownmixTarget::kAsCoded;
                continue;
            }
            if (value == "1") {
                out.output.target = ac3::DownmixTarget::kMono;
                continue;
            }
            if (value == "2") {
                // A downmix= earlier on the same command line already chose
                // the matrix; channels=2 only confirms the width.
                if (!out.downmix_named) {
                    out.output.target = ac3::DownmixTarget::kLoRo;
                }
                continue;
            }
            fmt::println(stderr,
                         "error: channels is '2' (§7.8 stereo), '1' (mono) or 'as-coded' (the "
                         "default - no downmix at all) (got '{}')",
                         token);
            return false;
        }
        if (key == "downmix") {
            if (value == "loro") {
                out.output.target = ac3::DownmixTarget::kLoRo;
            } else if (value == "ltrt") {
                out.output.target = ac3::DownmixTarget::kLtRt;
            } else if (value == "mono") {
                out.output.target = ac3::DownmixTarget::kMono;
            } else {
                fmt::println(stderr,
                             "error: downmix is 'loro' (§7.8.1), 'ltrt' (§7.8.2, Dolby Surround "
                             "compatible) or 'mono' (got '{}')",
                             token);
                return false;
            }
            out.downmix_named = true;
            continue;
        }
        if (key == "ltrt-phase") {
            // The 90-degree shift on Lt/Rt's surround sum is what §7.8.2
            // describes and costs a fixed delay on the whole output; 'off'
            // takes the sign-only matrix a lot of hardware implements
            // instead. Same key=off shape fast-mdct=/fast-imdct= use.
            if (value == "off") {
                out.output.ltrt_phase_shift = false;
                continue;
            }
            fmt::println(stderr,
                         "error: the Lt/Rt surround phase shift is the default; "
                         "'ltrt-phase=off' selects the sign-only matrix (got '{}')",
                         token);
            return false;
        }
        if (key == "drcmode") {
            // §7.7's two named consumer modes. Each sets dialnorm
            // normalisation AND which of dynrng/compr applies, which is what
            // distinguishes them from drc=/heavy - those are the individual
            // switches, these are the two combinations that have names.
            if (value == "line") {
                out.output.mode = ac3::OperatingMode::kLine;
            } else if (value == "rf") {
                out.output.mode = ac3::OperatingMode::kRf;
            } else if (value == "none") {
                out.output.mode = ac3::OperatingMode::kCustom;
            } else {
                fmt::println(stderr,
                             "error: drcmode is 'line' (§7.7.1), 'rf' (§7.7.2, with downmix "
                             "overload protection) or 'none' (the default) (got '{}')",
                             token);
                return false;
            }
            continue;
        }
        if (key == "conceal") {
            // §7.10. Off by default: a decode that hits a damaged frame says
            // so and stops, which is what a verification tool should do.
            if (value == "repeat") {
                out.concealment = ac3::ConcealmentPolicy::kRepeatFade;
            } else if (value == "mute") {
                out.concealment = ac3::ConcealmentPolicy::kMute;
            } else if (value == "off") {
                out.concealment = ac3::ConcealmentPolicy::kNone;
            } else {
                fmt::println(stderr,
                             "error: conceal is 'repeat' (repeat-and-fade), 'mute' (window-ramped "
                             "silence) or 'off' (the default) (got '{}')",
                             token);
                return false;
            }
            continue;
        }
        if (key == "drc") {
            // On the decode side drc= is a scale factor (§7.7.1 partial
            // compression); on the encode side it names a profile. A numeric
            // value is unambiguous, so one spelling serves both.
            double scale = 0.0;
            if (parse_double(value, scale)) {
                out.drc_scale = scale;
                continue;
            }
            ac3::meta::ProfileId id{};
            if (!ac3::meta::parse_profile(value, id)) {
                fmt::println(stderr, "error: unknown DRC profile '{}' ({})", value,
                             ac3::meta::kProfileNames);
                return false;
            }
            out.p.drc = ac3::meta::profile(id);
            continue;
        }
        if (key == "ceiling" || key == "dialogue") {
            double db = 0.0;
            if (!parse_double(value, db)) {
                fmt::println(stderr, "error: {} needs a level in dBFS", key);
                return false;
            }
            if (!out.p.heavy) {
                out.p.heavy.emplace();
            }
            if (key == "ceiling") {
                out.p.heavy->peak_ceiling_dbfs = db;
            } else {
                out.p.heavy->dialogue_target_dbfs = db;
            }
            continue;
        }
        if (key == "drc2") {
            // Encode-side only, unlike drc= - nothing on the decode side
            // corresponds to a per-programme DRC profile, since a decoder
            // just applies whatever dynrng2 the stream carries.
            ac3::meta::ProfileId id{};
            if (!ac3::meta::parse_profile(value, id)) {
                fmt::println(stderr, "error: unknown DRC profile '{}' ({})", value,
                             ac3::meta::kProfileNames);
                return false;
            }
            out.p.drc2 = ac3::meta::profile(id);
            continue;
        }
        if (key == "ceiling2" || key == "dialogue2") {
            double db = 0.0;
            if (!parse_double(value, db)) {
                fmt::println(stderr, "error: {} needs a level in dBFS", key);
                return false;
            }
            if (!out.p.heavy2) {
                out.p.heavy2.emplace();
            }
            if (key == "ceiling2") {
                out.p.heavy2->peak_ceiling_dbfs = db;
            } else {
                out.p.heavy2->dialogue_target_dbfs = db;
            }
            continue;
        }
        if (key == "dialnorm") {
            if (value == "auto") {
                out.p.measure_dialnorm = true;
                continue;
            }
            const auto n = parse_u32_or(value, 0);
            if (n < 1 || n > 31) {
                fmt::println(stderr, "error: dialnorm must be auto or 1..31 (§5.4.2.8)");
                return false;
            }
            out.p.dialnorm = static_cast<int>(n);
            continue;
        }
        if (key == "dialnorm2") {
            if (value == "auto") {
                out.p.measure_dialnorm2 = true;
                continue;
            }
            const auto n = parse_u32_or(value, 0);
            if (n < 1 || n > 31) {
                fmt::println(stderr, "error: dialnorm2 must be auto or 1..31 (§5.4.2.16)");
                return false;
            }
            out.p.dialnorm2 = static_cast<int>(n);
            continue;
        }
        if (key == "cmixlev") {
            if (value == "-3") {
                out.p.cmixlev = ac3::meta::CentreMixLevel::kMinus3dB;
            } else if (value == "-4.5") {
                out.p.cmixlev = ac3::meta::CentreMixLevel::kMinus4_5dB;
            } else if (value == "-6") {
                out.p.cmixlev = ac3::meta::CentreMixLevel::kMinus6dB;
            } else {
                fmt::println(stderr, "error: cmixlev must be -3, -4.5 or -6 (Table 5.9)");
                return false;
            }
            continue;
        }
        if (key == "surmixlev") {
            if (value == "-3") {
                out.p.surmixlev = ac3::meta::SurroundMixLevel::kMinus3dB;
            } else if (value == "-6") {
                out.p.surmixlev = ac3::meta::SurroundMixLevel::kMinus6dB;
            } else if (value == "off") {
                out.p.surmixlev = ac3::meta::SurroundMixLevel::kSilent;
            } else {
                fmt::println(stderr, "error: surmixlev must be -3, -6 or off (Table 5.10)");
                return false;
            }
            continue;
        }
        if (key == "lfemix") {
            out.p.mixmeta = true;
            if (value == "off") {
                out.p.lfemix = std::nullopt;
                continue;
            }
            const auto n = parse_u32_or(value, 99);
            if (n > 31) {
                fmt::println(stderr, "error: lfemix must be off or 0..31 (§E2.3.1.11)");
                return false;
            }
            out.p.lfemix = static_cast<int>(n);
            continue;
        }
        if (key == "dmixmod") {
            out.p.mixmeta = true;
            if (value == "ltrt") {
                out.p.dmixmod = ac3::meta::DownmixMode::kLtRt;
            } else if (value == "loro") {
                out.p.dmixmod = ac3::meta::DownmixMode::kLoRo;
            } else if (value == "none") {
                out.p.dmixmod = ac3::meta::DownmixMode::kNotIndicated;
            } else {
                fmt::println(stderr, "error: dmixmod must be ltrt, loro or none (Table D2.2)");
                return false;
            }
            continue;
        }
        if (key == "src") {
            if (value.empty()) {
                fmt::println(stderr, "error: src= needs a file path");
                return false;
            }
            out.sources.emplace_back(value);
            continue;
        }
        if (key == "map") {
            if (value.empty()) {
                fmt::println(stderr, "error: map= needs a spec ({})", plan::kAssignmentSyntax);
                return false;
            }
            out.map_spec = std::string{value};
            continue;
        }
        if (key == "offset") {
            const auto colon = value.find(':');
            std::size_t index = 0;
            double seconds = 0.0;
            bool ok = colon != std::string_view::npos;
            if (ok) {
                const auto index_text = value.substr(0, colon);
                const auto seconds_text = value.substr(colon + 1);
                const auto [ptr, ec] = std::from_chars(
                    index_text.data(), index_text.data() + index_text.size(), index);
                ok = ec == std::errc{} && ptr == index_text.data() + index_text.size();
                ok = ok && parse_double(seconds_text, seconds) && seconds >= 0.0;
            }
            if (!ok) {
                fmt::println(stderr,
                             "error: offset= needs <sourceIndex>:<seconds> (seconds >= 0)");
                return false;
            }
            // A given sourceIndex may appear more than once; offset_samples_for
            // reads this in order and keeps the last match, so no dedupe here.
            out.offsets.emplace_back(index, seconds);
            continue;
        }
        if (key == "capture2") {
            int index = 0;
            const auto [ptr, ec] =
                std::from_chars(value.data(), value.data() + value.size(), index);
            const bool ok =
                ec == std::errc{} && ptr == value.data() + value.size() && index >= 0;
            if (!ok) {
                fmt::println(stderr, "error: capture2= needs a non-negative device index");
                return false;
            }
            out.capture2 = index;
            continue;
        }
        if (key == "container") {
            if (value == "mkv" || value == "matroska") {
                out.container = RecordContainer::kMatroska;
            } else if (value == "fmp4" || value == "cmaf") {
                out.container = RecordContainer::kFmp4;
            } else if (value == "raw") {
                out.container = RecordContainer::kRaw;
            } else {
                fmt::println(stderr, "error: container must be raw, mkv or fmp4 (got '{}')",
                             token);
                return false;
            }
            continue;
        }
        if (key == "layout") {
            if (value == "rendered") {
                out.qc_rendered_layout = true;
            } else if (value == "bed") {
                out.qc_rendered_layout = false;
            } else {
                fmt::println(stderr, "error: layout must be bed or rendered (got '{}')", token);
                return false;
            }
            continue;
        }
        if (key == "fmp4-window") {
            std::uint32_t segments = 0;
            const auto [ptr, ec] =
                std::from_chars(value.data(), value.data() + value.size(), segments);
            if (ec != std::errc{} || ptr != value.data() + value.size()) {
                fmt::println(stderr, "error: fmp4-window= needs a segment count (0 keeps every "
                                     "segment)");
                return false;
            }
            out.fmp4_window_segments = segments;
            continue;
        }
        if (key == "preset") {
            if (value != "all") {
                ac3::meta::QcPresetId id{};
                if (!ac3::meta::parse_qc_preset(value, id)) {
                    fmt::println(stderr, "error: unknown qc preset '{}' ({} | all)", value,
                                 ac3::meta::kQcPresetNames);
                    return false;
                }
            }
            out.qc_preset = std::string{value};
            continue;
        }
        if (key == "json") {
            // 1/0 rather than a bare 'json' word: probe is the first command
            // whose OUTPUT FORM is a choice, and a value token says which
            // form was asked for even when a script builds the command line
            // programmatically ("json=$want"). '0' is accepted for exactly
            // that reason - a caller should not have to omit the token to
            // turn it off.
            if (value != "1" && value != "0") {
                fmt::println(stderr, "error: json must be 1 or 0 (got '{}')", token);
                return false;
            }
            out.json = value == "1";
            continue;
        }
        if (key == "detail") {
            if (value != "frames" && value != "blocks") {
                fmt::println(stderr, "error: detail must be frames or blocks (got '{}')", token);
                return false;
            }
            out.detail = std::string{value};
            continue;
        }
        if (key == "signing-key") {
            if (value.empty()) {
                fmt::println(stderr, "error: signing-key= needs a key file path");
                return false;
            }
            out.signing_key = std::string{value};
            continue;
        }
        fmt::println(stderr, "error: unknown option '{}'", token);
        print_meta_usage();
        return false;
    }
    return true;
}

std::optional<int> finish_measurement(const ac3::meta::LoudnessMeter& meter,
                                      std::string_view programme, std::string_view field,
                                      FILE* out) {
    const auto lkfs = meter.integrated_lkfs();
    if (!lkfs) {
        return std::nullopt;
    }
    const int dialnorm = ac3::meta::dialnorm_from_lkfs(*lkfs);
    if (programme.empty()) {
        fmt::println(out, "measured {:.2f} LKFS (BS.1770-4, gated) -> {} {}", *lkfs, field,
                     dialnorm);
    } else {
        fmt::println(out, "{} measured {:.2f} LKFS (BS.1770-4, gated) -> {} {}", programme, *lkfs,
                     field, dialnorm);
    }
    return dialnorm;
}

std::optional<int> measured_dialnorm(const ac3::io::WavData& wav, ac3::SampleRate rate,
                                     ac3::Acmod acmod, bool lfe, FILE* out) {
    ac3::meta::LoudnessMeter meter{rate, acmod, lfe};
    // LoudnessMeter takes its spans in AC-3 CODED order (Table 5.8: L, C, R,
    // Ls, Rs, LFE), which is not WAV order (FL, FR, FC, LFE, BL, BR) for any
    // layout wider than stereo. Pushing the file's own order straight in put
    // the LFE where Ls belongs - so BS.1770's +1.5 dB surround weight landed
    // on the LFE, which the standard excludes outright, while a real surround
    // landed in the excluded slot and was dropped. Measured against ffmpeg's
    // ebur128 on a 5.1 file with signal in one channel at a time, that read
    // the LFE-only case at -38.61 LKFS where the oracle correctly reported no
    // loudness at all.
    //
    // ac3_layout_for's wav_index[k] is "the position in a WAV frame of AC-3
    // channel k" - the same permutation run_levels already applies before it
    // meters, which is why that command never had the fault.
    const auto layout = ac3::io::ac3_layout_for(wav.channels.size());
    std::vector<std::span<const float>> views;
    views.reserve(wav.channels.size());
    if (layout && layout->wav_index.size() == wav.channels.size()) {
        for (const auto wav_slot : layout->wav_index) {
            views.emplace_back(wav.channels[wav_slot]);
        }
    } else {
        // No legal acmod carries this width (7 channels and up), so there is
        // no permutation to apply and no coded order to apply it to. The
        // caller has already decided what acmod to measure as; feeding the
        // file's own order is the only thing left, exactly as before.
        for (const auto& channel : wav.channels) {
            views.emplace_back(channel);
        }
    }
    meter.push(views);
    return finish_measurement(meter, {}, "dialnorm", out);
}

std::optional<int> measured_dialnorm_channel(std::span<const float> channel, ac3::SampleRate rate,
                                             std::string_view programme, std::string_view field,
                                             FILE* out) {
    ac3::meta::LoudnessMeter meter{rate, ac3::Acmod::k1_0, false};
    const std::array<std::span<const float>, 1> views{channel};
    meter.push(views);
    return finish_measurement(meter, programme, field, out);
}

bool prepare_dual_mono_source(ac3::io::WavData& wav, std::string_view layout,
                              std::string_view in2_path) {
    if (layout != "1+1") {
        if (!in2_path.empty()) {
            fmt::println(stderr,
                         "error: a second input file is only meaningful with layout 1+1 "
                         "(got layout '{}')",
                         layout);
            return false;
        }
        return true;
    }
    if (in2_path.empty()) {
        if (wav.channels.size() != 2) {
            fmt::println(stderr,
                         "error: layout 1+1 needs either one two-channel file (Ch1, Ch2) or "
                         "two mono files; the source has {} channel(s) and no second file "
                         "was given",
                         wav.channels.size());
            return false;
        }
        return true;
    }
    if (wav.channels.size() != 1) {
        fmt::println(stderr,
                     "error: layout 1+1 with a second input file needs the first file to be "
                     "mono (Ch1); it has {} channels",
                     wav.channels.size());
        return false;
    }
    auto second = ac3::io::read_wav(std::string{in2_path});
    if (!second) {
        fmt::println(stderr, "error: {}: {}", in2_path, ac3::io::describe(second.error()));
        return false;
    }
    if (second->channels.size() != 1) {
        fmt::println(stderr, "error: {} must be mono (Ch2); it has {} channels", in2_path,
                     second->channels.size());
        return false;
    }
    if (second->sample_rate != wav.sample_rate) {
        fmt::println(stderr,
                     "error: {} is {} Hz, but the first file is {} Hz - both programmes must "
                     "share a sample rate",
                     in2_path, second->sample_rate, wav.sample_rate);
        return false;
    }
    wav.channels.push_back(std::move(second->channels.front()));
    return true;
}

bool is_stdio_path(std::string_view path) { return path == "-"; }

FILE* status_stream(std::string_view out_path) { return is_stdio_path(out_path) ? stderr : stdout; }

bool write_frames(std::string_view path, std::span<const std::vector<std::byte>> frames) {
    if (is_stdio_path(path)) {
        // set_stdio_binary() before the first byte, not once at startup: a
        // command that never touches "-" (the overwhelming majority of
        // invocations) should not pay for it, and calling it more than once
        // in the rare case both the input and output of one command are "-"
        // is harmless - see platform/stdio_binary.hpp for what it fixes.
        ac3::cli::platform::set_stdio_binary();
        for (const auto& frame : frames) {
            std::cout.write(reinterpret_cast<const char*>(frame.data()),
                            static_cast<std::streamsize>(frame.size()));
        }
        std::cout.flush();
        if (!std::cout) {
            fmt::println(stderr, "error: cannot write to stdout");
            return false;
        }
        return true;
    }
    std::ofstream out{std::string{path}, std::ios::binary};
    if (!out) {
        fmt::println(stderr, "error: cannot open {} for writing", path);
        return false;
    }
    for (const auto& frame : frames) {
        out.write(reinterpret_cast<const char*>(frame.data()),
                  static_cast<std::streamsize>(frame.size()));
    }
    return true;
}

bool write_frames_or_mux(std::string_view path, bool matroska, const matroska::AudioTrack& track,
                         std::span<const std::vector<std::byte>> frames) {
    if (!matroska) {
        return write_frames(path, frames);
    }
    const auto file = matroska::mux(track, frames);
    if (!file) {
        fmt::println(stderr, "error: {}", matroska::describe(file.error()));
        return false;
    }
    std::ofstream out{std::string{path}, std::ios::binary};
    if (!out) {
        fmt::println(stderr, "error: cannot open {} for writing", path);
        return false;
    }
    out.write(reinterpret_cast<const char*>(file->data()),
             static_cast<std::streamsize>(file->size()));
    if (!out) {
        fmt::println(stderr, "error: write failed");
        return false;
    }
    return true;
}

namespace {

// The two file writers Fmp4SessionWriter needs, kept local: 'ac3cli fmp4' has
// its own pair in commands/containers.cpp for its own batch directory, and
// neither is worth a shared header for four lines apiece.
bool write_session_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

bool write_session_text(const std::filesystem::path& path, std::string_view text) {
    return write_session_bytes(
        path, std::as_bytes(std::span{reinterpret_cast<const char*>(text.data()), text.size()}));
}

}  // namespace

std::string Fmp4SessionWriter::open(std::string_view directory,
                                    std::uint32_t frames_per_fragment,
                                    std::uint32_t window_segments) {
    dir_ = std::filesystem::path{std::string{directory}};
    frames_per_fragment_ = frames_per_fragment;
    window_segments_ = window_segments;
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) {
        return fmt::format("cannot create directory {} ({})", directory, ec.message());
    }
    open_ = true;
    return {};
}

std::string Fmp4SessionWriter::start(std::span<const std::byte> first_frame) {
    // One access unit is enough for everything the track needs: kind, sample
    // rate, rendered channel count, the dac3/dec3 payload, the Table E2.5
    // channel map and the TS 103 420 object marker all come out of the first
    // unit's own headers - which is why this is deferred to the first push()
    // rather than done in open(). Exactly the re-scan 'ac3cli fmp4' and the
    // GUI's writeOutput already do before wrapping frames they just encoded.
    const auto scanned = ac3::io::scan(first_frame);
    if (!scanned) {
        return fmt::format("cannot describe the encoded stream ({})",
                           ac3::io::describe(scanned.error()));
    }
    const bool eac3 = scanned->kind == ac3::io::StreamKind::kEac3;
    track_ = mp4::AudioTrack{.codec_id = std::string{eac3 ? mp4::kCodecEac3 : mp4::kCodecAc3},
                             .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
                             .channels = scanned->channels,
                             .samples_per_frame = ac3::kSamplesPerFrame,
                             .codec_config = ac3::io::build_codec_config_box(*scanned)};
    // Dolby Digital Plus with Atmos objects: CHANNELS="<N>/JOC" for HLS (see
    // mp4/hls.hpp) and TS 103 420 §D.2's two SupplementalProperty descriptors
    // plus the 'ceao' brand for DASH/CMAF (see mp4/dash.hpp and
    // mp4::FragmentOptions::object_audio_brand).
    hls_ = mp4::HlsOptions{.channels_attribute =
                               scanned->oba_complexity_index
                                   ? fmt::format("{}/JOC", *scanned->oba_complexity_index)
                                   : std::string{}};
    dash_ = mp4::DashOptions{
        .joc_complexity_index = scanned->oba_complexity_index,
        .dolby_channel_configuration = ac3::io::dash_channel_configuration(*scanned)};

    auto writer = mp4::FragmentWriter::create(
        track_, mp4::FragmentOptions{.frames_per_fragment = frames_per_fragment_,
                                     .object_audio_brand = scanned->oba_complexity_index.has_value(),
                                     .playlist_window_segments = window_segments_});
    if (!writer) {
        return std::string{mp4::describe(writer.error())};
    }
    writer_ = std::move(*writer);
    if (!write_session_bytes(dir_ / "init.mp4", writer_->init_segment())) {
        return fmt::format("cannot write init.mp4 to {}", dir_.string());
    }
    // The live MPD's anchor: the wall-clock instant segment 1's playback
    // begins at. Read once, here, rather than per manifest rewrite - it must
    // not move as the session runs. mp4:: itself has no clock (no file I/O,
    // no time - see MpdOptions::availability_start_time), so the front end
    // stamps it.
    availability_start_ = fmt::format(
        "{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    return {};
}

std::string Fmp4SessionWriter::write_manifests(const mp4::FragmentWriter& writer,
                                              bool finished) {
    const auto window = writer.window();
    auto hls = hls_;
    hls.vod = finished;
    const auto media = mp4::build_hls_media_playlist(track_, window, hls);
    const auto master = mp4::build_hls_master_playlist(track_, window, "audio.m3u8", hls);
    if (!write_session_text(dir_ / "audio.m3u8", media) ||
        !write_session_text(dir_ / "master.m3u8", master)) {
        return fmt::format("cannot write the HLS playlists to {}", dir_.string());
    }
    const auto adaptation_set = mp4::build_dash_adaptation_set(track_, window, dash_);
    // While the session runs the MPD is dynamic (segments still appearing);
    // once it stops it becomes static, with the real total duration - the
    // same before/after pair the HLS playlist's #EXT-X-ENDLIST makes.
    // timeShiftBufferDepth matches the rolling window when there is one; with
    // fmp4-window=0 every segment stays on disk, so the whole presentation so
    // far is reachable and the depth is its own length.
    const double window_seconds =
        window.empty()
            ? 0.0
            : static_cast<double>(window.back().base_media_decode_time +
                                  window.back().duration_samples -
                                  window.front().base_media_decode_time) /
                  static_cast<double>(track_.sample_rate);
    const mp4::MpdOptions mpd_options{.is_static = finished,
                                      .availability_start_time = availability_start_,
                                      .time_shift_buffer_depth_seconds = window_seconds};
    if (!write_session_text(dir_ / "manifest.mpd",
                            mp4::build_dash_mpd(track_, window, adaptation_set, mpd_options))) {
        return fmt::format("cannot write manifest.mpd to {}", dir_.string());
    }
    return {};
}

std::string Fmp4SessionWriter::push(std::span<const std::byte> frame) {
    if (!open_) {
        return "the fMP4 session was never opened";
    }
    if (!writer_) {
        if (auto problem = start(frame); !problem.empty()) {
            return problem;
        }
    }
    // start() above engages writer_ on every path that returns empty, but
    // clang-tidy's bugprone-unchecked-optional-access does not trace an
    // optional's engagement across a member-function call - the same false
    // positive gui/encoder_controller.cpp already works around by binding
    // the optional's value once.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto& writer = *writer_;
    auto segment = writer.push(frame);
    if (!segment) {
        return std::string{mp4::describe(segment.error())};
    }
    if (!*segment) {
        return {};
    }
    const auto name = fmt::format("segment{}.m4s", (*segment)->sequence_number);
    if (!write_session_bytes(dir_ / name, (*segment)->bytes)) {
        return fmt::format("cannot write {} to {}", name, dir_.string());
    }
    ++segments_;
    return write_manifests(writer, false);
}

std::string Fmp4SessionWriter::close() {
    if (!open_ || !writer_) {
        return {};
    }
    auto& writer = *writer_;
    auto segment = writer.finalize();
    if (!segment) {
        return std::string{mp4::describe(segment.error())};
    }
    if (*segment) {
        const auto name = fmt::format("segment{}.m4s", (*segment)->sequence_number);
        if (!write_session_bytes(dir_ / name, (*segment)->bytes)) {
            return fmt::format("cannot write {} to {}", name, dir_.string());
        }
        ++segments_;
    }
    return write_manifests(writer, true);
}

std::string partial_output_path(std::string_view path) {
    const auto dot = path.rfind('.');
    const auto slash = path.find_last_of("/\\");
    if (dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash)) {
        return std::string(path.substr(0, dot)) + ".partial" + std::string(path.substr(dot));
    }
    return std::string(path) + ".partial";
}

bool EncodedStreamSink::open(std::string_view path, bool keep_partial, bool defer) {
    path_ = std::string{path};
    keep_partial_ = keep_partial;
    defer_ = defer;
    stdio_ = is_stdio_path(path);
    // Deferring means nothing leaves until close() - so nothing to create
    // yet either. The destination (file or "-", write_frames handles both)
    // is only touched then, exactly as the pre-sink code shape did.
    if (!stdio_ && !defer_) {
        file_.open(path_, std::ios::binary);
        if (!file_) {
            fmt::println(stderr, "error: cannot open {} for writing", path_);
            return false;
        }
    }
    open_ = true;
    return true;
}

bool EncodedStreamSink::push(std::vector<std::byte>&& frame) {
    if (!defer_) {
        return push(std::span<const std::byte>{frame});
    }
    min_bytes_ = frames_ == 0 ? frame.size() : std::min(min_bytes_, frame.size());
    max_bytes_ = std::max(max_bytes_, frame.size());
    total_bytes_ += frame.size();
    ++frames_;
    deferred_.push_back(std::move(frame));
    return true;
}

bool EncodedStreamSink::push(std::span<const std::byte> frame) {
    if (defer_) {
        deferred_.emplace_back(frame.begin(), frame.end());
    } else if (stdio_) {
        buffered_.insert(buffered_.end(), frame.begin(), frame.end());
    } else {
        file_.write(reinterpret_cast<const char*>(frame.data()),
                    static_cast<std::streamsize>(frame.size()));
        if (!file_) {
            fmt::println(stderr, "error: cannot write to {}", path_);
            return false;
        }
    }
    min_bytes_ = frames_ == 0 ? frame.size() : std::min(min_bytes_, frame.size());
    max_bytes_ = std::max(max_bytes_, frame.size());
    total_bytes_ += frame.size();
    ++frames_;
    return true;
}

bool EncodedStreamSink::close() {
    open_ = false;
    if (defer_) {
        return write_frames(path_, deferred_);
    }
    if (stdio_) {
        ac3::cli::platform::set_stdio_binary();
        std::cout.write(reinterpret_cast<const char*>(buffered_.data()),
                        static_cast<std::streamsize>(buffered_.size()));
        std::cout.flush();
        if (!std::cout) {
            fmt::println(stderr, "error: cannot write to stdout");
            return false;
        }
        return true;
    }
    file_.close();
    if (file_.fail()) {
        fmt::println(stderr, "error: cannot write to {}", path_);
        return false;
    }
    return true;
}

void EncodedStreamSink::abort() {
    if (!open_) {
        return;
    }
    open_ = false;
    if (defer_) {
        // Nothing has left this process yet, so the pre-sink helper IS the
        // right behaviour, note wording and all.
        write_partial_output(path_, keep_partial_, deferred_);
        return;
    }
    if (stdio_) {
        // "beside the intended output" has no meaning for a pipe - see
        // write_partial_output's identical stdout reasoning and wording.
        if (keep_partial_ && frames_ > 0) {
            ac3::cli::platform::set_stdio_binary();
            std::cout.write(reinterpret_cast<const char*>(buffered_.data()),
                            static_cast<std::streamsize>(buffered_.size()));
            std::cout.flush();
            if (std::cout) {
                fmt::println(stderr,
                             "note: the {} frames already encoded were written to stdout",
                             frames_);
            }
        }
        return;
    }
    file_.close();
    if (keep_partial_ && frames_ > 0) {
        // The bytes are already on disk at the intended path; keep-partial's
        // contract is that they live at the .partial name instead, so a
        // half-finished take can never be mistaken for a finished one.
        const auto partial = partial_output_path(path_);
        std::error_code ec;
        std::filesystem::rename(std::filesystem::path{path_}, std::filesystem::path{partial},
                                 ec);
        if (!ec) {
            fmt::println(stderr, "note: the {} frames already encoded are kept at {}", frames_,
                         partial);
        } else {
            // Same stance as write_partial_output: report, but the ORIGINAL
            // error stays the one that matters.
            fmt::println(stderr, "note: could not move the partial output to {} ({})", partial,
                         ec.message());
        }
    } else {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path{path_}, ec);
    }
}

namespace {

void put_u16(std::ostream& out, std::uint16_t value) {
    out.write(reinterpret_cast<const char*>(&value), 2);
}

void put_u32(std::ostream& out, std::uint32_t value) {
    out.write(reinterpret_cast<const char*>(&value), 4);
}

}  // namespace

bool Pcm16RawWavSink::open(std::string_view path, std::uint32_t sample_rate,
                           std::uint16_t channels) {
    path_ = std::string{path};
    data_bytes_ = 0;
    // Create/truncate first, then reopen read+write for the close()-time
    // size patch - the same two-step ac3::io::WavStreamWriter::open uses,
    // and for the same reason: `in|out|trunc` is not reliably
    // create-capable for a not-yet-existing file everywhere.
    {
        std::ofstream create{path_, std::ios::binary | std::ios::trunc};
        if (!create) {
            fmt::println(stderr, "error: cannot open {} for writing", path_);
            return false;
        }
        // Field for field ac3::io::write_wav_pcm16_raw's header (format tag
        // 1, 16-bit), sizes zero until close() patches them.
        const auto block_align = static_cast<std::uint32_t>(channels) * 2;
        create.write("RIFF", 4);
        put_u32(create, 36);
        create.write("WAVE", 4);
        create.write("fmt ", 4);
        put_u32(create, 16);
        put_u16(create, 1);  // PCM
        put_u16(create, channels);
        put_u32(create, sample_rate);
        put_u32(create, sample_rate * block_align);
        put_u16(create, static_cast<std::uint16_t>(block_align));
        put_u16(create, 16);
        create.write("data", 4);
        put_u32(create, 0);
        if (!create) {
            fmt::println(stderr, "error: cannot write to {}", path_);
            return false;
        }
    }
    file_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!file_) {
        fmt::println(stderr, "error: cannot open {} for writing", path_);
        return false;
    }
    file_.seekp(0, std::ios::end);
    open_ = true;
    return true;
}

bool Pcm16RawWavSink::push(std::span<const std::byte> bytes) {
    file_.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    if (!file_) {
        fmt::println(stderr, "error: cannot write to {}", path_);
        return false;
    }
    data_bytes_ += bytes.size();
    return true;
}

bool Pcm16RawWavSink::close() {
    open_ = false;
    const auto data_bytes = static_cast<std::uint32_t>(data_bytes_);
    file_.seekp(4, std::ios::beg);
    put_u32(file_, 36 + data_bytes);
    file_.seekp(40, std::ios::beg);
    put_u32(file_, data_bytes);
    file_.close();
    if (file_.fail()) {
        fmt::println(stderr, "error: cannot write to {}", path_);
        return false;
    }
    return true;
}

void Pcm16RawWavSink::abort() {
    if (!open_) {
        return;
    }
    open_ = false;
    file_.close();
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path{path_}, ec);
}

bool write_repeated_frame(std::string_view path, std::span<const std::byte> frame,
                          std::uint64_t count) {
    const auto emit = [&](std::ostream& out) {
        for (std::uint64_t i = 0; i < count; ++i) {
            out.write(reinterpret_cast<const char*>(frame.data()),
                      static_cast<std::streamsize>(frame.size()));
        }
        return static_cast<bool>(out);
    };
    if (is_stdio_path(path)) {
        ac3::cli::platform::set_stdio_binary();
        const bool ok = emit(std::cout);
        std::cout.flush();
        if (!ok || !std::cout) {
            fmt::println(stderr, "error: cannot write to stdout");
            return false;
        }
        return true;
    }
    std::ofstream out{std::string{path}, std::ios::binary};
    if (!out) {
        fmt::println(stderr, "error: cannot open {} for writing", path);
        return false;
    }
    return emit(out);
}

void write_partial_output(std::string_view out_path, bool keep_partial,
                          std::span<const std::vector<std::byte>> frames) {
    if (!keep_partial || frames.empty()) {
        return;
    }
    if (is_stdio_path(out_path)) {
        // "beside the intended output" (partial_output_path's naming below)
        // has no meaning for a pipe - stdout IS the intended output, and a
        // literal file called "-.partial" is not what keep-partial means
        // here. So the frames already encoded go straight to stdout instead,
        // the closest equivalent a single output stream can offer.
        if (write_frames(out_path, frames)) {
            fmt::println(stderr, "note: the {} frames already encoded were written to stdout",
                         frames.size());
        }
        return;
    }
    const auto partial = partial_output_path(out_path);
    if (write_frames(partial, frames)) {
        fmt::println(stderr, "note: the {} frames already encoded are kept at {}", frames.size(),
                     partial);
    }
}

std::vector<float> interleave_reordered(std::span<const std::vector<float>> channels,
                                        std::span<const std::size_t> order) {
    const auto frame_count = channels.empty() ? std::size_t{0} : channels.front().size();
    std::vector<float> out(frame_count * order.size());
    for (std::size_t i = 0; i < frame_count; ++i) {
        for (std::size_t ch = 0; ch < order.size(); ++ch) {
            out[i * order.size() + ch] = channels[order[ch]][i];
        }
    }
    return out;
}

std::vector<std::byte> read_all(std::string_view path) {
    if (is_stdio_path(path)) {
        // stdin's length is unknown up front, so this path keeps the
        // iterator read (and to_bytes' copy) the file branch below no
        // longer needs.
        ac3::cli::platform::set_stdio_binary();
        const std::vector<char> raw{std::istreambuf_iterator<char>(std::cin),
                                    std::istreambuf_iterator<char>()};
        return to_bytes(raw);
    }
    std::ifstream in{std::string{path}, std::ios::binary};
    if (!in) {
        return {};
    }
    // Sized read straight into the byte buffer: the iterator+to_bytes route
    // held the file twice (char copy plus byte copy) at its return point.
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (end < 0) {
        return {};
    }
    in.seekg(0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    in.read(reinterpret_cast<char*>(bytes.data()), end);
    if (in.gcount() != end) {
        return {};
    }
    return bytes;
}

std::expected<ac3::io::WavData, ac3::io::WavError> read_wav_arg(std::string_view path) {
    if (is_stdio_path(path)) {
        ac3::cli::platform::set_stdio_binary();
        return ac3::io::read_wav(std::cin);
    }
    return ac3::io::read_wav(std::string{path});
}

bool PlanarWavSink::open(std::string_view path, std::uint32_t sample_rate, std::size_t slots,
                         std::span<const std::size_t> order) {
    path_ = std::string{path};
    stdio_ = is_stdio_path(path);
    sample_rate_ = sample_rate;
    slots_.assign(slots, {});
    consumed_.assign(slots, 0);
    order_.assign(order.begin(), order.end());
    if (order_.empty()) {
        order_.resize(slots);
        for (std::size_t i = 0; i < slots; ++i) {
            order_[i] = i;
        }
    }
    if (!stdio_) {
        if (!writer_.open(path_, sample_rate, static_cast<std::uint16_t>(slots))) {
            return false;
        }
    }
    open_ = true;
    return true;
}

bool PlanarWavSink::append(std::size_t slot, std::span<const float> samples) {
    auto& buffer = slots_[slot];
    buffer.insert(buffer.end(), samples.begin(), samples.end());
    return drain();
}

std::expected<void, ac3::io::WavError> PlanarWavSink::close() {
    if (!open_) {
        return std::unexpected(ac3::io::WavError::kCannotOpen);
    }
    open_ = false;
    if (stdio_) {
        return write_wav_f32_arg(path_, slots_, sample_rate_, order_);
    }
    if (!drain()) {
        writer_.close();
        return std::unexpected(ac3::io::WavError::kCannotOpen);
    }
    for (std::size_t s = 0; s < slots_.size(); ++s) {
        if (slots_[s].size() != consumed_[s]) {
            fmt::println(stderr,
                         "warning: dropped a ragged tail the substreams never evened out");
            break;
        }
    }
    writer_.close();
    return {};
}

void PlanarWavSink::abort() {
    if (!open_) {
        return;
    }
    open_ = false;
    if (!stdio_) {
        writer_.close();
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path{path_}, ec);
    }
}

bool PlanarWavSink::drain() {
    if (stdio_) {
        return true;
    }
    std::size_t ready = std::numeric_limits<std::size_t>::max();
    for (std::size_t s = 0; s < slots_.size(); ++s) {
        ready = std::min(ready, slots_[s].size() - consumed_[s]);
    }
    if (ready == 0 || ready == std::numeric_limits<std::size_t>::max()) {
        return true;
    }
    scratch_.resize(ready * slots_.size());
    for (std::size_t pos = 0; pos < ready; ++pos) {
        for (std::size_t w = 0; w < order_.size(); ++w) {
            const auto slot = order_[w];
            scratch_[pos * order_.size() + w] = slots_[slot][consumed_[slot] + pos];
        }
    }
    if (!writer_.write(scratch_)) {
        return false;
    }
    for (std::size_t s = 0; s < slots_.size(); ++s) {
        consumed_[s] += ready;
        // Keep the carry small: once the consumed prefix dominates,
        // shift the remainder down rather than growing forever.
        if (consumed_[s] > 8192 && consumed_[s] > slots_[s].size() / 2) {
            slots_[s].erase(slots_[s].begin(),
                            slots_[s].begin() + static_cast<std::ptrdiff_t>(consumed_[s]));
            consumed_[s] = 0;
        }
    }
    return true;
}

std::string meter_bar(double db, int width) {
    std::string bar(static_cast<std::size_t>(width), '-');
    const auto filled = static_cast<int>(std::lround(ac3::analysis::meter_fraction(db) * width));
    for (int i = 0; i < filled; ++i) {
        bar[static_cast<std::size_t>(i)] = '#';
    }
    return bar;
}

void print_channel_summary(const ac3::analysis::LevelMeter& meter, FILE* out) {
    const auto acmod = meter.acmod();
    const bool lfe = meter.lfe();
    fmt::println(out, "");
    fmt::println(out, "per-channel levels ({}):", ac3::analysis::layout_name(acmod, lfe));
    fmt::println(out, "  {:<4} {:>8} {:>8}  {:<20} {}", "ch", "peak", "rms",
                "peak (-60..0 dBFS)", "clipped");
    for (int ch = 0; ch < meter.channel_count(); ++ch) {
        const auto& stats = meter.summary()[static_cast<std::size_t>(ch)];
        fmt::println(out, "  {:<4} {:>8.2f} {:>8.2f}  [{}] {}",
                     ac3::analysis::channel_name(acmod, lfe, ch), stats.peak_db(),
                     stats.rms_db(), meter_bar(stats.peak_db(), 18),
                     stats.clipped_samples > 0 ? std::to_string(stats.clipped_samples) : "-");
    }
    // The energy vector over the whole run, not the last few hundred
    // milliseconds levels() remembers: a summary line has to describe the
    // same span of audio as the table above it.
    std::vector<ac3::analysis::ChannelLevel> whole(
        static_cast<std::size_t>(meter.channel_count()));
    for (std::size_t ch = 0; ch < whole.size(); ++ch) {
        whole[ch].rms_db = meter.summary()[ch].rms_db();
    }
    const auto field = ac3::analysis::energy_vector(whole, acmod);
    if (ac3::fullbw_channel_count(acmod) >= 2 && field.magnitude > 0.0) {
        // A perfectly centred image leaves a vanishing negative y, which
        // rounds to a correct but ridiculous "-0°".
        const double azimuth = std::round(field.azimuth_deg);
        fmt::println(out, "  soundfield: {:.0f}° azimuth, focus {:.2f} (1.0 = a single speaker)",
                     azimuth == 0.0 ? 0.0 : azimuth, field.magnitude);
    }
}

void print_live_meter(const ac3::analysis::LevelMeter& meter, double seconds) {
    const bool narrow = meter.channel_count() > 2;
    const int width = narrow ? 8 : 14;
    std::string line = fmt::format("{:6.1f} s", seconds);
    for (int ch = 0; ch < meter.channel_count(); ++ch) {
        const auto& level = meter.levels()[static_cast<std::size_t>(ch)];
        line += fmt::format(
            "  {:>3} [{}]", ac3::analysis::channel_name(meter.acmod(), meter.lfe(), ch),
            meter_bar(level.peak_db, width));
        if (!narrow) {
            line += fmt::format(" {:>6.1f} {:<4}", level.peak_db, level.clipped ? "CLIP" : "");
        }
    }
    fmt::print("\r{}", line);
    // Without a newline nothing reaches the console on its own: stdout is
    // block-buffered the moment it is redirected, and a meter nobody sees
    // until the run ends is not a meter.
    (void)std::fflush(stdout);  // best-effort: a live meter with nothing left to do on failure
}

bool resolve_layout(std::string_view name, ac3::plan::Codec codec, ac3::plan::Plan& plan_out,
                    std::string& label) {
    if (const auto id = ac3::plan::parse_layout(name)) {
        if (!ac3::plan::carries(codec, *id)) {
            fmt::println(stderr, "error: {} cannot carry {} - {}", ac3::plan::codec_label(codec),
                         ac3::plan::layout(*id).label,
                         ac3::plan::describe(ac3::plan::PlanError::kLayoutNeedsEac3));
            return false;
        }
        plan_out.layout = *id;
        plan_out.custom_locations = std::nullopt;
        label = std::string(ac3::plan::layout(*id).label);
        return true;
    }
    const auto custom = ac3::plan::parse_channels(name);
    if (!custom) {
        fmt::println(stderr, "error: unknown layout '{}' ({})", name,
                     ac3::plan::layout_names(codec));
        return false;
    }
    const auto allocated = ac3::eac3::chanmap::allocate(*custom);
    if (!allocated) {
        fmt::println(stderr, "error: channel selection '{}' is invalid - {}", name,
                     ac3::eac3::chanmap::describe(allocated.error()));
        return false;
    }
    if (codec == ac3::plan::Codec::kAc3 && !allocated->dependents.empty()) {
        fmt::println(stderr, "error: {} cannot carry '{}' - {}", ac3::plan::codec_label(codec),
                     name, ac3::plan::describe(ac3::plan::PlanError::kLayoutNeedsEac3));
        return false;
    }
    plan_out.custom_locations = custom;
    label = ac3::plan::format_channels(*custom);
    return true;
}

std::optional<ac3::SampleRate> wav_sample_rate(std::uint32_t hz, std::string_view codec,
                                               bool eac3) {
    switch (hz) {
        case 48000: return ac3::SampleRate::k48000;
        case 44100: return ac3::SampleRate::k44100;
        case 32000: return ac3::SampleRate::k32000;
        case 24000: if (eac3) return ac3::SampleRate::k24000; break;
        case 22050: if (eac3) return ac3::SampleRate::k22050; break;
        case 16000: if (eac3) return ac3::SampleRate::k16000; break;
        default: break;
    }
    fmt::println(stderr, "error: sample rate {} is not legal for {} (need {})", hz, codec,
                eac3 ? "32/44.1/48 kHz, or 16/22.05/24 kHz" : "32/44.1/48 kHz");
    return std::nullopt;
}

std::optional<ac3::plan::Routing> routing_or_error(const ac3::plan::Plan& p, std::size_t channels) {
    auto routing = plan::route(plan::resolve(p), channels, p.meta.cmixlev, p.meta.surmixlev);
    if (!routing) {
        fmt::println(stderr, "error: {} channels - {}", channels,
                     plan::describe(plan::PlanError::kNoSourceLayout));
        return std::nullopt;
    }
    return routing;
}

std::optional<ac3::signing::VerifySummary> apply_object_verification(
    std::span<const std::byte> stream, const Options& meta) {
    if (!meta.verify_objects) {
        return ac3::signing::VerifySummary{};
    }
    const auto key = ac3::signing::load_signing_key(meta.signing_key.value_or(""));
    if (!key) {
        if (key.error().kind == ac3::signing::KeyErrorKind::kAbsent) {
            fmt::println(stderr,
                         "error: verify-objects needs a key — pass signing-key=<path>, or set "
                         "AC3FORGE_SIGNING_KEY_FILE / AC3FORGE_SIGNING_KEY");
        } else {
            fmt::println(stderr, "error: {}", key.error().message);
        }
        return std::nullopt;
    }
    const auto summary = ac3::signing::verify_atmos_stream(stream, *key);
    fmt::println("  object signature: {} valid, {} mismatched, {} unsigned frame(s)",
                 summary.valid, summary.mismatch, summary.no_container);
    if (summary.mismatch > 0) {
        fmt::println(stderr,
                     "error: object signature verification failed ({} of {} signed frames did "
                     "not match the supplied key)",
                     summary.mismatch, summary.valid + summary.mismatch);
        return std::nullopt;
    }
    return summary;
}

}  // namespace ac3cli

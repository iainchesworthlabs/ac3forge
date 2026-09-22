#include "support.hpp"

#include <algorithm>
#include <array>
#include <cassert>
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
#include "ac3/io/wav.hpp"
#include "ac3/meta/bsi.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/meta/mixing.hpp"
#include "ac3/meta/qc.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/quality/distortion.hpp"
#include "ac3/signing/emdf_atmos_signer.hpp"
#include "ac3/signing/signing_key.hpp"
#include "container_input.hpp"
#include "platform/stdio_binary.hpp"
#include "recording_sink.hpp"
#include "usage.hpp"

namespace ac3cli {

namespace plan = ac3::plan;

namespace {

bool parse_double(std::string_view text, double& out) {
    // from_chars for floating point needs the locale-independent form, which
    // is what a command line gives.
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

// A whole non-negative integer with nothing else in the token, so "3x" and
// "-1" are refused rather than silently becoming 3 and a fallback.
bool parse_index(std::string_view text, int high, int& out) {
    int value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() || value < 0 || value > high) {
        return false;
    }
    out = value;
    return true;
}

// Split on `sep`, keeping empty pieces - "3,,5" has to fail rather than
// quietly become two values.
std::vector<std::string_view> split(std::string_view text, char sep) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const auto at = text.find(sep, start);
        if (at == std::string_view::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, at - start));
        start = at + 1;
    }
}

// Tables D2.3-D2.6's eight levels, spelled as the decibel figure the table
// prints. "off" is the -inf row, which is a real value there rather than an
// absent field.
bool parse_mix_level(std::string_view text, ac3::meta::MixLevel& out) {
    static constexpr std::array<std::pair<std::string_view, ac3::meta::MixLevel>, 8> kLevels{{
        {"+3", ac3::meta::MixLevel::kPlus3dB},
        {"+1.5", ac3::meta::MixLevel::kPlus1_5dB},
        {"0", ac3::meta::MixLevel::kUnity},
        {"-1.5", ac3::meta::MixLevel::kMinus1_5dB},
        {"-3", ac3::meta::MixLevel::kMinus3dB},
        {"-4.5", ac3::meta::MixLevel::kMinus4_5dB},
        {"-6", ac3::meta::MixLevel::kMinus6dB},
        {"off", ac3::meta::MixLevel::kSilent},
    }};
    for (const auto& [name, level] : kLevels) {
        if (name == text) {
            out = level;
            return true;
        }
    }
    return false;
}

// §E2.3.1.13: code 0 is mute, 1..63 are -50..+12 dB in 1 dB steps. Taken as
// the decibel figure rather than the code, since that is what a mixing desk
// shows and the mapping is exact either way.
bool parse_pgm_scale(std::string_view text, int& out) {
    if (text == "mute") {
        out = ac3::meta::kPgmScaleMute;
        return true;
    }
    // A leading + is how a signed decibel figure reads on a mixing desk and
    // in this option's own documentation, but from_chars (parse_double) does
    // not accept one - it is not part of the grammar C++ gives it.
    if (text.starts_with("+")) {
        text.remove_prefix(1);
    }
    double db = 0.0;
    if (!parse_double(text, db) || db < -50.0 || db > 12.0) {
        return false;
    }
    const auto code = static_cast<int>(std::lround(db)) + 51;
    if (code < 1 || code > ac3::meta::kPgmScaleMax) {
        return false;
    }
    out = code;
    return true;
}

// "<dynrng|compr>:<external|local>:<0..7>" - §E2.3.1.19-21's three fields,
// which always travel together and so take one token.
bool parse_premix(std::string_view text, ac3::meta::PremixCompression& out) {
    const auto parts = split(text, ':');
    if (parts.size() != 3) {
        return false;
    }
    if (parts[0] == "dynrng") {
        out.premixcmpsel = ac3::meta::PremixCompressionSource::kDynrng;
    } else if (parts[0] == "compr") {
        out.premixcmpsel = ac3::meta::PremixCompressionSource::kCompr;
    } else {
        return false;
    }
    if (parts[1] == "external") {
        out.drcsrc = ac3::meta::DrcSource::kExternal;
    } else if (parts[1] == "local") {
        out.drcsrc = ac3::meta::DrcSource::kThisSubstream;
    } else {
        return false;
    }
    return parse_index(parts[2], 7, out.premixcmpscl);
}

// A comma-separated list of Table E2.8 codes, "off" for a channel the
// external programme does not have (§E2.3.1.25's own reading of a clear
// flag), which is not the same as a code of 0 dB.
bool parse_scale_list(std::string_view text, std::vector<std::optional<int>>& out) {
    out.clear();
    for (const auto part : split(text, ',')) {
        if (part == "off") {
            out.emplace_back();
            continue;
        }
        int code = 0;
        if (!parse_index(part, 15, code)) {
            return false;
        }
        out.emplace_back(code);
    }
    return true;
}

// "<spchdat>[,<spchdat1>:<spchan1att>[,<spchdat2>:<spchan2att>]]" - the
// nesting is §E2.3.1.44-51's own, each stage present only when the one above
// it is.
bool parse_speech(std::string_view text, ac3::meta::SpeechEnhancement& out) {
    const auto parts = split(text, ',');
    if (parts.empty() || parts.size() > 3) {
        return false;
    }
    if (!parse_index(parts[0], 31, out.spchdat)) {
        return false;
    }
    if (parts.size() == 1) {
        return true;
    }
    const auto pair = [](std::string_view piece, int data_high, int att_high, int& data,
                         int& att) {
        const auto halves = split(piece, ':');
        return halves.size() == 2 && parse_index(halves[0], data_high, data) &&
               parse_index(halves[1], att_high, att);
    };
    ac3::meta::SpeechEnhancement::Additional additional;
    if (!pair(parts[1], 31, 3, additional.spchdat1, additional.spchan1att)) {
        return false;
    }
    if (parts.size() == 3) {
        ac3::meta::SpeechEnhancement::Additional::More more;
        if (!pair(parts[2], 31, 7, more.spchdat2, more.spchan2att)) {
            return false;
        }
        additional.more = more;
    }
    out.additional = additional;
    return true;
}

// "<panmean>[:<paninfo>]" - the 6-bit paninfo half is reserved
// (§E2.3.1.55), so it defaults to zero and is rarely worth naming.
bool parse_pan(std::string_view text, ac3::meta::PanInfo& out) {
    const auto parts = split(text, ':');
    if (parts.size() > 2) {
        return false;
    }
    if (!parse_index(parts[0], ac3::meta::kPanMeanMax, out.panmean)) {
        return false;
    }
    return parts.size() == 1 || parse_index(parts[1], 63, out.paninfo);
}

// Six comma-separated 5-bit words, "-" for a block whose blkmixcfginfoe stays
// clear.
bool parse_block_mix_config(std::string_view text,
                            std::array<std::optional<int>, ac3::kBlocksPerFrame>& out) {
    const auto parts = split(text, ',');
    if (parts.size() != out.size()) {
        return false;
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (parts[i] == "-") {
            out[i].reset();
            continue;
        }
        int word = 0;
        if (!parse_index(parts[i], 31, word)) {
            return false;
        }
        out[i] = word;
    }
    return true;
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

// --- verbosity ------------------------------------------------------------
// One pair of file-scope flags rather than a field on Options threaded to
// every printer: `quiet`/`verbose` describe the invocation, not any one
// command's arguments, and main() settles both before the first handler runs
// (see set_verbosity's own header comment in support.hpp).
namespace {
bool g_quiet = false;
bool g_verbose = false;
}  // namespace

void set_verbosity(bool quiet, bool verbose) {
    // quiet wins if somebody passes both: "print nothing" is the safer
    // reading of a contradictory command line for a tool whose stdout may be
    // carrying a bitstream.
    g_quiet = quiet;
    g_verbose = verbose && !quiet;
}

double parse_seconds_or(std::string_view text, double fallback) {
    double value = 0.0;
    return parse_double(text, value) ? value : fallback;
}

bool verbose_mode() { return g_verbose; }

bool quiet_mode() { return g_quiet; }

// A run this long or longer prints the progress line without being asked -
// 500 access units is 16 s of audio at 48 kHz, past the point where a silent
// terminal starts to look like a hang. Shorter runs stay silent unless
// `verbose` asks, so the ordinary two-second encode is as quiet as it was.
constexpr std::uint64_t kProgressUnits = 500;

// How often the line is rewritten. Wall clock, not a frame count: what makes
// a progress line readable is a steady refresh rate, and a frame takes wildly
// different amounts of time across bitrates, layouts and tool sets.
constexpr std::chrono::milliseconds kProgressInterval{100};

void Progress::start(std::string_view verb, std::uint64_t total) {
    active_ = !quiet_mode() && (verbose_mode() || total >= kProgressUnits);
    verb_ = std::string{verb};
    total_ = total;
    done_ = 0;
    last_ = std::chrono::steady_clock::now();
}

void Progress::tick(std::uint64_t done) {
    done_ = done;
    if (!active_) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_ < kProgressInterval) {
        return;
    }
    last_ = now;
    if (total_ > 0) {
        fmt::print(stderr, "\r  {} {:>8} / {} units ({:>3}%)   ", verb_, done_, total_,
                   done_ * 100 / total_);
    } else {
        fmt::print(stderr, "\r  {} {:>8} units   ", verb_, done_);
    }
    // Same reason print_live_meter flushes: stderr is unbuffered on most
    // platforms but not guaranteed to be, and a progress line nobody sees
    // until the run ends is not a progress line.
    (void)std::fflush(stderr);
}

void Progress::finish() {
    if (!active_) {
        return;
    }
    active_ = false;
    if (total_ > 0) {
        fmt::println(stderr, "\r  {} {:>8} / {} units (100%)   ", verb_, done_, total_);
    } else {
        fmt::println(stderr, "\r  {} {:>8} units   ", verb_, done_);
    }}

bool parse_options(std::span<char*> tokens, Options& out, std::string_view command) {
    for (char* raw : tokens) {
        const std::string_view token{raw};
        const auto eq = token.find('=');
        const std::string_view key = token.substr(0, eq);
        const std::string_view value =
            eq == std::string_view::npos ? std::string_view{} : token.substr(eq + 1);

        if (token == "quiet" || token == "verbose") {
            // Recorded on Options for a command that wants to reason about
            // them (run_live names its legs only when verbose), but the
            // printers themselves read the file-scope flags set_verbosity
            // settles - see support.hpp.
            (token == "quiet" ? out.quiet : out.verbose) = true;
            continue;
        }
        if (token == "fallback-51") {
            out.hls_fallback_51 = true;
            continue;
        }
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
        if (token == "annexd") {
            out.p.annexd = true;
            continue;
        }
        if (token == "infomdat") {
            out.p.infomdat = true;
            continue;
        }
        if (token == "encinfo") {
            out.p.annexd = true;
            out.p.encinfo = true;
            continue;
        }
        if (token == "langcod" || token == "langcod2") {
            (token == "langcod" ? out.p.info.langcod : out.p.info.langcod2) = true;
            continue;
        }
        if (token == "copyright") {
            out.p.infomdat = true;
            out.p.info.copyrightb = true;
            continue;
        }
        if (token == "sourcefscod") {
            out.p.infomdat = true;
            out.p.info.sourcefscod = true;
            continue;
        }
        if (token == "fast-imdct") {
            out.fast_imdct = true;
            continue;
        }
        if (key == "mainid") {
            // A/52 Table A4.6 / EN 300 468 D.3: a number 0-7 naming a main
            // audio service, which associated services then point at.
            unsigned parsed = 0;
            const auto [ptr, ec] =
                std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (ec != std::errc{} || ptr != value.data() + value.size() || parsed > 7) {
                fmt::println(stderr, "error: mainid must be 0-7 (got '{}')", token);
                return false;
            }
            out.mainid = static_cast<int>(parsed);
            continue;
        }
        if (key == "asvc") {
            // Eight bits, one per main service this associated service may be
            // reproduced with; bit 7 is main service 7. Accepts decimal or
            // 0x-prefixed hex, since it reads as a mask far more often than
            // as a number.
            const bool hex = value.starts_with("0x") || value.starts_with("0X");
            const std::string_view digits = hex ? value.substr(2) : value;
            unsigned parsed = 0;
            const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(),
                                                   parsed, hex ? 16 : 10);
            if (ec != std::errc{} || ptr != digits.data() + digits.size() || parsed > 255) {
                fmt::println(stderr, "error: asvc must be 0-255 or 0x00-0xFF (got '{}')", token);
                return false;
            }
            out.asvc = static_cast<int>(parsed);
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
        if (token == "verify") {
            out.verify = true;
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
        if (key == "joc-domain") {
            if (value == "qmf") {
                out.joc_domain = ac3::oba::joc::Domain::kQmf;
                continue;
            }
            if (value == "mdct") {
                out.joc_domain = ac3::oba::joc::Domain::kMdctBand;
                continue;
            }
            fmt::println(stderr,
                         "error: joc-domain is 'qmf' (the default, §7.1's complex filterbank) "
                         "or 'mdct' (the 256-bin approximation) (got '{}')",
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
        if (key == "fgaincod") {
            // §7.2.2.4 fast gain, Table 7.11 - search='s other axis, held for
            // a whole encode instead of chosen per frame. Not scoped to one
            // command: both codecs have the field and every encoding command
            // routes through plan::Tools, the same reach dither= and search=
            // already have.
            //
            // 'auto' spells the default rather than -1 doing it alone, since
            // "auto" is what the two codecs' automatic behaviours have in
            // common and not a number either of them uses (AC-3 follows the
            // measured curve, E-AC-3 leaves Table E1.4's implied 0x4 and
            // writes no element - see Options::fgaincod). -1 is accepted as
            // the same thing spelled the way the library field is.
            if (value == "auto" || value == "-1") {
                out.fgaincod = -1;
                continue;
            }
            unsigned parsed = 0;
            const auto [ptr, ec] =
                std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (ec != std::errc{} || ptr != value.data() + value.size() || parsed > 7) {
                fmt::println(stderr,
                             "error: fgaincod must be 'auto' or 0-7 (Table 7.11) (got '{}')",
                             token);
                return false;
            }
            out.fgaincod = static_cast<int>(parsed);
            continue;
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
            // Two unrelated commands share this key: live's on/off toggle for
            // the parallel AC-3 downmix leg (§ record/live take options), and
            // decode/monitor's §7.8 output-stage fold target. Their value
            // spaces do not overlap, so the value itself disambiguates.
            if (value == "on") {
                out.downmix_leg = true;
            } else if (value == "off") {
                out.downmix_leg = false;
            } else if (value == "loro") {
                out.output.target = ac3::DownmixTarget::kLoRo;
                out.downmix_named = true;
            } else if (value == "ltrt") {
                out.output.target = ac3::DownmixTarget::kLtRt;
                out.downmix_named = true;
            } else if (value == "mono") {
                out.output.target = ac3::DownmixTarget::kMono;
                out.downmix_named = true;
            } else {
                fmt::println(stderr,
                             "error: downmix is 'on'/'off' (live) or 'loro' (§7.8.1)/'ltrt' "
                             "(§7.8.2, Dolby Surround compatible)/'mono' (decode/monitor) "
                             "(got '{}')",
                             token);
                return false;
            }
            continue;
        }
        if (key == "follow") {
            // 'play' only: the sink-following fallback (roadmap UX9) toggle.
            // Unlike downmix=, no other command reads this key, so there is
            // no value space to disambiguate against.
            if (value == "on") {
                out.follow_sink = true;
            } else if (value == "off") {
                out.follow_sink = false;
            } else {
                fmt::println(stderr, "error: follow is 'on'/'off' (got '{}')", token);
                return false;
            }
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
                out.dialnorm_given = true;
                continue;
            }
            const auto n = parse_u32_or(value, 0);
            if (n < 1 || n > 31) {
                fmt::println(stderr, "error: dialnorm must be auto or 1..31 (§5.4.2.8)");
                return false;
            }
            out.p.dialnorm = static_cast<int>(n);
            out.dialnorm_given = true;
            continue;
        }
        if (key == "dialnorm2") {
            if (value == "auto") {
                out.p.measure_dialnorm2 = true;
                out.dialnorm2_given = true;
                continue;
            }
            const auto n = parse_u32_or(value, 0);
            if (n < 1 || n > 31) {
                fmt::println(stderr, "error: dialnorm2 must be auto or 1..31 (§5.4.2.16)");
                return false;
            }
            out.p.dialnorm2 = static_cast<int>(n);
            out.dialnorm2_given = true;
            continue;
        }
        if (key == "compr" || key == "compr2") {
            // A dB gain, converted to §7.7.2's own 8-bit word. Rounded DOWN
            // (encode_compr_at_most) rather than to nearest, for the reason
            // ac3/meta/drc.hpp gives: §7.7.2 exists to give "an assured upper
            // limit", and a ceiling exceeded by half a step is not assured.
            double db = 0.0;
            if (!parse_double(value, db)) {
                fmt::println(stderr, "error: {} takes a gain in dB (got '{}')", key, value);
                return false;
            }
            const auto word = ac3::meta::encode_compr_at_most(db);
            if (key == "compr") {
                out.compr_word = word;
            } else {
                out.compr2_word = word;
            }
            continue;
        }
        // bsmod/dsurmod are read by two different consumers: `metadata`/
        // `transcode` (the raw code, `out.bsmod`/`out.dsurmod`, straight off
        // Table 5.5/5.11) and `encode`/`eac3-encode` (the same code wrapped
        // in `ac3::meta::BitstreamMode`/`SurroundMode` for the Plan below).
        // One parse feeds both, so a value valid for one is valid for the
        // other and the two consumers can never disagree about what was
        // typed.
        if (key == "bsmod") {
            ac3::meta::BitstreamMode mode{};
            // parse_bsmod already accepts the raw Table 5.7 code as well as
            // the named service tokens - see its own comment.
            if (!ac3::meta::parse_bsmod(value, mode)) {
                fmt::println(stderr, "error: bsmod must be 0..7 (Table 5.5's service type) "
                                     "or one of: {}",
                             ac3::meta::kBsmodNames);
                return false;
            }
            out.bsmod = static_cast<int>(mode);
            out.p.infomdat = true;
            out.p.info.bsmod = mode;
            continue;
        }
        if (key == "dsurmod") {
            // Table 5.11's own 2-bit field, including its reserved code 3 -
            // parse_surround_mode has no member for that (§5.4.2.6 reads it
            // as "not indicated", same as 0), so the raw digit is read
            // directly rather than routed through the named-token parser.
            const auto n = parse_u32_or(value, 4);
            ac3::meta::SurroundMode mode{};
            if (n <= 3) {
                mode = n < 3 ? static_cast<ac3::meta::SurroundMode>(n)
                             : ac3::meta::SurroundMode::kNotIndicated;
            } else if (!ac3::meta::parse_surround_mode(value, mode)) {
                fmt::println(stderr, "error: dsurmod must be 0..3 (Table 5.11's Dolby Surround "
                                     "mode) or one of: {}",
                             ac3::meta::kSurroundModeNames);
                return false;
            }
            out.dsurmod = n <= 3 ? static_cast<int>(n) : static_cast<int>(mode);
            out.p.infomdat = true;
            out.p.info.dsurmod = mode;
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
            // On E-AC-3 the preferred downmix rides mixmdate; on AC-3 it has
            // nowhere to go but Annex D's xbsi1, so naming it asks for both
            // and each codec path reads only its own flag.
            out.p.mixmeta = true;
            out.p.annexd = true;
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
        if (key == "ltrtcmixlev" || key == "lorocmixlev" || key == "ltrtsurmixlev" ||
            key == "lorosurmixlev") {
            ac3::meta::MixLevel level{};
            if (!parse_mix_level(value, level)) {
                fmt::println(stderr,
                             "error: {} must be +3, +1.5, 0, -1.5, -3, -4.5, -6 or off "
                             "(Tables D2.3-D2.6)",
                             key);
                return false;
            }
            const bool surround = key == "ltrtsurmixlev" || key == "lorosurmixlev";
            // Tables D2.4/D2.6 reserve the three loudest surround codes, and a
            // decoder receiving one substitutes 0.841 - so the level asked for
            // is not the level applied. Refuse rather than write it.
            if (surround && !ac3::meta::valid_surround_mix_level(level)) {
                fmt::println(stderr,
                             "error: {} must be -1.5, -3, -4.5, -6 or off - Tables D2.4/D2.6 "
                             "reserve the three louder codes",
                             key);
                return false;
            }
            out.p.mixmeta = true;
            out.p.annexd = true;
            if (key == "ltrtcmixlev") {
                out.p.ltrtcmixlev = level;
            } else if (key == "lorocmixlev") {
                out.p.lorocmixlev = level;
            } else if (key == "ltrtsurmixlev") {
                out.p.ltrtsurmixlev = level;
            } else {
                out.p.lorosurmixlev = level;
            }
            continue;
        }
        if (key == "dsurexmod" || key == "dheadphonmod" || key == "adconvtyp") {
            // All three live in E-AC-3's infomdat and in AC-3's xbsi2, so
            // naming one asks for whichever element this codec has.
            out.p.infomdat = true;
            out.p.annexd = true;
            bool ok = false;
            if (key == "dsurexmod") {
                ok = ac3::meta::parse_surround_ex_mode(value, out.p.info.dsurexmod);
            } else if (key == "dheadphonmod") {
                ok = ac3::meta::parse_headphone_mode(value, out.p.info.dheadphonmod);
            } else {
                ok = ac3::meta::parse_ad_converter(value, out.p.adconvtyp);
            }
            if (!ok) {
                fmt::println(stderr, "error: {} must be one of: {}", key,
                             key == "dsurexmod"      ? ac3::meta::kSurroundExModeNames
                             : key == "dheadphonmod" ? ac3::meta::kHeadphoneModeNames
                                                     : ac3::meta::kAdConverterNames);
                return false;
            }
            continue;
        }
        if (key == "codec" && command == "transcode") {
            // 'transcode' only - disambiguated from record/live's own codec=
            // below the same way layout= is, a few blocks down. Named rather
            // than inferred when out_path is "-" or has no .ac3/.ec3 suffix
            // to read - see Options::codec.
            if (value == "ac3") {
                out.codec = ac3::plan::Codec::kAc3;
            } else if (value == "eac3" || value == "ec3") {
                out.codec = ac3::plan::Codec::kEac3;
            } else {
                fmt::println(stderr, "error: codec must be ac3 or eac3 (got '{}')", value);
                return false;
            }
            continue;
        }
        if (key == "mixlevel" || key == "mixlevel2") {
            const auto db = parse_u32_or(value, 0);
            if (db < 80 || db > 111) {
                fmt::println(stderr,
                             "error: {} is a peak mixing level of 80..111 dB SPL (§5.4.2.14)",
                             key);
                return false;
            }
            out.p.infomdat = true;
            auto& production = key == "mixlevel" ? out.p.info.audprod : out.p.info.audprod2;
            if (!production) {
                production.emplace();
            }
            production->mixlevel = static_cast<int>(db) - ac3::meta::kMixLevelBaseDbSpl;
            continue;
        }
        if (key == "roomtyp" || key == "roomtyp2") {
            ac3::meta::RoomType room{};
            if (!ac3::meta::parse_room_type(value, room)) {
                fmt::println(stderr, "error: {} must be one of: {} (Table 5.12)", key,
                             ac3::meta::kRoomTypeNames);
                return false;
            }
            out.p.infomdat = true;
            auto& production = key == "roomtyp" ? out.p.info.audprod : out.p.info.audprod2;
            if (!production) {
                production.emplace();
            }
            production->roomtyp = room;
            continue;
        }
        if (key == "origbs") {
            out.p.infomdat = true;
            if (value == "on") {
                out.p.info.origbs = true;
            } else if (value == "off") {
                out.p.info.origbs = false;
            } else {
                fmt::println(stderr, "error: origbs must be on or off (§5.4.2.25)");
                return false;
            }
            continue;
        }
        if (key == "timecode") {
            ac3::meta::TimeCodeCoarse coarse;
            ac3::meta::TimeCodeFine fine;
            if (!ac3::meta::parse_timecode(value, coarse, fine)) {
                fmt::println(stderr, "error: timecode is {} (§5.4.2.26-28)",
                             ac3::meta::kTimeCodeSyntax);
                return false;
            }
            out.p.info.timecod1 = coarse;
            out.p.info.timecod2 = fine;
            continue;
        }
        if (key == "pgmscl" || key == "pgmscl2" || key == "extpgmscl") {
            int code = 0;
            if (!parse_pgm_scale(value, code)) {
                fmt::println(stderr,
                             "error: {} is mute or a level in -50..+12 dB (§E2.3.1.13)", key);
                return false;
            }
            out.p.mixmeta = true;
            if (key == "pgmscl") {
                out.p.mixdepth.pgmscl = code;
            } else if (key == "pgmscl2") {
                out.p.mixdepth.pgmscl2 = code;
            } else {
                out.p.mixdepth.extpgmscl = code;
            }
            continue;
        }
        if (key == "mixdef") {
            out.p.mixmeta = true;
            if (value == "none") {
                out.p.mixdepth.mixing.mixdef = ac3::meta::MixDefinition::kNone;
            } else if (value == "premix") {
                out.p.mixdepth.mixing.mixdef = ac3::meta::MixDefinition::kPremix;
            } else if (value == "reserved") {
                out.p.mixdepth.mixing.mixdef = ac3::meta::MixDefinition::kReserved;
            } else if (value == "ext") {
                out.p.mixdepth.mixing.mixdef = ac3::meta::MixDefinition::kExtended;
            } else {
                fmt::println(stderr,
                             "error: mixdef must be none, premix, reserved or ext "
                             "(Table E2.6)");
                return false;
            }
            continue;
        }
        if (key == "premixcmp") {
            ac3::meta::PremixCompression premix;
            if (!parse_premix(value, premix)) {
                fmt::println(stderr,
                             "error: premixcmp is <dynrng|compr>:<external|local>:<0..7> "
                             "(§E2.3.1.19-21)");
                return false;
            }
            out.p.mixmeta = true;
            out.p.mixdepth.mixing.premix = premix;
            // mixdef 0x3 carries its own copy inside mixdata2e, so the value
            // has to reach whichever of the two the mixdef= token selects.
            if (!out.p.mixdepth.mixing.external) {
                out.p.mixdepth.mixing.external.emplace();
            }
            out.p.mixdepth.mixing.external->premix = premix;
            continue;
        }
        if (key == "mixdata") {
            const auto bits = parse_u32_or(value, 0xFFFF);
            if (bits > 0x0FFF) {
                fmt::println(stderr,
                             "error: mixdata is the twelve bits mixdef=reserved reserves, "
                             "0..4095 (§E2.3.1.23)");
                return false;
            }
            out.p.mixmeta = true;
            out.p.mixdepth.mixing.reserved = static_cast<std::uint16_t>(bits);
            continue;
        }
        if (key == "extmix" || key == "auxmix") {
            std::vector<std::optional<int>> scales;
            const std::size_t wanted = key == "extmix" ? 6 : 2;
            if (!parse_scale_list(value, scales) || scales.size() < wanted ||
                scales.size() > wanted + (key == "extmix" ? 1 : 0)) {
                fmt::println(stderr,
                             "error: {} takes {} Table E2.8 codes (0..15 or 'off'){}", key,
                             wanted,
                             key == "extmix" ? ", optionally a seventh for the downmix scale"
                                             : "");
                return false;
            }
            out.p.mixmeta = true;
            if (!out.p.mixdepth.mixing.external) {
                out.p.mixdepth.mixing.external.emplace();
            }
            auto& external = *out.p.mixdepth.mixing.external;
            if (key == "extmix") {
                external.left = scales[0];
                external.centre = scales[1];
                external.right = scales[2];
                external.left_surround = scales[3];
                external.right_surround = scales[4];
                external.lfe = scales[5];
                external.dmixscl = scales.size() > 6 ? scales[6] : std::nullopt;
            } else {
                external.auxiliary = std::array<std::optional<int>, 2>{scales[0], scales[1]};
            }
            continue;
        }
        if (key == "speechmix") {
            ac3::meta::SpeechEnhancement speech;
            if (!parse_speech(value, speech)) {
                fmt::println(stderr,
                             "error: speechmix is <0..31>[,<0..31>:<0..3>[,<0..31>:<0..7>]] "
                             "(§E2.3.1.44-51)");
                return false;
            }
            out.p.mixmeta = true;
            out.p.mixdepth.mixing.speech = speech;
            continue;
        }
        if (key == "paninfo" || key == "paninfo2") {
            ac3::meta::PanInfo pan;
            if (!parse_pan(value, pan)) {
                fmt::println(stderr,
                             "error: {} is <0..239>[:<0..63>] - 1.5 degree steps clockwise "
                             "from centre (§E2.3.1.54)",
                             key);
                return false;
            }
            out.p.mixmeta = true;
            (key == "paninfo" ? out.p.mixdepth.pan : out.p.mixdepth.pan2) = pan;
            continue;
        }
        if (key == "blkmixcfg") {
            std::array<std::optional<int>, ac3::kBlocksPerFrame> words{};
            if (!parse_block_mix_config(value, words)) {
                fmt::println(stderr,
                             "error: blkmixcfg is six comma-separated 0..31 words, '-' for a "
                             "block that sends none (§E2.3.1.59-61)");
                return false;
            }
            out.p.mixmeta = true;
            out.p.mixdepth.blkmixcfginfo = words;
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
            // The same five containers RecordingSink streams incrementally,
            // shared verbatim with the GUI's own Container combo for a live
            // take (roadmap IO9). Plain mp4 is deliberately absent: moov/stco
            // need every frame's final offset, so the standalone 'mp4'
            // command wraps an already-finished file instead ('ts' IS
            // streamable, hence its own token below).
            if (value == "raw") {
                out.container = RecordingSink::Container::kElementary;
            } else if (value == "mkv" || value == "matroska") {
                out.container = RecordingSink::Container::kMatroska;
            } else if (value == "ts" || value == "mpegts") {
                out.container = RecordingSink::Container::kMpegts;
            } else if (value == "spdif") {
                out.container = RecordingSink::Container::kSpdif;
            } else if (value == "fmp4" || value == "cmaf") {
                out.container = RecordingSink::Container::kFmp4;
            } else {
                fmt::println(stderr,
                             "error: container must be raw, mkv, ts, spdif or fmp4 (got '{}')",
                             token);
                return false;
            }
            continue;
        }
        if (key == "layout" && command == "qc") {
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
        if (key == "objects" && command == "qc") {
            const auto id = ac3::plan::parse_layout(value);
            if (!id) {
                fmt::println(stderr, "error: objects layout '{}' not recognised ({})", value,
                             ac3::plan::layout_names());
                return false;
            }
            out.qc_objects_layout = id;
            continue;
        }
        if (key == "layout") {
            // record/live only. Validated where it is used rather than here:
            // whether a layout is legal depends on the codec, which codec=
            // (below, and possibly later on the command line) can still
            // change - and resolve_layout already reports a bad token
            // against the set the codec can actually carry.
            if (value.empty()) {
                fmt::println(stderr, "error: layout= needs a layout name or channel list");
                return false;
            }
            out.take_layout = std::string{value};
            continue;
        }
        if (key == "codec") {
            if (value == "ac3") {
                out.take_codec = plan::Codec::kAc3;
            } else if (value == "eac3" || value == "ec3") {
                out.take_codec = plan::Codec::kEac3;
            } else {
                fmt::println(stderr, "error: codec must be ac3 or eac3 (got '{}')", token);
                return false;
            }
            continue;
        }
        if (key == "watchdog") {
            double seconds = 0.0;
            if (!parse_double(value, seconds) || seconds < 0.0 || seconds > 3600.0) {
                fmt::println(stderr,
                             "error: watchdog= needs a timeout in seconds (0 disables, "
                             "3600 max)");
                return false;
            }
            out.watchdog =
                std::chrono::milliseconds{static_cast<std::int64_t>(seconds * 1000.0)};
            continue;
        }
        if (key == "objects") {
            const auto n = parse_u32_or(value, 0);
            if (n < 1 || n > 15) {
                fmt::println(stderr,
                             "error: objects= needs 1 to 15 slots (the bed's LFE is the 16th, "
                             "and TS 103 420 §8.3.2.2 caps the total at 16)");
                return false;
            }
            out.live_objects = static_cast<std::size_t>(n);
            continue;
        }
        if (key == "positions") {
            // <scheme>:[<bind>:]<port> - see PositionSourceSpec's own
            // comment in support.hpp for the grammar and why it is
            // scheme-prefixed. Split on the FIRST ':' for the scheme, then
            // (if a second ':' remains) the LAST ':' for bind vs port - an
            // IPv4 dotted-quad has no colons of its own, so this never
            // misreads one as part of the port.
            const auto scheme_end = value.find(':');
            if (scheme_end == std::string_view::npos) {
                fmt::println(stderr, "error: positions= needs a scheme (positions=osc:<port>)");
                return false;
            }
            const auto scheme = value.substr(0, scheme_end);
            if (scheme != "osc") {
                fmt::println(stderr,
                             "error: positions= scheme must be 'osc' (got '{}'; MIDI and a "
                             "game controller are not implemented yet)",
                             scheme);
                return false;
            }
            const auto rest = value.substr(scheme_end + 1);
            std::string_view bind_token = "local";
            std::string_view port_token = rest;
            if (const auto bind_end = rest.rfind(':'); bind_end != std::string_view::npos) {
                bind_token = rest.substr(0, bind_end);
                port_token = rest.substr(bind_end + 1);
            }
            std::string bind_address;
            if (bind_token == "local") {
                bind_address = "127.0.0.1";
            } else if (bind_token == "any") {
                bind_address = "0.0.0.0";
            } else {
                bind_address = std::string{bind_token};
            }
            std::uint32_t port_value = 0;
            const auto [ptr, ec] =
                std::from_chars(port_token.data(), port_token.data() + port_token.size(), port_value);
            if (ec != std::errc{} || ptr != port_token.data() + port_token.size() ||
                port_value < 1 || port_value > 65535) {
                fmt::println(stderr,
                             "error: positions=osc:[local|any|<ipv4>:]<port> needs a port from "
                             "1 to 65535");
                return false;
            }
            out.positions = PositionSourceSpec{.scheme = std::string{scheme},
                                               .bind = std::move(bind_address),
                                               .port = static_cast<std::uint16_t>(port_value)};
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
        if (key == "programme") {
            // §E2.3.1.2 numbers independent substreams I0-I7, so the id is
            // the whole of what selects a programme - there is no separate
            // index. Checked against what the stream actually carries by the
            // command itself, which is the only place that knows.
            const auto id = parse_u32_or(value, 8);
            if (id > 7) {
                fmt::println(stderr, "error: programme= needs a substream id 0..7 (got '{}')",
                             token);
                return false;
            }
            out.programme = static_cast<int>(id);
            continue;
        }
        if (key == "programme2") {
            if (value.empty()) {
                fmt::println(stderr, "error: programme2= needs an input file path");
                return false;
            }
            out.programme2 = std::string{value};
            continue;
        }
        if (key == "programme2-layout") {
            if (value.empty()) {
                fmt::println(stderr, "error: programme2-layout= needs a layout name ({})",
                             plan::layout_names(plan::Codec::kEac3));
                return false;
            }
            out.programme2_layout = std::string{value};
            continue;
        }
        if (key == "programme2-bitrate") {
            const auto kbps = parse_u32_or(value, 0);
            if (kbps == 0) {
                fmt::println(stderr,
                             "error: programme2-bitrate= needs a rate in kbit/s (got '{}')",
                             token);
                return false;
            }
            out.programme2_bitrate = kbps;
            continue;
        }
        if (key == "programme2-dialnorm") {
            const auto dialnorm = parse_u32_or(value, 0);
            if (dialnorm < 1 || dialnorm > 31) {
                fmt::println(stderr,
                             "error: programme2-dialnorm= needs 1..31 (§5.4.2.8; got '{}')",
                             token);
                return false;
            }
            out.programme2_dialnorm = static_cast<int>(dialnorm);
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
        status_println(out, "measured {:.2f} LKFS (BS.1770-4, gated) -> {} {}", *lkfs, field,
                     dialnorm);
    } else {
        status_println(out, "{} measured {:.2f} LKFS (BS.1770-4, gated) -> {} {}", programme, *lkfs,
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

FILE* status_stream(std::string_view out_path) {
    if (quiet_mode()) {
        return nullptr;
    }
    return is_stdio_path(out_path) ? stderr : stdout;
}

FILE* status_stream() { return quiet_mode() ? nullptr : stdout; }

std::string format_programme_ids(std::span<const int> ids) {
    std::string out;
    for (const int id : ids) {
        if (!out.empty()) {
            out += ", ";
        }
        out += fmt::format("{}", id);
    }
    return out;
}

std::optional<int> choose_programme(std::span<const int> ids, std::optional<int> wanted) {
    assert(!ids.empty());
    // Omitted takes the first programme the stream carries rather than a
    // hard-coded 0: §E2.3.1.2 numbers independent substreams from 0, but a
    // stream someone has already cut a programme out of need not still start
    // at one, and refusing it would be refusing a stream that decodes fine.
    if (!wanted) {
        return ids.front();
    }
    if (std::ranges::find(ids, *wanted) == ids.end()) {
        fmt::println(stderr, "error: no programme {} in this stream (it carries {})", *wanted,
                     format_programme_ids(ids));
        return std::nullopt;
    }
    return wanted;
}

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

std::vector<std::byte> read_elementary_stream(std::string_view in_path) {
    auto bytes = read_all(in_path);
    if (bytes.empty()) {
        fmt::println(stderr, "error: cannot read {}", in_path);
        return {};
    }
    auto result = ac3::apps::elementary_stream_from_bytes(bytes);
    if (!result.error.empty()) {
        fmt::println(stderr, "error: {} is a {}", in_path, result.error);
        return {};
    }
    return std::move(result.bytes);
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
    if (out == nullptr) {
        return;
    }
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
    if (quiet_mode()) {
        return;
    }
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

std::vector<ObjectSlot> object_slots_from_assignment(
    const ac3::plan::Assignment& assignment,
    std::span<const ac3::plan::SourceShape> shapes) {
    // Where source `s`'s channel `c` lands in the flattened space.
    const auto flat = [&](std::size_t source, std::size_t channel) {
        std::size_t base = 0;
        for (std::size_t i = 0; i < source && i < shapes.size(); ++i) {
            base += shapes[i].channels;
        }
        return base + channel;
    };
    std::vector<ObjectSlot> slots;
    for (const auto& [source, channel] :
         assignment.rows_of(ac3::plan::DestinationKind::kObject)) {
        const auto dest = assignment.at(source, channel);
        slots.push_back(
            {.taps = {{flat(source, channel), std::pow(10.0, dest.trim_db / 20.0)}}});
    }
    // rows_of() hands them back in (source, then channel) order, which is what
    // makes "the maximal contiguous run within one source" a well-defined
    // grouping - see DestinationKind::kObjectMono's own comment on why the
    // grouping is by adjacency rather than a stored group id.
    const auto mono_rows = assignment.rows_of(ac3::plan::DestinationKind::kObjectMono);
    for (std::size_t i = 0; i < mono_rows.size();) {
        std::size_t j = i + 1;
        while (j < mono_rows.size() && mono_rows[j].first == mono_rows[i].first &&
               mono_rows[j].second == mono_rows[j - 1].second + 1) {
            ++j;
        }
        const auto n = static_cast<double>(j - i);
        ObjectSlot slot;
        for (std::size_t k = i; k < j; ++k) {
            const auto dest = assignment.at(mono_rows[k].first, mono_rows[k].second);
            slot.taps.emplace_back(flat(mono_rows[k].first, mono_rows[k].second),
                                   std::pow(10.0, dest.trim_db / 20.0) / n);
        }
        slots.push_back(std::move(slot));
        i = j;
    }
    return slots;
}

std::string_view container_note(RecordingSink::Container container) {
    switch (container) {
        case RecordingSink::Container::kElementary: return {};
        case RecordingSink::Container::kMatroska: return " (Matroska)";
        case RecordingSink::Container::kMpegts: return " (MPEG-TS)";
        case RecordingSink::Container::kSpdif: return " (IEC 61937 WAV carrier)";
        case RecordingSink::Container::kFmp4: return " (fragmented MP4/CMAF)";
    }
    return {};
}

std::optional<TakePlan> resolve_take_plan(const Options& meta, std::uint32_t bitrate,
                                          ac3::SampleRate rate) {
    // dialnorm=auto measures a whole programme's BS.1770 loudness before
    // encoding it, which a live capture has not got: the programme does not
    // exist yet when the first frame has to be encoded. Refused rather than
    // silently ignored, the same stance atmos-adm takes for the same reason -
    // "a silently ignored metadata flag looks exactly like metadata that did
    // not work" (parse_options' own comment). Every other metadata option
    // reaches the encoder through plan::ac3_config/eac3_config below.
    if (meta.p.measure_dialnorm || meta.p.measure_dialnorm2) {
        fmt::println(stderr,
                     "error: dialnorm=auto needs a whole programme to measure, which a live "
                     "capture has not got yet; pass dialnorm=<1..31> explicitly");
        return std::nullopt;
    }
    TakePlan take;
    take.plan.bitrate_kbps = bitrate;
    take.plan.sample_rate = rate;
    take.plan.meta = meta.p;
    take.plan.tools.fast_mdct = meta.fast_mdct;
    // Resolved against E-AC-3 first whatever codec= says, because E-AC-3
    // carries every layout AC-3 does and more - so this pass either succeeds
    // or the layout name itself is wrong, and the "AC-3 cannot carry this"
    // diagnosis below can name the layout it is refusing instead of the
    // parser failing first.
    const std::string_view name =
        meta.take_layout.empty() ? std::string_view{"stereo"} : std::string_view{meta.take_layout};
    take.plan.codec = ac3::plan::Codec::kEac3;
    if (!resolve_layout(name, ac3::plan::Codec::kEac3, take.plan, take.label)) {
        return std::nullopt;
    }
    // Whether plain AC-3 could carry what was asked for: a named layout says
    // so directly, a custom Table E2.5 selection says so by needing no
    // dependent substream. Same two questions resolve_layout itself asks when
    // it is given kAc3 - asked here without printing, because a "no" is the
    // ordinary path into E-AC-3 rather than an error.
    bool ac3_can_carry = false;
    if (take.plan.custom_locations) {
        const auto allocated = ac3::eac3::chanmap::allocate(*take.plan.custom_locations);
        ac3_can_carry = allocated.has_value() && allocated->dependents.empty();
    } else {
        ac3_can_carry = ac3::plan::carries(ac3::plan::Codec::kAc3, take.plan.layout);
    }
    take.plan.codec = meta.take_codec.value_or(ac3_can_carry ? ac3::plan::Codec::kAc3
                                                            : ac3::plan::Codec::kEac3);
    if (take.plan.codec == ac3::plan::Codec::kAc3 && !ac3_can_carry) {
        fmt::println(stderr, "error: {} cannot carry {} - {}",
                     ac3::plan::codec_label(ac3::plan::Codec::kAc3), take.label,
                     ac3::plan::describe(ac3::plan::PlanError::kLayoutNeedsEac3));
        return std::nullopt;
    }
    if (const auto bad = ac3::plan::validate(take.plan)) {
        fmt::println(stderr, "error: {}", ac3::plan::describe(*bad));
        return std::nullopt;
    }
    take.eac3 = take.plan.codec == ac3::plan::Codec::kEac3;
    const auto channel_plan = ac3::plan::resolve(take.plan);
    take.coded_channels = static_cast<int>(ac3::plan::coded_channels(channel_plan).size());
    take.rendered_channels = ac3::plan::rendered_channel_count(channel_plan);
    return take;
}

RecordingSink::Config take_sink_config(const Options& meta, const TakePlan& take,
                                       std::uint32_t sample_rate_hz) {
    return RecordingSink::Config{.container = meta.container,
                                 .eac3 = take.eac3,
                                 .sample_rate = sample_rate_hz,
                                 .channels = take.rendered_channels,
                                 .fmp4_window_segments = meta.fmp4_window_segments};
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

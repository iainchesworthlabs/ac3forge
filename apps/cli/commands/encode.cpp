#include "encode.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <fmt/base.h>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../support.hpp"
#include "ac3/analysis/levels.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/loudness.hpp"
#include "../multi_source.hpp"

namespace ac3cli::commands {

namespace plan = ac3::plan;

namespace {

// Reports a bad tool token against the syntax, the same way a bad layout is
// reported against the layout list.
bool tools_or_error(std::string_view text, plan::Tools& out) {
    if (plan::parse_tools(text, out)) {
        return true;
    }
    fmt::println(stderr, "error: unknown tool set '{}' ({})", text, plan::kToolsSyntax);
    return false;
}

// As above, for the vbr argument.
bool vbr_or_error(std::string_view text, std::optional<ac3::eac3::VbrConfig>& out) {
    if (plan::parse_vbr(text, out)) {
        return true;
    }
    fmt::println(stderr, "error: unrecognised vbr setting '{}' ({})", text, plan::kVbrSyntax);
    return false;
}

// A second programme's own input file (programme2=), read one frame at a time.
//
// Its own reader rather than a second pass through the primary path's
// streaming state: the two sources are consumed in lockstep but are otherwise
// unrelated - different channel counts, different lengths, different routings
// - and the primary's inline state is already threaded through a long
// function. Streams like the primary does, falling back to a whole-file read
// for the inputs WavStreamReader declines (stdin among them), so a second
// programme costs no more memory than the first for the ordinary file case.
class ProgrammeSource {
   public:
    bool open(std::string_view path) {
        if (!is_stdio_path(path) && stream_.open(std::string{path}).has_value()) {
            streaming_ = true;
            hold_.assign(channels(), 0.0f);
            return true;
        }
        auto whole = read_wav_arg(path);
        if (!whole) {
            fmt::println(stderr, "error: {}: {}", path, ac3::io::describe(whole.error()));
            return false;
        }
        whole_ = std::move(*whole);
        return true;
    }

    [[nodiscard]] std::uint32_t sample_rate() const {
        return streaming_ ? stream_.sample_rate() : whole_.sample_rate;
    }
    [[nodiscard]] std::size_t channels() const {
        return streaming_ ? stream_.channels() : whole_.channels.size();
    }
    [[nodiscard]] std::size_t frame_count() const {
        return streaming_ ? static_cast<std::size_t>(stream_.frame_count())
                          : whole_.frame_count();
    }

    // Fills `dest` (one vector per channel, each kSamplesPerFrame long) with
    // the samples starting at `start`. Past end-of-file every channel holds
    // its own last real sample rather than dropping to zero, for exactly the
    // reason the primary path does: a sudden drop to silence is itself a
    // transient the encoder would (correctly) spend a block switch on, for a
    // discontinuity that only exists because the file ended mid-frame. The
    // streaming form ignores `start` beyond checking it advances in order,
    // which the single encode loop guarantees.
    bool fill(std::size_t start, std::vector<std::vector<float>>& dest,
              std::string_view path) {
        const std::size_t frames = frame_count();
        if (!streaming_) {
            for (std::size_t c = 0; c < dest.size(); ++c) {
                const float hold = frames > 0 ? whole_.channels[c][frames - 1] : 0.0f;
                for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                    const std::size_t at = start + static_cast<std::size_t>(i);
                    dest[c][static_cast<std::size_t>(i)] =
                        at < frames ? whole_.channels[c][at] : hold;
                }
            }
            return true;
        }
        const std::size_t want =
            std::min<std::size_t>(ac3::kSamplesPerFrame, frames - std::min(frames, consumed_));
        std::vector<std::span<float>> dst(dest.size());
        for (std::size_t c = 0; c < dest.size(); ++c) {
            dst[c] = std::span{dest[c]}.first(want);
        }
        if (want > 0) {
            const auto got = stream_.read_planar(dst, want);
            if (!got || *got != want) {
                fmt::println(stderr, "error: {}: {}", path,
                             ac3::io::describe(got ? ac3::io::WavError::kTruncated
                                                   : got.error()));
                return false;
            }
            consumed_ += want;
        }
        for (std::size_t c = 0; c < dest.size(); ++c) {
            if (want > 0) {
                hold_[c] = dest[c][want - 1];
            }
            std::fill(dest[c].begin() + static_cast<std::ptrdiff_t>(want), dest[c].end(),
                      hold_[c]);
        }
        return true;
    }

   private:
    bool streaming_ = false;
    ac3::io::WavStreamReader stream_;
    ac3::io::WavData whole_;
    std::vector<float> hold_;
    std::size_t consumed_ = 0;
};

// The second programme's plan, its source and its routing, assembled together
// because the encode loop needs all three in step.
struct SecondProgramme {
    plan::Plan p;
    std::string label;
    // Kept here rather than re-read from Options::programme2 at every use: this
    // struct only exists once that option HAS a value, so carrying the path
    // means nothing downstream has to re-establish that.
    std::string path;
    ProgrammeSource source;
    plan::Routing routing;
};

// Opens and plans that second programme, or reports why not and returns
// nullptr. `rate` is the PRIMARY programme's sample rate: every substream of an
// access unit codes the same frame period, so a second programme sampled
// differently cannot ride along.
std::unique_ptr<SecondProgramme> open_second_programme(std::string_view path,
                                                       const Options& meta,
                                                       ac3::SampleRate rate,
                                                       std::uint32_t primary_kbps,
                                                       const plan::Tools& tools) {
    auto out = std::make_unique<SecondProgramme>();
    out->path = std::string{path};
    if (!out->source.open(out->path)) {
        return nullptr;
    }
    const auto second_rate = wav_sample_rate(out->source.sample_rate(), "E-AC-3", true);
    if (!second_rate) {
        return nullptr;
    }
    if (*second_rate != rate) {
        fmt::println(stderr,
                     "error: programme2= is {} Hz but the primary programme is {} Hz - every "
                     "substream of an access unit codes the same frame period",
                     out->source.sample_rate(), sample_rate_hz(rate));
        return nullptr;
    }
    out->p.codec = plan::Codec::kEac3;
    out->p.sample_rate = rate;
    // Half the primary's rate by default: an associated service is normally
    // much narrower than the main mix, and it is spent ON TOP of the
    // primary's, not carved out of it.
    out->p.bitrate_kbps =
        meta.programme2_bitrate.value_or(std::max<std::uint32_t>(primary_kbps / 2, 32));
    out->p.tools = tools;
    // Its own dialnorm, never the primary's - see Options::programme2_dialnorm.
    // The rest of `meta.p` belongs to the primary programme: a second
    // programme's DRC profile, mix metadata and downmix levels are its own,
    // and this first cut does not offer a way to say what they are.
    out->p.meta.dialnorm = meta.programme2_dialnorm;
    if (meta.programme2_layout.empty()) {
        const auto id = plan::layout_for_source(out->source.channels());
        if (!id) {
            fmt::println(stderr, "error: {} has {} channels - {}", out->path,
                         out->source.channels(),
                         plan::describe(plan::PlanError::kNoSourceLayout));
            return nullptr;
        }
        out->p.layout = *id;
        out->label = std::string(plan::layout(*id).label);
    } else if (!resolve_layout(meta.programme2_layout, plan::Codec::kEac3, out->p, out->label)) {
        return nullptr;
    }
    if (plan::resolve(out->p).bed_acmod == ac3::Acmod::kDualMono) {
        // 1+1 is itself two programmes sharing one syncframe (§E1.3), levelled
        // by dialnorm and dialnorm2. Stacking it inside a second independent
        // substream would mean three programmes described by two different
        // mechanisms, with only one dialnorm reachable from here - refuse it
        // rather than emit something whose second half cannot be levelled.
        fmt::println(stderr,
                     "error: programme2-layout=1+1 is not supported: 1+1 already carries two "
                     "programmes in one substream. Use it on the primary programme, or give "
                     "programme2 a layout of its own");
        return nullptr;
    }
    auto routing = routing_or_error(out->p, out->source.channels());
    if (!routing) {
        return nullptr;
    }
    out->routing = std::move(*routing);
    return out;
}

// Whether AccessUnitEncoder accepted the configuration at all, and if not, why
// in terms a caller can act on.
//
// AccessUnitEncoder's constructor cannot fail: a configuration its own
// substream_configs() rejects leaves it holding NO substreams rather than
// reporting anything, so channel_count() answers 0 and the first
// encode_access_unit() call returns an error whose text ("the encoder cannot
// express this configuration") names no cause. Zero is unambiguous - a built
// encoder always codes at least the independent substream - so it is checked
// here, before a frame is attempted, and diagnosed.
//
// The reachable cause is the rate/sample-rate pair. §E2.3.1.3's frmsiz is an
// 11-bit word count, so a syncframe can never exceed kMaxFrameWords words
// however legal both halves are on their own: at the Annex E half rates a
// nominal Table 5.18 bitrate the CLI accepts everywhere else runs past it -
// every rate above 320 kbps at 16 kHz, above 448 at 22.05 kHz and above 512 at
// 24 kHz. Both are ordinary things to type, nothing in the CLI's own grammar
// marks the combination, and before this check the zero met the assert() below
// instead - which is compiled out under NDEBUG, so a release build fell
// through to encode_access_unit's causeless message while any build with
// assertions live aborted outright. atmos-encode, which never took this path,
// refused cleanly throughout. Found by tools/ci/fuzz_eac3_encoder_space.py.
bool eac3_config_accepted(const ac3::eac3::AccessUnitEncoder& encoder, std::uint32_t bitrate,
                          ac3::SampleRate rate, bool vbr) {
    if (encoder.channel_count() != 0) {
        return true;
    }
    // VBR sizes each syncframe from the content, so frame_words() does not
    // describe it and quoting a word count would be a guess.
    if (!vbr) {
        const auto words = ac3::eac3::frame_words(rate, bitrate);
        if (words > ac3::eac3::kMaxFrameWords) {
            fmt::println(stderr,
                        "error: {} kbps at {} Hz needs {} words per syncframe, past the {} "
                        "that E-AC-3's 11-bit frmsiz (§E2.3.1.3) can signal - lower the "
                        "bitrate or raise the sample rate",
                        bitrate, ac3::sample_rate_hz(rate), words, ac3::eac3::kMaxFrameWords);
            return false;
        }
    }
    fmt::println(stderr, "error: the encoder cannot express this configuration");
    return false;
}

}  // namespace

int run_eac3_encode_multi(std::string_view in_path, std::string_view out_path,
                          std::uint32_t bitrate, std::string_view tools,
                          std::string_view layout, std::string_view vbr, const Options& meta) {
    auto sources = load_sources(in_path, meta.sources, meta.offsets);
    if (!sources) {
        return 1;
    }
    const auto sr = wav_sample_rate(sources->sample_rate, "E-AC-3", true);
    if (!sr) {
        return 1;
    }
    plan::Plan p{.codec = plan::Codec::kEac3,
                 .sample_rate = *sr,
                 .bitrate_kbps = bitrate,
                 .meta = meta.p};
    std::string label;
    if (layout.empty()) {
        std::size_t total_channels = 0;
        for (const auto& shape : sources->shapes) {
            total_channels += shape.channels;
        }
        const auto id = plan::layout_for_source(total_channels);
        if (!id) {
            fmt::println(stderr, "error: {} channels - {}", total_channels,
                         plan::describe(plan::PlanError::kNoSourceLayout));
            return 1;
        }
        p.layout = *id;
        label = std::string(plan::layout(*id).label);
    } else if (!resolve_layout(layout, plan::Codec::kEac3, p, label)) {
        return 1;
    }

    p.tools.fast_mdct = meta.fast_mdct;
    p.tools.search = meta.search;
    p.tools.dither = meta.dither;
    if (!tools_or_error(tools, p.tools)) {
        return 1;
    }
    if (!vbr_or_error(vbr, p.vbr)) {
        return 1;
    }

    const auto routing = routing_for_sources(p, *sources, meta.map_spec);
    if (!routing) {
        return 1;
    }

    const auto nchans = static_cast<std::size_t>(routing->coded_channels);
    const std::size_t total = sources->total_frames;
    const auto source_channels = static_cast<std::size_t>(routing->source_channels);

    std::vector<std::vector<float>> source(source_channels,
                                           std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::vector<float>> block(nchans, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> in(source_channels);
    std::vector<std::span<float>> out(nchans);
    std::vector<std::span<const float>> views(nchans);
    for (std::size_t c = 0; c < nchans; ++c) {
        out[c] = block[c];
        views[c] = block[c];
    }
    // Renders one frame's worth of every source's samples onto the coded
    // channels `out`/`views` alias - shared by the measurement pre-pass below
    // and the real encode loop after it, so the two can never render this
    // programme two different ways.
    auto route_frame = [&](std::size_t start) {
        gather_frame(*sources, start, source);
        for (std::size_t c = 0; c < source_channels; ++c) {
            in[c] = source[c];
        }
        plan::render(*routing, in, out, ac3::kSamplesPerFrame);
    };

    const auto cp = plan::resolve(p);
    const bool dual_mono = cp.bed_acmod == ac3::Acmod::kDualMono;
    const bool want_dialnorm = p.meta.measure_dialnorm;
    // dialnorm2 only means anything under 1+1 - silently inert otherwise,
    // exactly like run_eac3_encode's identical check for its one file.
    const bool want_dialnorm2 = dual_mono && p.meta.measure_dialnorm2;
    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the E-AC-3 bytes this function writes below already own stdout in
    // that case, and no human-readable report (the dialnorm=auto measurement
    // just below included) may land in the middle of them.
    const auto status = status_stream(out_path);
    if (want_dialnorm || want_dialnorm2) {
        // §5.4.2.8's BS.1770 pass has to measure what the encoder actually
        // receives - the routed/rendered coded channels, not each source's
        // own raw layout, since map= can permute, trim or fold several
        // sources onto them - so this renders the entire programme once
        // purely to measure it. Dual mono gets one single-channel meter per
        // programme (Ch1/Ch2 are unrelated, §E1.3 - see
        // measured_dialnorm_channel's own comment); every other target gets
        // one whole-programme meter, the same BS.1770 channel weighting
        // measured_dialnorm uses for the single-file case.
        std::optional<ac3::meta::LoudnessMeter> whole;
        std::optional<ac3::meta::LoudnessMeter> ch1;
        std::optional<ac3::meta::LoudnessMeter> ch2;
        if (dual_mono) {
            if (want_dialnorm) {
                ch1.emplace(*sr, ac3::Acmod::k1_0, false);
            }
            if (want_dialnorm2) {
                ch2.emplace(*sr, ac3::Acmod::k1_0, false);
            }
        } else if (want_dialnorm) {
            whole.emplace(*sr, cp.bed_acmod, cp.bed_lfe);
        }
        for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
            route_frame(start);
            if (whole) {
                whole->push(views);
            }
            if (ch1) {
                const std::array<std::span<const float>, 1> v{views[0]};
                ch1->push(v);
            }
            if (ch2) {
                const std::array<std::span<const float>, 1> v{views[1]};
                ch2->push(v);
            }
        }
        if (want_dialnorm) {
            const auto measured = dual_mono ? finish_measurement(*ch1, "Ch1", "dialnorm", status)
                                            : finish_measurement(*whole, {}, "dialnorm", status);
            if (!measured) {
                fmt::println(stderr, "error: {}no audio above the -70 LKFS absolute gate; "
                                     "pass dialnorm=<1..31> explicitly",
                             dual_mono ? "Ch1 has " : "");
                return 1;
            }
            p.meta.dialnorm = *measured;
        }
        if (want_dialnorm2) {
            const auto measured2 = finish_measurement(*ch2, "Ch2", "dialnorm2", status);
            if (!measured2) {
                fmt::println(stderr, "error: Ch2 has no audio above the -70 LKFS absolute gate; "
                                     "pass dialnorm2=<1..31> explicitly");
                return 1;
            }
            p.meta.dialnorm2 = *measured2;
        }
    }

    ac3::eac3::AccessUnitEncoder encoder{plan::eac3_config(p)};
    if (!eac3_config_accepted(encoder, bitrate, *sr, p.vbr.has_value())) {
        return 1;
    }
    assert(static_cast<int>(nchans) == encoder.channel_count());
    // Streamed out as encoded, exactly as run_eac3_encode below - the
    // multi-source shape only differs on the INPUT side (route_frame over
    // whole sources), not in what leaves.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, meta.keep_partial)) {
        return 1;
    }
    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        route_frame(start);
        auto unit = encoder.encode_access_unit(views);
        if (!unit) {
            fmt::println(stderr, "error: the encoder cannot express this configuration");
            out_sink.abort();
            return 1;
        }
        if (!out_sink.push(std::move(unit->bytes))) {
            out_sink.abort();
            return 1;
        }
    }
    if (!out_sink.close()) {
        return 1;
    }
    if (p.vbr) {
        // bitrate_kbps is only the nominal reference vbr's tool heuristics
        // used, not a target - what a VBR run actually spent is the sizes it
        // produced, so that is what gets reported instead of one number.
        const double mean_bytes = out_sink.frames() == 0
                                      ? 0.0
                                      : static_cast<double>(out_sink.total_bytes()) /
                                            static_cast<double>(out_sink.frames());
        const double mean_kbps = mean_bytes * 8.0 * static_cast<double>(sources->sample_rate) /
                                 (1000.0 * ac3::kSamplesPerFrame);
        fmt::println(status,
                     "encoded {} E-AC-3 access units (vbr {}, {} Hz, {}, {} coded channels, "
                     "tools: {}) to {}",
                     out_sink.frames(), plan::format_vbr(p.vbr), sources->sample_rate, label,
                     nchans, plan::format_tools(p.tools), out_path);
        fmt::println(status,
                     "  access unit size: {}-{} bytes, {:.0f} bytes mean (~{:.0f} kbps mean)",
                     out_sink.min_bytes(), out_sink.max_bytes(), mean_bytes, mean_kbps);
    } else {
        fmt::println(status,
                     "encoded {} E-AC-3 access units ({} kbps, {} Hz, {}, {} coded channels, "
                     "tools: {}) to {}",
                     out_sink.frames(), bitrate, sources->sample_rate, label, nchans,
                     plan::format_tools(p.tools), out_path);
    }
    print_routing(p, *routing, label, status);
    return 0;
}

int run_eac3_encode(std::string_view in_path, std::string_view out_path,
                    std::uint32_t bitrate, std::string_view tools, std::string_view layout,
                    std::string_view vbr, const Options& meta,
                    std::string_view in2_path) {
    if (!meta.sources.empty() || meta.map_spec) {
        if (!in2_path.empty()) {
            fmt::println(stderr,
                         "error: use either a second positional file or src=/map=, not both");
            return 1;
        }
        if (meta.programme2) {
            // src=/map= route several sources onto ONE programme's channels;
            // programme2= adds a second programme with its own source and its
            // own routing. Combining them is not ambiguous so much as
            // unimplemented - the multi-source path has no notion of a second
            // programme to assign channels to - so say so rather than
            // silently ignore one of them.
            fmt::println(stderr,
                         "error: programme2= and src=/map= cannot be combined yet - the "
                         "multi-source router assigns channels to one programme");
            return 1;
        }
        return run_eac3_encode_multi(in_path, out_path, bitrate, tools, layout, vbr, meta);
    }
    // The same streaming-vs-whole-file split as run_encode, for the same
    // reasons - see its comment. A failed open falls through so read_wav_arg
    // produces the error message it always has.
    ac3::io::WavStreamReader stream_in;
    const bool streaming = !is_stdio_path(in_path) && !meta.p.measure_dialnorm &&
                           !meta.p.measure_dialnorm2 && in2_path.empty() &&
                           stream_in.open(std::string{in_path}).has_value();
    std::expected<ac3::io::WavData, ac3::io::WavError> wav =
        std::unexpected(ac3::io::WavError::kCannotOpen);
    if (!streaming) {
        wav = read_wav_arg(in_path);
        if (!wav) {
            fmt::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
            return 1;
        }
        if (!prepare_dual_mono_source(*wav, layout, in2_path)) {
            return 1;
        }
    } else if (layout == "1+1" && stream_in.channels() != 2) {
        fmt::println(stderr,
                     "error: layout 1+1 needs either one two-channel file (Ch1, Ch2) or "
                     "two mono files; the source has {} channel(s) and no second file "
                     "was given",
                     stream_in.channels());
        return 1;
    }
    const std::uint32_t src_rate = streaming ? stream_in.sample_rate() : wav->sample_rate;
    const std::size_t src_channels =
        streaming ? stream_in.channels() : wav->channels.size();
    const auto sr = wav_sample_rate(src_rate, "E-AC-3", true);
    if (!sr) {
        return 1;
    }
    plan::Plan p{.codec = plan::Codec::kEac3,
                 .sample_rate = *sr,
                 .bitrate_kbps = bitrate,
                 .meta = meta.p};
    std::string label;
    if (layout.empty()) {
        // An unnamed layout follows the source, which is what this command
        // did before it could be told otherwise.
        const auto id = plan::layout_for_source(src_channels);
        if (!id) {
            fmt::println(stderr, "error: {} channels - {}", src_channels,
                         plan::describe(plan::PlanError::kNoSourceLayout));
            return 1;
        }
        p.layout = *id;
        label = std::string(plan::layout(*id).label);
    } else if (!resolve_layout(layout, plan::Codec::kEac3, p, label)) {
        return 1;
    }

    p.tools.fast_mdct = meta.fast_mdct;
    p.tools.search = meta.search;
    p.tools.dither = meta.dither;
    if (!tools_or_error(tools, p.tools)) {
        return 1;
    }
    if (!vbr_or_error(vbr, p.vbr)) {
        return 1;
    }
    const auto cp = plan::resolve(p);
    const bool dual_mono = cp.bed_acmod == ac3::Acmod::kDualMono;
    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the E-AC-3 bytes this function writes below already own stdout in
    // that case, and no human-readable report (the dialnorm=auto measurement
    // just below included) may land in the middle of them.
    const auto status = status_stream(out_path);
    if (p.meta.measure_dialnorm) {
        // Dual mono has no "whole programme" a single BS.1770 pass can mean -
        // Ch1 and Ch2 are unrelated (§E1.3, no downmix between them), so this
        // measures Ch1's own channel alone, exactly like Ch2 just below,
        // rather than folding both into one measured_dialnorm() call the way
        // every other layout can.
        const auto measured = dual_mono
                                  ? measured_dialnorm_channel(wav->channels[0], *sr, "Ch1",
                                                              "dialnorm", status)
                                  : measured_dialnorm(*wav, *sr, cp.bed_acmod, cp.bed_lfe, status);
        if (!measured) {
            fmt::println(stderr, "error: {}no audio above the -70 LKFS absolute gate; "
                                 "pass dialnorm=<1..31> explicitly",
                         dual_mono ? "Ch1 has " : "");
            return 1;
        }
        p.meta.dialnorm = *measured;
    }
    if (dual_mono && p.meta.measure_dialnorm2) {
        const auto measured2 =
            measured_dialnorm_channel(wav->channels[1], *sr, "Ch2", "dialnorm2", status);
        if (!measured2) {
            fmt::println(stderr, "error: Ch2 has no audio above the -70 LKFS absolute gate; "
                                 "pass dialnorm2=<1..31> explicitly");
            return 1;
        }
        p.meta.dialnorm2 = *measured2;
    }

    const auto routing = routing_or_error(p, src_channels);
    if (!routing) {
        return 1;
    }

    // §E2.3.1.2's second independent substream, when one was asked for: its
    // own source, layout, rate and dialnorm, riding in the same access units.
    std::unique_ptr<SecondProgramme> second;
    if (meta.programme2) {
        second = open_second_programme(*meta.programme2, meta, *sr, bitrate, p.tools);
        if (!second) {
            return 1;
        }
    }
    auto config = plan::eac3_config(p);
    if (second) {
        config.additional.push_back(plan::eac3_programme(second->p));
    }
    ac3::eac3::AccessUnitEncoder encoder{config};
    if (!eac3_config_accepted(encoder, bitrate, *sr, p.vbr.has_value())) {
        return 1;
    }
    const auto nchans = static_cast<std::size_t>(routing->coded_channels);
    const std::size_t second_nchans =
        second ? static_cast<std::size_t>(second->routing.coded_channels) : 0;
    assert(static_cast<int>(nchans + second_nchans) == encoder.channel_count());
    // The classic path has exactly one source, always index 0 in offset='s
    // numbering - see LoadedSources::offset_samples for the multi-source
    // equivalent of this same leading silence.
    const std::size_t offset = offset_samples_for(meta.offsets, 0, src_rate);
    const std::size_t frame_count =
        streaming ? static_cast<std::size_t>(stream_in.frame_count()) : wav->frame_count();
    // A second programme keeps the run going to whichever source is longer -
    // the two programmes share the frame period, so the shorter one holds its
    // last sample rather than the stream ending early on the longer one.
    const std::size_t total =
        std::max(offset + frame_count, second ? second->source.frame_count() : 0);

    std::vector<std::vector<float>> source(src_channels,
                                           std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::vector<float>> block(nchans + second_nchans,
                                          std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> in(source.size());
    std::vector<std::span<float>> out(nchans);
    // Every coded channel of the access unit: the first programme's, then the
    // second's - the same order encode_access_unit expects them, and the same
    // order the substreams themselves go on the wire.
    std::vector<std::span<const float>> views(nchans + second_nchans);
    for (std::size_t c = 0; c < nchans; ++c) {
        out[c] = block[c];
    }
    for (std::size_t c = 0; c < views.size(); ++c) {
        views[c] = block[c];
    }
    // The second programme's own per-frame source and coded-channel spans,
    // aliasing the tail of `block` so one encode call sees both programmes.
    std::vector<std::vector<float>> second_source;
    std::vector<std::span<const float>> second_in;
    std::vector<std::span<float>> second_out(second_nchans);
    if (second) {
        second_source.assign(second->source.channels(),
                             std::vector<float>(ac3::kSamplesPerFrame));
        second_in.resize(second_source.size());
        for (std::size_t c = 0; c < second_source.size(); ++c) {
            second_in[c] = second_source[c];
        }
        for (std::size_t c = 0; c < second_nchans; ++c) {
            second_out[c] = block[nchans + c];
        }
    }
    // The encoded access units leave as they are produced - see
    // EncodedStreamSink; its stats also feed the VBR report below, which
    // used to re-walk the whole frame list for them.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, meta.keep_partial)) {
        return 1;
    }
    // See run_encode's identical streaming state: the loop consumes the
    // source strictly in order, so a rolling read position and the last
    // real sample per channel are all the streaming path needs.
    std::vector<float> stream_hold(src_channels, 0.0f);
    std::vector<std::span<float>> stream_dst(src_channels);
    std::size_t consumed = 0;
    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        // Hold the last real sample past end-of-file rather than dropping to
        // hard zero - see run_encode's identical padding for why: a sudden
        // drop to silence is itself a transient the encoder would (correctly)
        // spend a block-switch on, for a discontinuity that only exists
        // because this frame ends mid-buffer. Ahead of the source's own
        // samples, offset= silence is real silence, not padding.
        if (streaming) {
            const std::size_t lead =
                start < offset ? std::min<std::size_t>(offset - start, ac3::kSamplesPerFrame)
                               : 0;
            std::size_t want = 0;
            if (lead < ac3::kSamplesPerFrame) {
                const std::size_t remaining = frame_count - std::min(frame_count, consumed);
                want = std::min<std::size_t>(ac3::kSamplesPerFrame - lead, remaining);
            }
            for (std::size_t c = 0; c < src_channels; ++c) {
                std::fill_n(source[c].begin(), lead, 0.0f);
                stream_dst[c] = std::span{source[c]}.subspan(lead, want);
            }
            if (want > 0) {
                const auto got = stream_in.read_planar(stream_dst, want);
                if (!got || *got != want) {
                    fmt::println(stderr, "error: {}: {}", in_path,
                                 ac3::io::describe(got ? ac3::io::WavError::kTruncated
                                                       : got.error()));
                    out_sink.abort();
                    return 1;
                }
                consumed += want;
            }
            for (std::size_t c = 0; c < src_channels; ++c) {
                if (want > 0) {
                    stream_hold[c] = source[c][lead + want - 1];
                }
                std::fill(source[c].begin() + static_cast<std::ptrdiff_t>(lead + want),
                          source[c].end(), stream_hold[c]);
                in[c] = source[c];
            }
        } else {
            for (std::size_t c = 0; c < source.size(); ++c) {
                const float hold = frame_count > 0 ? wav->channels[c][frame_count - 1] : 0.0f;
                for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                    const std::size_t at = start + static_cast<std::size_t>(i);
                    if (at < offset) {
                        source[c][static_cast<std::size_t>(i)] = 0.0f;
                        continue;
                    }
                    const std::size_t shifted = at - offset;
                    source[c][static_cast<std::size_t>(i)] =
                        shifted < frame_count ? wav->channels[c][shifted] : hold;
                }
                in[c] = source[c];
            }
        }
        plan::render(*routing, in, out, ac3::kSamplesPerFrame);
        if (second) {
            if (!second->source.fill(start, second_source, second->path)) {
                out_sink.abort();
                return 1;
            }
            plan::render(second->routing, second_in, second_out, ac3::kSamplesPerFrame);
        }
        auto unit = encoder.encode_access_unit(views);
        if (!unit) {
            fmt::println(stderr, "error: the encoder cannot express this configuration");
            out_sink.abort();
            return 1;
        }
        if (!out_sink.push(unit->bytes)) {
            out_sink.abort();
            return 1;
        }
    }
    if (!out_sink.close()) {
        return 1;
    }
    if (p.vbr) {
        // bitrate_kbps is only the nominal reference vbr's tool heuristics
        // used, not a target - what a VBR run actually spent is the sizes it
        // produced, so that is what gets reported instead of one number:
        // the sink kept the tally as the units streamed out.
        const double mean_bytes = out_sink.frames() == 0
                                      ? 0.0
                                      : static_cast<double>(out_sink.total_bytes()) /
                                            static_cast<double>(out_sink.frames());
        const double mean_kbps = mean_bytes * 8.0 * static_cast<double>(src_rate) /
                                 (1000.0 * ac3::kSamplesPerFrame);
        fmt::println(status,
                     "encoded {} E-AC-3 access units (vbr {}, {} Hz, {}, {} coded channels, "
                     "tools: {}) to {}",
                     out_sink.frames(), plan::format_vbr(p.vbr), src_rate, label, nchans,
                     plan::format_tools(p.tools), out_path);
        fmt::println(status,
                     "  access unit size: {}-{} bytes, {:.0f} bytes mean (~{:.0f} kbps mean)",
                     out_sink.min_bytes(), out_sink.max_bytes(), mean_bytes, mean_kbps);
    } else {
        fmt::println(status,
                     "encoded {} E-AC-3 access units ({} kbps, {} Hz, {}, {} coded channels, "
                     "tools: {}) to {}",
                     out_sink.frames(), bitrate, src_rate, label, nchans,
                     plan::format_tools(p.tools), out_path);
    }
    print_routing(p, *routing, label, status);
    if (second) {
        fmt::println(status,
                     "  programme 1 (§E2.3.1.2 I1): {} kbps, {}, {} coded channels, "
                     "dialnorm {} from {}",
                     second->p.bitrate_kbps, second->label, second_nchans,
                     second->p.meta.dialnorm, second->path);
        print_routing(second->p, second->routing, second->label, status);
    }
    return 0;
}

int run_encode_multi(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
                     bool couple, std::string_view layout, const Options& meta) {
    auto sources = load_sources(in_path, meta.sources, meta.offsets);
    if (!sources) {
        return 1;
    }
    const auto sr = wav_sample_rate(sources->sample_rate, "AC-3", false);
    if (!sr) {
        return 1;
    }
    plan::Plan p{.codec = plan::Codec::kAc3,
                 .sample_rate = *sr,
                 .bitrate_kbps = bitrate,
                 .meta = meta.p};
    std::string label;
    if (layout.empty()) {
        std::size_t total_channels = 0;
        for (const auto& shape : sources->shapes) {
            total_channels += shape.channels;
        }
        const auto id = plan::layout_for_source(total_channels);
        if (!id || !plan::carries(plan::Codec::kAc3, *id)) {
            fmt::println(stderr,
                         "error: encode handles 1 to 6 channels ({} given); no AC-3 coding "
                         "mode is wider than 3/2 + LFE",
                         total_channels);
            return 1;
        }
        p.layout = *id;
        label = std::string(plan::layout(*id).label);
    } else if (!resolve_layout(layout, plan::Codec::kAc3, p, label)) {
        return 1;
    }
    p.tools.coupling = couple;
    p.tools.fast_mdct = meta.fast_mdct;
    p.tools.search = meta.search;
    p.tools.dither = meta.dither;
    if (const auto bad = plan::validate(p)) {
        fmt::println(stderr, "error: {}", plan::describe(*bad));
        return 1;
    }

    const auto routing = routing_for_sources(p, *sources, meta.map_spec);
    if (!routing) {
        return 1;
    }

    const auto nchans = static_cast<std::size_t>(routing->coded_channels);
    const std::size_t total = sources->total_frames;
    const auto source_channels = static_cast<std::size_t>(routing->source_channels);

    std::vector<std::vector<float>> source(source_channels,
                                           std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::vector<float>> block(nchans, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> in(source_channels);
    std::vector<std::span<float>> out(nchans);
    std::vector<std::span<const float>> views(nchans);
    std::vector<std::span<const float>> metered(nchans);
    for (std::size_t c = 0; c < nchans; ++c) {
        out[c] = block[c];
        views[c] = block[c];
    }
    // Renders one frame's worth of every source's samples onto the coded
    // channels `out`/`views` alias - shared by the measurement pre-pass below
    // and the real encode loop after it, so the two can never render this
    // programme two different ways.
    auto route_frame = [&](std::size_t start) {
        gather_frame(*sources, start, source);
        for (std::size_t c = 0; c < source_channels; ++c) {
            in[c] = source[c];
        }
        plan::render(*routing, in, out, ac3::kSamplesPerFrame);
    };

    const auto cp = plan::resolve(p);
    const bool dual_mono = cp.bed_acmod == ac3::Acmod::kDualMono;
    const bool want_dialnorm = p.meta.measure_dialnorm;
    // dialnorm2 only means anything under 1+1 - silently inert otherwise,
    // exactly like run_encode's identical check for its one file.
    const bool want_dialnorm2 = dual_mono && p.meta.measure_dialnorm2;
    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the AC-3 bytes this function writes below already own stdout in that
    // case, and no human-readable report (the dialnorm=auto measurement just
    // below included) may land in the middle of them.
    const auto status = status_stream(out_path);
    if (want_dialnorm || want_dialnorm2) {
        // §5.4.2.8's BS.1770 pass has to measure what the encoder actually
        // receives - the routed/rendered coded channels, not each source's
        // own raw layout, since map= can permute, trim or fold several
        // sources onto them - so this renders the entire programme once
        // purely to measure it. Dual mono gets one single-channel meter per
        // programme (Ch1/Ch2 are unrelated, §E1.3 - see
        // measured_dialnorm_channel's own comment); every other target gets
        // one whole-programme meter, the same BS.1770 channel weighting
        // measured_dialnorm uses for the single-file case.
        std::optional<ac3::meta::LoudnessMeter> whole;
        std::optional<ac3::meta::LoudnessMeter> ch1;
        std::optional<ac3::meta::LoudnessMeter> ch2;
        if (dual_mono) {
            if (want_dialnorm) {
                ch1.emplace(*sr, ac3::Acmod::k1_0, false);
            }
            if (want_dialnorm2) {
                ch2.emplace(*sr, ac3::Acmod::k1_0, false);
            }
        } else if (want_dialnorm) {
            whole.emplace(*sr, cp.bed_acmod, cp.bed_lfe);
        }
        for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
            route_frame(start);
            if (whole) {
                whole->push(views);
            }
            if (ch1) {
                const std::array<std::span<const float>, 1> v{views[0]};
                ch1->push(v);
            }
            if (ch2) {
                const std::array<std::span<const float>, 1> v{views[1]};
                ch2->push(v);
            }
        }
        if (want_dialnorm) {
            const auto measured = dual_mono ? finish_measurement(*ch1, "Ch1", "dialnorm", status)
                                            : finish_measurement(*whole, {}, "dialnorm", status);
            if (!measured) {
                fmt::println(stderr, "error: {}no audio above the -70 LKFS absolute gate; "
                                     "pass dialnorm=<1..31> explicitly",
                             dual_mono ? "Ch1 has " : "");
                return 1;
            }
            p.meta.dialnorm = *measured;
        }
        if (want_dialnorm2) {
            const auto measured2 = finish_measurement(*ch2, "Ch2", "dialnorm2", status);
            if (!measured2) {
                fmt::println(stderr, "error: Ch2 has no audio above the -70 LKFS absolute gate; "
                                     "pass dialnorm2=<1..31> explicitly");
                return 1;
            }
            p.meta.dialnorm2 = *measured2;
        }
    }

    const auto config = plan::ac3_config(p);
    auto encoder = std::make_unique<ac3::FrameEncoder>(config);
    ac3::analysis::LevelMeter meter{config.acmod, config.lfe, sources->sample_rate};
    // Streamed out as encoded, exactly as run_encode below - see
    // run_eac3_encode_multi's identical note.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, meta.keep_partial)) {
        return 1;
    }
    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        const auto valid = std::min<std::size_t>(ac3::kSamplesPerFrame, total - start);
        route_frame(start);
        for (std::size_t c = 0; c < nchans; ++c) {
            metered[c] = std::span{block[c]}.first(valid);
        }
        meter.process(metered);
        auto frame = encoder->encode_frame(views);
        if (!frame) {
            fmt::println(stderr, "error: bitrate must be a legal AC-3 rate");
            out_sink.abort();
            return 1;
        }
        if (!out_sink.push(std::move(*frame))) {
            out_sink.abort();
            return 1;
        }
    }
    if (!out_sink.close()) {
        return 1;
    }
    fmt::println(status, "encoded {} frames ({} kbps, {} Hz, {}) to {}", out_sink.frames(),
                 bitrate, sources->sample_rate,
                 ac3::analysis::layout_name(config.acmod, config.lfe), out_path);
    print_routing(p, *routing, label, status);
    print_channel_summary(meter, status);
    return 0;
}

int run_encode(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
               bool couple, std::string_view layout, const Options& meta,
               std::string_view in2_path) {
    if (!meta.sources.empty() || meta.map_spec) {
        if (!in2_path.empty()) {
            fmt::println(stderr,
                         "error: use either a second positional file or src=/map=, not both");
            return 1;
        }
        return run_encode_multi(in_path, out_path, bitrate, couple, layout, meta);
    }
    // A seekable file whose whole-programme passes are not needed - no
    // dialnorm=auto BS.1770 measurement, no second dual-mono file to merge -
    // streams one frame-sized block at a time, holding ~40 KB of samples
    // resident instead of the whole file plus its planar float copy (the
    // measured peak was linear in duration before this: 152 MiB for a 60 s
    // 5.1 encode, 438 MiB for 180 s). Everything else takes the whole-file
    // read below, unchanged - including a failed open, which falls through
    // so read_wav_arg can produce the error message it always has.
    ac3::io::WavStreamReader stream_in;
    const bool streaming = !is_stdio_path(in_path) && !meta.p.measure_dialnorm &&
                           !meta.p.measure_dialnorm2 && in2_path.empty() &&
                           stream_in.open(std::string{in_path}).has_value();
    std::expected<ac3::io::WavData, ac3::io::WavError> wav =
        std::unexpected(ac3::io::WavError::kCannotOpen);
    if (!streaming) {
        wav = read_wav_arg(in_path);
        if (!wav) {
            fmt::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
            return 1;
        }
        if (!prepare_dual_mono_source(*wav, layout, in2_path)) {
            return 1;
        }
    } else if (layout == "1+1" && stream_in.channels() != 2) {
        // The same refusal prepare_dual_mono_source gives the one-file 1+1
        // case; the streaming path validates off the header instead.
        fmt::println(stderr,
                     "error: layout 1+1 needs either one two-channel file (Ch1, Ch2) or "
                     "two mono files; the source has {} channel(s) and no second file "
                     "was given",
                     stream_in.channels());
        return 1;
    }
    const std::uint32_t src_rate = streaming ? stream_in.sample_rate() : wav->sample_rate;
    const std::size_t src_channels =
        streaming ? stream_in.channels() : wav->channels.size();
    const auto sr = wav_sample_rate(src_rate, "AC-3", false);
    if (!sr) {
        return 1;
    }
    plan::Plan p{.codec = plan::Codec::kAc3,
                 .sample_rate = *sr,
                 .bitrate_kbps = bitrate,
                 .meta = meta.p};
    std::string label;
    if (layout.empty()) {
        // An unnamed layout follows the source, which is what this command
        // did before it could be told otherwise. Naming one is how a stereo
        // file reaches a 5.1 stream, or a 5.1 file gets folded down per §7.8.
        const auto id = plan::layout_for_source(src_channels);
        if (!id || !plan::carries(plan::Codec::kAc3, *id)) {
            fmt::println(stderr,
                         "error: encode handles 1 to 6 channels ({} given); no AC-3 coding "
                         "mode is wider than 3/2 + LFE",
                         src_channels);
            return 1;
        }
        p.layout = *id;
        label = std::string(plan::layout(*id).label);
    } else if (!resolve_layout(layout, plan::Codec::kAc3, p, label)) {
        return 1;
    }
    p.tools.coupling = couple;
    p.tools.fast_mdct = meta.fast_mdct;
    p.tools.search = meta.search;
    p.tools.dither = meta.dither;
    if (const auto bad = plan::validate(p)) {
        fmt::println(stderr, "error: {}", plan::describe(*bad));
        return 1;
    }

    // §5.4.2.8 says dialnorm "shall affect the sound reproduction level", so
    // getting it wrong is not a cosmetic error - a stream that claims 31 when
    // dialogue is really at -18 plays 13 dB too loud on a levelled system.
    // Measuring needs the whole programme (the BS.1770 relative gate does),
    // which is why it happens here rather than inside the frame encoder. It
    // gets the OUTPUT layout, because the BS.1770 channel weighting depends on
    // which coded positions are surrounds.
    const auto cp = plan::resolve(p);
    const bool dual_mono = cp.bed_acmod == ac3::Acmod::kDualMono;
    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the AC-3 bytes this function writes below already own stdout in that
    // case, and no human-readable report (the dialnorm=auto measurement just
    // below included) may land in the middle of them.
    const auto status = status_stream(out_path);
    if (p.meta.measure_dialnorm) {
        // Dual mono has no "whole programme" a single BS.1770 pass can mean -
        // Ch1 and Ch2 are unrelated (§E1.3, no downmix between them), so this
        // measures Ch1's own channel alone, exactly like Ch2 just below,
        // rather than folding both into one measured_dialnorm() call the way
        // every other layout can.
        const auto measured = dual_mono
                                  ? measured_dialnorm_channel(wav->channels[0], *sr, "Ch1",
                                                              "dialnorm", status)
                                  : measured_dialnorm(*wav, *sr, cp.bed_acmod, cp.bed_lfe, status);
        if (!measured) {
            fmt::println(stderr,
                         "error: {}no audio above the -70 LKFS absolute gate; "
                         "pass dialnorm=<1..31> explicitly",
                         dual_mono ? "Ch1 has " : "");
            return 1;
        }
        p.meta.dialnorm = *measured;
    }
    if (dual_mono && p.meta.measure_dialnorm2) {
        const auto measured2 =
            measured_dialnorm_channel(wav->channels[1], *sr, "Ch2", "dialnorm2", status);
        if (!measured2) {
            fmt::println(stderr,
                         "error: Ch2 has no audio above the -70 LKFS absolute gate; "
                         "pass dialnorm2=<1..31> explicitly");
            return 1;
        }
        p.meta.dialnorm2 = *measured2;
    }

    const auto routing = routing_or_error(p, src_channels);
    if (!routing) {
        return 1;
    }

    const auto config = plan::ac3_config(p);
    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state, and this function only constructs it once (PREfast's C6262).
    auto encoder = std::make_unique<ac3::FrameEncoder>(config);
    ac3::analysis::LevelMeter meter{config.acmod, config.lfe, src_rate};
    const auto nchans = static_cast<std::size_t>(routing->coded_channels);
    // The classic path has exactly one source, always index 0 in offset='s
    // numbering - see LoadedSources::offset_samples for the multi-source
    // equivalent of this same leading silence.
    const std::size_t offset = offset_samples_for(meta.offsets, 0, src_rate);
    const std::size_t frame_count =
        streaming ? static_cast<std::size_t>(stream_in.frame_count()) : wav->frame_count();
    const std::size_t total = offset + frame_count;

    std::vector<std::vector<float>> source(src_channels,
                                           std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::vector<float>> block(nchans, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> in(source.size());
    std::vector<std::span<float>> out(nchans);
    std::vector<std::span<const float>> views(nchans);
    std::vector<std::span<const float>> metered(nchans);
    for (std::size_t c = 0; c < nchans; ++c) {
        out[c] = block[c];
        views[c] = block[c];
    }
    // The encoded frames leave as they are produced - see EncodedStreamSink
    // for how a failed run still honours keep-partial exactly as the old
    // accumulate-then-write shape did.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, meta.keep_partial)) {
        return 1;
    }
    // The streaming path's own state: the frame loop below asks for the
    // source's samples strictly in order (offset= only ever shifts where
    // they land inside a frame, never which come next), so a rolling read
    // position plus the last real sample per channel - for the same
    // hold-padding the whole-file path applies - is all it takes.
    std::vector<float> stream_hold(src_channels, 0.0f);
    std::vector<std::span<float>> stream_dst(src_channels);
    std::size_t consumed = 0;
    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        // The tail frame is padded to a full 1536 samples; the meter sees only
        // the real ones, so the padding cannot pull the RMS down. Padding
        // holds the last real sample rather than dropping to hard zero: a
        // sudden drop to silence is itself a transient, and the encoder's own
        // §8.2.2 detector would (correctly) spend a block-switch on it,
        // paying real side-info bits to preserve a discontinuity that exists
        // only because this frame ends mid-buffer, not in the source audio.
        // Ahead of the source's own samples, offset= silence is real
        // silence, not padding.
        const auto valid = std::min<std::size_t>(ac3::kSamplesPerFrame, total - start);
        if (streaming) {
            const std::size_t lead =
                start < offset ? std::min<std::size_t>(offset - start, ac3::kSamplesPerFrame)
                               : 0;
            std::size_t want = 0;
            if (lead < ac3::kSamplesPerFrame) {
                const std::size_t remaining = frame_count - std::min(frame_count, consumed);
                want = std::min<std::size_t>(ac3::kSamplesPerFrame - lead, remaining);
            }
            for (std::size_t c = 0; c < src_channels; ++c) {
                std::fill_n(source[c].begin(), lead, 0.0f);
                stream_dst[c] = std::span{source[c]}.subspan(lead, want);
            }
            if (want > 0) {
                const auto got = stream_in.read_planar(stream_dst, want);
                if (!got || *got != want) {
                    fmt::println(stderr, "error: {}: {}", in_path,
                                 ac3::io::describe(got ? ac3::io::WavError::kTruncated
                                                       : got.error()));
                    out_sink.abort();
                    return 1;
                }
                consumed += want;
            }
            for (std::size_t c = 0; c < src_channels; ++c) {
                if (want > 0) {
                    stream_hold[c] = source[c][lead + want - 1];
                }
                std::fill(source[c].begin() + static_cast<std::ptrdiff_t>(lead + want),
                          source[c].end(), stream_hold[c]);
                in[c] = source[c];
            }
        } else {
            for (std::size_t c = 0; c < source.size(); ++c) {
                const float hold = frame_count > 0 ? wav->channels[c][frame_count - 1] : 0.0f;
                for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
                    const std::size_t at = start + static_cast<std::size_t>(i);
                    if (at < offset) {
                        source[c][static_cast<std::size_t>(i)] = 0.0f;
                        continue;
                    }
                    const std::size_t shifted = at - offset;
                    source[c][static_cast<std::size_t>(i)] =
                        shifted < frame_count ? wav->channels[c][shifted] : hold;
                }
                in[c] = source[c];
            }
        }
        plan::render(*routing, in, out, ac3::kSamplesPerFrame);
        for (std::size_t c = 0; c < nchans; ++c) {
            metered[c] = std::span{block[c]}.first(valid);
        }
        meter.process(metered);
        auto frame = encoder->encode_frame(views);
        if (!frame) {
            fmt::println(stderr, "error: bitrate must be a legal AC-3 rate");
            out_sink.abort();
            return 1;
        }
        if (!out_sink.push(*frame)) {
            out_sink.abort();
            return 1;
        }
    }
    if (!out_sink.close()) {
        return 1;
    }
    fmt::println(status, "encoded {} frames ({} kbps, {} Hz, {}) to {}", out_sink.frames(),
                bitrate, src_rate, ac3::analysis::layout_name(config.acmod, config.lfe),
                out_path);
    print_routing(p, *routing, label, status);
    print_channel_summary(meter, status);
    return 0;
}

}  // namespace ac3cli::commands

#include "encode.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <memory>
#include <optional>
#include <fmt/base.h>
#include <fmt/format.h>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../exit_codes.hpp"
#include "../support.hpp"
#include "ac3/analysis/levels.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/verify/eac3_selfcheck.hpp"
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

// An extra programme's own input file (programmeN=), read one frame at a
// time.
//
// Its own reader rather than a second pass through the primary path's
// streaming state: the sources are consumed in lockstep but are otherwise
// unrelated - different channel counts, different lengths, different routings
// - and the primary's inline state is already threaded through a long
// function. Streams like the primary does, falling back to a whole-file read
// for the inputs WavStreamReader declines (stdin among them), so an extra
// programme costs no more memory than the first for the ordinary file case.
class ProgrammeSource {
   public:
    // `allow_streaming` is false when this programme's own dialnorm=auto
    // needs a measurement pre-pass ahead of the real encode loop (see
    // open_extra_programme): the streaming reader can only be walked once,
    // forward, so a caller that has to read this source twice needs the
    // whole file resident instead - the same reason run_eac3_encode itself
    // disables streaming for the primary under measure_dialnorm.
    bool open(std::string_view path, bool allow_streaming = true) {
        if (allow_streaming && !is_stdio_path(path) &&
            stream_.open(std::string{path}).has_value()) {
            streaming_ = true;
            hold_.assign(channels(), 0.0f);
            return true;
        }
        auto whole = read_wav_arg(path);
        if (!whole.has_value()) {
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

    // Fills `dest` (one vector per channel, each `frame_len` long - usually
    // kSamplesPerFrame, shorter when the primary programme's own numblkscod
    // is pinned below its default) with the samples starting at `start`.
    // Past end-of-file every channel holds its own last real sample rather
    // than dropping to zero, for exactly the reason the primary path does: a
    // sudden drop to silence is itself a transient the encoder would
    // (correctly) spend a block switch on, for a discontinuity that only
    // exists because the file ended mid-frame. The streaming form ignores
    // `start` beyond checking it advances in order, which the single encode
    // loop guarantees.
    bool fill(std::size_t start, std::vector<std::vector<float>>& dest, std::size_t frame_len,
              std::string_view path) {
        const std::size_t frames = frame_count();
        if (!streaming_) {
            for (std::size_t c = 0; c < dest.size(); ++c) {
                const float hold = frames > 0 ? whole_.channels[c][frames - 1] : 0.0f;
                for (std::size_t i = 0; i < frame_len; ++i) {
                    const std::size_t at = start + i;
                    dest[c][i] = at < frames ? whole_.channels[c][at] : hold;
                }
            }
            return true;
        }
        const std::size_t want =
            std::min<std::size_t>(frame_len, frames - std::min(frames, consumed_));
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

// One extra programme's plan, its source and its routing, assembled together
// because the encode loop needs all three in step.
struct PlannedExtraProgramme {
    plan::Plan p;
    std::string label;
    // Kept here rather than re-read from the Options::ExtraProgramme at every
    // use: this struct only exists once that slot HAS a path, so carrying it
    // means nothing downstream has to re-establish that.
    std::string path;
    // §E2.3.1.2's own numbering for THIS programme - 1 for programme2= (I1)
    // up to 7 for programme8= (I7) - kept for status/error messages so they
    // name the same Ix a caller's programmeN= token did.
    int index = 0;
    ProgrammeSource source;
    plan::Routing routing;
};

// Opens and plans one extra programme, or reports why not and returns
// nullptr. `n` is this programme's own §E2.3.1.2 number (2 for programme2=,
// up to 8), used only to phrase messages the way the token that named it
// reads. `rate` is the PRIMARY programme's sample rate: every substream of an
// access unit codes the same frame period, so an extra programme sampled
// differently cannot ride along. On nullptr, `exit_code` holds the class the
// refusal belongs in (exit_codes.hpp) - the same class run_eac3_encode gives
// the matching refusal of its primary programme: an unreadable source or
// one this format cannot carry is an input error, a layout/routing/rate
// combination the command line asked for is a usage one, and nothing above
// the loudness gate is a runtime one.
std::unique_ptr<PlannedExtraProgramme> open_extra_programme(int n,
                                                            const Options::ExtraProgramme& extra,
                                                            ac3::SampleRate rate,
                                                            std::uint32_t primary_kbps,
                                                            const plan::Tools& tools,
                                                            FILE* status, int& exit_code) {
    assert(extra.path.has_value());
    exit_code = kExitUsage;
    auto out = std::make_unique<PlannedExtraProgramme>();
    out->path = *extra.path;
    out->index = n - 1;
    // dialnorm=auto needs to read this programme's own source TWICE - once to
    // measure, once for the real encode loop below - which the streaming
    // reader cannot do (it only ever walks forward). Load the whole file
    // instead, the same trade run_eac3_encode itself makes for the primary.
    if (!out->source.open(out->path, /*allow_streaming=*/!extra.meta.measure_dialnorm)) {
        exit_code = kExitInput;
        return nullptr;
    }
    const auto extra_rate = wav_sample_rate(out->source.sample_rate(), "E-AC-3", true);
    if (!extra_rate.has_value()) {
        exit_code = kExitInput;
        return nullptr;
    }
    if (*extra_rate != rate) {
        fmt::println(stderr,
                     "error: programme{}= is {} Hz but the primary programme is {} Hz - every "
                     "substream of an access unit codes the same frame period",
                     n, out->source.sample_rate(), sample_rate_hz(rate));
        return nullptr;
    }
    out->p.codec = plan::Codec::kEac3;
    out->p.sample_rate = rate;
    // Half the primary's rate by default: an associated service is normally
    // much narrower than the main mix, and it is spent ON TOP of the
    // primary's, not carved out of it.
    out->p.bitrate_kbps = extra.bitrate.value_or(std::max<std::uint32_t>(primary_kbps / 2, 32));
    out->p.tools = tools;
    // Its own dialnorm, DRC profile, bsmod and full mixmdate group - never the
    // primary's, and never shared between two extra programmes either: a
    // commentary track is levelled independently of the mix it plays
    // against, which is the whole point of carrying it as a separate
    // programme. See Options::ExtraProgramme::meta / parse_programme_metadata_option.
    out->p.meta = extra.meta;
    if (extra.layout.empty()) {
        const auto id = plan::layout_for_source(out->source.channels());
        if (!id.has_value()) {
            fmt::println(stderr, "error: {} has {} channels - {}", out->path,
                         out->source.channels(),
                         plan::describe(plan::PlanError::kNoSourceLayout));
            return nullptr;
        }
        out->p.layout = *id;
        out->label = std::string(plan::layout(*id).label);
    } else if (!resolve_layout(extra.layout, plan::Codec::kEac3, out->p, out->label)) {
        return nullptr;
    }
    if (plan::resolve(out->p).bed_acmod == ac3::Acmod::kDualMono) {
        // 1+1 is itself two programmes sharing one syncframe (§E1.3), levelled
        // by dialnorm and dialnorm2. Stacking it inside a second independent
        // substream would mean three programmes described by two different
        // mechanisms, with only one dialnorm reachable from here - refuse it
        // rather than emit something whose second half cannot be levelled.
        fmt::println(stderr,
                     "error: programme{}-layout=1+1 is not supported: 1+1 already carries two "
                     "programmes in one substream. Use it on the primary programme, or give "
                     "programme{} a layout of its own",
                     n, n);
        return nullptr;
    }
    auto routing = routing_or_error(out->p, out->source.channels());
    if (!routing.has_value()) {
        return nullptr;
    }
    out->routing = std::move(*routing);
    if (out->p.meta.measure_dialnorm) {
        // The same §5.4.2.8 BS.1770 pass run_eac3_encode gives the primary,
        // over this programme's own routed/rendered coded channels - dual
        // mono's Ch1/Ch2 split does not apply here, since 1+1 is refused as
        // an extra programme's layout just above.
        const auto cp = plan::resolve(out->p);
        ac3::meta::LoudnessMeter meter{rate, cp.bed_acmod, cp.bed_lfe};
        const auto frame_len = static_cast<std::size_t>(
            ac3::eac3::blocks_per_syncframe(tools.numblkscod) * ac3::kSamplesPerBlock);
        const auto frames = out->source.frame_count();
        std::vector<std::vector<float>> src(out->source.channels(),
                                            std::vector<float>(frame_len));
        std::vector<std::span<const float>> in(src.size());
        std::vector<std::vector<float>> coded(
            static_cast<std::size_t>(out->routing.coded_channels), std::vector<float>(frame_len));
        std::vector<std::span<float>> coded_out(coded.size());
        std::vector<std::span<const float>> coded_views(coded.size());
        for (std::size_t c = 0; c < coded.size(); ++c) {
            coded_out[c] = coded[c];
            coded_views[c] = coded[c];
        }
        for (std::size_t start = 0; start < frames; start += frame_len) {
            if (!out->source.fill(start, src, frame_len, out->path)) {
                exit_code = kExitInput;
                return nullptr;
            }
            for (std::size_t c = 0; c < src.size(); ++c) {
                in[c] = src[c];
            }
            plan::render(out->routing, in, coded_out, frame_len);
            meter.push(coded_views);
        }
        const auto measured =
            finish_measurement(meter, fmt::format("programme{}", n), "dialnorm", status);
        if (!measured.has_value()) {
            fmt::println(stderr,
                         "error: programme{} has no audio above the -70 LKFS absolute gate; "
                         "pass programme{}-dialnorm=<1..31> explicitly",
                         n, n);
            exit_code = kExitRuntime;
            return nullptr;
        }
        out->p.meta.dialnorm = *measured;
    }
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
// here, before a frame is attempted, and diagnosed. Takes the count rather
// than the encoder itself so the same check covers Eac3Units below, whose
// verify=true path wraps a MirrorEncoder instead of a bare AccessUnitEncoder.
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
bool eac3_config_accepted(int channel_count, std::uint32_t bitrate, ac3::SampleRate rate,
                          bool vbr) {
    if (channel_count != 0) {
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

// eac3-encode's access-unit source: the plain encoder, or - when the operator
// passed `verify` - ac3::verify's mirror self-check wrapped around the same
// encoder, which decodes every access unit it emits and diffs the decoder's
// model against the encoder's own (ac3/verify/eac3_mirror.hpp).
//
// One type for both so the two encode loops below stay one loop each. The
// mirror is non-movable (its configs hold pointers into its own trace
// members), hence the unique_ptr rather than an optional.
class Eac3Units {
   public:
    Eac3Units(const ac3::eac3::AccessUnitConfig& config, bool verify)
        : plain_(verify ? nullptr : std::make_unique<ac3::eac3::AccessUnitEncoder>(config)),
          checked_(verify ? std::make_unique<ac3::verify::Eac3MirrorEncoder>(config)
                          : nullptr) {}

    [[nodiscard]] int channel_count() const {
        return plain_ ? plain_->channel_count() : checked_->channel_count();
    }

    // One access unit, or std::nullopt with the error already printed. A
    // self-check disagreement refuses the run rather than warning about it:
    // the two sides having parted company means the stream this command is
    // writing is not the stream it thinks it is.
    [[nodiscard]] std::optional<std::vector<std::byte>> next(
        std::span<const std::span<const float>> channels) {
        if (plain_) {
            auto unit = plain_->encode_access_unit(channels);
            if (!unit.has_value()) {
                fmt::println(stderr, "error: the encoder cannot express this configuration");
                return std::nullopt;
            }
            return std::move(unit->bytes);
        }
        auto unit = checked_->encode_access_unit(channels);
        if (!unit.has_value()) {
            fmt::println(stderr, "error: the encoder cannot express this configuration");
            return std::nullopt;
        }
        if (!unit->ok()) {
            fmt::println(stderr,
                         "error: verify: the encoder and decoder disagree about access unit {}",
                         checked_->frames_encoded() - 1);
            const auto report = checked_->last_report();
            if (!report.empty()) {
                fmt::println(stderr, "{}", report);
            }
            if (unit->decode_error.has_value()) {
                fmt::println(stderr, "  the decoder also refused a substream outright ({})",
                             ac3::describe(*unit->decode_error));
            }
            return std::nullopt;
        }
        return std::move(unit->unit.bytes);
    }

    [[nodiscard]] std::uint64_t checked_units() const {
        return checked_ ? checked_->frames_encoded() : 0;
    }
    [[nodiscard]] bool verifying() const { return checked_ != nullptr; }

   private:
    std::unique_ptr<ac3::eac3::AccessUnitEncoder> plain_;
    std::unique_ptr<ac3::verify::Eac3MirrorEncoder> checked_;
};

}  // namespace

int run_eac3_encode_multi(std::string_view in_path, std::string_view out_path,
                          std::uint32_t bitrate, std::string_view tools,
                          std::string_view layout, std::string_view vbr, const Options& meta) {
    auto sources = load_sources(in_path, meta.sources, meta.offsets);
    if (!sources.has_value()) {
        return kExitInput;
    }
    const auto sr = wav_sample_rate(sources->sample_rate, "E-AC-3", true);
    if (!sr.has_value()) {
        return kExitInput;
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
        if (!id.has_value()) {
            fmt::println(stderr, "error: {} channels - {}", total_channels,
                         plan::describe(plan::PlanError::kNoSourceLayout));
            return kExitUsage;
        }
        p.layout = *id;
        label = std::string(plan::layout(*id).label);
    } else if (!resolve_layout(layout, plan::Codec::kEac3, p, label)) {
        return kExitUsage;
    }

    p.tools.fast_mdct = meta.fast_mdct;
    p.tools.search = meta.search;
    p.tools.fgaincod = meta.fgaincod;
    p.tools.dither = meta.dither;
    p.tools.delta = meta.delta;
    if (!tools_or_error(tools, p.tools)) {
        return kExitUsage;
    }
    if (!vbr_or_error(vbr, p.vbr)) {
        return kExitUsage;
    }

    const auto routing = routing_for_sources(p, *sources, meta.map_spec);
    if (!routing.has_value()) {
        return kExitUsage;
    }

    const auto nchans = static_cast<std::size_t>(routing->coded_channels);
    const std::size_t total = sources->total_frames;
    const auto source_channels = static_cast<std::size_t>(routing->source_channels);
    // Usually kSamplesPerFrame - shorter when the caller pinned numblkscod
    // (the "numblkscod:N" tools token) to something below its default 3.
    const auto samples_per_frame = static_cast<std::size_t>(
        ac3::eac3::blocks_per_syncframe(p.tools.numblkscod) * ac3::kSamplesPerBlock);

    std::vector<std::vector<float>> source(source_channels,
                                           std::vector<float>(samples_per_frame));
    std::vector<std::vector<float>> block(nchans, std::vector<float>(samples_per_frame));
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
        gather_frame(*sources, start, source, static_cast<int>(samples_per_frame));
        for (std::size_t c = 0; c < source_channels; ++c) {
            in[c] = source[c];
        }
        plan::render(*routing, in, out, samples_per_frame);
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
        Progress progress;
        progress.start("measuring", (total + samples_per_frame - 1) / samples_per_frame);
        for (std::size_t start = 0; start < total; start += samples_per_frame) {
            route_frame(start);
            if (whole.has_value()) {
                whole->push(views);
            }
            if (ch1.has_value()) {
                const std::array<std::span<const float>, 1> v{views[0]};
                ch1->push(v);
            }
            if (ch2.has_value()) {
                const std::array<std::span<const float>, 1> v{views[1]};
                ch2->push(v);
            }
            progress.tick(start / samples_per_frame + 1);
        }
        progress.finish();
        if (want_dialnorm) {
            const auto measured = dual_mono ? finish_measurement(*ch1, "Ch1", "dialnorm", status)
                                            : finish_measurement(*whole, {}, "dialnorm", status);
            if (!measured.has_value()) {
                fmt::println(stderr, "error: {}no audio above the -70 LKFS absolute gate; "
                                     "pass dialnorm=<1..31> explicitly",
                             dual_mono ? "Ch1 has " : "");
                return kExitRuntime;
            }
            p.meta.dialnorm = *measured;
        }
        if (want_dialnorm2) {
            const auto measured2 = finish_measurement(*ch2, "Ch2", "dialnorm2", status);
            if (!measured2.has_value()) {
                fmt::println(stderr, "error: Ch2 has no audio above the -70 LKFS absolute gate; "
                                     "pass dialnorm2=<1..31> explicitly");
                return kExitRuntime;
            }
            p.meta.dialnorm2 = *measured2;
        }
    }

    Eac3Units encoder{plan::eac3_config(p), meta.verify};
    if (!eac3_config_accepted(encoder.channel_count(), bitrate, *sr, p.vbr.has_value())) {
        return kExitUsage;
    }
    assert(static_cast<int>(nchans) == encoder.channel_count());
    // Streamed out as encoded, exactly as run_eac3_encode below - the
    // multi-source shape only differs on the INPUT side (route_frame over
    // whole sources), not in what leaves.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, meta.keep_partial)) {
        return kExitOutput;
    }
    Progress progress;
    progress.start("encoding", (total + samples_per_frame - 1) / samples_per_frame);
    for (std::size_t start = 0; start < total; start += samples_per_frame) {
        route_frame(start);
        auto unit = encoder.next(views);
        if (!unit.has_value()) {
            // Eac3Units::next() already printed the specific error - a self-
            // check disagreement's report, or the encoder's own refusal.
            out_sink.abort();
            return kExitUsage;
        }
        if (!out_sink.push(std::move(*unit))) {
            out_sink.abort();
            return kExitOutput;
        }
        progress.tick(start / samples_per_frame + 1);
    }
    progress.finish();
    if (!out_sink.close()) {
        return kExitOutput;
    }
    if (p.vbr.has_value()) {
        // bitrate_kbps is only the nominal reference vbr's tool heuristics
        // used, not a target - what a VBR run actually spent is the sizes it
        // produced, so that is what gets reported instead of one number.
        const double mean_bytes = out_sink.frames() == 0
                                      ? 0.0
                                      : static_cast<double>(out_sink.total_bytes()) /
                                            static_cast<double>(out_sink.frames());
        const double mean_kbps = mean_bytes * 8.0 * static_cast<double>(sources->sample_rate) /
                                 (1000.0 * static_cast<double>(samples_per_frame));
        status_println(status,
                       "encoded {} E-AC-3 access units (vbr {}, {} Hz, {}, {} coded channels, "
                       "tools: {}) to {}",
                       out_sink.frames(), plan::format_vbr(p.vbr), sources->sample_rate, label,
                       nchans, plan::format_tools(p.tools), out_path);
        status_println(status,
                       "  access unit size: {}-{} bytes, {:.0f} bytes mean (~{:.0f} kbps mean)",
                       out_sink.min_bytes(), out_sink.max_bytes(), mean_bytes, mean_kbps);
    } else {
        status_println(status,
                       "encoded {} E-AC-3 access units ({} kbps, {} Hz, {}, {} coded channels, "
                       "tools: {}) to {}",
                       out_sink.frames(), bitrate, sources->sample_rate, label, nchans,
                       plan::format_tools(p.tools), out_path);
    }
    if (encoder.verifying()) {
        // Only ever printed on success: the check refuses the run at the
        // first disagreement, so reaching here means every unit agreed.
        status_println(status, "  verify: encoder and decoder agree on all {} access units",
                       encoder.checked_units());
    }
    print_routing(p, *routing, label, status);
    return kExitOk;
}

int run_eac3_encode(std::string_view in_path, std::string_view out_path,
                    std::uint32_t bitrate, std::string_view tools, std::string_view layout,
                    std::string_view vbr, const Options& meta,
                    std::string_view in2_path) {
    if (!meta.sources.empty() || meta.map_spec.has_value()) {
        if (!in2_path.empty()) {
            fmt::println(stderr,
                         "error: use either a second positional file or src=/map=, not both");
            return kExitUsage;
        }
        if (std::ranges::any_of(meta.extra_programmes,
                               [](const auto& extra) { return extra.path.has_value(); })) {
            // src=/map= route several sources onto ONE programme's channels;
            // programmeN= adds another programme with its own source and its
            // own routing. Combining them is not ambiguous so much as
            // unimplemented - the multi-source path has no notion of a second
            // programme to assign channels to - so say so rather than
            // silently ignore one of them.
            fmt::println(stderr,
                         "error: programmeN= and src=/map= cannot be combined yet - the "
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
        if (!wav.has_value()) {
            fmt::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
            return kExitInput;        }
        if (!prepare_dual_mono_source(*wav, layout, in2_path)) {
            return kExitInput;
        }
    } else if (layout == "1+1" && stream_in.channels() != 2) {
        fmt::println(stderr,
                     "error: layout 1+1 needs either one two-channel file (Ch1, Ch2) or "
                     "two mono files; the source has {} channel(s) and no second file "
                     "was given",
                     stream_in.channels());
        return kExitUsage;
    }
    const std::uint32_t src_rate = streaming ? stream_in.sample_rate() : wav->sample_rate;
    const std::size_t src_channels =
        streaming ? stream_in.channels() : wav->channels.size();
    const auto sr = wav_sample_rate(src_rate, "E-AC-3", true);
    if (!sr.has_value()) {
        return kExitInput;
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
        if (!id.has_value()) {
            fmt::println(stderr, "error: {} channels - {}", src_channels,
                         plan::describe(plan::PlanError::kNoSourceLayout));
            return kExitUsage;
        }
        p.layout = *id;
        label = std::string(plan::layout(*id).label);
    } else if (!resolve_layout(layout, plan::Codec::kEac3, p, label)) {
        return kExitUsage;
    }

    p.tools.fast_mdct = meta.fast_mdct;
    p.tools.search = meta.search;
    p.tools.fgaincod = meta.fgaincod;
    p.tools.dither = meta.dither;
    p.tools.delta = meta.delta;
    if (!tools_or_error(tools, p.tools)) {
        return kExitUsage;
    }
    if (!vbr_or_error(vbr, p.vbr)) {
        return kExitUsage;
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
        if (!measured.has_value()) {
            fmt::println(stderr, "error: {}no audio above the -70 LKFS absolute gate; "
                                 "pass dialnorm=<1..31> explicitly",
                         dual_mono ? "Ch1 has " : "");
            return kExitRuntime;
        }
        p.meta.dialnorm = *measured;
    }
    if (dual_mono && p.meta.measure_dialnorm2) {
        const auto measured2 =
            measured_dialnorm_channel(wav->channels[1], *sr, "Ch2", "dialnorm2", status);
        if (!measured2.has_value()) {
            fmt::println(stderr, "error: Ch2 has no audio above the -70 LKFS absolute gate; "
                                 "pass dialnorm2=<1..31> explicitly");
            return kExitRuntime;
        }
        p.meta.dialnorm2 = *measured2;
    }

    const auto routing = routing_or_error(p, src_channels);
    if (!routing.has_value()) {
        return kExitUsage;
    }

    // §E2.3.1.2's further independent substreams, when any were asked for:
    // each its own source, layout, rate and metadata, riding in the same
    // access units. Built in order (programme2= first) and refused on a gap
    // - substreamid is assigned sequentially with no way to skip one, so
    // programme4= without programme2=/programme3= cannot become I3 the way
    // its own number promises.
    std::vector<std::unique_ptr<PlannedExtraProgramme>> extra;
    for (std::size_t i = 0; i < meta.extra_programmes.size(); ++i) {
        const auto& slot = meta.extra_programmes[i];
        const int n = static_cast<int>(i) + 2;
        if (!slot.path.has_value()) {
            if (std::ranges::any_of(meta.extra_programmes.begin() +
                                       static_cast<std::ptrdiff_t>(i) + 1,
                                   meta.extra_programmes.end(),
                                   [](const auto& s) { return s.path.has_value(); })) {
                fmt::println(stderr,
                             "error: a later programmeN= was given without programme{}= - "
                             "§E2.3.1.2 assigns substream ids in order, with no gaps",
                             n);
                return kExitUsage;
            }
            break;
        }
        int refused = kExitUsage;
        auto planned = open_extra_programme(n, slot, *sr, bitrate, p.tools, status, refused);
        if (!planned) {
            return refused;
        }
        extra.push_back(std::move(planned));
    }
    auto config = plan::eac3_config(p);
    for (const auto& x : extra) {
        config.additional.push_back(plan::eac3_programme(x->p));
    }
    Eac3Units encoder{config, meta.verify};
    if (!eac3_config_accepted(encoder.channel_count(), bitrate, *sr, p.vbr.has_value())) {
        return kExitUsage;
    }
    const auto nchans = static_cast<std::size_t>(routing->coded_channels);
    std::size_t second_nchans = 0;
    for (const auto& x : extra) {
        second_nchans += static_cast<std::size_t>(x->routing.coded_channels);
    }
    assert(static_cast<int>(nchans + second_nchans) == encoder.channel_count());
    // Usually kSamplesPerFrame - shorter when the caller pinned numblkscod
    // (the "numblkscod:N" tools token) to something below its default 3.
    const auto samples_per_frame = static_cast<std::size_t>(
        ac3::eac3::blocks_per_syncframe(p.tools.numblkscod) * ac3::kSamplesPerBlock);
    // The classic path has exactly one source, always index 0 in offset='s
    // numbering - see LoadedSources::offset_samples for the multi-source
    // equivalent of this same leading silence.
    const std::size_t offset = offset_samples_for(meta.offsets, 0, src_rate);
    const std::size_t frame_count =
        streaming ? static_cast<std::size_t>(stream_in.frame_count()) : wav->frame_count();
    // Every extra programme keeps the run going to whichever source is
    // longer - every programme shares the frame period, so a shorter one
    // holds its last sample rather than the stream ending early on the
    // longest.
    std::size_t total = offset + frame_count;
    for (const auto& x : extra) {
        total = std::max(total, x->source.frame_count());
    }

    std::vector<std::vector<float>> source(src_channels, std::vector<float>(samples_per_frame));
    std::vector<std::vector<float>> block(nchans + second_nchans,
                                          std::vector<float>(samples_per_frame));
    std::vector<std::span<const float>> in(source.size());
    std::vector<std::span<float>> out(nchans);
    // Every coded channel of the access unit: the primary programme's, then
    // each extra one's in turn - the same order encode_access_unit expects
    // them, and the same order the substreams themselves go on the wire.
    std::vector<std::span<const float>> views(nchans + second_nchans);
    for (std::size_t c = 0; c < nchans; ++c) {
        out[c] = block[c];
    }
    for (std::size_t c = 0; c < views.size(); ++c) {
        views[c] = block[c];
    }
    // Each extra programme's own per-frame source and coded-channel spans,
    // aliasing its own slice of `block`'s tail so one encode call sees every
    // programme at once.
    struct ExtraFeed {
        std::vector<std::vector<float>> source;
        std::vector<std::span<const float>> in;
        std::vector<std::span<float>> out;
    };
    std::vector<ExtraFeed> feeds(extra.size());
    {
        std::size_t at = nchans;
        for (std::size_t i = 0; i < extra.size(); ++i) {
            auto& feed = feeds[i];
            feed.source.assign(extra[i]->source.channels(),
                               std::vector<float>(samples_per_frame));
            feed.in.resize(feed.source.size());
            for (std::size_t c = 0; c < feed.source.size(); ++c) {
                feed.in[c] = feed.source[c];
            }
            const auto n = static_cast<std::size_t>(extra[i]->routing.coded_channels);
            feed.out.resize(n);
            for (std::size_t c = 0; c < n; ++c) {
                feed.out[c] = block[at + c];
            }
            at += n;
        }
    }
    // The encoded access units leave as they are produced - see
    // EncodedStreamSink; its stats also feed the VBR report below, which
    // used to re-walk the whole frame list for them.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, meta.keep_partial)) {
        return kExitOutput;
    }
    // See run_encode's identical streaming state: the loop consumes the
    // source strictly in order, so a rolling read position and the last
    // real sample per channel are all the streaming path needs.
    std::vector<float> stream_hold(src_channels, 0.0f);
    std::vector<std::span<float>> stream_dst(src_channels);
    std::size_t consumed = 0;
    Progress progress;
    progress.start("encoding", (total + samples_per_frame - 1) / samples_per_frame);
    for (std::size_t start = 0; start < total; start += samples_per_frame) {
        // Hold the last real sample past end-of-file rather than dropping to
        // hard zero - see run_encode's identical padding for why: a sudden
        // drop to silence is itself a transient the encoder would (correctly)
        // spend a block-switch on, for a discontinuity that only exists
        // because this frame ends mid-buffer. Ahead of the source's own
        // samples, offset= silence is real silence, not padding.
        if (streaming) {
            const std::size_t lead =
                start < offset ? std::min<std::size_t>(offset - start, samples_per_frame) : 0;
            std::size_t want = 0;
            if (lead < samples_per_frame) {
                const std::size_t remaining = frame_count - std::min(frame_count, consumed);
                want = std::min<std::size_t>(samples_per_frame - lead, remaining);
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
                    return kExitInput;
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
                for (std::size_t i = 0; i < samples_per_frame; ++i) {
                    const std::size_t at = start + i;
                    if (at < offset) {
                        source[c][i] = 0.0f;
                        continue;
                    }
                    const std::size_t shifted = at - offset;
                    source[c][i] = shifted < frame_count ? wav->channels[c][shifted] : hold;
                }
                in[c] = source[c];
            }
        }
        plan::render(*routing, in, out, samples_per_frame);
        for (std::size_t i = 0; i < extra.size(); ++i) {
            if (!extra[i]->source.fill(start, feeds[i].source, samples_per_frame,
                                       extra[i]->path)) {
                out_sink.abort();
                return kExitInput;  // that programme's source stopped reading
            }
            plan::render(extra[i]->routing, feeds[i].in, feeds[i].out, samples_per_frame);
        }
        auto unit = encoder.next(views);
        if (!unit.has_value()) {
            // Eac3Units::next() already printed the specific error - a self-
            // check disagreement's report, or the encoder's own refusal.
            out_sink.abort();
            return kExitUsage;
        }
        if (!out_sink.push(std::move(*unit))) {
            out_sink.abort();
            return kExitOutput;
        }
        progress.tick(start / samples_per_frame + 1);
    }
    progress.finish();
    if (!out_sink.close()) {
        return kExitOutput;
    }
    if (p.vbr.has_value()) {
        // bitrate_kbps is only the nominal reference vbr's tool heuristics
        // used, not a target - what a VBR run actually spent is the sizes it
        // produced, so that is what gets reported instead of one number:
        // the sink kept the tally as the units streamed out.
        const double mean_bytes = out_sink.frames() == 0
                                      ? 0.0
                                      : static_cast<double>(out_sink.total_bytes()) /
                                            static_cast<double>(out_sink.frames());
        const double mean_kbps = mean_bytes * 8.0 * static_cast<double>(src_rate) /
                                 (1000.0 * static_cast<double>(samples_per_frame));
        status_println(status,
                       "encoded {} E-AC-3 access units (vbr {}, {} Hz, {}, {} coded channels, "
                       "tools: {}) to {}",
                       out_sink.frames(), plan::format_vbr(p.vbr), src_rate, label, nchans,
                       plan::format_tools(p.tools), out_path);
        status_println(status,
                       "  access unit size: {}-{} bytes, {:.0f} bytes mean (~{:.0f} kbps mean)",
                       out_sink.min_bytes(), out_sink.max_bytes(), mean_bytes, mean_kbps);
    } else {
        status_println(status,
                       "encoded {} E-AC-3 access units ({} kbps, {} Hz, {}, {} coded channels, "
                       "tools: {}) to {}",
                       out_sink.frames(), bitrate, src_rate, label, nchans,
                       plan::format_tools(p.tools), out_path);
    }
    if (encoder.verifying()) {
        // Only ever printed on success: the check refuses the run at the
        // first disagreement, so reaching here means every unit agreed.
        status_println(status, "  verify: encoder and decoder agree on all {} access units",
                       encoder.checked_units());
    }
    print_routing(p, *routing, label, status);
    for (const auto& x : extra) {
        status_println(status,
                       "  programme {} (§E2.3.1.2 I{}): {} kbps, {}, {} coded channels, "
                       "dialnorm {} from {}",
                       x->index, x->index, x->p.bitrate_kbps, x->label,
                       x->routing.coded_channels, x->p.meta.dialnorm, x->path);
        print_routing(x->p, x->routing, x->label, status);
    }
    return kExitOk;
}

int run_encode_multi(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
                     bool couple, std::string_view layout, const Options& meta) {
    auto sources = load_sources(in_path, meta.sources, meta.offsets);
    if (!sources.has_value()) {
        return kExitInput;
    }
    const auto sr = wav_sample_rate(sources->sample_rate, "AC-3", false);
    if (!sr.has_value()) {
        return kExitInput;
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
        if (!id.has_value() || !plan::carries(plan::Codec::kAc3, *id)) {
            fmt::println(stderr,
                         "error: encode handles 1 to 6 channels ({} given); no AC-3 coding "
                         "mode is wider than 3/2 + LFE",
                         total_channels);
            return kExitUsage;
        }
        p.layout = *id;
        label = std::string(plan::layout(*id).label);
    } else if (!resolve_layout(layout, plan::Codec::kAc3, p, label)) {
        return kExitUsage;
    }
    p.tools.coupling = couple;
    p.tools.fast_mdct = meta.fast_mdct;
    p.tools.search = meta.search;
    p.tools.fgaincod = meta.fgaincod;
    p.tools.dither = meta.dither;
    p.tools.delta = meta.delta;
    if (const auto bad = plan::validate(p)) {
        fmt::println(stderr, "error: {}", plan::describe(*bad));
        return kExitUsage;
    }

    const auto routing = routing_for_sources(p, *sources, meta.map_spec);
    if (!routing.has_value()) {
        return kExitUsage;
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
        Progress progress;
        progress.start("measuring", (total + ac3::kSamplesPerFrame - 1) / ac3::kSamplesPerFrame);
        for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
            route_frame(start);
            if (whole.has_value()) {
                whole->push(views);
            }
            if (ch1.has_value()) {
                const std::array<std::span<const float>, 1> v{views[0]};
                ch1->push(v);
            }
            if (ch2.has_value()) {
                const std::array<std::span<const float>, 1> v{views[1]};
                ch2->push(v);
            }
            progress.tick(start / ac3::kSamplesPerFrame + 1);
        }
        progress.finish();
        if (want_dialnorm) {
            const auto measured = dual_mono ? finish_measurement(*ch1, "Ch1", "dialnorm", status)
                                            : finish_measurement(*whole, {}, "dialnorm", status);
            if (!measured.has_value()) {
                fmt::println(stderr, "error: {}no audio above the -70 LKFS absolute gate; "
                                     "pass dialnorm=<1..31> explicitly",
                             dual_mono ? "Ch1 has " : "");
                return kExitRuntime;
            }
            p.meta.dialnorm = *measured;
        }
        if (want_dialnorm2) {
            const auto measured2 = finish_measurement(*ch2, "Ch2", "dialnorm2", status);
            if (!measured2.has_value()) {
                fmt::println(stderr, "error: Ch2 has no audio above the -70 LKFS absolute gate; "
                                     "pass dialnorm2=<1..31> explicitly");
                return kExitRuntime;
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
        return kExitOutput;
    }
    Progress progress;
    progress.start("encoding", (total + ac3::kSamplesPerFrame - 1) / ac3::kSamplesPerFrame);
    for (std::size_t start = 0; start < total; start += ac3::kSamplesPerFrame) {
        const auto valid = std::min<std::size_t>(ac3::kSamplesPerFrame, total - start);
        route_frame(start);
        for (std::size_t c = 0; c < nchans; ++c) {
            metered[c] = std::span{block[c]}.first(valid);
        }
        meter.process(metered);
        auto frame = encoder->encode_frame(views);
        if (!frame.has_value()) {
            fmt::println(stderr, "error: bitrate must be a legal AC-3 rate");
            out_sink.abort();
            return kExitUsage;
        }
        if (!out_sink.push(std::move(*frame))) {
            out_sink.abort();
            return kExitOutput;
        }
        progress.tick(start / ac3::kSamplesPerFrame + 1);
    }
    progress.finish();
    if (!out_sink.close()) {
        return kExitOutput;
    }
    status_println(status, "encoded {} frames ({} kbps, {} Hz, {}) to {}", out_sink.frames(),
                   bitrate, sources->sample_rate,
                   ac3::analysis::layout_name(config.acmod, config.lfe), out_path);
    print_routing(p, *routing, label, status);
    print_channel_summary(meter, status);
    return kExitOk;
}

int run_encode(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
               bool couple, std::string_view layout, const Options& meta,
               std::string_view in2_path) {
    if (!meta.sources.empty() || meta.map_spec.has_value()) {
        if (!in2_path.empty()) {
            fmt::println(stderr,
                         "error: use either a second positional file or src=/map=, not both");
            return kExitUsage;
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
        if (!wav.has_value()) {
            fmt::println(stderr, "error: {}: {}", in_path, ac3::io::describe(wav.error()));
            return kExitInput;        }
        if (!prepare_dual_mono_source(*wav, layout, in2_path)) {
            return kExitInput;
        }
    } else if (layout == "1+1" && stream_in.channels() != 2) {
        // The same refusal prepare_dual_mono_source gives the one-file 1+1
        // case; the streaming path validates off the header instead.
        fmt::println(stderr,
                     "error: layout 1+1 needs either one two-channel file (Ch1, Ch2) or "
                     "two mono files; the source has {} channel(s) and no second file "
                     "was given",
                     stream_in.channels());
        return kExitUsage;
    }
    const std::uint32_t src_rate = streaming ? stream_in.sample_rate() : wav->sample_rate;
    const std::size_t src_channels =
        streaming ? stream_in.channels() : wav->channels.size();
    const auto sr = wav_sample_rate(src_rate, "AC-3", false);
    if (!sr.has_value()) {
        return kExitInput;
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
        if (!id.has_value() || !plan::carries(plan::Codec::kAc3, *id)) {
            fmt::println(stderr,
                         "error: encode handles 1 to 6 channels ({} given); no AC-3 coding "
                         "mode is wider than 3/2 + LFE",
                         src_channels);
            return kExitUsage;
        }
        p.layout = *id;
        label = std::string(plan::layout(*id).label);
    } else if (!resolve_layout(layout, plan::Codec::kAc3, p, label)) {
        return kExitUsage;
    }
    p.tools.coupling = couple;
    p.tools.fast_mdct = meta.fast_mdct;
    p.tools.search = meta.search;
    p.tools.fgaincod = meta.fgaincod;
    p.tools.dither = meta.dither;
    p.tools.delta = meta.delta;
    if (const auto bad = plan::validate(p)) {
        fmt::println(stderr, "error: {}", plan::describe(*bad));
        return kExitUsage;
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
        if (!measured.has_value()) {
            fmt::println(stderr,
                         "error: {}no audio above the -70 LKFS absolute gate; "
                         "pass dialnorm=<1..31> explicitly",
                         dual_mono ? "Ch1 has " : "");
            return kExitRuntime;
        }
        p.meta.dialnorm = *measured;
    }
    if (dual_mono && p.meta.measure_dialnorm2) {
        const auto measured2 =
            measured_dialnorm_channel(wav->channels[1], *sr, "Ch2", "dialnorm2", status);
        if (!measured2.has_value()) {
            fmt::println(stderr,
                         "error: Ch2 has no audio above the -70 LKFS absolute gate; "
                         "pass dialnorm2=<1..31> explicitly");
            return kExitRuntime;
        }
        p.meta.dialnorm2 = *measured2;
    }

    const auto routing = routing_or_error(p, src_channels);
    if (!routing.has_value()) {
        return kExitUsage;
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
        return kExitOutput;
    }
    // The streaming path's own state: the frame loop below asks for the
    // source's samples strictly in order (offset= only ever shifts where
    // they land inside a frame, never which come next), so a rolling read
    // position plus the last real sample per channel - for the same
    // hold-padding the whole-file path applies - is all it takes.
    std::vector<float> stream_hold(src_channels, 0.0f);
    std::vector<std::span<float>> stream_dst(src_channels);
    std::size_t consumed = 0;
    Progress progress;
    progress.start("encoding", (total + ac3::kSamplesPerFrame - 1) / ac3::kSamplesPerFrame);
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
                    return kExitInput;
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
        if (!frame.has_value()) {
            fmt::println(stderr, "error: bitrate must be a legal AC-3 rate");
            out_sink.abort();
            return kExitUsage;
        }
        if (!out_sink.push(*frame)) {
            out_sink.abort();
            return kExitOutput;
        }
        progress.tick(start / ac3::kSamplesPerFrame + 1);
    }
    progress.finish();
    if (!out_sink.close()) {
        return kExitOutput;
    }
    status_println(status, "encoded {} frames ({} kbps, {} Hz, {}) to {}", out_sink.frames(),
                   bitrate, src_rate, ac3::analysis::layout_name(config.acmod, config.lfe),
                   out_path);
    print_routing(p, *routing, label, status);
    print_channel_summary(meter, status);
    return kExitOk;
}

}  // namespace ac3cli::commands

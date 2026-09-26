#include "stream_tools.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fmt/base.h>
#include <fmt/format.h>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../ac4_channels.hpp"
#include "../exit_codes.hpp"
#include "../support.hpp"
#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4enc/encoder.hpp"
#include "analysis.hpp"
#include "ac3/analysis/levels.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/metadata_edit.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/meta/mixing.hpp"
#include "stream_playback.hpp"

namespace ac3cli::commands {

namespace plan = ac3::plan;

// The INPUT is read whole, the same way decode/mkv/mp4/ts/spdif already read
// theirs: an elementary stream has to be framed before anything can be done
// with it, and there is no seek. What the 0.9.0 memory work fixed, and what
// every command here keeps, is the OUTPUT side - bytes leave through
// EncodedStreamSink (or, for 'play's fallback, a hardware sink) as they are
// produced rather than accumulating.
namespace {

// `bytes`, read from `path`, scanned.
std::optional<LoadedStream> scan_loaded(std::vector<std::byte> bytes, std::string_view path) {
    LoadedStream loaded;
    loaded.bytes = std::move(bytes);
    auto scanned = ac3::io::scan(loaded.bytes);
    if (!scanned.has_value()) {
        fmt::println(stderr, "error: {}: {}", path, ac3::io::describe(scanned.error()));
        return std::nullopt;
    }
    loaded.scan = std::move(*scanned);
    return loaded;
}

}  // namespace

std::optional<LoadedStream> load_stream(std::string_view path) {
    auto bytes = read_all(path);
    if (bytes.empty()) {
        fmt::println(stderr, "error: cannot read {}", path);
        return std::nullopt;
    }
    return scan_loaded(std::move(bytes), path);
}

namespace {

std::string_view codec_label(ac3::io::StreamKind kind) {
    // kAc3CoreEac3Extension (§E2.3.1.2's legacy core) reads through the same
    // access-unit path as plain E-AC-3 below - see decode_and_render - so it
    // is labelled the same way rather than defaulting to "AC-3" by falling
    // through a two-way test (see StreamKind's own comment on why that is the
    // wrong instinct for this third kind).
    return kind == ac3::io::StreamKind::kAc3 ? "AC-3" : "E-AC-3";
}

// Writes access units out through the same sink every encoding command uses,
// so "-" and keep-partial behave identically here.
bool write_units(std::string_view out_path, std::span<const std::span<const std::byte>> units,
                 bool keep_partial) {
    EncodedStreamSink sink;
    if (!sink.open(out_path, keep_partial)) {
        return false;
    }
    for (const auto& unit : units) {
        if (!sink.push(unit)) {
            sink.abort();
            return false;
        }
    }
    return sink.close();
}

// --- transcode --------------------------------------------------------------

// Which codec to write. The suffix decides, because `transcode in.ec3
// out.ac3` should not also need to be told what ".ac3" means; codec= covers
// stdout and any name the suffix cannot speak for.
std::optional<plan::Codec> output_codec(std::string_view out_path, const Options& meta) {
    if (meta.codec.has_value()) {
        return meta.codec;
    }
    if (out_path.ends_with(".ac3")) {
        return plan::Codec::kAc3;
    }
    if (out_path.ends_with(".ec3") || out_path.ends_with(".eac3")) {
        return plan::Codec::kEac3;
    }
    if (out_path.ends_with(".ac4")) {
        return plan::Codec::kAc4;
    }
    fmt::println(stderr,
                 "error: cannot tell which codec to write from '{}' - name it .ac3, .ec3 or "
                 ".ac4, or pass codec=ac3|eac3|ac4",
                 out_path);
    return std::nullopt;
}

// E-AC-3's 3-bit mixmdate levels onto the two coarse ones AC-3 has room for
// (Table 5.9 / Table 5.10), by nearest linear COEFFICIENT rather than by enum
// ordinal: the tables do not run in step (MixLevel has +3 dB and +1.5 dB
// entries AC-3 has no equivalent of), so comparing ordinals would silently
// shift a real level.
template <typename Narrow, std::size_t N>
Narrow nearest_level(ac3::meta::MixLevel wide, const std::array<Narrow, N>& candidates) {
    const double target = ac3::meta::coefficient(wide);
    Narrow best = candidates.front();
    double best_error = -1.0;
    for (const auto candidate : candidates) {
        const double error = std::abs(ac3::meta::coefficient(candidate) - target);
        if (best_error < 0.0 || error < best_error) {
            best_error = error;
            best = candidate;
        }
    }
    return best;
}

// The source's downmix intent, expressed in the fields plan::Metadata carries.
//
// AC-3 puts two coarse levels in bsi; E-AC-3 drops those and carries the
// richer mixmdate group instead, so crossing between them is a real
// conversion rather than a copy. What survives in both directions is the
// Lo/Ro pair plus dmixmod and the LFE mix level; the Lt/Rt pair does not,
// because plan::mix_metadata derives it (at a fixed -3 dB) rather than
// accepting one. That is a limitation of the plan layer, not of the wire
// format - and it costs nothing on the DD+-to-DD path this command exists
// for, where AC-3 has no Lt/Rt fields to write anyway.
//
// dmixmod picks WHICH pair to read on the way down: a stream that says its
// intended downmix is Lt/Rt is described by its Lt/Rt levels, and taking
// Lo/Ro there would carry the wrong intent across.
void carry_mix_metadata(const ac3::io::FrameMetadata& source, plan::Metadata& target) {
    static constexpr std::array kCentre{ac3::meta::CentreMixLevel::kMinus3dB,
                                        ac3::meta::CentreMixLevel::kMinus4_5dB,
                                        ac3::meta::CentreMixLevel::kMinus6dB};
    static constexpr std::array kSurround{ac3::meta::SurroundMixLevel::kMinus3dB,
                                          ac3::meta::SurroundMixLevel::kMinus6dB,
                                          ac3::meta::SurroundMixLevel::kSilent};
    if (source.cmixlev.has_value()) {
        target.cmixlev = *source.cmixlev;
    }
    if (source.surmixlev.has_value()) {
        target.surmixlev = *source.surmixlev;
    }
    if (!source.mix.has_value()) {
        return;
    }
    const bool ltrt = source.mix->dmixmod == ac3::meta::DownmixMode::kLtRt;
    const auto centre = ltrt ? source.mix->ltrtcmixlev : source.mix->lorocmixlev;
    const auto surround = ltrt ? source.mix->ltrtsurmixlev : source.mix->lorosurmixlev;
    if (centre.has_value()) {
        target.cmixlev = nearest_level(*centre, kCentre);
    }
    if (surround.has_value()) {
        target.surmixlev = nearest_level(*surround, kSurround);
    }
    if (source.mix->dmixmod.has_value()) {
        // A reserved '11' is carried as "not indicated", §D2.3.1.2's reading
        // of it: the encoder will not write the reserved code itself (see
        // meta::valid_downmix_mode), and there is no preference to keep.
        target.dmixmod = ac3::meta::valid_downmix_mode(*source.mix->dmixmod)
                             ? *source.mix->dmixmod
                             : ac3::meta::DownmixMode::kNotIndicated;
    }
    // §E2.3.1.10: absent means LFE mixing is DISABLED, which is a decision in
    // its own right - so an absent lfemixlevcod is carried across as absent,
    // not as plan::Metadata's own default.
    target.lfemix = source.mix->lfemixlevcod;
}

// One frame's worth of decoded programme, held until the encoder has 1536
// samples to take. An E-AC-3 access unit codes 256, 512, 768 or 1536 samples
// (numblkscod, §E2.3.1.4) while both encoders here always write six blocks,
// so decode-side and encode-side frame lengths do not have to line up and a
// transcode cannot assume they do. O(1) in the stream's length: the queue
// never holds more than one access unit plus one frame.
class SampleQueue {
   public:
    void reset(std::size_t channels) {
        channels_.assign(channels, {});
        consumed_ = 0;
    }

    void push(std::size_t channel, std::span<const float> samples) {
        auto& queue = channels_[channel];
        queue.insert(queue.end(), samples.begin(), samples.end());
    }

    [[nodiscard]] std::size_t available() const {
        if (channels_.empty()) {
            return 0;
        }
        std::size_t least = channels_.front().size();
        for (const auto& queue : channels_) {
            least = std::min(least, queue.size());
        }
        return least - consumed_;
    }

    // Copies `count` samples per channel into `into` (one span per channel,
    // each at least kSamplesPerFrame long) and pads the tail by HOLDING the
    // last real sample rather than dropping to zero - a sudden drop to
    // silence is itself a transient the encoder's §8.2.2 detector would spend
    // a block switch on, for a discontinuity that exists only because the
    // stream ended mid-frame. Same rule run_encode applies to a short WAV.
    void take(std::size_t count, std::span<const std::span<float>> into) {
        for (std::size_t ch = 0; ch < channels_.size(); ++ch) {
            const auto& queue = channels_[ch];
            float hold = 0.0f;
            for (std::size_t i = 0; i < static_cast<std::size_t>(ac3::kSamplesPerFrame); ++i) {
                if (i < count) {
                    hold = queue[consumed_ + i];
                    into[ch][i] = hold;
                } else {
                    into[ch][i] = hold;
                }
            }
        }
        consumed_ += count;
        compact();
    }

   private:
    // Drops what has already been taken once it is worth doing, so the
    // queues do not grow with the stream.
    void compact() {
        if (consumed_ == 0 || channels_.empty() || consumed_ < channels_.front().size() / 2) {
            return;
        }
        for (auto& queue : channels_) {
            queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(consumed_));
        }
        consumed_ = 0;
    }

    std::vector<std::vector<float>> channels_;
    std::size_t consumed_ = 0;
};

// The routing half of a transcode, whatever decodes the source: the source's
// channels, in the WAV order the routing was built for, in; whole
// ac3::kSamplesPerFrame frames rendered through the routing out as soon as
// there are any, so neither the decoded programme nor whatever `on_frame`
// does with it is ever held whole. The planar buffers are allocated once.
class RenderQueue {
   public:
    RenderQueue(plan::Routing routing, std::size_t source_channels, std::size_t coded_channels)
        : routing_(std::move(routing)),
          source_(source_channels, std::vector<float>(ac3::kSamplesPerFrame)),
          coded_(coded_channels, std::vector<float>(ac3::kSamplesPerFrame)),
          in_(source_channels),
          out_(coded_channels),
          views_(coded_channels),
          source_spans_(source_channels) {
        queue_.reset(source_channels);
        for (std::size_t c = 0; c < source_channels; ++c) {
            in_[c] = source_[c];
            source_spans_[c] = source_[c];
        }
        for (std::size_t c = 0; c < coded_channels; ++c) {
            out_[c] = coded_[c];
            views_[c] = coded_[c];
        }
    }
    RenderQueue(const RenderQueue&) = delete;
    RenderQueue& operator=(const RenderQueue&) = delete;
    RenderQueue(RenderQueue&&) = delete;
    RenderQueue& operator=(RenderQueue&&) = delete;
    ~RenderQueue() = default;

    void push(std::size_t channel, std::span<const float> samples) {
        queue_.push(channel, samples);
    }

    // Every whole frame the queue holds, and with `flush` the rest as one
    // last frame; false where `on_frame` refuses one.
    [[nodiscard]] bool drain(bool flush, const RenderedFrame& on_frame) {
        while (queue_.available() >= static_cast<std::size_t>(ac3::kSamplesPerFrame) ||
               (flush && queue_.available() > 0)) {
            const auto count = std::min<std::size_t>(queue_.available(), ac3::kSamplesPerFrame);
            queue_.take(count, source_spans_);
            plan::render(routing_, in_, out_, ac3::kSamplesPerFrame);
            if (!on_frame(views_, count)) {
                return false;
            }
        }
        return true;
    }

   private:
    plan::Routing routing_;
    SampleQueue queue_;
    std::vector<std::vector<float>> source_;
    std::vector<std::vector<float>> coded_;
    std::vector<std::span<const float>> in_;
    std::vector<std::span<float>> out_;
    std::vector<std::span<const float>> views_;
    std::vector<std::span<float>> source_spans_;
};

// The encode half of a transcode: one plan, one encoder, frames out through
// the sink. Kept as a type rather than a lambda because AC-3, E-AC-3 and AC-4
// have separate encoder classes with no common base, and the decode loop
// above it should not care which one it is feeding.
class TranscodeEncoder {
   public:
    // AC-4 (ETSI TS 103 190), through the encoder record and live take
    // theirs from: `config` carries the metadata, the plan the layout, rate
    // and bitrate, and each frame leaves as a sync frame with Part 2 Annex
    // G's CRC.
    [[nodiscard]] bool open_ac4(const plan::Plan& p, ac4::EncoderConfig config,
                                std::string_view out_path, bool keep_partial) {
        codec_ = plan::Codec::kAc4;
        if (const std::string why = ac4_.open(p, std::move(config)); !why.empty()) {
            fmt::println(stderr, "error: {}", why);
            return false;
        }
        coded_channels_ = ac4_.coded_channels();
        return sink_.open(out_path, keep_partial);
    }

    [[nodiscard]] bool open(const plan::Plan& p, std::string_view out_path, bool keep_partial,
                            std::optional<std::uint8_t> compr_passthrough) {
        codec_ = p.codec;
        compr_ = compr_passthrough;
        if (codec_ == plan::Codec::kAc3) {
            // Heap-allocated: FrameEncoder carries several KB of MDCT
            // scratch/history state (PREfast's C6262), same as run_encode.
            ac3_ = std::make_unique<ac3::FrameEncoder>(plan::ac3_config(p));
            coded_channels_ = static_cast<std::size_t>(ac3_->channel_count());
        } else {
            const auto config = plan::eac3_config(p);
            eac3_ = std::make_unique<ac3::eac3::AccessUnitEncoder>(config);
            coded_channels_ = static_cast<std::size_t>(eac3_->channel_count());
            // AccessUnitEncoder refuses a configuration by building no
            // substreams, which leaves channel_count() at 0 - the check
            // encode.cpp's eac3_config_accepted() makes for eac3-encode.
            // plan::validate() does not check metadata, and here part of it
            // comes from the source, which can carry a value neither encoder
            // writes: §5.4.2.8 reserves a dialnorm of 0, and a decoder reads
            // it as 31. Unchecked, decode_and_render() would size its channel
            // list from the 0 while plan::render() still filled every channel
            // the plan codes. build_silent_access_unit() starts with the
            // checks the constructor made, so it fails on the same one and
            // names it.
            if (coded_channels_ == 0) {
                const auto check = ac3::eac3::build_silent_access_unit(config);
                fmt::println(stderr, "error: the encoder cannot express this configuration: {}",
                             check.has_value() ? std::string_view{"no substreams were built"}
                                               : ac3::describe(check.error()));
                return false;
            }
        }
        return sink_.open(out_path, keep_partial);
    }

    [[nodiscard]] std::size_t coded_channels() const { return coded_channels_; }

    // A frame of the plan's coded channels, `samples` of them the
    // programme's: AC-3 and E-AC-3 code the frame whole, and AC-4, which takes
    // input of any length, the programme's samples alone.
    [[nodiscard]] bool encode(std::span<const std::span<const float>> channels,
                              std::size_t samples) {
        if (codec_ == plan::Codec::kAc4) {
            auto units = ac4_.encode(channels, samples);
            if (!units.has_value()) {
                fmt::println(stderr, "error: {}", units.error());
                sink_.abort();
                return false;
            }
            return push_ac4(*units);
        }
        std::vector<std::byte> frame;
        if (ac3_) {
            // An illegal AC-3 rate never gets this far (run_transcode's
            // plan::validate() refuses it), so the refusal is named from the
            // FrameError rather than assumed.
            auto encoded = ac3_->encode_frame(channels);
            if (!encoded.has_value()) {
                fmt::println(stderr, "error: the encoder cannot express this configuration: {}",
                             ac3::describe(encoded.error()));
                sink_.abort();
                return false;
            }
            frame = std::move(*encoded);
        } else {
            auto unit = eac3_->encode_access_unit(channels);
            if (!unit.has_value()) {
                fmt::println(stderr, "error: the encoder cannot express this configuration: {}",
                             ac3::describe(unit.error()));
                sink_.abort();
                return false;
            }
            frame = std::move(unit->bytes);
        }
        // The source's own compr word, stamped back onto the frame the
        // encoder just produced. §7.7.2's ceiling is a property of the
        // PROGRAMME, not of this generation's coding, so re-deriving it from
        // the re-encoded audio would replace the delivery decision the source
        // already recorded. The rewrite needs compre already set, which is
        // why the plan carries a HeavyConfig whenever this is engaged - see
        // run_transcode.
        if (compr_.has_value()) {
            const auto edited = ac3::io::edit_frame_metadata(frame, {.compr = compr_});
            if (!edited.has_value()) {
                fmt::println(stderr, "error: cannot carry compr across: {}",
                             ac3::io::describe(edited.error()));
                sink_.abort();
                return false;
            }
        }
        if (!sink_.push(std::move(frame))) {
            write_failed_ = true;
            sink_.abort();
            return false;
        }
        return true;
    }

    // True once encode() or close() failed because the destination did,
    // rather than because the encoder refused a frame - run_transcode's exit
    // class.
    [[nodiscard]] bool write_failed() const { return write_failed_; }
    // AC-4's encoder first hands over the frames its delay still holds.
    [[nodiscard]] bool close() {
        if (codec_ == plan::Codec::kAc4) {
            auto rest = ac4_.flush();
            if (!rest.has_value()) {
                fmt::println(stderr, "error: {}", rest.error());
                sink_.abort();
                return false;
            }
            if (!push_ac4(*rest)) {
                return false;
            }
        }
        if (!sink_.close()) {
            write_failed_ = true;
            return false;
        }
        return true;
    }
    void abort() { sink_.abort(); }
    [[nodiscard]] std::size_t frames() const { return sink_.frames(); }
    [[nodiscard]] std::size_t iframes() const { return iframes_; }

   private:
    [[nodiscard]] bool push_ac4(std::vector<TakeEncoder::Unit>& units) {
        for (TakeEncoder::Unit& unit : units) {
            iframes_ += unit.sync ? 1 : 0;
            if (!sink_.push(std::move(unit.bytes))) {
                write_failed_ = true;
                sink_.abort();
                return false;
            }
        }
        return true;
    }

    plan::Codec codec_ = plan::Codec::kAc3;
    std::unique_ptr<ac3::FrameEncoder> ac3_;
    std::unique_ptr<ac3::eac3::AccessUnitEncoder> eac3_;
    TakeEncoder ac4_;
    std::size_t iframes_ = 0;
    std::size_t coded_channels_ = 0;
    std::optional<std::uint8_t> compr_;
    bool write_failed_ = false;
    EncodedStreamSink sink_;
};

// --- AC-4's metadata, to and from A/52's ---------------------------------------

// E-AC-3's 3-bit mix levels (Tables D2.3 to D2.6) in dB: AC-4's Table 149
// values, and for the surrounds Table 149a's.
double mix_level_db(ac3::meta::MixLevel level) {
    using M = ac3::meta::MixLevel;
    switch (level) {
        case M::kPlus3dB:
            return 3.0;
        case M::kPlus1_5dB:
            return 1.5;
        case M::kUnity:
            return 0.0;
        case M::kMinus1_5dB:
            return -1.5;
        case M::kMinus3dB:
            return -3.0;
        case M::kMinus4_5dB:
            return -4.5;
        case M::kMinus6dB:
            return -6.0;
        case M::kSilent:
            break;
    }
    return -std::numeric_limits<double>::infinity();
}

// A surround level in dB: Tables D2.4 and D2.6 reserve the three codes above
// -1.5 dB, which a decoder takes as -1.5 dB.
double surround_level_db(ac3::meta::MixLevel level) {
    return ac3::meta::valid_surround_mix_level(level) ? mix_level_db(level) : -1.5;
}

double centre_level_db(ac3::meta::CentreMixLevel level) {
    switch (level) {
        case ac3::meta::CentreMixLevel::kMinus3dB:
            return -3.0;
        case ac3::meta::CentreMixLevel::kMinus4_5dB:
            break;
        case ac3::meta::CentreMixLevel::kMinus6dB:
            return -6.0;
    }
    return -4.5;
}

double surround_level_db(ac3::meta::SurroundMixLevel level) {
    switch (level) {
        case ac3::meta::SurroundMixLevel::kMinus3dB:
            return -3.0;
        case ac3::meta::SurroundMixLevel::kMinus6dB:
            break;
        case ac3::meta::SurroundMixLevel::kSilent:
            return -std::numeric_limits<double>::infinity();
    }
    return -6.0;
}

// The E-AC-3 level nearest a gain in dB, by linear coefficient as
// nearest_level() compares them; for a surround, among the levels Tables D2.4
// and D2.6 do not reserve, which AC-4's 0 dB is not.
ac3::meta::MixLevel mix_level_of(double db, bool surround) {
    const double target = std::isinf(db) ? 0.0 : std::pow(10.0, db / 20.0);
    auto best = ac3::meta::MixLevel::kSilent;
    double best_error = std::numeric_limits<double>::infinity();
    for (std::uint8_t code = 0; code < 8; ++code) {
        const auto level = static_cast<ac3::meta::MixLevel>(code);
        if (surround && !ac3::meta::valid_surround_mix_level(level)) {
            continue;
        }
        const double error = std::abs(ac3::meta::coefficient(level) - target);
        if (error < best_error) {
            best_error = error;
            best = level;
        }
    }
    return best;
}

std::string db_text(double db) {
    return std::isinf(db) ? std::string{"off"} : fmt::format("{:g} dB", db);
}

// The downmix an AC-3 or E-AC-3 source describes, as AC-4 sends it (ETSI TS
// 103 190-1 clauses 4.3.12.2.8 to 4.3.12.2.19): E-AC-3's mixing metadata and
// AC-3's Annex D levels are Table 149's and 149a's values, AC-3's two bsi
// levels are the Lo/Ro pair, and the LFE's 10 - lfemixlevcod dB, in whole dB,
// goes to AC-4's half-dB steps half a dB down (AC-4 back to A/52 takes it half
// a dB up, so the two directions undo each other).
ac4::DownmixConfig ac4_downmix_of(const ac3::io::FrameMetadata& source) {
    ac4::DownmixConfig out;
    out.preferred = ac4::PreferredDownmix::kNotIndicated;
    if (source.cmixlev.has_value()) {
        out.loro_centre_db = centre_level_db(*source.cmixlev);
    }
    if (source.surmixlev.has_value()) {
        out.loro_surround_db = surround_level_db(*source.surmixlev);
    }
    if (!source.mix.has_value()) {
        return out;
    }
    const ac3::io::WireMixMetadata& mix = *source.mix;
    if (mix.lorocmixlev.has_value()) {
        out.loro_centre_db = mix_level_db(*mix.lorocmixlev);
    }
    if (mix.lorosurmixlev.has_value()) {
        out.loro_surround_db = surround_level_db(*mix.lorosurmixlev);
    }
    if (mix.ltrtcmixlev.has_value()) {
        out.ltrt_centre_db = mix_level_db(*mix.ltrtcmixlev);
    }
    if (mix.ltrtsurmixlev.has_value()) {
        out.ltrt_surround_db = surround_level_db(*mix.ltrtsurmixlev);
    }
    if (mix.dmixmod == ac3::meta::DownmixMode::kLtRt) {
        out.preferred = ac4::PreferredDownmix::kLtRt;
    } else if (mix.dmixmod == ac3::meta::DownmixMode::kLoRo) {
        out.preferred = ac4::PreferredDownmix::kLoRo;
    }
    if (mix.lfemixlevcod.has_value()) {
        out.lfe_db = std::clamp(ac3::meta::lfe_mix_level_db(*mix.lfemixlevcod) - 0.5, -25.5, 5.5);
    }
    return out;
}

std::string_view preferred_text(ac4::PreferredDownmix preferred) {
    switch (preferred) {
        case ac4::PreferredDownmix::kLoRo:
            return "Lo/Ro";
        case ac4::PreferredDownmix::kLtRt:
            return "Lt/Rt";
        case ac4::PreferredDownmix::kLtRtProLogicII:
            return "Lt/Rt for Pro Logic II";
        case ac4::PreferredDownmix::kNotIndicated:
            break;
    }
    return "none preferred";
}

std::string downmix_text(const ac4::DownmixConfig& d) {
    std::string text = fmt::format(
        "{}{}; Lo/Ro centre {}, surround {}", preferred_text(d.preferred),
        d.preferred == ac4::PreferredDownmix::kNotIndicated ? "" : " preferred",
        db_text(d.loro_centre_db), db_text(d.loro_surround_db));
    if (d.ltrt_centre_db.has_value() || d.ltrt_surround_db.has_value()) {
        text += fmt::format("; Lt/Rt centre {}, surround {}",
                            db_text(d.ltrt_centre_db.value_or(d.loro_centre_db)),
                            db_text(d.ltrt_surround_db.value_or(d.loro_surround_db)));
    }
    text += d.lfe_db.has_value() ? fmt::format("; LFE {}", db_text(*d.lfe_db))
                                 : std::string{"; LFE left out"};
    return text;
}

// Table 160's drc_eac3_profile as the profile an A/52 encoder compresses
// with: 1 to 5 the five profiles, 0 "None" and 6 and 7, reserved, none.
std::optional<ac3::meta::ProfileId> eac3_profile_of(int drc_eac3_profile) {
    switch (drc_eac3_profile) {
        case 1:
            return ac3::meta::ProfileId::kFilmStandard;
        case 2:
            return ac3::meta::ProfileId::kFilmLight;
        case 3:
            return ac3::meta::ProfileId::kMusicStandard;
        case 4:
            return ac3::meta::ProfileId::kMusicLight;
        case 5:
            return ac3::meta::ProfileId::kSpeech;
        default:
            return std::nullopt;
    }
}

// What an AC-4 source's presentation sends that AC-3 or E-AC-3 has a field
// for, into `p` where the options leave it to the source, a line for each
// into `notes`: dialnorm, to the dB; drc_eac3_profile, the DRC profile Part 1
// clause 5.7.9.4 has a transcoder compress with (src/ac4dec/ERRATA.md, "The
// profile a transcoder to AC-3 or E-AC-3 takes"); and the downmix values, as
// E-AC-3's mixing metadata or as AC-3's two bsi levels.
void carry_ac4_metadata(const ac4::PresentationMetadata& source, plan::Codec target,
                        const Options& meta, plan::Metadata& p, std::vector<std::string>& notes) {
    if (meta.p.measure_dialnorm) {
        // Measured by the caller.
    } else if (meta.dialnorm_given) {
        notes.push_back(fmt::format("dialnorm {} (from dialnorm=)", p.dialnorm));
    } else if (const std::optional<double> dialnorm = source.loudness.dialnorm_dbfs) {
        p.dialnorm = std::clamp(static_cast<int>(std::lround(-*dialnorm)), 1, 31);
        notes.push_back(
            fmt::format("dialnorm {} (the source's {:g} dBFS, to the dB)", p.dialnorm, *dialnorm));
    } else {
        notes.push_back(fmt::format("dialnorm {} (the source sent none)", p.dialnorm));
    }

    if (meta.p.drc.has_value()) {
        notes.emplace_back("DRC      the profile drc= names");
    } else if (!source.drc.has_value()) {
        notes.emplace_back("DRC      none: the source sends no DRC (pass drc=<profile>)");
    } else if (const auto id = eac3_profile_of(source.drc->eac3_profile)) {
        p.drc = ac3::meta::profile(*id);
        notes.push_back(
            fmt::format("DRC      {}, the source's drc_eac3_profile (ETSI TS 103 190-1 clause "
                        "5.7.9.4), decoded without DRC",
                        ac3::meta::profile_name(*id)));
    } else {
        notes.push_back(fmt::format("DRC      none: the source's drc_eac3_profile {} names none",
                                    source.drc->eac3_profile));
    }

    if (!source.downmix.has_value()) {
        return;
    }
    const ac4::DownmixInfo& d = *source.downmix;
    using Preferred = ac4::DownmixInfo::Preferred;
    const bool ltrt = d.preferred == Preferred::kLtRt || d.preferred == Preferred::kLtRtProLogicII;
    // AC-3's bsi levels, and the fold a routing makes, take the pair the
    // stream prefers, as carry_mix_metadata reads an A/52 source.
    static constexpr std::array kCentre{ac3::meta::CentreMixLevel::kMinus3dB,
                                        ac3::meta::CentreMixLevel::kMinus4_5dB,
                                        ac3::meta::CentreMixLevel::kMinus6dB};
    static constexpr std::array kSurround{ac3::meta::SurroundMixLevel::kMinus3dB,
                                          ac3::meta::SurroundMixLevel::kMinus6dB,
                                          ac3::meta::SurroundMixLevel::kSilent};
    p.cmixlev =
        nearest_level(mix_level_of(ltrt ? d.ltrt_centre_db : d.loro_centre_db, false), kCentre);
    p.surmixlev = nearest_level(
        mix_level_of(ltrt ? d.ltrt_surround_db : d.loro_surround_db, true), kSurround);
    p.lorocmixlev = mix_level_of(d.loro_centre_db, false);
    p.lorosurmixlev = mix_level_of(d.loro_surround_db, true);
    p.ltrtcmixlev = mix_level_of(d.ltrt_centre_db, false);
    p.ltrtsurmixlev = mix_level_of(d.ltrt_surround_db, true);
    // A/52 has no code for Pro Logic II's Lt/Rt (Table D2.2 reserves '11'),
    // so it goes as Lt/Rt, the downmix it decodes.
    p.dmixmod = ltrt ? ac3::meta::DownmixMode::kLtRt
                     : (d.preferred == Preferred::kLoRo ? ac3::meta::DownmixMode::kLoRo
                                                        : ac3::meta::DownmixMode::kNotIndicated);
    p.lfemix = d.lfe_db.has_value()
                   ? std::optional<int>{std::clamp(
                         static_cast<int>(std::lround(10.0 - (*d.lfe_db + 0.5))), 0, 31)}
                   : std::nullopt;
    if (target == plan::Codec::kEac3) {
        p.mixmeta = true;
        notes.push_back(fmt::format(
            "downmix  Lo/Ro centre {}, surround {}; Lt/Rt centre {}, surround {}; LFE {}; {} "
            "preferred, as E-AC-3's mixing metadata",
            db_text(mix_level_db(*p.lorocmixlev)), db_text(mix_level_db(*p.lorosurmixlev)),
            db_text(mix_level_db(*p.ltrtcmixlev)), db_text(mix_level_db(*p.ltrtsurmixlev)),
            p.lfemix.has_value() ? db_text(ac3::meta::lfe_mix_level_db(*p.lfemix))
                                 : std::string{"left out"},
            ltrt ? "Lt/Rt" : (d.preferred == Preferred::kLoRo ? "Lo/Ro" : "none")));
    } else {
        notes.push_back(fmt::format("downmix  centre {}, surround {}, the {} pair, as AC-3's bsi",
                                    db_text(centre_level_db(p.cmixlev)),
                                    db_text(surround_level_db(p.surmixlev)),
                                    ltrt ? "Lt/Rt" : "Lo/Ro"));
    }
}

// What an AC-4 source is, from the first frame that decodes with `config`:
// the channels it comes out in, its rate, the presentation, and what the
// presentation sends, which the I-frame a decoder starts at carries whole.
struct Ac4Source {
    std::vector<ac4::Speaker> speakers{};
    int sample_rate_hz = 0;
    std::size_t presentation = 0;
    std::optional<int> presentation_id{};
    std::size_t presentations = 0;
    ac4::PresentationMetadata metadata{};
};

std::optional<Ac4Source> first_decoded_ac4(std::span<const ac4::SyncFrame> frames,
                                           const ac4::DecoderConfig& config,
                                           std::string_view in_path) {
    ac4::Decoder decoder(config);
    std::size_t number = 0;
    for (const ac4::SyncFrame& frame : frames) {
        ++number;
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        if (!decoded.has_value()) {
            fmt::println(stderr, "error: {}: frame {}: {}", in_path, number,
                         decoder.refusal_reason());
            return std::nullopt;
        }
        if (!decoded->has_value()) {
            continue;  // waiting for an I-frame
        }
        const ac4::DecodedFrame& pcm = **decoded;
        Ac4Source source;
        source.speakers = pcm.speakers;
        source.sample_rate_hz = pcm.sample_rate_hz;
        source.presentation = pcm.presentation;
        source.presentation_id = pcm.presentation_id;
        source.presentations = decoder.presentations().size();
        source.metadata = decoder.metadata();
        return source;
    }
    fmt::println(stderr, "error: {}: no frame decoded; the stream sent no I-frame", in_path);
    return std::nullopt;
}

// Whether a presentation's channels go beyond 5.1: a 7.X element's last pair.
bool beyond_51(std::span<const ac4::Speaker> speakers) {
    return std::ranges::any_of(speakers, [](ac4::Speaker s) { return ac4_meter_rank(s) >= 99; });
}

// 7.X 3/4/0 with its LFE, which a routing takes as an eight-channel WAV file's
// 7.1: L R C LFE, the back pair at BL and BR, Ls and Rs at SL and SR.
bool is_71_back(std::span<const ac4::Speaker> speakers) {
    using S = ac4::Speaker;
    return speakers.size() == 8 && std::ranges::find(speakers, S::kLfe) != speakers.end() &&
           std::ranges::find(speakers, S::kLeftBack) != speakers.end();
}

// AC-4 to AC-3 or E-AC-3: the presentation decode's options choose, decoded
// as coded (no output level, so no DRC: ETSI TS 103 190-1 clause 5.7.9.4),
// becomes the programme.
int transcode_from_ac4(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
                       std::string_view layout, const Options& meta, plan::Codec target,
                       std::span<const std::byte> stream) {
    if (target == plan::Codec::kAc4) {
        fmt::println(stderr,
                     "error: {} is AC-4 already; transcode goes between AC-4 and AC-3 or E-AC-3",
                     in_path);
        return kExitUsage;
    }
    if (meta.dialnorm2_given || meta.p.measure_dialnorm2) {
        fmt::println(stderr,
                     "error: dialnorm2= is a 1+1 stream's Ch2, and an AC-4 presentation becomes "
                     "one programme");
        return kExitUsage;
    }
    const ac4::ScanResult scan = ac4::scan(stream);
    if (scan.frames.empty()) {
        fmt::println(stderr, "error: {} holds no AC-4 sync frame", in_path);
        return kExitInput;
    }
    if (scan.stopped_at.has_value()) {
        fmt::println(stderr,
                     "warning: {}: the sync frames stop at byte {} ({}); transcoding the {} "
                     "before it",
                     in_path, scan.stopped_at_offset, ac4::describe(*scan.stopped_at),
                     scan.frames.size());
    }
    const auto status = status_stream(out_path);
    const std::string_view target_label = plan::codec_label(target);

    ac4::DecoderConfig config = ac4_coded_config(meta);
    if (meta.ac4_fold_5x) {
        config.output.downmix = ac4::DownmixTarget::k5X;
    }
    auto source = first_decoded_ac4(scan.frames, config, in_path);
    if (!source.has_value()) {
        return kExitInput;
    }
    // A 7.X element's last pair has a Table E2.5 location in E-AC-3 (the rear
    // surrounds, the wides or the vertical heights) and none in AC-3, which
    // takes the element folded to 5.X as AC-4 folds it (Part 1 Table 219); so
    // does a [layout] of the operator's, routed from the 5.X, but for 7.X
    // 3/4/0 with its LFE, which a routing takes as 7.1.
    bool folded = false;
    if (beyond_51(source->speakers) &&
        (target == plan::Codec::kAc3 || (!layout.empty() && !is_71_back(source->speakers)))) {
        config.output.downmix = ac4::DownmixTarget::k5X;
        source = first_decoded_ac4(scan.frames, config, in_path);
        if (!source.has_value()) {
            return kExitInput;
        }
        folded = true;
    }
    const auto source_rate = static_cast<std::uint32_t>(source->sample_rate_hz);
    const auto rate = wav_sample_rate(source_rate, target_label, target == plan::Codec::kEac3);
    if (!rate.has_value()) {
        return kExitUsage;
    }

    plan::Plan p{.codec = target, .sample_rate = *rate, .bitrate_kbps = bitrate, .meta = meta.p};
    std::vector<std::string> notes;
    carry_ac4_metadata(source->metadata, target, meta, p.meta, notes);
    if (meta.p.measure_dialnorm) {
        const auto measured = measure_ac4_loudness(stream, in_path, meta);
        if (!measured.has_value()) {
            return kExitInput;
        }
        if (!measured->integrated_lkfs.has_value()) {
            fmt::println(stderr, "error: no audio above the -70 LKFS absolute gate; "
                                 "pass dialnorm=<1..31> explicitly");
            return kExitRuntime;
        }
        p.meta.dialnorm = ac3::meta::dialnorm_from_lkfs(*measured->integrated_lkfs);
        notes.insert(notes.begin(), fmt::format("dialnorm {} (measured, {:.2f} LKFS)",
                                                p.meta.dialnorm, *measured->integrated_lkfs));
    }

    // The decoded channels at their Table E2.5 locations, in the WAV order a
    // routing takes a source in.
    std::vector<ac3::eac3::chanmap::Location> locations;
    std::string names;
    for (const ac4::Speaker speaker : source->speakers) {
        locations.push_back(ac4_location(speaker));
        names += (names.empty() ? "" : ",") + std::string{ac3::eac3::chanmap::name(locations.back())};
    }
    const std::vector<std::size_t> wav = plan::wav_order(locations);
    const std::size_t source_channels = locations.size();
    std::string label;
    if (!layout.empty()) {
        if (!resolve_layout(layout, target, p, label)) {
            return kExitUsage;
        }
    } else if (source_channels <= 2) {
        p.layout = source_channels == 1 ? plan::LayoutId::kMono : plan::LayoutId::kStereo;
        label = std::string(plan::layout(p.layout).label);
    } else if (source_channels == 6 && !beyond_51(source->speakers)) {
        p.layout = plan::LayoutId::k51;
        label = std::string(plan::layout(p.layout).label);
    } else {
        // 3.0, 5.0 and 7.X: the channels themselves.
        p.custom_locations = plan::parse_channels(names);
        label = p.custom_locations.has_value() ? plan::format_channels(*p.custom_locations) : names;
    }
    p.tools.fast_mdct = meta.fast_mdct;
    if (const auto bad = plan::validate(p)) {
        fmt::println(stderr, "error: {}", plan::describe(*bad));
        return kExitUsage;
    }
    const auto routing = routing_or_error(p, source_channels);
    if (!routing.has_value()) {
        return kExitUsage;
    }

    TranscodeEncoder encoder;
    if (!encoder.open(p, out_path, meta.keep_partial, std::nullopt)) {
        return encoder.coded_channels() == 0 ? kExitUsage : kExitOutput;
    }
    const auto coded_plan = plan::resolve(p);
    ac3::analysis::LevelMeter meter{coded_plan.bed_acmod, coded_plan.bed_lfe, source_rate};
    RenderQueue queue(*routing, source_channels, encoder.coded_channels());
    bool encode_failed = false;
    const RenderedFrame on_frame = [&meter, &encoder, &encode_failed](
                                       std::span<const std::span<const float>> views,
                                       std::size_t samples) {
        meter.process(views);
        encode_failed = !encoder.encode(views, samples);
        return !encode_failed;
    };
    const auto failure = [&encoder, &encode_failed] {
        if (!encode_failed) {
            return kExitInput;
        }
        return encoder.write_failed() ? kExitOutput : kExitUsage;
    };

    ac4::Decoder decoder(config);
    std::size_t decoded_frames = 0;
    std::size_t number = 0;
    for (const ac4::SyncFrame& frame : scan.frames) {
        ++number;
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        if (!decoded.has_value()) {
            fmt::println(stderr, "error: {}: frame {}: {}", in_path, number,
                         decoder.refusal_reason());
            encoder.abort();
            return kExitInput;
        }
        if (!decoded->has_value()) {
            continue;
        }
        const ac4::DecodedFrame& pcm = **decoded;
        if (pcm.speakers != source->speakers || pcm.sample_rate_hz != source->sample_rate_hz) {
            fmt::println(stderr,
                         "error: {}: frame {}: the presentation's channels or sample rate change "
                         "part-way through, which one routing cannot describe",
                         in_path, number);
            encoder.abort();
            return kExitInput;
        }
        ++decoded_frames;
        for (std::size_t w = 0; w < wav.size(); ++w) {
            queue.push(w, pcm.channels[wav[w]]);
        }
        if (!queue.drain(false, on_frame)) {
            return failure();
        }
    }
    if (!queue.drain(true, on_frame)) {
        return failure();
    }
    if (!encoder.close()) {
        return encoder.write_failed() ? kExitOutput : kExitUsage;
    }

    status_println(status, "transcoded {} AC-4 frames -> {} {} frames ({} kbps, {} Hz) in {}",
                   decoded_frames, encoder.frames(), target_label, bitrate, source_rate, out_path);
    const std::string id = source->presentation_id.has_value()
                               ? fmt::format(", presentation_id {}", *source->presentation_id)
                               : std::string{};
    status_println(status, "  presentation {}{} of {}, decoded without DRC", source->presentation,
                   id, source->presentations);
    status_println(status, "  layout {} <- {} decoded channels{}", label, source_channels,
                   folded ? " (the 7.X element folded to 5.X, Part 1 Table 219)" : "");
    for (const std::string& note : notes) {
        status_println(status, "  {}", note);
    }
    print_channel_summary(meter, status);
    return kExitOk;
}

}  // namespace

std::optional<DecodeRenderStats> decode_and_render(
    std::string_view in_path, const LoadedStream& loaded, const plan::Routing& routing,
    std::size_t coded_channels, const RenderedFrame& on_frame,
    const std::function<void()>& on_abort) {
    // §E2.3.1.2's legacy core (kAc3CoreEac3Extension) opens with an AC-3
    // syncframe but carries Annex E dependents behind it. Eac3Decoder reads
    // the whole access unit correctly (it has folded a legacy core's own
    // channels correctly since #690); FrameDecoder only knows how to split
    // plain AC-3 syncframes and was never meant to skip over the trailing
    // dependent bytes loaded.scan.access_units bundles in with it here. Route
    // it down the same path as plain E-AC-3, matching run_decode's own
    // dispatch (ac3::stream_bsid(...) > 8 || ac3::has_eac3_extension_substreams(...),
    // apps/cli/commands/decode.cpp) and apps/common/stream_playback.hpp's
    // reads_as_access_units - both of which test the stream's content rather
    // than trust a two-way read of this enum.
    const bool eac3_source = loaded.scan.kind == ac3::io::StreamKind::kEac3 ||
                             loaded.scan.kind == ac3::io::StreamKind::kAc3CoreEac3Extension;
    const auto source_channels = static_cast<std::size_t>(loaded.scan.channels);

    RenderQueue queue(routing, source_channels, coded_channels);
    const auto drain = [&](bool flush) -> bool { return queue.drain(flush, on_frame); };

    DecodeRenderStats stats;
    const auto track_dynrng = [&](std::span<const std::uint8_t> words) {
        for (const auto word : words) {
            const double db = ac3::meta::to_db(ac3::meta::dynrng_gain(word));
            stats.dynrng_min_db = stats.dynrng_words == 0 ? db : std::min(stats.dynrng_min_db, db);
            stats.dynrng_max_db = stats.dynrng_words == 0 ? db : std::max(stats.dynrng_max_db, db);
            ++stats.dynrng_words;
        }
    };

    // A stream whose channel count changes part-way through would leave the
    // queue's channels at different lengths and silently truncate at the
    // shortest, so it is refused with a reason instead. `routing` was fixed
    // from the scan's own channel count before the first decode.
    const auto require_width = [&](std::size_t got) -> bool {
        if (got == source_channels) {
            return true;
        }
        fmt::println(stderr,
                     "error: {}: the programme changes from {} channels to {} part-way through, "
                     "which one routing cannot describe",
                     in_path, source_channels, got);
        on_abort();
        return false;
    };

    if (eac3_source) {
        // Heap-allocated for the same C6262 reason measure_qc_eac3 gives.
        auto decoder = std::make_unique<ac3::Eac3Decoder>();
        // The programme's Table E2.5 layout and the WAV position each of its
        // slots occupies, fixed from the first access unit that decodes -
        // the flush below needs both, and by then there is no access unit
        // left to read them off.
        ac3::eac3::chanmap::Layout programme_layout{};
        std::vector<std::size_t> slot_to_wav;
        for (const auto& unit : loaded.scan.access_units) {
            const auto decoded = decoder->decode_access_unit(unit);
            if (!decoded.has_value()) {
                fmt::println(stderr, "error: {}: {}", in_path, ac3::describe(decoded.error()));
                on_abort();
                return std::nullopt;
            }
            if (!decoded->has_value()) {
                // §3.7 transient pre-noise hold-back - nothing ready yet.
                continue;
            }
            const auto& programme = **decoded;
            ++stats.units_in;
            track_dynrng(std::span{programme.dynrng}.first(static_cast<std::size_t>(
                ac3::eac3::blocks_per_syncframe(programme.numblkscod))));
            if (slot_to_wav.empty()) {
                programme_layout = programme.layout;
                // The decoded programme is in Table E2.5 slot order; the
                // routing was built for a source in WAV order, the same
                // reconciliation `decode` makes before writing a WAV. Dual
                // mono has no Table E2.5 location at all (Ch1 and Ch2 are
                // programmes, not directions), so it stays in coded order.
                const auto order =
                    programme.acmod == ac3::Acmod::kDualMono
                        ? std::vector<std::size_t>{}
                        : plan::wav_order(std::span{programme_layout.items}.first(
                              static_cast<std::size_t>(programme_layout.count)));
                slot_to_wav.assign(programme.channels.size(), 0);
                for (std::size_t wav = 0; wav < slot_to_wav.size(); ++wav) {
                    slot_to_wav[order.empty() ? wav : order[wav]] = wav;
                }
            }
            if (!require_width(programme.channels.size())) {
                return std::nullopt;
            }
            for (std::size_t slot = 0; slot < programme.channels.size(); ++slot) {
                queue.push(slot_to_wav[slot], programme.channels[slot]);
            }
            if (!drain(false)) {
                return std::nullopt;
            }
        }
        // Whatever transient pre-noise processing was still holding back
        // (§3.7). held_back_unit assembles the flushed substreams the way
        // decode_access_unit's own §E3.8.2 assembly would have, onto
        // programme_layout - see its own doc comment for the placement
        // rules this used to duplicate here.
        const auto flushed = decoder->flush();
        if (!flushed.empty()) {
            if (slot_to_wav.empty()) {
                fmt::println(stderr,
                             "error: {}: no access unit ever completed, so there is no programme "
                             "layout to place the held-back frames into",
                             in_path);
                on_abort();
                return std::nullopt;
            }
            // decode_and_render never folds - routing/coded_channels already
            // describe the desired shape (plan::Routing, above), so this
            // decoder is always constructed at kAsCoded.
            const auto held = ac3::apps::held_back_unit(flushed, programme_layout, false);
            if (held.has_value()) {
                ++stats.units_in;
                const auto count = std::min(held->channels.size(), slot_to_wav.size());
                for (std::size_t slot = 0; slot < count; ++slot) {
                    queue.push(slot_to_wav[slot], held->channels[slot]);
                }
            }
            if (!drain(false)) {
                return std::nullopt;
            }
        }
    } else {
        ac3::FrameDecoder decoder;
        const auto order = ac3::io::wav_channel_order(loaded.scan.acmod, loaded.scan.lfe);
        for (const auto& frame : loaded.scan.access_units) {
            const auto decoded = decoder.decode_frame(frame);
            if (!decoded.has_value()) {
                fmt::println(stderr, "error: {}: {}", in_path, ac3::describe(decoded.error()));
                on_abort();
                return std::nullopt;
            }
            ++stats.units_in;
            track_dynrng(decoded->dynrng);
            if (!require_width(decoded->channels.size())) {
                return std::nullopt;
            }
            for (std::size_t i = 0; i < order.size(); ++i) {
                queue.push(i, decoded->channels[order[i]]);
            }
            if (!drain(false)) {
                return std::nullopt;
            }
        }
    }
    if (!drain(true)) {
        return std::nullopt;
    }
    return stats;
}

int run_transcode(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
                  std::string_view layout, const Options& meta) {
    // A stream inside a container is read as decode reads it.
    auto bytes = read_elementary_stream(in_path);
    if (bytes.empty()) {
        return kExitInput;
    }
    const auto target_codec = output_codec(out_path, meta);
    if (!target_codec.has_value()) {
        return kExitUsage;
    }
    const bool to_ac4 = *target_codec == plan::Codec::kAc4;
    if (bitrate == 0) {
        bitrate = to_ac4 ? 192 : 448;
    }
    if (is_ac4_stream(bytes)) {
        return transcode_from_ac4(in_path, out_path, bitrate, layout, meta, *target_codec, bytes);
    }
    // AC-3's and E-AC-3's own metadata has no AC-4 counterpart, so asking
    // for it is refused rather than dropped, as ac4-encode refuses it.
    if (to_ac4 && (meta.p.heavy.has_value() || meta.p.heavy2.has_value() ||
                   meta.p.drc2.has_value() || meta.p.mixmeta || meta.p.infomdat ||
                   meta.p.annexd || meta.dialnorm2_given)) {
        fmt::println(stderr,
                     "error: heavy, heavy2, drc2=, dialnorm2=, infomdat, annexd and E-AC-3's "
                     "mixing metadata have no AC-4 counterpart; a transcode to AC-4 carries the "
                     "source's dialnorm and downmix values, and drc= names its DRC profile");
        return kExitUsage;
    }
    const auto loaded = scan_loaded(std::move(bytes), in_path);
    if (!loaded.has_value()) {
        return kExitInput;
    }
    const auto source_meta = ac3::io::read_frame_metadata(loaded->bytes);
    if (!source_meta.has_value()) {
        fmt::println(stderr, "error: {}: {}", in_path, ac3::io::describe(source_meta.error()));
        return kExitInput;
    }
    if (to_ac4 && loaded->scan.acmod == ac3::Acmod::kDualMono) {
        fmt::println(stderr,
                     "error: {} is 1+1, two programmes in one frame, and AC-4 has no dual mono; "
                     "transcode it to E-AC-3, or decode it and ac4-encode each programme",
                     in_path);
        return kExitUsage;
    }

    // status_stream(out_path): stderr instead of stdout when the encoded
    // bytes themselves are going to stdout.
    const auto status = status_stream(out_path);
    const std::string_view target_label = plan::codec_label(*target_codec);
    const auto source_rate = ac3::sample_rate_hz(loaded->scan.sample_rate);
    const auto rate =
        wav_sample_rate(source_rate, target_label, *target_codec == plan::Codec::kEac3);
    if (!rate.has_value()) {
        return kExitUsage;
    }

    plan::Plan p{.codec = *target_codec,
                 .sample_rate = *rate,
                 .bitrate_kbps = bitrate,
                 .meta = meta.p};

    // --- what carries across ------------------------------------------------
    // dialnorm is the one that matters most: §5.4.2.8 says it "shall affect
    // the sound reproduction level", so a transcode that reset it to 31 would
    // play the programme up to 30 dB too loud on a levelled system. Preserved
    // unless the operator names a different one (or asks to measure).
    if (!meta.dialnorm_given) {
        p.meta.dialnorm = source_meta->dialnorm;
    }
    if (!meta.dialnorm2_given && source_meta->dialnorm2.has_value()) {
        p.meta.dialnorm2 = *source_meta->dialnorm2;
    }
    // dialnorm=auto/dialnorm2=auto ask for a measurement, not a carry.
    // parse_options already set dialnorm_given for "auto", so the carry above
    // was skipped - without this, plan::Metadata's default of 31 reached the
    // encoder, unmeasured, labelled as if the operator had typed it. A
    // transcode has exactly one source, so it is measured the same way
    // run_normalize measures one: a BS.1770 pass over the stream as authored,
    // before this command's own routing/folding, matching run_eac3_encode's
    // single-source dialnorm=auto (the multi-source route_frame path is the
    // one exception, measuring the rendered channels instead - it is
    // combining several sources, so there is no single "the source" to
    // measure pre-routing).
    // AC-4 sends dialnorm in steps of 0.25 dB (ETSI TS 103 190-1 clause
    // 4.3.12.2.1), so a measured one keeps them.
    double ac4_dialnorm_db = -static_cast<double>(p.meta.dialnorm);
    if (meta.p.measure_dialnorm || meta.p.measure_dialnorm2) {
        const auto measured = measure_stream_loudness(loaded->bytes);
        if (!measured.has_value()) {
            return kExitInput;
        }
        if (meta.p.measure_dialnorm) {
            if (!measured->integrated_lkfs.has_value()) {
                fmt::println(stderr, "error: no audio above the -70 LKFS absolute gate; "
                                     "pass dialnorm=<1..31> explicitly");
                return kExitRuntime;
            }
            p.meta.dialnorm = ac3::meta::dialnorm_from_lkfs(*measured->integrated_lkfs);
            ac4_dialnorm_db =
                0.0 - std::clamp(std::round(-*measured->integrated_lkfs * 4.0) / 4.0, 0.0, 31.75);
        }
        if (meta.p.measure_dialnorm2) {
            if (!measured->ch2_lkfs.has_value()) {
                fmt::println(stderr,
                             "error: {} is not 1+1, so there is no Ch2 to measure for "
                             "dialnorm2=auto",
                             in_path);
                return kExitUsage;
            }
            p.meta.dialnorm2 = ac3::meta::dialnorm_from_lkfs(*measured->ch2_lkfs);
        }
    }
    carry_mix_metadata(*source_meta, p.meta);
    if (*target_codec == plan::Codec::kEac3 && source_meta->mix.has_value()) {
        p.meta.mixmeta = true;
    }
    // A compr word the operator did not override is carried across verbatim
    // (TranscodeEncoder::encode stamps it back), which needs compre set in
    // the output - hence a HeavyConfig here even though its own detector's
    // answer is then overwritten.
    std::optional<std::uint8_t> compr_passthrough;
    if (source_meta->compr.has_value() && !meta.p.heavy.has_value() && !to_ac4) {
        p.meta.heavy.emplace();
        compr_passthrough = source_meta->compr;
    }

    // --- layout -------------------------------------------------------------
    std::string label;
    const auto source_channels = static_cast<std::size_t>(loaded->scan.channels);
    if (!layout.empty()) {
        if (!resolve_layout(layout, *target_codec, p, label)) {
            return kExitUsage;
        }
    } else if (to_ac4 && !loaded->scan.lfe && source_channels >= 3 && source_channels <= 5) {
        // 3/0, 2/1, 3/1, 2/2 and 3/2 without the LFE: AC-4's 5.0, the
        // channels the source lacks silent, rather than a 5.1 with a silent
        // LFE.
        p.custom_locations = plan::parse_channels("L,C,R,Ls,Rs");
        label = "5.0";
    } else if (loaded->scan.acmod == ac3::Acmod::kDualMono) {
        // 1+1 is two independent programmes sharing one syncframe, not a
        // soundfield (§E1.3, no downmix between them). Its two channels are
        // Ch1 and Ch2, not L and R, so following the channel COUNT the way
        // every other layout does would quietly turn a bilingual programme
        // into a stereo one.
        p.layout = plan::LayoutId::kDualMono;
        label = std::string(plan::layout(plan::LayoutId::kDualMono).label);
        if (!meta.dialnorm2_given && !source_meta->dialnorm2.has_value()) {
            fmt::println(stderr, "error: {} is 1+1 but carries no dialnorm2", in_path);
            return kExitInput;  // both bsi syntaxes always send it for 1+1
        }
    } else {
        auto id = plan::layout_for_source(source_channels);
        if (id.has_value() && !plan::carries(*target_codec, *id)) {
            // The DD+-to-DD case this command exists for: an immersive or 7.1
            // programme has no AC-3 coding mode, so it folds to 5.1 per §7.8
            // using the mix levels carried across just above. Said out loud
            // rather than done quietly - it is a real change to what the
            // listener hears. AC-4's encoder stops at 5.1 in the same way.
            status_println(status,
                           "note: {} channels have no {} coding mode; folding down to 5.1 "
                           "(§7.8, using the stream's own mix levels)",
                           source_channels, target_label);
            id = plan::LayoutId::k51;
        }
        if (!id.has_value()) {
            fmt::println(stderr, "error: no standard layout has {} channels; name one with the "
                                 "[layout] argument",
                         source_channels);
            return kExitUsage;
        }
        p.layout = *id;
        label = std::string(plan::layout(*id).label);
    }
    p.tools.fast_mdct = meta.fast_mdct;
    if (const auto bad = plan::validate(p)) {
        fmt::println(stderr, "error: {}", plan::describe(*bad));
        return kExitUsage;
    }

    const auto routing = routing_or_error(p, source_channels);
    if (!routing.has_value()) {
        return kExitUsage;
    }

    // AC-4's metadata from the same values: dialnorm, the downmix of a 5.X
    // target, and the DRC profile drc= names, which an AC-3 or E-AC-3
    // stream's dynrng and compr words do not.
    const auto coded_plan = plan::resolve(p);
    ac4::EncoderConfig ac4_config = ac4_config_for(p);
    const bool ac4_downmix = to_ac4 && coded_plan.bed_acmod == ac3::Acmod::k3_2 &&
                             (source_meta->cmixlev.has_value() ||
                              source_meta->surmixlev.has_value() || source_meta->mix.has_value());
    std::optional<ac3::meta::ProfileId> ac4_drc;
    if (to_ac4) {
        ac4_config.dialnorm_db = ac4_dialnorm_db;
        if (ac4_downmix) {
            ac4_config.downmix = ac4_downmix_of(*source_meta);
        }
        if (meta.p.drc.has_value()) {
            ac4_drc = profile_id_of(*meta.p.drc);
        }
    }

    TranscodeEncoder encoder;
    const bool opened = to_ac4 ? encoder.open_ac4(p, ac4_config, out_path, meta.keep_partial)
                               : encoder.open(p, out_path, meta.keep_partial, compr_passthrough);
    if (!opened) {
        // open() refuses for one of two reasons, and each has its own exit
        // class: an encoder that built no channels could not express the
        // configuration (a usage error), otherwise it was the sink that
        // could not be opened (an output one).
        return encoder.coded_channels() == 0 ? kExitUsage : kExitOutput;
    }
    const auto coded_channels = encoder.coded_channels();
    ac3::analysis::LevelMeter meter{coded_plan.bed_acmod, coded_plan.bed_lfe, source_rate};

    // Latched so a failure can be put in its exit class afterwards:
    // decode_and_render only says THAT it stopped, and it stops both when
    // the source stops decoding (an input fault) and when this callback
    // refuses (the encoder or its sink - see TranscodeEncoder::encode).
    bool encode_failed = false;
    const auto stats = decode_and_render(
        in_path, *loaded, *routing, coded_channels,
        [&meter, &encoder, &encode_failed](std::span<const std::span<const float>> views,
                                           std::size_t samples) {
            meter.process(views);
            encode_failed = !encoder.encode(views, samples);
            return !encode_failed;
        },
        [&encoder] { encoder.abort(); });
    if (!stats.has_value()) {
        if (!encode_failed) {
            return kExitInput;
        }
        return encoder.write_failed() ? kExitOutput : kExitUsage;
    }
    if (!encoder.close()) {
        return encoder.write_failed() ? kExitOutput : kExitUsage;
    }

    status_println(status, "transcoded {} {} access units -> {} {} frames ({} kbps, {} Hz) in {}",
                   stats->units_in, codec_label(loaded->scan.kind), encoder.frames(),
                   target_label, bitrate, source_rate, out_path);
    status_println(status, "  layout {} <- {} source channels", label, source_channels);
    if (to_ac4) {
        status_println(status, "  dialnorm {:g} dBFS{}", ac4_dialnorm_db,
                       meta.p.measure_dialnorm ? " (measured)"
                       : meta.dialnorm_given   ? " (from dialnorm=)"
                                               : " (carried from the source)");
        if (ac4_drc.has_value()) {
            status_println(status, "  DRC      {} (from drc=), as drc_eac3_profile",
                           ac3::meta::profile_name(*ac4_drc));
        } else {
            status_println(status,
                           "  DRC      none: the source's dynrng and compr words name no profile "
                           "(pass drc=<profile>)");
        }
        if (ac4_config.downmix.has_value()) {
            status_println(status, "  downmix  {}, carried from the source",
                           downmix_text(*ac4_config.downmix));
        }
        status_println(status, "  {} I-frames", encoder.iframes());
        print_channel_summary(meter, status);
        return kExitOk;
    }
    // Three-way, not two: a measured value did set dialnorm_given (that is
    // what tells run_metadata's own dialnorm=auto guard to refuse it further
    // down this file), but it did not come from the operator typing a
    // number - saying "(from dialnorm=)" for it would claim a number nobody
    // gave.
    std::string_view dialnorm_note = " (carried from the source)";
    if (meta.p.measure_dialnorm) {
        dialnorm_note = " (measured)";
    } else if (meta.dialnorm_given) {
        dialnorm_note = " (from dialnorm=)";
    }
    status_println(status, "  dialnorm {}{}", p.meta.dialnorm, dialnorm_note);
    if (compr_passthrough.has_value()) {
        status_println(status, "  compr    {:+.2f} dB carried across verbatim",
                       ac3::meta::to_db(ac3::meta::compr_gain(*compr_passthrough)));
    } else if (p.meta.heavy.has_value()) {
        status_println(status, "  compr    re-derived (heavy given on the command line)");
    } else {
        status_println(status, "  compr    absent in the source");
    }
    // dynrng is a per-BLOCK word derived from the signal, so a re-encode has
    // to produce its own rather than copy the source's - there is no bsi
    // field to stamp it into the way compr has. Reported so the difference is
    // visible rather than discovered.
    if (stats->dynrng_words > 0 && (stats->dynrng_min_db != 0.0 || stats->dynrng_max_db != 0.0)) {
        status_println(status,
                       "  dynrng   source carried {:+.2f} .. {:+.2f} dB; the re-encode {}",
                       stats->dynrng_min_db, stats->dynrng_max_db,
                       p.meta.drc ? "derives its own from drc="
                                  : "writes none (pass drc=<profile>)");
    }
    print_channel_summary(meter, status);
    return 0;
}

int run_metadata(std::string_view in_path, std::string_view out_path, const Options& meta) {
    auto loaded = load_stream(in_path);
    if (!loaded.has_value()) {
        return kExitInput;
    }
    const auto before = ac3::io::read_frame_metadata(loaded->bytes);
    if (!before.has_value()) {
        fmt::println(stderr, "error: {}: {}", in_path, ac3::io::describe(before.error()));
        return kExitInput;
    }

    // Only what the operator actually named. dialnorm's plan::Metadata
    // default of 31 is a real value, so "was it given" comes from the parse
    // (Options::dialnorm_given) rather than from comparing against it.
    ac3::io::MetadataEdit edit;
    if (meta.dialnorm_given) {
        if (meta.p.measure_dialnorm) {
            fmt::println(stderr,
                         "error: dialnorm=auto needs a measurement - use 'ac3cli normalize', "
                         "which decodes the stream to measure it");
            return kExitUsage;
        }
        edit.dialnorm = meta.p.dialnorm;
    }
    if (meta.dialnorm2_given) {
        if (meta.p.measure_dialnorm2) {
            fmt::println(stderr, "error: dialnorm2=auto needs a measurement - use 'ac3cli "
                                 "normalize'");
            return kExitUsage;
        }
        edit.dialnorm2 = meta.p.dialnorm2;
    }
    if (meta.compr_word.has_value()) {
        edit.compr = meta.compr_word;
    }
    if (meta.compr2_word.has_value()) {
        edit.compr2 = meta.compr2_word;
    }
    edit.bsmod = meta.bsmod;
    edit.dsurmod = meta.dsurmod;
    if (!edit.dialnorm && !edit.dialnorm2 && !edit.compr && !edit.compr2 && !edit.bsmod &&
        !edit.dsurmod) {
        fmt::println(stderr,
                     "error: nothing to change - give at least one of dialnorm=, dialnorm2=, "
                     "compr=, compr2=, bsmod=, dsurmod=");
        return kExitUsage;
    }

    const auto summary = ac3::io::edit_stream_metadata(loaded->bytes, edit);
    if (!summary.has_value()) {
        fmt::println(stderr, "error: {}: {}", in_path, ac3::io::describe(summary.error()));
        return kExitUsage;
    }
    // Re-scanned rather than reusing the pre-edit spans: edit_stream_metadata
    // rewrote the buffer those pointed into, and re-deriving the framing from
    // the rewritten bytes is also a check that the rewrite left it walkable.
    const auto rescanned = ac3::io::scan(loaded->bytes);
    if (!rescanned.has_value()) {
        fmt::println(stderr, "error: the rewritten stream no longer scans: {}",
                     ac3::io::describe(rescanned.error()));
        return kExitInternal;  // the rewrite itself broke the framing
    }
    if (!write_units(out_path, rescanned->access_units, meta.keep_partial)) {
        return kExitOutput;
    }

    const auto status = status_stream(out_path);
    const auto after = ac3::io::read_frame_metadata(loaded->bytes);
    status_println(status, "rewrote {} of {} {} syncframes -> {} (audio untouched)",
                   summary->changed, summary->syncframes, codec_label(loaded->scan.kind), out_path);
    if (after.has_value()) {
        status_println(status, "  dialnorm {} -> {}", before->dialnorm, after->dialnorm);
        if (before->compr.has_value() && after->compr.has_value()) {
            status_println(status, "  compr    {:+.2f} -> {:+.2f} dB",
                           ac3::meta::to_db(ac3::meta::compr_gain(*before->compr)),
                           ac3::meta::to_db(ac3::meta::compr_gain(*after->compr)));
        }
        if (before->bsmod.has_value() && after->bsmod.has_value()) {
            status_println(status, "  bsmod    {} -> {}", *before->bsmod, *after->bsmod);
        }
        if (before->dsurmod.has_value() && after->dsurmod.has_value()) {
            status_println(status, "  dsurmod  {} -> {}", *before->dsurmod, *after->dsurmod);
        }
    }
    return 0;
}

int run_normalize(std::string_view in_path, std::string_view out_path, const Options& meta) {
    auto loaded = load_stream(in_path);
    if (!loaded.has_value()) {
        return kExitInput;
    }
    const auto before = ac3::io::read_frame_metadata(loaded->bytes);
    if (!before.has_value()) {
        fmt::println(stderr, "error: {}: {}", in_path, ac3::io::describe(before.error()));
        return kExitInput;
    }
    // The measurement is a full decode - the BS.1770-4 relative gate needs
    // the whole programme, so there is no shortcut - but the audio it
    // produces is thrown away: only the dialnorm it implies is written back.
    const auto measured = measure_stream_loudness(loaded->bytes);
    if (!measured.has_value()) {
        return kExitInput;
    }
    if (!measured->integrated_lkfs.has_value()) {
        fmt::println(stderr,
                     "error: no audio above the -70 LKFS absolute gate; nothing to normalise "
                     "against");
        return kExitRuntime;
    }

    // ATSC A/85 §8: dialnorm states where dialogue sits relative to full
    // scale, and the anchor for a programme with no separate dialogue
    // measurement is its own integrated loudness.
    ac3::io::MetadataEdit edit;
    edit.dialnorm = ac3::meta::dialnorm_from_lkfs(*measured->integrated_lkfs);
    if (before->dialnorm2.has_value() && measured->ch2_lkfs.has_value()) {
        // 1+1 levels its two programmes independently (§E1.3, no downmix
        // between them), so Ch2 gets its own measurement rather than Ch1's.
        edit.dialnorm2 = ac3::meta::dialnorm_from_lkfs(*measured->ch2_lkfs);
    }

    const auto summary = ac3::io::edit_stream_metadata(loaded->bytes, edit);
    if (!summary.has_value()) {
        fmt::println(stderr, "error: {}: {}", in_path, ac3::io::describe(summary.error()));
        return kExitUsage;
    }
    const auto rescanned = ac3::io::scan(loaded->bytes);
    if (!rescanned.has_value()) {
        fmt::println(stderr, "error: the rewritten stream no longer scans: {}",
                     ac3::io::describe(rescanned.error()));
        return kExitInternal;  // the rewrite itself broke the framing
    }
    if (!write_units(out_path, rescanned->access_units, meta.keep_partial)) {
        return kExitOutput;
    }

    const auto status = status_stream(out_path);
    status_println(status, "normalised {} ({} syncframes, audio untouched) -> {}", in_path,
                   summary->syncframes, out_path);
    status_println(status, "  measured   {:+.2f} LKFS (BS.1770-4 gated)",
                   *measured->integrated_lkfs);
    status_println(status, "  dialnorm   {} -> {} (ATSC A/85 §8)", before->dialnorm,
                   *edit.dialnorm);
    // All three re-checked together: the block above sets dialnorm2 only when
    // the other two hold, but that is two screens away and nothing local
    // says so.
    if (edit.dialnorm2.has_value() && before->dialnorm2.has_value() && measured->ch2_lkfs.has_value()) {
        status_println(status, "  dialnorm2  {} -> {} (Ch2 measured {:+.2f} LKFS)",
                       *before->dialnorm2, *edit.dialnorm2, *measured->ch2_lkfs);
    }
    return 0;
}

int run_cut(std::string_view in_path, std::string_view out_path, std::string_view start_seconds,
            std::string_view duration_seconds) {
    const auto loaded = load_stream(in_path);
    if (!loaded.has_value()) {
        return kExitInput;
    }
    const auto& scan = loaded->scan;
    const auto total_units = scan.access_units.size();

    const double start = start_seconds.empty() ? 0.0 : parse_seconds_or(start_seconds, 0.0);
    if (start < 0.0) {
        fmt::println(stderr, "error: start must not be negative");
        return kExitUsage;
    }
    const auto first = start == 0.0
                           ? std::optional<std::size_t>{0}
                           : ac3::io::access_unit_at_seconds(scan, start);
    if (!first.has_value()) {
        fmt::println(stderr, "error: start {:.3f} s is past the end of {} ({:.3f} s)", start,
                     in_path, ac3::io::stream_duration_seconds(scan));
        return kExitUsage;
    }

    std::size_t last = total_units;  // exclusive
    if (!duration_seconds.empty()) {
        const double duration = parse_seconds_or(duration_seconds, 0.0);
        if (duration <= 0.0) {
            fmt::println(stderr, "error: duration must be positive");
            return kExitUsage;
        }
        const auto start_timing = ac3::io::access_unit_timing(scan, *first);
        // Measured from the access unit the cut actually starts at, not from
        // the requested time: the boundary is where the extract really
        // begins, and asking for 1.0 s from a start that snapped 20 ms
        // earlier should give 1.0 s of stream, not 0.98.
        const double end_seconds =
            (start_timing ? start_timing->start_seconds() : 0.0) + duration;
        const auto end_unit = ac3::io::access_unit_at_seconds(scan, end_seconds);
        // Past the end simply means "to the end", which is what a duration
        // longer than the remainder should do.
        last = end_unit ? *end_unit : total_units;
        if (last <= *first) {
            // The requested duration is shorter than one access unit. One
            // whole unit is the smallest thing a frame-aligned cut can
            // produce, so that is what comes out - said plainly rather than
            // rounded to nothing.
            fmt::println(stderr,
                         "note: {:.3f} s is shorter than one access unit ({:.3f} s); writing one",
                         duration,
                         start_timing ? start_timing->duration_seconds() : 0.0);
            last = *first + 1;
        }
    }

    const auto units =
        std::span{scan.access_units}.subspan(*first, last - *first);
    if (!write_units(out_path, units, false)) {
        return kExitOutput;
    }

    const auto status = status_stream(out_path);
    const auto from = ac3::io::access_unit_timing(scan, *first);
    std::uint64_t kept_samples = 0;
    for (std::size_t i = *first; i < last; ++i) {
        kept_samples += scan.access_unit_samples[i];
    }
    const auto rate = ac3::sample_rate_hz(scan.sample_rate);
    status_println(status, "cut {} access units of {} from {} -> {}", units.size(), total_units,
                   in_path, out_path);
    status_println(status, "  {}{}, {:.3f} s from {:.3f} s (access-unit aligned)",
                   codec_label(scan.kind),
                   scan.substreams_per_unit > 1
                       ? fmt::format(", {} substreams per unit", scan.substreams_per_unit)
                       : std::string{},
                   rate == 0 ? 0.0 : static_cast<double>(kept_samples) / rate,
                   from ? from->start_seconds() : 0.0);
    return 0;
}

int run_cat(std::string_view out_path, std::span<const std::string_view> in_paths) {
    if (in_paths.size() < 2) {
        fmt::println(stderr, "error: cat needs at least two inputs");
        return kExitUsage;
    }
    // Unlike every other command here, this opens its output BEFORE reading
    // its inputs - one at a time, so only one input's bytes are resident at
    // once however many are joined. That makes naming an input as the output
    // destructive rather than merely odd (the sink truncates it first), so it
    // is refused. Compared as paths rather than as text, so "./a.ac3" and
    // "a.ac3" are recognised as the same file.
    //
    // Refused whether or not the output exists yet: an output that does not
    // exist when this runs is created by the sink before the loop below
    // reaches the input of the same name, which would then read back this
    // command's own half-written output. fs::equivalent alone cannot see
    // that case - it reports false (with an error) when either path is
    // missing - so it is backed by a comparison of the normalised absolute
    // paths, which needs neither file to exist. equivalent() still runs
    // first, since only it sees a hard link or a symlink to the same file
    // under another name.
    const auto same_file = [](const std::filesystem::path& a, const std::filesystem::path& b) {
        std::error_code ec;
        if (std::filesystem::equivalent(a, b, ec)) {
            return true;
        }
        const auto a_norm = std::filesystem::weakly_canonical(a, ec);
        if (ec) {
            return false;
        }
        const auto b_norm = std::filesystem::weakly_canonical(b, ec);
        return !ec && a_norm == b_norm;
    };
    if (!is_stdio_path(out_path)) {
        const std::filesystem::path out_fs{std::string{out_path}};
        for (const auto path : in_paths) {
            if (!is_stdio_path(path) &&
                same_file(out_fs, std::filesystem::path{std::string{path}})) {
                fmt::println(stderr, "error: {} is both an input and the output", path);
                return kExitUsage;
            }
        }
    }
    // Loaded one at a time and written straight through, so only one input's
    // bytes are resident at once however many are joined.
    EncodedStreamSink sink;
    if (!sink.open(out_path, false)) {
        return kExitOutput;
    }
    // The comparable fields only, copied out by value: a ScannedStream's
    // access_units are spans into the buffer it was scanned from, and that
    // buffer is freed at the end of each iteration below. Nothing here needs
    // them, and keeping a whole ScannedStream would leave dangling spans
    // sitting in scope waiting for someone to use them.
    struct Shape {
        ac3::io::StreamKind kind = ac3::io::StreamKind::kAc3;
        ac3::SampleRate sample_rate = ac3::SampleRate::k48000;
        ac3::Acmod acmod = ac3::Acmod::k2_0;
        bool lfe = false;
        int channels = 0;
        std::size_t substreams_per_unit = 0;
    };
    std::optional<Shape> reference;
    std::string_view reference_path;
    std::size_t units = 0;
    std::uint64_t samples = 0;
    for (const auto path : in_paths) {
        const auto loaded = load_stream(path);
        if (!loaded.has_value()) {
            sink.abort();
            return kExitInput;
        }
        const auto& scan = loaded->scan;
        if (!reference.has_value()) {
            reference = Shape{.kind = scan.kind,
                              .sample_rate = scan.sample_rate,
                              .acmod = scan.acmod,
                              .lfe = scan.lfe,
                              .channels = scan.channels,
                              .substreams_per_unit = scan.substreams_per_unit};
            reference_path = path;
        } else {
            // A decoder walking a concatenated stream has no way to be told
            // the format changed mid-file - it would simply mis-render from
            // the join onward - so a mismatch is refused rather than joined.
            const auto mismatch = [&]() -> std::string_view {
                if (scan.kind != reference->kind) return "codec";
                if (scan.sample_rate != reference->sample_rate) return "sample rate";
                if (scan.acmod != reference->acmod) return "coding mode (acmod)";
                if (scan.lfe != reference->lfe) return "LFE presence";
                if (scan.channels != reference->channels) return "rendered channel count";
                if (scan.substreams_per_unit != reference->substreams_per_unit) {
                    return "substream count per access unit";
                }
                return {};
            }();
            if (!mismatch.empty()) {
                fmt::println(stderr,
                             "error: {} differs from {} in {} - a decoder cannot follow that "
                             "across a join",
                             path, reference_path, mismatch);
                sink.abort();
                return kExitUsage;
            }
        }
        for (const auto& unit : scan.access_units) {
            if (!sink.push(unit)) {
                sink.abort();
                return kExitOutput;
            }
        }
        units += scan.access_units.size();
        samples += ac3::io::stream_duration_samples(scan);
    }
    if (!sink.close()) {
        return kExitOutput;
    }

    if (!reference.has_value()) {
        // Unreachable: the loop above runs at least twice (in_paths.size() >= 2
        // is checked at the top) and fills this on its first pass. Stated as a
        // real check rather than an assert, so the report below reads a value
        // that is checked where it is used.
        fmt::println(stderr, "error: nothing was joined");
        return kExitInternal;
    }
    const auto status = status_stream(out_path);
    const auto rate = ac3::sample_rate_hz(reference->sample_rate);
    status_println(status, "joined {} files, {} {} access units -> {}", in_paths.size(), units,
                   codec_label(reference->kind), out_path);
    status_println(status, "  {:.3f} s, {} Hz, {} channels",
                   rate == 0 ? 0.0 : static_cast<double>(samples) / rate, rate,
                   reference->channels);
    return 0;
}

}  // namespace ac3cli::commands

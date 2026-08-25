#include "stream_tools.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fmt/base.h>
#include <fmt/format.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../support.hpp"
#include "analysis.hpp"
#include "ac3/analysis/levels.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/metadata_edit.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/loudness.hpp"
#include "ac3/meta/mixing.hpp"

namespace ac3cli::commands {

namespace plan = ac3::plan;

namespace {

// --- shared plumbing --------------------------------------------------------

// The whole input, scanned. Every command here needs the same two things
// first - the bytes, and what the bitstream says it is - and reports the same
// two failures.
//
// The INPUT is read whole, the same way decode/mkv/mp4/ts/spdif already read
// theirs: an elementary stream has to be framed before anything can be done
// with it, and there is no seek. What the 0.9.0 memory work fixed, and what
// every command here keeps, is the OUTPUT side - bytes leave through
// EncodedStreamSink as they are produced rather than accumulating.
struct LoadedStream {
    std::vector<std::byte> bytes;
    ac3::io::ScannedStream scan;
};

std::optional<LoadedStream> load_stream(std::string_view path) {
    LoadedStream loaded;
    loaded.bytes = read_all(path);
    if (loaded.bytes.empty()) {
        fmt::println(stderr, "error: cannot read {}", path);
        return std::nullopt;
    }
    auto scanned = ac3::io::scan(loaded.bytes);
    if (!scanned) {
        fmt::println(stderr, "error: {}: {}", path, ac3::io::describe(scanned.error()));
        return std::nullopt;
    }
    loaded.scan = std::move(*scanned);
    return loaded;
}

std::string_view codec_label(ac3::io::StreamKind kind) {
    return kind == ac3::io::StreamKind::kEac3 ? "E-AC-3" : "AC-3";
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
    if (meta.codec) {
        return meta.codec;
    }
    if (out_path.ends_with(".ac3")) {
        return plan::Codec::kAc3;
    }
    if (out_path.ends_with(".ec3") || out_path.ends_with(".eac3")) {
        return plan::Codec::kEac3;
    }
    fmt::println(stderr,
                 "error: cannot tell which codec to write from '{}' - name it .ac3 or .ec3, "
                 "or pass codec=ac3|eac3",
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
    if (source.cmixlev) {
        target.cmixlev = *source.cmixlev;
    }
    if (source.surmixlev) {
        target.surmixlev = *source.surmixlev;
    }
    if (!source.mix) {
        return;
    }
    const bool ltrt = source.mix->dmixmod == ac3::meta::DownmixMode::kLtRt;
    const auto centre = ltrt ? source.mix->ltrtcmixlev : source.mix->lorocmixlev;
    const auto surround = ltrt ? source.mix->ltrtsurmixlev : source.mix->lorosurmixlev;
    if (centre) {
        target.cmixlev = nearest_level(*centre, kCentre);
    }
    if (surround) {
        target.surmixlev = nearest_level(*surround, kSurround);
    }
    if (source.mix->dmixmod) {
        target.dmixmod = *source.mix->dmixmod;
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

// The encode half of a transcode: one plan, one encoder, frames out through
// the sink. Kept as a type rather than a lambda because AC-3 and E-AC-3 have
// separate encoder classes with no common base, and the decode loop above it
// should not care which one it is feeding.
class TranscodeEncoder {
   public:
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
            eac3_ = std::make_unique<ac3::eac3::AccessUnitEncoder>(plan::eac3_config(p));
            coded_channels_ = static_cast<std::size_t>(eac3_->channel_count());
        }
        return sink_.open(out_path, keep_partial);
    }

    [[nodiscard]] std::size_t coded_channels() const { return coded_channels_; }

    [[nodiscard]] bool encode(std::span<const std::span<const float>> channels) {
        std::vector<std::byte> frame;
        if (ac3_) {
            auto encoded = ac3_->encode_frame(channels);
            if (!encoded) {
                fmt::println(stderr, "error: encode failed - bitrate must be a legal AC-3 rate");
                sink_.abort();
                return false;
            }
            frame = std::move(*encoded);
        } else {
            auto unit = eac3_->encode_access_unit(channels);
            if (!unit) {
                fmt::println(stderr, "error: the encoder cannot express this configuration");
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
        if (compr_) {
            const auto edited = ac3::io::edit_frame_metadata(frame, {.compr = compr_});
            if (!edited) {
                fmt::println(stderr, "error: cannot carry compr across: {}",
                             ac3::io::describe(edited.error()));
                sink_.abort();
                return false;
            }
        }
        if (!sink_.push(std::move(frame))) {
            sink_.abort();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool close() { return sink_.close(); }
    void abort() { sink_.abort(); }
    [[nodiscard]] std::size_t frames() const { return sink_.frames(); }

   private:
    plan::Codec codec_ = plan::Codec::kAc3;
    std::unique_ptr<ac3::FrameEncoder> ac3_;
    std::unique_ptr<ac3::eac3::AccessUnitEncoder> eac3_;
    std::size_t coded_channels_ = 0;
    std::optional<std::uint8_t> compr_;
    EncodedStreamSink sink_;
};

}  // namespace

int run_transcode(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
                  std::string_view layout, const Options& meta) {
    const auto loaded = load_stream(in_path);
    if (!loaded) {
        return 1;
    }
    const auto target_codec = output_codec(out_path, meta);
    if (!target_codec) {
        return 1;
    }
    const auto source_meta = ac3::io::read_frame_metadata(loaded->bytes);
    if (!source_meta) {
        fmt::println(stderr, "error: {}: {}", in_path, ac3::io::describe(source_meta.error()));
        return 1;
    }

    // status_stream(out_path): stderr instead of stdout when the encoded
    // bytes themselves are going to stdout.
    const auto status = status_stream(out_path);
    const bool eac3_source = loaded->scan.kind == ac3::io::StreamKind::kEac3;
    const auto source_rate = ac3::sample_rate_hz(loaded->scan.sample_rate);
    const auto rate = wav_sample_rate(
        source_rate, *target_codec == plan::Codec::kAc3 ? "AC-3" : "E-AC-3",
        *target_codec == plan::Codec::kEac3);
    if (!rate) {
        return 1;
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
    if (!meta.dialnorm2_given && source_meta->dialnorm2) {
        p.meta.dialnorm2 = *source_meta->dialnorm2;
    }
    carry_mix_metadata(*source_meta, p.meta);
    if (*target_codec == plan::Codec::kEac3 && source_meta->mix) {
        p.meta.mixmeta = true;
    }
    // A compr word the operator did not override is carried across verbatim
    // (TranscodeEncoder::encode stamps it back), which needs compre set in
    // the output - hence a HeavyConfig here even though its own detector's
    // answer is then overwritten.
    std::optional<std::uint8_t> compr_passthrough;
    if (source_meta->compr && !meta.p.heavy) {
        p.meta.heavy.emplace();
        compr_passthrough = source_meta->compr;
    }

    // --- layout -------------------------------------------------------------
    std::string label;
    const auto source_channels = static_cast<std::size_t>(loaded->scan.channels);
    if (!layout.empty()) {
        if (!resolve_layout(layout, *target_codec, p, label)) {
            return 1;
        }
    } else if (loaded->scan.acmod == ac3::Acmod::kDualMono) {
        // 1+1 is two independent programmes sharing one syncframe, not a
        // soundfield (§E1.3, no downmix between them). Its two channels are
        // Ch1 and Ch2, not L and R, so following the channel COUNT the way
        // every other layout does would quietly turn a bilingual programme
        // into a stereo one.
        p.layout = plan::LayoutId::kDualMono;
        label = std::string(plan::layout(plan::LayoutId::kDualMono).label);
        if (!meta.dialnorm2_given && !source_meta->dialnorm2) {
            fmt::println(stderr, "error: {} is 1+1 but carries no dialnorm2", in_path);
            return 1;
        }
    } else {
        auto id = plan::layout_for_source(source_channels);
        if (id && !plan::carries(*target_codec, *id)) {
            // The DD+-to-DD case this command exists for: an immersive or 7.1
            // programme has no AC-3 coding mode, so it folds to 5.1 per §7.8
            // using the mix levels carried across just above. Said out loud
            // rather than done quietly - it is a real change to what the
            // listener hears.
            fmt::println(status,
                         "note: {} channels have no AC-3 coding mode; folding down to 5.1 "
                         "(§7.8, using the stream's own mix levels)",
                         source_channels);
            id = plan::LayoutId::k51;
        }
        if (!id) {
            fmt::println(stderr, "error: no standard layout has {} channels; name one with the "
                                 "[layout] argument",
                         source_channels);
            return 1;
        }
        p.layout = *id;
        label = std::string(plan::layout(*id).label);
    }
    p.tools.fast_mdct = meta.fast_mdct;
    if (const auto bad = plan::validate(p)) {
        fmt::println(stderr, "error: {}", plan::describe(*bad));
        return 1;
    }

    const auto routing = routing_or_error(p, source_channels);
    if (!routing) {
        return 1;
    }

    TranscodeEncoder encoder;
    if (!encoder.open(p, out_path, meta.keep_partial, compr_passthrough)) {
        return 1;
    }
    const auto coded_channels = encoder.coded_channels();

    // Planar buffers, allocated once: the queue feeds `source`, plan::render
    // writes `coded`, the encoder reads it.
    SampleQueue queue;
    queue.reset(source_channels);
    std::vector<std::vector<float>> source(source_channels,
                                           std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::vector<float>> coded(coded_channels,
                                          std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> in(source_channels);
    std::vector<std::span<float>> out(coded_channels);
    std::vector<std::span<const float>> views(coded_channels);
    std::vector<std::span<float>> source_spans(source_channels);
    for (std::size_t c = 0; c < source_channels; ++c) {
        in[c] = source[c];
        source_spans[c] = source[c];
    }
    for (std::size_t c = 0; c < coded_channels; ++c) {
        out[c] = coded[c];
        views[c] = coded[c];
    }
    const auto coded_plan = plan::resolve(p);
    ac3::analysis::LevelMeter meter{coded_plan.bed_acmod, coded_plan.bed_lfe, source_rate};

    // Whole frames out of the queue as soon as there are any, so neither the
    // decoded programme nor the encoded stream is ever held whole.
    const auto drain = [&](bool flush) -> bool {
        while (queue.available() >= static_cast<std::size_t>(ac3::kSamplesPerFrame) ||
               (flush && queue.available() > 0)) {
            const auto count =
                std::min<std::size_t>(queue.available(), ac3::kSamplesPerFrame);
            queue.take(count, source_spans);
            plan::render(*routing, in, out, ac3::kSamplesPerFrame);
            meter.process(views);
            if (!encoder.encode(views)) {
                return false;
            }
        }
        return true;
    };

    std::size_t units_in = 0;
    double dynrng_min_db = 0.0;
    double dynrng_max_db = 0.0;
    std::size_t dynrng_words = 0;
    const auto track_dynrng = [&](std::span<const std::uint8_t> words) {
        for (const auto word : words) {
            const double db = ac3::meta::to_db(ac3::meta::dynrng_gain(word));
            dynrng_min_db = dynrng_words == 0 ? db : std::min(dynrng_min_db, db);
            dynrng_max_db = dynrng_words == 0 ? db : std::max(dynrng_max_db, db);
            ++dynrng_words;
        }
    };

    // A stream whose channel count changes part-way through would leave the
    // queue's channels at different lengths and silently truncate the encode
    // at the shortest, so it is refused with a reason instead. The routing
    // was fixed from the scan's own channel count before the first decode.
    const auto require_width = [&](std::size_t got) -> bool {
        if (got == source_channels) {
            return true;
        }
        fmt::println(stderr,
                     "error: {}: the programme changes from {} channels to {} part-way through, "
                     "which one routing cannot describe",
                     in_path, source_channels, got);
        encoder.abort();
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
        for (const auto& unit : loaded->scan.access_units) {
            const auto decoded = decoder->decode_access_unit(unit);
            if (!decoded) {
                fmt::println(stderr, "error: {}: {}", in_path, ac3::describe(decoded.error()));
                encoder.abort();
                return 1;
            }
            if (!decoded->has_value()) {
                // §3.7 transient pre-noise hold-back - nothing ready yet.
                continue;
            }
            const auto& programme = **decoded;
            ++units_in;
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
                return 1;
            }
            for (std::size_t slot = 0; slot < programme.channels.size(); ++slot) {
                queue.push(slot_to_wav[slot], programme.channels[slot]);
            }
            if (!drain(false)) {
                return 1;
            }
        }
        // Whatever transient pre-noise processing was still holding back
        // (§3.7). flush() returns raw per-substream results rather than
        // assembled access units, so each substream's channels are placed at
        // the slot its own Table E2.5 location occupies in the programme
        // layout - exactly as decode_access_unit's own §E3.8.2 assembly
        // would have. Appending them by coded index instead would write a
        // dependent's height channels over the bed's L/R.
        const auto flushed = decoder->flush();
        if (!flushed.empty() && slot_to_wav.empty()) {
            fmt::println(stderr,
                         "error: {}: no access unit ever completed, so there is no programme "
                         "layout to place the held-back frames into",
                         in_path);
            encoder.abort();
            return 1;
        }
        for (const auto& substream : flushed) {
            if (substream.strmtyp == ac3::eac3::StreamType::kIndependent) {
                ++units_in;
            }
            const auto locations = ac3::eac3::chanmap::expand(substream.location_map());
            for (int i = 0; i < locations.count; ++i) {
                const int slot = programme_layout.index_of(locations[i]);
                if (slot < 0 || static_cast<std::size_t>(slot) >= slot_to_wav.size()) {
                    continue;
                }
                queue.push(slot_to_wav[static_cast<std::size_t>(slot)],
                           substream.channels[static_cast<std::size_t>(i)]);
            }
        }
        if (!flushed.empty() && !drain(false)) {
            return 1;
        }
    } else {
        // Heap-allocated for the same C6262 reason the eac3_source branch's
        // own decoder above gives: FrameDecoder carries ~12 KB of per-channel
        // overlap-add state (decoder.hpp's own comment on it).
        auto decoder = std::make_unique<ac3::FrameDecoder>();
        const auto order = ac3::io::wav_channel_order(loaded->scan.acmod, loaded->scan.lfe);
        for (const auto& frame : loaded->scan.access_units) {
            const auto decoded = decoder->decode_frame(frame);
            if (!decoded) {
                fmt::println(stderr, "error: {}: {}", in_path, ac3::describe(decoded.error()));
                encoder.abort();
                return 1;
            }
            ++units_in;
            track_dynrng(decoded->dynrng);
            if (!require_width(decoded->channels.size())) {
                return 1;
            }
            for (std::size_t i = 0; i < order.size(); ++i) {
                queue.push(i, decoded->channels[order[i]]);
            }
            if (!drain(false)) {
                return 1;
            }
        }
    }
    if (!drain(true)) {
        return 1;
    }
    if (!encoder.close()) {
        return 1;
    }

    fmt::println(status, "transcoded {} {} access units -> {} {} frames ({} kbps, {} Hz) in {}",
                 units_in, codec_label(loaded->scan.kind), encoder.frames(),
                 *target_codec == plan::Codec::kAc3 ? "AC-3" : "E-AC-3", bitrate, source_rate,
                 out_path);
    fmt::println(status, "  layout {} <- {} source channels", label, source_channels);
    fmt::println(status, "  dialnorm {}{}", p.meta.dialnorm,
                 meta.dialnorm_given ? " (from dialnorm=)" : " (carried from the source)");
    if (compr_passthrough) {
        fmt::println(status, "  compr    {:+.2f} dB carried across verbatim",
                     ac3::meta::to_db(ac3::meta::compr_gain(*compr_passthrough)));
    } else if (p.meta.heavy) {
        fmt::println(status, "  compr    re-derived (heavy given on the command line)");
    } else {
        fmt::println(status, "  compr    absent in the source");
    }
    // dynrng is a per-BLOCK word derived from the signal, so a re-encode has
    // to produce its own rather than copy the source's - there is no bsi
    // field to stamp it into the way compr has. Reported so the difference is
    // visible rather than discovered.
    if (dynrng_words > 0 && (dynrng_min_db != 0.0 || dynrng_max_db != 0.0)) {
        fmt::println(status,
                     "  dynrng   source carried {:+.2f} .. {:+.2f} dB; the re-encode {}",
                     dynrng_min_db, dynrng_max_db,
                     p.meta.drc ? "derives its own from drc=" : "writes none (pass drc=<profile>)");
    }
    print_channel_summary(meter, status);
    return 0;
}

int run_metadata(std::string_view in_path, std::string_view out_path, const Options& meta) {
    auto loaded = load_stream(in_path);
    if (!loaded) {
        return 1;
    }
    const auto before = ac3::io::read_frame_metadata(loaded->bytes);
    if (!before) {
        fmt::println(stderr, "error: {}: {}", in_path, ac3::io::describe(before.error()));
        return 1;
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
            return 1;
        }
        edit.dialnorm = meta.p.dialnorm;
    }
    if (meta.dialnorm2_given) {
        if (meta.p.measure_dialnorm2) {
            fmt::println(stderr, "error: dialnorm2=auto needs a measurement - use 'ac3cli "
                                 "normalize'");
            return 1;
        }
        edit.dialnorm2 = meta.p.dialnorm2;
    }
    if (meta.compr_word) {
        edit.compr = meta.compr_word;
    }
    if (meta.compr2_word) {
        edit.compr2 = meta.compr2_word;
    }
    edit.bsmod = meta.bsmod;
    edit.dsurmod = meta.dsurmod;
    if (!edit.dialnorm && !edit.dialnorm2 && !edit.compr && !edit.compr2 && !edit.bsmod &&
        !edit.dsurmod) {
        fmt::println(stderr,
                     "error: nothing to change - give at least one of dialnorm=, dialnorm2=, "
                     "compr=, compr2=, bsmod=, dsurmod=");
        return 1;
    }

    const auto summary = ac3::io::edit_stream_metadata(loaded->bytes, edit);
    if (!summary) {
        fmt::println(stderr, "error: {}: {}", in_path, ac3::io::describe(summary.error()));
        return 1;
    }
    // Re-scanned rather than reusing the pre-edit spans: edit_stream_metadata
    // rewrote the buffer those pointed into, and re-deriving the framing from
    // the rewritten bytes is also a check that the rewrite left it walkable.
    const auto rescanned = ac3::io::scan(loaded->bytes);
    if (!rescanned) {
        fmt::println(stderr, "error: the rewritten stream no longer scans: {}",
                     ac3::io::describe(rescanned.error()));
        return 1;
    }
    if (!write_units(out_path, rescanned->access_units, meta.keep_partial)) {
        return 1;
    }

    const auto status = status_stream(out_path);
    const auto after = ac3::io::read_frame_metadata(loaded->bytes);
    fmt::println(status, "rewrote {} of {} {} syncframes -> {} (audio untouched)",
                 summary->changed, summary->syncframes, codec_label(loaded->scan.kind), out_path);
    if (after) {
        fmt::println(status, "  dialnorm {} -> {}", before->dialnorm, after->dialnorm);
        if (before->compr && after->compr) {
            fmt::println(status, "  compr    {:+.2f} -> {:+.2f} dB",
                         ac3::meta::to_db(ac3::meta::compr_gain(*before->compr)),
                         ac3::meta::to_db(ac3::meta::compr_gain(*after->compr)));
        }
        if (before->bsmod && after->bsmod) {
            fmt::println(status, "  bsmod    {} -> {}", *before->bsmod, *after->bsmod);
        }
        if (before->dsurmod && after->dsurmod) {
            fmt::println(status, "  dsurmod  {} -> {}", *before->dsurmod, *after->dsurmod);
        }
    }
    return 0;
}

int run_normalize(std::string_view in_path, std::string_view out_path, const Options& meta) {
    auto loaded = load_stream(in_path);
    if (!loaded) {
        return 1;
    }
    const auto before = ac3::io::read_frame_metadata(loaded->bytes);
    if (!before) {
        fmt::println(stderr, "error: {}: {}", in_path, ac3::io::describe(before.error()));
        return 1;
    }
    // The measurement is a full decode - the BS.1770-4 relative gate needs
    // the whole programme, so there is no shortcut - but the audio it
    // produces is thrown away: only the dialnorm it implies is written back.
    const auto measured = measure_stream_loudness(loaded->bytes);
    if (!measured) {
        return 1;
    }
    if (!measured->integrated_lkfs) {
        fmt::println(stderr,
                     "error: no audio above the -70 LKFS absolute gate; nothing to normalise "
                     "against");
        return 1;
    }

    // ATSC A/85 §8: dialnorm states where dialogue sits relative to full
    // scale, and the anchor for a programme with no separate dialogue
    // measurement is its own integrated loudness.
    ac3::io::MetadataEdit edit;
    edit.dialnorm = ac3::meta::dialnorm_from_lkfs(*measured->integrated_lkfs);
    if (before->dialnorm2 && measured->ch2_lkfs) {
        // 1+1 levels its two programmes independently (§E1.3, no downmix
        // between them), so Ch2 gets its own measurement rather than Ch1's.
        edit.dialnorm2 = ac3::meta::dialnorm_from_lkfs(*measured->ch2_lkfs);
    }

    const auto summary = ac3::io::edit_stream_metadata(loaded->bytes, edit);
    if (!summary) {
        fmt::println(stderr, "error: {}: {}", in_path, ac3::io::describe(summary.error()));
        return 1;
    }
    const auto rescanned = ac3::io::scan(loaded->bytes);
    if (!rescanned) {
        fmt::println(stderr, "error: the rewritten stream no longer scans: {}",
                     ac3::io::describe(rescanned.error()));
        return 1;
    }
    if (!write_units(out_path, rescanned->access_units, meta.keep_partial)) {
        return 1;
    }

    const auto status = status_stream(out_path);
    fmt::println(status, "normalised {} ({} syncframes, audio untouched) -> {}", in_path,
                 summary->syncframes, out_path);
    fmt::println(status, "  measured   {:+.2f} LKFS (BS.1770-4 gated)", *measured->integrated_lkfs);
    fmt::println(status, "  dialnorm   {} -> {} (ATSC A/85 §8)", before->dialnorm, *edit.dialnorm);
    // All three re-checked together: the block above sets dialnorm2 only when
    // the other two hold, but that is two screens away and nothing local
    // says so.
    if (edit.dialnorm2 && before->dialnorm2 && measured->ch2_lkfs) {
        fmt::println(status, "  dialnorm2  {} -> {} (Ch2 measured {:+.2f} LKFS)",
                     *before->dialnorm2, *edit.dialnorm2, *measured->ch2_lkfs);
    }
    return 0;
}

int run_cut(std::string_view in_path, std::string_view out_path, std::string_view start_seconds,
            std::string_view duration_seconds) {
    const auto loaded = load_stream(in_path);
    if (!loaded) {
        return 1;
    }
    const auto& scan = loaded->scan;
    const auto total_units = scan.access_units.size();

    const double start = start_seconds.empty() ? 0.0 : parse_seconds_or(start_seconds, 0.0);
    if (start < 0.0) {
        fmt::println(stderr, "error: start must not be negative");
        return 1;
    }
    const auto first = start == 0.0
                           ? std::optional<std::size_t>{0}
                           : ac3::io::access_unit_at_seconds(scan, start);
    if (!first) {
        fmt::println(stderr, "error: start {:.3f} s is past the end of {} ({:.3f} s)", start,
                     in_path, ac3::io::stream_duration_seconds(scan));
        return 1;
    }

    std::size_t last = total_units;  // exclusive
    if (!duration_seconds.empty()) {
        const double duration = parse_seconds_or(duration_seconds, 0.0);
        if (duration <= 0.0) {
            fmt::println(stderr, "error: duration must be positive");
            return 1;
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
        return 1;
    }

    const auto status = status_stream(out_path);
    const auto from = ac3::io::access_unit_timing(scan, *first);
    std::uint64_t kept_samples = 0;
    for (std::size_t i = *first; i < last; ++i) {
        kept_samples += scan.access_unit_samples[i];
    }
    const auto rate = ac3::sample_rate_hz(scan.sample_rate);
    fmt::println(status, "cut {} access units of {} from {} -> {}", units.size(), total_units,
                 in_path, out_path);
    fmt::println(status, "  {}{}, {:.3f} s from {:.3f} s (access-unit aligned)",
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
        return 1;
    }
    // Unlike every other command here, this opens its output BEFORE reading
    // its inputs - one at a time, so only one input's bytes are resident at
    // once however many are joined. That makes naming an input as the output
    // destructive rather than merely odd (the sink truncates it first), so it
    // is refused. Compared as paths rather than as text, so "./a.ac3" and
    // "a.ac3" are recognised as the same file.
    for (const auto path : in_paths) {
        std::error_code ec;
        if (std::filesystem::equivalent(std::filesystem::path{std::string{out_path}},
                                        std::filesystem::path{std::string{path}}, ec)) {
            fmt::println(stderr, "error: {} is both an input and the output", path);
            return 1;
        }
    }
    // Loaded one at a time and written straight through, so only one input's
    // bytes are resident at once however many are joined.
    EncodedStreamSink sink;
    if (!sink.open(out_path, false)) {
        return 1;
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
        if (!loaded) {
            sink.abort();
            return 1;
        }
        const auto& scan = loaded->scan;
        if (!reference) {
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
                return 1;
            }
        }
        for (const auto& unit : scan.access_units) {
            if (!sink.push(unit)) {
                sink.abort();
                return 1;
            }
        }
        units += scan.access_units.size();
        samples += ac3::io::stream_duration_samples(scan);
    }
    if (!sink.close()) {
        return 1;
    }

    if (!reference) {
        // Unreachable: the loop above runs at least twice (in_paths.size() >= 2
        // is checked at the top) and fills this on its first pass. Stated as a
        // real check rather than an assert, so the report below reads a value
        // that is checked where it is used.
        fmt::println(stderr, "error: nothing was joined");
        return 1;
    }
    const auto status = status_stream(out_path);
    const auto rate = ac3::sample_rate_hz(reference->sample_rate);
    fmt::println(status, "joined {} files, {} {} access units -> {}", in_paths.size(), units,
                 codec_label(reference->kind), out_path);
    fmt::println(status, "  {:.3f} s, {} Hz, {} channels",
                 rate == 0 ? 0.0 : static_cast<double>(samples) / rate, rate,
                 reference->channels);
    return 0;
}

}  // namespace ac3cli::commands

#include "stream_decoder.hpp"

#include <algorithm>
#include <fmt/format.h>
#include <utility>
#include <vector>

#include "ac3/decoder/output.hpp"
#include "ac3/io/elementary.hpp"
#include "ac4_stream.hpp"

// See stream_decoder.hpp. The decode path is apps/hearth/testsink/
// burst_output.cpp's, which a player needs as much as a test sink does; the
// end-of-stream path is the part only a player needs.

namespace ac3::hearth {

eac3::chanmap::Layout ac4_bed(std::span<const ac4::Speaker> speakers) {
    using eac3::chanmap::Location;
    eac3::chanmap::Layout bed;
    for (const ac4::Speaker speaker : speakers) {
        if (bed.count >= eac3::chanmap::kMaxChannels) {
            break;
        }
        Location location = Location::kLeft;
        // clang-format off
        switch (speaker) {
            case ac4::Speaker::kLeft: location = Location::kLeft; break;
            case ac4::Speaker::kRight: location = Location::kRight; break;
            case ac4::Speaker::kCentre: location = Location::kCentre; break;
            case ac4::Speaker::kLfe: location = Location::kLfe; break;
            case ac4::Speaker::kLeftSurround: location = Location::kLeftSurround; break;
            case ac4::Speaker::kRightSurround: location = Location::kRightSurround; break;
            case ac4::Speaker::kLeftBack: location = Location::kLrs; break;
            case ac4::Speaker::kRightBack: location = Location::kRrs; break;
            case ac4::Speaker::kLeftWide: location = Location::kLw; break;
            case ac4::Speaker::kRightWide: location = Location::kRw; break;
            case ac4::Speaker::kTopFrontLeft: location = Location::kVhl; break;
            case ac4::Speaker::kTopFrontRight: location = Location::kVhr; break;
            case ac4::Speaker::kTopBackLeft:
            case ac4::Speaker::kTopSideLeft: location = Location::kLts; break;
            case ac4::Speaker::kTopBackRight:
            case ac4::Speaker::kTopSideRight: location = Location::kRts; break;
            case ac4::Speaker::kLfe2: location = Location::kLfe2; break;
        }
        // clang-format on
        bed.items[static_cast<std::size_t>(bed.count++)] = location;
    }
    return bed;
}

Acmod ac4_acmod(std::span<const ac4::Speaker> speakers) {
    const auto has = [&speakers](ac4::Speaker speaker) {
        return std::ranges::find(speakers, speaker) != speakers.end();
    };
    if (!has(ac4::Speaker::kLeft)) {
        return Acmod::k1_0;
    }
    if (has(ac4::Speaker::kLeftSurround)) {
        return has(ac4::Speaker::kCentre) ? Acmod::k3_2 : Acmod::k2_2;
    }
    return has(ac4::Speaker::kCentre) ? Acmod::k3_0 : Acmod::k2_0;
}

namespace {

// The layout an access unit decodes to, from its headers alone: each
// syncframe's acmod and lfeon, and a dependent's chanmap where it carries one,
// unioned in Table E2.5 order as the decoder assembles it (§E3.8.2). Needed
// before the decode, since the block form hands the samples over during the
// call. The same reading as the test sink's.
[[nodiscard]] std::optional<eac3::chanmap::Layout> peek_layout(std::span<const std::byte> unit) {
    std::uint16_t map = 0;
    std::size_t offset = 0;
    while (offset < unit.size()) {
        const auto header = io::read_frame_header(unit.subspan(offset));
        if (!header || header->bytes == 0) {
            return std::nullopt;
        }
        const std::uint16_t own = eac3::chanmap::acmod_map(header->acmod, header->lfe);
        if (header->kind == io::StreamKind::kEac3 &&
            header->strmtyp == eac3::StreamType::kDependent) {
            map = static_cast<std::uint16_t>(map | header->chanmap.value_or(own));
        } else {
            map = static_cast<std::uint16_t>(map | own);
        }
        offset += header->bytes;
    }
    if (map == 0) {
        return std::nullopt;
    }
    return eac3::chanmap::expand(map);
}

[[nodiscard]] bool same_layout(const eac3::chanmap::Layout& a, const eac3::chanmap::Layout& b) {
    if (a.count != b.count) {
        return false;
    }
    for (int i = 0; i < a.count; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

// The downmix levels a released substream carries: Annex D's for a legacy
// core, which reports them in cmixlev/surmixlev/alternate_bsi, and Table
// E1.2's mixmdate for a genuine E-AC-3 substream - which mix_levels() turns
// into the AC-3 defaults when the stream says nothing.
[[nodiscard]] MixLevels levels_of(const DecodedSubstream& substream) {
    if (substream.cmixlev || substream.surmixlev || substream.alternate_bsi) {
        return mix_levels(substream.acmod, substream.cmixlev, substream.surmixlev,
                          substream.alternate_bsi);
    }
    return mix_levels(substream.mixing);
}

// The same choice for an assembled access unit, whose fields are its
// independent substream's.
[[nodiscard]] MixLevels levels_of(const DecodedAccessUnit& unit) {
    if (unit.cmixlev || unit.surmixlev || unit.alternate_bsi) {
        return mix_levels(unit.acmod, unit.cmixlev, unit.surmixlev, unit.alternate_bsi);
    }
    return mix_levels(unit.mixing);
}

[[nodiscard]] std::optional<int> bsmod_of(const std::optional<meta::BsiInfo>& info) {
    if (!info) {
        return std::nullopt;
    }
    return static_cast<int>(info->bsmod);
}

void report_frame(const DecodedFrame& frame, UnitReport& out) {
    out.acmod = frame.acmod;
    out.lfe = frame.lfe;
    out.substreams = 1;
    out.layout = frame.acmod == Acmod::kDualMono
                     ? eac3::chanmap::Layout{}
                     : eac3::chanmap::expand(eac3::chanmap::acmod_map(frame.acmod, frame.lfe));
    out.bsmod = frame.bsmod;
    out.dialnorm = frame.dialnorm;
    out.dialnorm2 = frame.dialnorm2;
    out.compr = frame.compr;
    out.compr2 = frame.compr2;
    out.dynrng = frame.dynrng;
    out.blocks = kBlocksPerFrame;
    int short_blocks = 0;
    for (int blk = 0; blk < kBlocksPerFrame; ++blk) {
        const bool any = std::ranges::any_of(
            frame.blksw, [blk](const std::array<bool, kBlocksPerFrame>& channel) {
                return channel[static_cast<std::size_t>(blk)];
            });
        short_blocks += any ? 1 : 0;
    }
    out.short_blocks = short_blocks;
    out.levels = mix_levels(frame.acmod, frame.cmixlev, frame.surmixlev, frame.alternate_bsi);
    out.concealed = frame.concealed;
    out.objects.reset();
    out.ac4.reset();
}

void report_unit(const DecodedAccessUnit& unit, UnitReport& out) {
    out.acmod = unit.acmod;
    out.layout = unit.acmod == Acmod::kDualMono ? eac3::chanmap::Layout{} : unit.layout;
    out.lfe = unit.layout.index_of(eac3::chanmap::Location::kLfe) >= 0;
    out.substreams = unit.substream_count;
    out.bsmod = bsmod_of(unit.info);
    out.dialnorm = unit.dialnorm;
    out.dialnorm2 = unit.dialnorm2;
    out.compr = unit.compr;
    out.compr2.reset();
    out.dynrng = unit.dynrng;
    out.blocks = eac3::blocks_per_syncframe(unit.numblkscod);
    out.short_blocks.reset();
    out.levels = levels_of(unit);
    out.concealed = unit.concealed;
    out.objects = unit.object_metadata;
    out.ac4.reset();
}

// What an AC-4 frame's concealment did, in the report's terms. Its error is
// AC-4's, which the report's own type does not name; the action is what the
// Play page shows.
[[nodiscard]] Concealment concealment_of(const ac4::Concealment& concealed) {
    return Concealment{.error = DecodeError::kInvalidStream,
                       .action = concealed.action == ac4::ConcealmentAction::kRepeatFade
                                     ? ConcealmentAction::kRepeatFade
                                     : ConcealmentAction::kMute};
}

}  // namespace

StreamDecoder::StreamDecoder(const render::OutputLayout& layout, std::uint32_t sample_rate,
                             const DecoderSettings& settings, Substreams substreams)
    : layout_(layout),
      sample_rate_(sample_rate),
      settings_(settings),
      substreams_(substreams),
      serving_(decoder_setup(settings, layout).serving),
      config_(decoder_setup(settings, layout).config),
      renderer_(layout, sample_rate),
      ac4_config_(decoder_setup(settings, layout).ac4) {
    renderer_.set_joc_domain(config_.joc_domain);
}

void StreamDecoder::reset() {
    ac3_decoder_.reset();
    eac3_decoder_.reset();
    if (ac4_decoder_) {
        ac4_decoder_->reset();
    }
    ac4_speakers_.clear();
    programme_.reset();
    beds_.clear();
    renderer_bed_.reset();
    dual_mono_ = false;
    renderer_ = render::LayoutRenderer{layout_, sample_rate_, crossover_hz_};
    // A fresh LayoutRenderer starts at kQmf's own lag (render.hpp) whatever
    // config_ says, so a seek (what reset() is for) needs this reapplied or
    // the LFE would fall out of step with the objects beside it from the
    // seek point on - the same reason crossover_hz_ above has to be handed
    // back in rather than left to the class's own default.
    renderer_.set_joc_domain(config_.joc_domain);
}

bool StreamDecoder::set_crossover_hz(double hz) {
    if (!renderer_.set_crossover_hz(hz)) {
        return false;
    }
    crossover_hz_ = hz;
    return true;
}

void StreamDecoder::finish_report(UnitReport& out, std::optional<std::size_t> unit_bytes) {
    out.sequence = ++sequence_;
    if (!unit_bytes) {
        // render_flushed()'s own final unit: no raw bytes survive flush() to
        // measure a bitrate from, only the already-decoded PCM.
        out.bitrate_kbps.reset();
        return;
    }
    const double seconds =
        static_cast<double>(out.blocks) * kSamplesPerBlock / static_cast<double>(sample_rate_);
    out.bitrate_kbps = seconds > 0.0
                            ? std::optional<double>(static_cast<double>(*unit_bytes) * 8.0 / 1000.0 / seconds)
                            : std::nullopt;
}

std::expected<std::size_t, std::string> StreamDecoder::decode(std::span<const std::byte> whole,
                                                              const BlockFn& deliver,
                                                              const UnitFn& reported,
                                                              std::uint32_t unit_samples) {
    delivered_ = 0;
    if (starts_ac4(whole)) {
        return decode_ac4(whole, deliver, reported, unit_samples);
    }
    const auto header = io::read_frame_header(whole);
    if (!header) {
        reset();
        return std::unexpected(std::string{"A frame header could not be read."});
    }
    // A unit's first syncframe is its independent substream (or its AC-3
    // core); the dependents follow it.
    const std::span<const std::byte> unit =
        substreams_ == Substreams::kIndependent && header->bytes < whole.size()
            ? whole.first(header->bytes)
            : whole;
    if (header->kind == io::StreamKind::kEac3) {
        // One programme: the first unit's. A unit starts at an independent
        // substream, so this is that programme's id, not a dependent's.
        if (!programme_) {
            programme_ = header->substreamid;
        } else if (header->substreamid != *programme_) {
            return std::size_t{0};
        }
    }
    const std::optional<eac3::chanmap::Layout> bed = peek_layout(unit);
    if (!bed) {
        reset();
        return std::unexpected(std::string{"A unit's channel layout could not be read."});
    }
    beds_.push_back(UnitBed{.layout = *bed, .dual_mono = header->acmod == Acmod::kDualMono});
    const auto sink = [this, &deliver](const PcmBlock& block) { place(block, deliver); };

    // A unit that is one AC-3 syncframe goes to the AC-3 decoder, which a fold
    // applies to as well; anything else, an AC-3 core with E-AC-3 dependents
    // included, to the E-AC-3 decoder.
    if (header->kind == io::StreamKind::kAc3 && header->bytes == unit.size()) {
        if (!ac3_decoder_) {
            ac3_decoder_.emplace(config_);
        }
        const auto decoded = ac3_decoder_->decode_frame_by_block(unit, sink);
        if (!decoded) {
            reset();
            return std::unexpected(
                fmt::format("An AC-3 frame could not be decoded: {}.", describe(decoded.error())));
        }
        if (reported) {
            report_frame(*decoded, report_);
            finish_report(report_, unit.size());
            reported(report_);
        }
        return delivered_;
    }
    if (!eac3_decoder_) {
        eac3_decoder_.emplace(config_);
    }
    const auto decoded = eac3_decoder_->decode_access_unit_by_block(unit, sink);
    if (!decoded) {
        reset();
        return std::unexpected(fmt::format("An E-AC-3 access unit could not be decoded: {}.",
                                           describe(decoded.error())));
    }
    // Nothing comes back for a unit held back; what does is the unit whose
    // blocks this call delivered.
    if (*decoded && reported) {
        report_unit(**decoded, report_);
        finish_report(report_, unit.size());
        reported(report_);
    }
    return delivered_;
}

void StreamDecoder::place(const PcmBlock& block, const BlockFn& deliver) {
    const std::size_t slots = layout_.slots();
    std::array<std::span<float>, render::OutputLayout::kMaxSlots> spans{};
    for (std::size_t slot = 0; slot < slots; ++slot) {
        spans[slot] = std::span<float>(block_[slot]);
    }
    const std::span<const std::span<float>> out(spans.data(), slots);
    if (block.index == 0 && !beds_.empty()) {
        // A unit's first block: the bed of the oldest unit not yet placed.
        const UnitBed bed = beds_.front();
        beds_.pop_front();
        dual_mono_ = bed.dual_mono;
        if (!serving_.fold && (!renderer_bed_ || !same_layout(*renderer_bed_, bed.layout))) {
            renderer_.set_bed(bed.layout);
            renderer_bed_ = bed.layout;
        }
    }

    // Dual mono: channel 1 and channel 2 are two programmes, and the one not
    // chosen is replaced by the one that is, so it plays from both sides. An
    // LFE after them, which 1+1 may carry, is left where it is.
    PcmBlock chosen = block;
    std::array<std::span<const float>, eac3::chanmap::kMaxChannels> channels{};
    if (dual_mono_ && settings_.dual_mono != DualMonoChoice::kBoth &&
        block.channels.size() >= 2 && block.channels.size() <= channels.size()) {
        std::copy(block.channels.begin(), block.channels.end(), channels.begin());
        const std::span<const float> heard =
            block.channels[settings_.dual_mono == DualMonoChoice::kFirst ? 0 : 1];
        channels[0] = heard;
        channels[1] = heard;
        chosen.channels = std::span<const std::span<const float>>(channels.data(),
                                                                  block.channels.size());
    }

    if (serving_.fold) {
        renderer_.render_folded(chosen, 1.0F, out);
    } else {
        if (serving_.reconstruct && chosen.index == 0) {
            renderer_.set_objects(chosen.object_metadata, chosen.objects.size());
        }
        renderer_.render(chosen, serving_.reconstruct, 1.0F, out);
    }
    const std::size_t frames =
        block.channels.empty() ? 0 : std::min(block.channels.front().size(), block_[0].size());
    std::array<std::span<const float>, render::OutputLayout::kMaxSlots> rendered{};
    for (std::size_t slot = 0; slot < slots; ++slot) {
        rendered[slot] = std::span<const float>(block_[slot].data(), frames);
    }
    delivered_ += frames;
    deliver(std::span<const std::span<const float>>(rendered.data(), slots), frames);
}

std::size_t StreamDecoder::finish(const BlockFn& deliver, const UnitFn& reported) {
    std::size_t frames = 0;
    if (eac3_decoder_) {
        std::vector<DecodedSubstream> released = eac3_decoder_->flush();
        frames = render_flushed(released, deliver, reported);
    }
    if (ac4_decoder_) {
        // The last frame was reported when it decoded; what comes out now is
        // the rest of it, held back short of a whole block.
        const std::size_t before = delivered_;
        flush_ac4(deliver);
        frames += delivered_ - before;
    }
    reset();
    return frames;
}

bool StreamDecoder::apply(const DecoderSettings& settings) {
    if (ac3_decoder_ || eac3_decoder_) {
        return false;
    }
    const DecoderSetup setup = decoder_setup(settings, layout_);
    // The concealment policy is fixed for an AC-4 decoder, which only its
    // output and presentation change in place.
    if (ac4_decoder_ && setup.ac4.concealment != ac4_config_.concealment) {
        return false;
    }
    settings_ = settings;
    serving_ = setup.serving;
    config_ = setup.config;
    renderer_.set_joc_domain(config_.joc_domain);
    ac4_config_ = setup.ac4;
    if (ac4_decoder_) {
        ac4_decoder_->set_output(ac4_config_.output);
        ac4_decoder_->set_presentation(ac4_config_.presentation);
    }
    return true;
}

std::expected<std::size_t, std::string> StreamDecoder::decode_ac4(std::span<const std::byte> unit,
                                                                  const BlockFn& deliver,
                                                                  const UnitFn& reported,
                                                                  std::uint32_t unit_samples) {
    const std::span<const std::byte> raw = raw_frame_of(unit);
    if (raw.empty()) {
        flush_ac4(deliver);
        return std::unexpected(std::string{"An AC-4 unit is not one whole sync frame."});
    }
    if (!ac4_decoder_) {
        ac4_decoder_.emplace(ac4_config_);
    }
    const auto sink = [this, &deliver](const ac4::PcmBlock& block) { place_ac4(block, deliver); };
    const auto decoded = ac4_decoder_->decode_by_block(raw, sink);
    if (!decoded) {
        // No concealment policy covered it. What the decoder holds is from
        // the frames before, and goes out first, so that what has come out
        // is everything before this frame.
        flush_ac4(deliver);
        return std::unexpected(
            fmt::format("An AC-4 frame could not be decoded: {}.", ac4_decoder_->refusal_reason()));
    }
    if (!*decoded) {
        // Waiting for an I-frame: the frame's time passes in silence.
        flush_ac4(deliver);
        deliver_silence(unit_samples, deliver);
        return delivered_;
    }
    report_ac4(**decoded, unit.size(), reported);
    return delivered_;
}

void StreamDecoder::flush_ac4(const BlockFn& deliver) {
    if (!ac4_decoder_) {
        return;
    }
    const auto sink = [this, &deliver](const ac4::PcmBlock& block) { place_ac4(block, deliver); };
    (void)ac4_decoder_->flush(sink);
}

void StreamDecoder::deliver_silence(std::size_t frames, const BlockFn& deliver) {
    const std::size_t slots = layout_.slots();
    std::array<std::span<const float>, render::OutputLayout::kMaxSlots> spans{};
    while (frames > 0) {
        const std::size_t n = std::min(frames, zeros_.size());
        for (std::size_t slot = 0; slot < slots; ++slot) {
            spans[slot] = std::span<const float>(zeros_).first(n);
        }
        delivered_ += n;
        deliver(std::span<const std::span<const float>>(spans.data(), slots), n);
        frames -= n;
    }
}

void StreamDecoder::place_ac4(const ac4::PcmBlock& block, const BlockFn& deliver) {
    const std::size_t slots = layout_.slots();
    std::array<std::span<float>, render::OutputLayout::kMaxSlots> spans{};
    for (std::size_t slot = 0; slot < slots; ++slot) {
        spans[slot] = std::span<float>(block_[slot]);
    }
    const std::span<const std::span<float>> out(spans.data(), slots);
    // A layout that folds takes the decoder's own downmix as it comes; a
    // wider one places the channels by their speakers, as a bed.
    if (!serving_.fold && !std::ranges::equal(ac4_speakers_, block.speakers)) {
        renderer_.set_bed(ac4_bed(block.speakers));
        ac4_speakers_.assign(block.speakers.begin(), block.speakers.end());
        // The E-AC-3 path's own record of the bed no longer describes it.
        renderer_bed_.reset();
    }
    const PcmBlock placed{.index = 0,
                          .blocks = 1,
                          .channels = block.channels,
                          .objects = {},
                          .object_indices = {},
                          .object_metadata = nullptr};
    if (serving_.fold) {
        renderer_.render_folded(placed, 1.0F, out);
    } else {
        renderer_.render(placed, false, 1.0F, out);
    }
    const std::size_t frames = std::min(block.samples, block_[0].size());
    std::array<std::span<const float>, render::OutputLayout::kMaxSlots> rendered{};
    for (std::size_t slot = 0; slot < slots; ++slot) {
        rendered[slot] = std::span<const float>(block_[slot].data(), frames);
    }
    delivered_ += frames;
    deliver(std::span<const std::span<const float>>(rendered.data(), slots), frames);
}

void StreamDecoder::report_ac4(const ac4::FrameInfo& info, std::size_t unit_bytes,
                               const UnitFn& reported) {
    if (!reported) {
        return;
    }
    UnitReport& out = report_;
    out.acmod = ac4_acmod(info.speakers);
    out.lfe = std::ranges::find(info.speakers, ac4::Speaker::kLfe) != info.speakers.end();
    const std::span<const ac4::PresentationInfo> presentations = ac4_decoder_->presentations();
    out.substreams = info.presentation < presentations.size()
                         ? static_cast<int>(std::max<std::size_t>(
                               presentations[info.presentation].members.size(), 1))
                         : 1;
    out.layout = ac4_bed(info.speakers);
    out.bsmod.reset();
    out.dialnorm = 31;
    out.dialnorm2.reset();
    out.compr.reset();
    out.compr2.reset();
    out.dynrng = {};
    out.blocks = 0;
    out.short_blocks.reset();
    out.levels = MixLevels{};
    out.concealed =
        info.concealed ? std::optional<Concealment>{concealment_of(*info.concealed)} : std::nullopt;
    out.objects.reset();
    const ac4::PresentationMetadata& metadata = ac4_decoder_->metadata();
    out.ac4 = Ac4UnitReport{.presentation = info.presentation,
                            .presentation_id = info.presentation_id,
                            .dialnorm_dbfs = metadata.loudness.dialnorm_dbfs,
                            .drc_mode = metadata.drc ? metadata.drc->applied_mode : std::nullopt,
                            .samples = info.samples,
                            .latency_samples = ac4_decoder_->latency_samples()};
    out.sequence = ++sequence_;
    const double seconds = info.sample_rate_hz > 0 ? static_cast<double>(info.samples) /
                                                         static_cast<double>(info.sample_rate_hz)
                                                   : 0.0;
    out.bitrate_kbps =
        seconds > 0.0
            ? std::optional<double>(static_cast<double>(unit_bytes) * 8.0 / 1000.0 / seconds)
            : std::nullopt;
    reported(out);
}

std::size_t StreamDecoder::render_flushed(std::span<DecodedSubstream> substreams,
                                          const BlockFn& deliver, const UnitFn& reported) {
    // This programme's independent substream, and every dependent released
    // with it. flush() makes no promise about their order, and a dependent's
    // substreamid numbers the dependents rather than naming the programme, so
    // the dependents cannot be matched to a programme here; a multi-programme
    // stream that ends with two programmes' dependents both held back is the
    // one case this lays over the wrong bed, and it is a case the test sink
    // never meets either.
    const DecodedSubstream* independent = nullptr;
    std::vector<const DecodedSubstream*> dependents;
    for (const DecodedSubstream& substream : substreams) {
        if (substream.strmtyp == eac3::StreamType::kDependent) {
            dependents.push_back(&substream);
        } else if (independent == nullptr &&
                   (!programme_ || substream.substreamid == *programme_)) {
            independent = &substream;
        }
    }
    if (independent == nullptr || independent->channels.empty()) {
        // Nothing of this programme was held back; the bed queued for a unit
        // that never came out has nothing to place.
        return 0;
    }

    // Assembled as the decoder assembles a programme (§E3.8.2): the
    // independent substream's bed, with each dependent's channels laid over
    // it, in Table E2.5 location order.
    std::uint16_t map = independent->location_map();
    std::size_t length = independent->channels.front().size();
    for (const DecodedSubstream* dependent : dependents) {
        map = static_cast<std::uint16_t>(map | dependent->location_map());
        if (!dependent->channels.empty()) {
            length = std::max(length, dependent->channels.front().size());
        }
    }
    const eac3::chanmap::Layout layout = eac3::chanmap::expand(map);
    std::vector<std::vector<float>> channels(static_cast<std::size_t>(layout.count),
                                             std::vector<float>(length, 0.0F));
    const auto lay_over = [&](const DecodedSubstream& substream) {
        const eac3::chanmap::Layout own = eac3::chanmap::expand(substream.location_map());
        for (int i = 0; i < own.count && static_cast<std::size_t>(i) < substream.channels.size();
             ++i) {
            const int at = layout.index_of(own[i]);
            if (at < 0) {
                continue;
            }
            const std::vector<float>& source = substream.channels[static_cast<std::size_t>(i)];
            std::copy_n(source.begin(), std::min(length, source.size()),
                        channels[static_cast<std::size_t>(at)].begin());
        }
    };
    lay_over(*independent);
    for (const DecodedSubstream* dependent : dependents) {
        lay_over(*dependent);
    }

    // The output stage the decoder would have run on the assembled
    // programme: dialnorm for line mode, and the fold when there is one. A
    // fresh stage - see the header on what that costs.
    std::vector<std::span<float>> stage_spans;
    stage_spans.reserve(channels.size());
    for (std::vector<float>& channel : channels) {
        stage_spans.emplace_back(channel);
    }
    OutputStage stage{config_.output};
    stage.apply(std::span<const std::span<float>>(stage_spans), layout, independent->acmod,
                independent->lfe, levels_of(*independent), independent->dialnorm,
                independent->dialnorm2);
    const std::size_t count =
        serving_.fold ? static_cast<std::size_t>(
                            output_channel_count(config_.output, independent->acmod, independent->lfe))
                      : channels.size();

    // Reconstructed objects travel with the independent substream when the
    // decoder produced them; the renderer places the bed alone otherwise.
    const bool objects = serving_.reconstruct && independent->object_metadata.has_value() &&
                         !independent->object_audio.empty();
    if (!serving_.fold && (!renderer_bed_ || !same_layout(*renderer_bed_, layout))) {
        renderer_.set_bed(layout);
        renderer_bed_ = layout;
    }
    // place() takes a bed off the queue for every unit's first block; this
    // unit's own bed is the one queued for it, and it has just been set
    // directly, so the queue is cleared rather than consulted.
    beds_.clear();
    dual_mono_ = independent->acmod == Acmod::kDualMono;

    const std::size_t blocks = length / kSamplesPerBlock;
    std::vector<std::span<const float>> channel_views(count);
    std::vector<std::span<const float>> object_views(objects ? independent->object_audio.size() : 0);
    std::size_t frames = 0;
    for (std::size_t b = 0; b < blocks; ++b) {
        const std::size_t offset = b * kSamplesPerBlock;
        for (std::size_t c = 0; c < count; ++c) {
            channel_views[c] = std::span<const float>(channels[c]).subspan(offset, kSamplesPerBlock);
        }
        for (std::size_t o = 0; o < object_views.size(); ++o) {
            const std::vector<float>& audio = independent->object_audio[o];
            object_views[o] = audio.size() >= offset + kSamplesPerBlock
                                  ? std::span<const float>(audio).subspan(offset, kSamplesPerBlock)
                                  : std::span<const float>{};
        }
        const PcmBlock block{
            .index = static_cast<int>(b),
            .blocks = static_cast<int>(blocks),
            .channels = std::span<const std::span<const float>>(channel_views),
            .objects = std::span<const std::span<const float>>(object_views),
            .object_indices = objects ? std::span<const int>(independent->object_indices)
                                      : std::span<const int>{},
            .object_metadata = objects ? &*independent->object_metadata : nullptr};
        const std::size_t before = delivered_;
        place(block, deliver);
        frames += delivered_ - before;
    }

    if (reported && frames > 0) {
        // The independent substream's own words, as an assembled unit
        // reports them; the objects from whichever substream carried them.
        report_.acmod = independent->acmod;
        report_.lfe = independent->lfe;
        report_.substreams = static_cast<int>(1 + dependents.size());
        report_.layout = independent->acmod == Acmod::kDualMono ? eac3::chanmap::Layout{} : layout;
        report_.bsmod = bsmod_of(independent->info);
        report_.dialnorm = independent->dialnorm;
        report_.dialnorm2 = independent->dialnorm2;
        report_.compr = independent->compr;
        report_.compr2.reset();
        report_.dynrng = independent->dynrng;
        report_.blocks = eac3::blocks_per_syncframe(independent->numblkscod);
        report_.short_blocks.reset();
        report_.levels = levels_of(*independent);
        report_.concealed = independent->concealed;
        report_.objects = independent->object_metadata;
        report_.ac4.reset();
        for (const DecodedSubstream* dependent : dependents) {
            if (!report_.objects && dependent->object_metadata) {
                report_.objects = dependent->object_metadata;
            }
        }
        finish_report(report_, std::nullopt);
        reported(report_);
    }
    return frames;
}

}  // namespace ac3::hearth

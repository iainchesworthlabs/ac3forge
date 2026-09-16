#include "stream_decoder.hpp"

#include <algorithm>
#include <fmt/format.h>
#include <utility>
#include <vector>

#include "ac3/decoder/output.hpp"
#include "ac3/io/elementary.hpp"

// See stream_decoder.hpp. The decode path is apps/hearth/testsink/
// burst_output.cpp's, which a player needs as much as a test sink does; the
// end-of-stream path is the part only a player needs.

namespace ac3::hearth {

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

// Line mode, and whatever fold and object policy the layout calls for: the
// same configuration the test sink decodes with, so the two render a stream
// alike.
[[nodiscard]] DecoderConfig configured(const render::Serving& serving) {
    DecoderConfig config;
    config.output.mode = OperatingMode::kLine;
    render::configure_decoder(serving, config);
    return config;
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

}  // namespace

StreamDecoder::StreamDecoder(const render::OutputLayout& layout, std::uint32_t sample_rate)
    : layout_(layout),
      sample_rate_(sample_rate),
      serving_(render::serve(layout, DownmixTarget::kLoRo, render::ObjectsPolicy::kAuto)),
      config_(configured(serving_)),
      renderer_(layout, sample_rate) {}

void StreamDecoder::reset() {
    ac3_decoder_.reset();
    eac3_decoder_.reset();
    programme_.reset();
    beds_.clear();
    renderer_bed_.reset();
    renderer_ = render::LayoutRenderer{layout_, sample_rate_};
}

std::expected<std::size_t, std::string> StreamDecoder::decode(std::span<const std::byte> unit,
                                                              const BlockFn& deliver) {
    delivered_ = 0;
    const auto header = io::read_frame_header(unit);
    if (!header) {
        reset();
        return std::unexpected(std::string{"A frame header could not be read."});
    }
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
    beds_.push_back(*bed);
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
        const eac3::chanmap::Layout bed = beds_.front();
        beds_.pop_front();
        if (!serving_.fold && (!renderer_bed_ || !same_layout(*renderer_bed_, bed))) {
            renderer_.set_bed(bed);
            renderer_bed_ = bed;
        }
    }
    if (serving_.fold) {
        renderer_.render_folded(block, 1.0F, out);
    } else {
        if (serving_.reconstruct && block.index == 0) {
            renderer_.set_objects(block.object_metadata, block.objects.size());
        }
        renderer_.render(block, serving_.reconstruct, 1.0F, out);
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

std::size_t StreamDecoder::finish(const BlockFn& deliver) {
    std::size_t frames = 0;
    if (eac3_decoder_) {
        std::vector<DecodedSubstream> released = eac3_decoder_->flush();
        frames = render_flushed(released, deliver);
    }
    reset();
    return frames;
}

std::size_t StreamDecoder::render_flushed(std::span<DecodedSubstream> substreams,
                                          const BlockFn& deliver) {
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
    return frames;
}

}  // namespace ac3::hearth

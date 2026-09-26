#include "burst_output.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/iec61937/iec61937.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/render/render.hpp"
#include "ac3/render/serving.hpp"
#include "ac3/sendspin/ac3forge_player.hpp"
#include "ac3/sendspin/chunks.hpp"
#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"

namespace ac3::hearth::testsink {

namespace {

namespace ac = sendspin::ac3forge;

// Samples in every burst (planning/hearth-sendspin-extension.md, Burst chunks).
constexpr std::uint64_t kSamplesPerBurst = 1536;

// The layout an access unit decodes to, from its headers alone: each syncframe's acmod and lfeon,
// and a dependent's chanmap where it carries one, unioned in Table E2.5 order, as the decoder
// assembles it (§E3.8.2). Needed before the decode, since the block form hands the samples over
// during the call.
[[nodiscard]] std::optional<eac3::chanmap::Layout> peek_layout(std::span<const std::byte> unit) {
    std::uint16_t map = 0;
    std::size_t offset = 0;
    while (offset < unit.size()) {
        const auto header = io::read_frame_header(unit.subspan(offset));
        if (!header || header->bytes == 0) {
            return std::nullopt;
        }
        const std::uint16_t own = eac3::chanmap::acmod_map(header->acmod, header->lfe);
        if (header->kind == io::StreamKind::kEac3 && header->strmtyp == eac3::StreamType::kDependent) {
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

// The access units a burst's payload holds, whole, in order: each starts at an independent
// substream or an AC-3 syncframe, and takes the dependents after it.
[[nodiscard]] std::optional<std::vector<std::span<const std::byte>>> access_units(std::span<const std::byte> payload) {
    std::vector<std::span<const std::byte>> units;
    std::size_t start = 0;
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const auto header = io::read_frame_header(payload.subspan(offset));
        if (!header || header->bytes == 0 || header->bytes > payload.size() - offset) {
            return std::nullopt;
        }
        const bool dependent = header->kind == io::StreamKind::kEac3 && header->strmtyp == eac3::StreamType::kDependent;
        if (!dependent && offset > start) {
            units.push_back(payload.subspan(start, offset - start));
            start = offset;
        }
        offset += header->bytes;
    }
    if (offset > start) {
        units.push_back(payload.subspan(start, offset - start));
    }
    return units;
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

[[nodiscard]] DecoderConfig configured(const render::Serving& serving) {
    DecoderConfig config;
    config.output.mode = OperatingMode::kLine;
    render::configure_decoder(serving, config);
    return config;
}

// The whole AC-4 sync frame at the start of a burst's payload, less HBR16's padding after it, and
// its raw frame; nothing when the payload does not start with one.
struct Ac4Payload {
    std::span<const std::byte> sync_frame;
    std::span<const std::byte> raw_ac4_frame;
};

[[nodiscard]] std::optional<Ac4Payload> ac4_payload(std::span<const std::byte> payload) {
    const ac4::ScanResult scanned = ac4::scan(payload);
    if (scanned.frames.empty() || scanned.frames.front().offset != 0) {
        return std::nullopt;
    }
    const ac4::SyncFrame& frame = scanned.frames.front();
    const auto end = static_cast<std::size_t>(frame.raw_ac4_frame.data() - payload.data()) +
                     frame.raw_ac4_frame.size() + (frame.sync_word == 0xAC41 ? 2U : 0U);
    if (end > payload.size()) {
        return std::nullopt;
    }
    return Ac4Payload{.sync_frame = payload.first(end), .raw_ac4_frame = frame.raw_ac4_frame};
}

// The samples an AC-4 frame lasts at the base sampling frequency: its data-burst's repetition
// period (IEC 61937-14 Tables 5 and 6), the data-burst being sequence_counter mod 5, which is the
// packer's choice for every frame but the first after a splice. Nothing for a frame whose rate the
// tables have no row for.
[[nodiscard]] std::uint64_t ac4_frame_samples(std::span<const std::byte> sync_frame) {
    const std::optional<iec61937::Ac4SyncFrame> read = iec61937::read_ac4_sync_frame(sync_frame);
    if (!read) {
        return 0;
    }
    const std::optional<iec61937::Ac4BurstTiming> timing = iec61937::ac4_burst_timing(
        iec61937::BurstDataType::kAc4, read->fs_index, read->frame_rate_index);
    return timing ? timing->periods[static_cast<std::size_t>(read->sequence_counter % 5)] : 0;
}

// The A/52 audio coding mode with the decoded channels' front and surround speakers (Table 5.8):
// what DecoderReport::acmod says for AC-4, whose channel modes are all one of 1/0, 2/0, 3/0 or
// 3/2 there, with any back, wide or height pair left out.
[[nodiscard]] std::int32_t ac4_acmod(std::span<const ac4::Speaker> speakers) {
    const auto has = [&](ac4::Speaker speaker) {
        return std::find(speakers.begin(), speakers.end(), speaker) != speakers.end();
    };
    if (!has(ac4::Speaker::kLeft)) {
        return 1;
    }
    if (has(ac4::Speaker::kLeftSurround)) {
        return has(ac4::Speaker::kCentre) ? 7 : 6;
    }
    return has(ac4::Speaker::kCentre) ? 3 : 2;
}

}  // namespace

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
        }
        // clang-format on
        bed.items[static_cast<std::size_t>(bed.count++)] = location;
    }
    return bed;
}

BurstOutput::BurstOutput(std::filesystem::path directory, std::string prefix, const render::OutputLayout& layout)
    : directory_(std::move(directory)),
      prefix_(std::move(prefix)),
      layout_(layout),
      serving_(render::serve(layout, DownmixTarget::kLoRo, render::ObjectsPolicy::kAuto)),
      config_(configured(serving_)),
      renderer_(layout) {}

// As WavOutput's: writer_'s and log_'s own destructors close both files as end() does.
BurstOutput::~BurstOutput() = default;

bool BurstOutput::start(const ac::StreamStart& stream) {
    end();
    ++streams_;
    stream_frames_ = 0;
    stream_ = stream;
    reset_decoding();
    decoder_.reset();
    if (directory_.empty()) {
        return true;
    }
    file_ = directory_ / (prefix_ + "-" + std::to_string(streams_) + ".wav");
    if (!writer_.open(file_.string(), static_cast<std::uint32_t>(stream.sample_rate),
                      static_cast<std::uint16_t>(layout_.slots()))) {
        stream_.reset();
        return false;
    }
    log_.open(std::filesystem::path(file_).replace_extension(".times.csv"), std::ios::trunc);
    log_ << "local_time_us,first_frame,frames\n";
    return true;
}

void BurstOutput::clear() {
    if (log_.is_open()) {
        log_ << "clear," << stream_frames_ << "\n";
    }
    reset_decoding();
}

void BurstOutput::end() {
    writer_.close();
    if (log_.is_open()) {
        log_.close();
    }
    stream_.reset();
    decoder_.reset();
}

void BurstOutput::reset_decoding() {
    ac3_decoder_.reset();
    eac3_decoder_.reset();
    ac4_decoder_.reset();
    programme_.reset();
    beds_.clear();
    renderer_bed_.reset();
    renderer_ = render::LayoutRenderer{layout_};
}

void BurstOutput::write(const sendspin::BurstChunk& chunk, std::int64_t local_time) {
    ++bursts_;
    if (!stream_) {
        return;
    }
    if (stream_->data_type == ac::DataType::kAc4) {
        write_ac4(chunk, local_time);
        return;
    }
    if (log_.is_open()) {
        log_ << local_time << "," << stream_frames_ << "," << kSamplesPerBurst << "\n";
    }
    stream_frames_ += kSamplesPerBurst;
    const std::optional<std::vector<std::span<const std::byte>>> units = access_units(std::as_bytes(chunk.chunk.data));
    if (!units) {
        ++undecodable_;
        reset_decoding();
        return;
    }
    for (const std::span<const std::byte> unit : *units) {
        decode_unit(unit);
    }
}

void BurstOutput::decode_unit(std::span<const std::byte> unit) {
    const auto header = io::read_frame_header(unit);
    if (!header) {
        ++undecodable_;
        reset_decoding();
        return;
    }
    if (header->kind == io::StreamKind::kEac3) {
        // One programme: the first unit's.
        if (!programme_) {
            programme_ = header->substreamid;
        } else if (header->substreamid != *programme_) {
            return;
        }
    }
    const std::optional<eac3::chanmap::Layout> bed = peek_layout(unit);
    if (!bed) {
        ++undecodable_;
        reset_decoding();
        return;
    }
    beds_.push_back(*bed);
    const auto deliver = [this](const PcmBlock& block) { place(block); };

    // A unit that is one AC-3 syncframe goes to the AC-3 decoder, which a fold applies to as
    // well; anything else, an AC-3 core with E-AC-3 dependents included, to the E-AC-3 decoder.
    if (header->kind == io::StreamKind::kAc3 && header->bytes == unit.size()) {
        if (!ac3_decoder_) {
            ac3_decoder_.emplace(config_);
        }
        const std::expected<DecodedFrame, DecodeError> decoded = ac3_decoder_->decode_frame_by_block(unit, deliver);
        if (!decoded) {
            ++undecodable_;
            reset_decoding();
            return;
        }
        if (!decoder_) {
            decoder_ = ac::DecoderReport{.data_type = stream_->data_type,
                                         .acmod = static_cast<std::int32_t>(decoded->acmod),
                                         .lfe = decoded->lfe,
                                         .substreams = 1,
                                         .objects = 0,
                                         .objects_placed = false,
                                         .dialnorm = -static_cast<double>(decoded->dialnorm)};
        }
        return;
    }
    if (!eac3_decoder_) {
        eac3_decoder_.emplace(config_);
    }
    const std::expected<std::optional<DecodedAccessUnit>, DecodeError> decoded =
        eac3_decoder_->decode_access_unit_by_block(unit, deliver);
    if (!decoded) {
        ++undecodable_;
        reset_decoding();
        return;
    }
    // The report comes from the first unit that decodes, and again from the first to carry objects
    // if that one did not: one per JOC output, as describe_objects counts them.
    if (!*decoded || (decoder_ && (decoder_->objects > 0 || !(*decoded)->object_metadata))) {
        return;
    }
    const DecodedAccessUnit& au = **decoded;
    const std::int32_t objects =
        au.object_metadata ? static_cast<std::int32_t>(oba::describe_objects(*au.object_metadata).size()) : 0;
    decoder_ = ac::DecoderReport{.data_type = stream_->data_type,
                                 .acmod = static_cast<std::int32_t>(au.acmod),
                                 .lfe = au.layout.index_of(eac3::chanmap::Location::kLfe) >= 0,
                                 .substreams = au.substream_count,
                                 .objects = objects,
                                 .objects_placed = objects > 0 && serving_.reconstruct,
                                 .dialnorm = -static_cast<double>(au.dialnorm)};
}

void BurstOutput::place(const PcmBlock& block) {
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
    emit(block.channels.empty() ? 0 : std::min(block.channels.front().size(), block_[0].size()));
}

void BurstOutput::emit(std::size_t n) {
    const std::size_t slots = layout_.slots();
    frames_ += n;
    if (!writer_.is_open()) {
        return;
    }
    interleaved_.resize(n * slots);
    for (std::size_t t = 0; t < n; ++t) {
        for (std::size_t slot = 0; slot < slots; ++slot) {
            interleaved_[(t * slots) + slot] = block_[slot][t];
        }
    }
    (void)writer_.write(interleaved_);
}

void BurstOutput::write_ac4(const sendspin::BurstChunk& chunk, std::int64_t local_time) {
    const std::optional<Ac4Payload> payload = ac4_payload(std::as_bytes(chunk.chunk.data));
    const std::uint64_t samples = payload ? ac4_frame_samples(payload->sync_frame) : 0;
    if (log_.is_open()) {
        log_ << local_time << "," << stream_frames_ << "," << samples << "\n";
    }
    stream_frames_ += samples;
    if (!payload) {
        ++undecodable_;
        reset_decoding();
        return;
    }
    if (!ac4_decoder_) {
        ac4_decoder_.emplace();
    }
    const std::expected<std::optional<ac4::DecodedFrame>, ac4::DecodeError> decoded =
        ac4_decoder_->decode(payload->raw_ac4_frame);
    if (!decoded) {
        ++undecodable_;
        reset_decoding();
        return;
    }
    // Nothing yet: a frame that needs configuration no I-frame has sent.
    if (*decoded) {
        render_ac4(**decoded);
    }
}

void BurstOutput::render_ac4(const ac4::DecodedFrame& frame) {
    const eac3::chanmap::Layout bed = ac4_bed(frame.speakers);
    if (!renderer_bed_ || !same_layout(*renderer_bed_, bed)) {
        renderer_.set_bed(bed);
        renderer_bed_ = bed;
    }
    const std::size_t slots = layout_.slots();
    std::array<std::span<float>, render::OutputLayout::kMaxSlots> spans{};
    for (std::size_t slot = 0; slot < slots; ++slot) {
        spans[slot] = std::span<float>(block_[slot]);
    }
    const std::span<const std::span<float>> out(spans.data(), slots);
    const std::size_t n = frame.channels.empty() ? 0 : frame.channels.front().size();
    ac4_block_.resize(frame.channels.size());
    const auto blocks = static_cast<int>((n + kSamplesPerBlock - 1) / kSamplesPerBlock);
    for (std::size_t at = 0; at < n; at += kSamplesPerBlock) {
        const std::size_t m = std::min<std::size_t>(kSamplesPerBlock, n - at);
        for (std::size_t channel = 0; channel < frame.channels.size(); ++channel) {
            ac4_block_[channel] = std::span<const float>(frame.channels[channel]).subspan(at, m);
        }
        const PcmBlock block{.index = static_cast<int>(at / kSamplesPerBlock),
                             .blocks = blocks,
                             .channels = ac4_block_,
                             .objects = {},
                             .object_indices = {},
                             .object_metadata = nullptr};
        renderer_.render(block, false, 1.0F, out);
        emit(m);
    }
    if (!decoder_) {
        decoder_ = ac::DecoderReport{.data_type = ac::DataType::kAc4,
                                     .acmod = ac4_acmod(frame.speakers),
                                     .lfe = std::find(frame.speakers.begin(), frame.speakers.end(),
                                                      ac4::Speaker::kLfe) != frame.speakers.end(),
                                     .substreams = 1,
                                     .objects = 0,
                                     .objects_placed = false,
                                     .dialnorm = 0.0};
    }
}

}  // namespace ac3::hearth::testsink

#include "ac4_stream.hpp"

#include <fmt/format.h>

#include <utility>

// See ac4_stream.hpp.

namespace ac3::hearth {

namespace {

// Part 2 clause 5.11's cycle.
constexpr int kPhases = 5;

}  // namespace

bool starts_ac4(std::span<const std::byte> bytes) {
    return bytes.size() >= 2 && bytes[0] == std::byte{0xAC} &&
           (bytes[1] == std::byte{0x40} || bytes[1] == std::byte{0x41});
}

std::uint32_t ac4_frame_samples(const ac4::Toc& toc, int phase) {
    const std::optional<ac4::MediaTiming> timing = ac4::media_timing(toc);
    if (!timing || timing->timescale == 0 || toc.sample_rate_hz <= 0) {
        return 0;
    }
    // A frame is sample_delta / timescale seconds; `span` is its length in
    // output samples times the time scale, so each count below is whole.
    const std::uint64_t span = static_cast<std::uint64_t>(timing->sample_delta) *
                               static_cast<std::uint64_t>(toc.sample_rate_hz);
    const auto at = static_cast<std::uint64_t>(phase % kPhases);
    const std::uint64_t scale = timing->timescale;
    return static_cast<std::uint32_t>((((at + 1) * span) / scale) - ((at * span) / scale));
}

std::span<const std::byte> raw_frame_of(std::span<const std::byte> sync_frame) {
    const ac4::ScanResult scanned = ac4::scan(sync_frame);
    if (scanned.frames.size() != 1 || scanned.frames.front().offset != 0) {
        return {};
    }
    return scanned.frames.front().raw_ac4_frame;
}

std::expected<Ac4Units, std::string> read_ac4_units(std::span<const std::byte> bytes) {
    const ac4::ScanResult scanned = ac4::scan(bytes);
    if (scanned.frames.empty()) {
        return std::unexpected(scanned.stopped_at
                                   ? fmt::format("No AC-4 sync frame could be read: {}.",
                                                 ac4::describe(*scanned.stopped_at))
                                   : std::string{"It holds no AC-4 sync frame."});
    }
    Ac4Units units;
    const std::size_t count = scanned.frames.size();
    units.frames.reserve(count);
    units.samples.reserve(count);
    units.iframes.assign(count, false);
    std::optional<int> phase;
    std::optional<ac4::Toc> last_toc;
    for (std::size_t i = 0; i < count; ++i) {
        const ac4::SyncFrame& frame = scanned.frames[i];
        // The whole frame: its raw_ac4_frame, and the CRC word after it.
        const auto raw_end = static_cast<std::size_t>(frame.raw_ac4_frame.data() - bytes.data()) +
                             frame.raw_ac4_frame.size();
        const std::size_t end = raw_end + (frame.sync_word == 0xAC41 ? 2U : 0U);
        units.frames.push_back(bytes.subspan(frame.offset, end - frame.offset));

        const std::expected<ac4::RawFrame, ac4::Error> parsed =
            ac4::parse_raw_frame(frame.raw_ac4_frame);
        if (!parsed) {
            // The frame the stream expected, as the decoder takes it.
            ++units.unread;
            phase = phase ? (*phase + 1) % kPhases : 0;
            units.samples.push_back(last_toc ? ac4_frame_samples(*last_toc, *phase) : 0);
            continue;
        }
        const ac4::Toc& toc = parsed->toc;
        const int counter = toc.sequence_counter;
        phase = counter != 0 ? counter % kPhases : (phase ? (*phase + 1) % kPhases : 0);
        const std::uint32_t samples = ac4_frame_samples(toc, *phase);
        if (samples == 0) {
            return std::unexpected(fmt::format(
                "Frame {} has a frame rate this decoder does not define (frame_rate_index "
                "{} at {} Hz).",
                i, toc.frame_rate_index, toc.sample_rate_hz));
        }
        if (units.sample_rate == 0) {
            units.sample_rate = static_cast<std::uint32_t>(toc.sample_rate_hz);
            units.first = *parsed;
        }
        units.samples.push_back(samples);
        units.iframes[i] = toc.b_iframe_global;
        last_toc = toc;
    }
    if (units.sample_rate == 0) {
        return std::unexpected(std::string{"No AC-4 frame's table of contents could be read."});
    }
    // Frames before the first that read take its length.
    for (std::size_t i = 0; i < count && units.samples[i] == 0; ++i) {
        units.samples[i] = ac4_frame_samples(units.first.toc, 0);
    }
    return units;
}

}  // namespace ac3::hearth

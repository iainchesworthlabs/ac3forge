#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/latency.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/mixing.hpp"
#include "decoder_settings.hpp"
#include "stream_decoder.hpp"

// The streaming transcode to AC-3 (planning/hearth-reference-player.md, A3;
// the appliance plan's gap 4): an E-AC-3 item for a receiver that takes AC-3
// but not E-AC-3.
//
// The player decodes the item for it onto a fixed 5.1 layout with neutral
// settings (transcode_settings()), whose slots are already in the order an
// AC-3 encoder takes them - L C R Ls Rs, then the LFE. This gathers those
// blocks into six-block frames and encodes each as 3/2 with LFE at 448 kbit/s,
// the rate `ac3cli transcode` uses.
//
// What the source says about itself goes with it, frame by frame, from the
// reports of the units the frame was encoded from:
//   * dialnorm, without which a levelled system plays the programme up to
//     30 dB too loud, and the service (bsmod), where it means the same for 3/2
//     - a voice-over's code is karaoke's there. Both are the unit's whose
//     samples sit mid-way through what the frame decodes to, so at a join a
//     frame takes the item that fills most of it; a service stays said for the
//     rest of its item and no further.
//   * the compr word. Every frame has one, so a receiver in RF mode is kept
//     from overload whatever the source sent. Where the units the frame
//     reaches all sent one, the frame has the most attenuating of them: a
//     decoder crossfades one frame's gain into the next, so the frame's word
//     governs some of the unit before too. Where any sent none, the frame's
//     own word, from a heavy compressor that meters the frame the way the
//     encoder does against the frame's own dialnorm, counts as well. (The
//     command line writes no compr for a source with none.)
//   * for dual mono heard as its second channel, that channel's dialnorm and
//     compr.
// The fold levels an encoder writes are fixed for it, so they are an
// argument (fold_levels() reads an item's), and an item that folds at other
// levels does not join. dynrng is not carried, as the command line's
// transcode does not carry it: nothing can write it into a frame after the
// encoder.
//
// The encoder's output runs 256 samples behind its input (kDelay): the first
// frame a receiver decodes starts with the transform's own 256 samples, and
// the last 256 samples taken come out only in the frame after. finish() pads
// the last frame by holding each channel's last sample - a drop to silence
// would be a transient the encoder spends a block switch on - and encodes
// until every sample taken has come out.

namespace ac3::hearth {

class Ac3Transcoder {
public:
    // What the link carries: 3/2 with LFE, at the most AC-3 allows for it.
    static constexpr std::uint32_t kBitrateKbps = 448;
    static constexpr std::size_t kChannels = 6;
    static constexpr std::uint64_t kDelay = kTransformDelaySamples;

    // The two downmix levels AC-3's bsi carries.
    struct FoldLevels {
        meta::CentreMixLevel centre = meta::CentreMixLevel::kMinus4_5dB;
        meta::SurroundMixLevel surround = meta::SurroundMixLevel::kMinus6dB;

        friend bool operator==(const FoldLevels&, const FoldLevels&) = default;
    };

    // Samples of one history record, in the order they were taken.
    struct Span {
        std::size_t record = 0;
        std::uint64_t frames = 0;
    };

    // One AC-3 syncframe, and whose samples it was encoded from - the frame's
    // padding belongs to no one. Both are valid for the duration of the call.
    using FrameFn =
        std::function<void(std::span<const std::byte> frame, std::span<const Span> spans)>;

    // Whether AC-3 codes this rate at all: 48, 44.1 or 32 kHz.
    [[nodiscard]] static bool carries(std::uint32_t sample_rate);

    // The levels a transcode of the stream `unit` starts would write: its own
    // where it is AC-3, else its E-AC-3 Lo/Ro pair - or its Lt/Rt pair where
    // it prefers that fold - taken to the nearest AC-3 level, as `ac3cli
    // transcode` takes them. The defaults where it says nothing.
    [[nodiscard]] static FoldLevels fold_levels(std::span<const std::byte> unit);

    // `sample_rate` is one carries() allows.
    Ac3Transcoder(std::uint32_t sample_rate, FoldLevels fold);

    // One block of the 5.1 layout's six slots, belonging to `record`. Missing
    // slots are silent and extra ones ignored.
    void take(std::span<const std::span<const float>> slots, std::size_t frames,
              std::size_t record);
    // What the unit behind the last `frames` samples taken said about itself,
    // for `record`, with the listener's choice of a dual mono programme's
    // channels.
    void describe_source(const UnitReport& report, std::uint64_t frames, std::size_t record,
                         DualMonoChoice dual_mono = DualMonoChoice::kBoth);
    // Encodes every whole frame taken.
    [[nodiscard]] std::expected<void, std::string> encode_ready(const FrameFn& out);
    // Nothing more is coming: the last frame is padded, and frames follow
    // until every sample taken has come out. Leaves the transcoder as reset()
    // does.
    [[nodiscard]] std::expected<void, std::string> finish(const FrameFn& out);
    // Forgets what was taken and what the source said, and starts the
    // encoder again.
    void reset();

    // Samples taken and not yet in a frame.
    [[nodiscard]] std::uint64_t buffered() const { return taken_ - consumed_; }
    [[nodiscard]] std::uint32_t sample_rate() const { return sample_rate_; }
    [[nodiscard]] FoldLevels fold() const { return fold_; }

private:
    // What one unit said, from the sample taken its own samples start at.
    struct Said {
        std::uint64_t from = 0;
        std::size_t record = 0;
        int dialnorm = 31;
        std::optional<std::uint8_t> compr{};
        std::optional<int> bsmod{};
    };

    // Encodes one frame from the next `count` samples taken, padded to a
    // frame, and hands it on.
    [[nodiscard]] std::expected<void, std::string> encode_frame(std::size_t count,
                                                                const FrameFn& out);
    [[nodiscard]] std::expected<void, std::string> make_encoder();
    // The compr word for the frame starting at sample `start`, whose own
    // dialnorm is `dialnorm`, from the frame's samples.
    [[nodiscard]] std::uint8_t compr_for(std::uint64_t start, int dialnorm,
                                         std::span<const std::span<const float>> channels);
    // Drops what has been encoded from the front of the queue once it is the
    // larger part, so the queue stays about a frame long.
    void compact();

    std::uint32_t sample_rate_;
    FoldLevels fold_;
    std::unique_ptr<FrameEncoder> encoder_;
    std::optional<meta::HeavyCompressor> heavy_;
    // Per channel, the samples taken; the first `head_` have been encoded.
    std::array<std::vector<float>, kChannels> queue_{};
    std::size_t head_ = 0;
    // Whose the queued samples are, oldest first, and the spans of the frame
    // being handed on.
    std::vector<Span> spans_;
    std::vector<Span> frame_spans_;
    // What the units said, oldest first; kept until no frame still to come
    // reaches them.
    std::vector<Said> said_;
    // Since the last reset: samples taken, samples encoded from them, and
    // samples encoded including padding.
    std::uint64_t taken_ = 0;
    std::uint64_t consumed_ = 0;
    std::uint64_t encoded_ = 0;
    // Each channel's last sample taken, which padding repeats.
    std::array<float, kChannels> hold_{};
    std::array<std::array<float, kSamplesPerFrame>, kChannels> frame_{};
    // The last block of the frame before, per full-bandwidth channel: the
    // encoder's first block of a frame overlaps it, and so does the metering.
    std::array<std::array<float, kSamplesPerBlock>, kChannels - 1> history_{};
};

}  // namespace ac3::hearth

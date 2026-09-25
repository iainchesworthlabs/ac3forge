#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "ac4dec/decoder.hpp"
#include "pcm/aspx.hpp"

// Mixing a presentation's substreams (ETSI TS 103 190-1 V1.4.1 clause 6.2.16,
// ETSI TS 103 190-2 V1.3.1 clauses 4.8.3.17 to 4.8.4): in the QMF domain,
// after each substream's dialogue enhancement and before DRC, the dialogue and
// associated audio substreams are added into the channels of the main or
// music and effects substream, each channel scaled by its substream group's
// gain (Part 2 Table 70) and the gains the associated audio sets on the main
// audio (scale_main, scale_main_front and scale_main_centre); the dialogue by
// the listener's g_dialog, the associated audio by g_assoc; a mono substream
// panned by its angle (pan_dialog, pan_associated) with the law Part 1 Table
// 216 fixes three points of, and the others channel to channel. The sum is not
// divided by the number of substreams (src/ac4dec/ERRATA.md, "The mixer's
// sum").

namespace ac4::detail {

// The most substreams a presentation mixes into its main audio; the rest of a
// larger one are left out (ERRATA, "Where the substreams are mixed").
inline constexpr std::size_t kMaxMixMembers = 8;

// The most channels a layout here has in the horizontal ring a pan moves
// round.
inline constexpr std::size_t kMaxRing = 32;

// One dialogue or associated audio substream as it is mixed in.
struct MixMember {
    int key = 0;  // its SubstreamPcm's, among the MixSources
    // Linear: its group's gain, the listener's g_dialog or g_assoc, and a
    // version 0 presentation's levelling.
    double gain = 1.0;
    // The scale of its own channels before they are panned (Part 2 clause
    // 4.8.3.17's "Scale dialogue substream"): every channel, L and R, and C.
    double scale_all = 1.0;
    double scale_front = 1.0;
    double scale_centre = 1.0;
    // The angle its first and second channels are panned to, in degrees
    // clockwise from the front (Part 1 clause 4.3.12.4.9); unset takes a
    // channel to the one of its own name. A mono substream without one is
    // panned to 0 degrees.
    std::array<std::optional<double>, 2> pan{};
};

// One frame's mixing, held with the frame's other control data until its
// signal reaches the QMF domain.
struct MixValues {
    bool active = false;  // a presentation of more than one substream to mix
    // The main or music and effects audio: its group's gain, and with
    // associated audio scale_main (every channel), scale_main_front (L and R)
    // and scale_main_centre (C).
    double main_gain = 1.0;
    double scale_all = 1.0;
    double scale_front = 1.0;
    double scale_centre = 1.0;
    std::array<MixMember, kMaxMixMembers> members{};
    std::size_t count = 0;
};

// A substream's QMF-domain matrices as another substream's decode takes them:
// one per channel of `speakers`, and the same channels before its dialogue
// enhancement, DRC's side chain (the same matrices where the tool left them
// alone).
struct MixSource {
    int key = 0;
    std::span<const Speaker> speakers;
    std::span<std::vector<QmfValue>* const> matrices;
    std::span<std::vector<QmfValue>* const> side;
};

// The gain of each channel of `into` for a mono signal panned to `degrees`,
// written to `gains` (as long as `into`): the two channels of the horizontal
// ring either side of the angle share it linearly, as Part 1 Table 216's 0.5
// and 0.5 at 0 degrees between L and R has it; the channels sit where the pan
// clause puts L, C and R (330, 0 and 30 degrees clockwise) and Part 1 Table
// D.1 puts the others; the LFE and the top channels take none.
void pan_gains(double degrees, std::span<const Speaker> into, std::span<double> gains);

// The matrix that takes `member` into the main audio, row-major: one row per
// channel of `into`, one column per channel of `from`, written to `matrix`
// (into.size() x from.size() values); `gains` is scratch as long as `into`.
void member_matrix(const MixMember& member, std::span<const Speaker> from, std::span<const Speaker> into,
                   std::span<double> matrix, std::span<double> gains);

class MixStage {
   public:
    // Scales `matrices`, the main audio's channels named by `speakers`, and
    // adds each member from `sources` (matched by key; a member without a
    // source is left out) into them; the same into `side` where
    // `side_separate` says it is not `matrices` itself.
    void mix(const MixValues& values, std::span<const Speaker> speakers,
             std::span<std::vector<QmfValue>* const> matrices, std::span<std::vector<QmfValue>* const> side,
             bool side_separate, std::span<const MixSource> sources);

   private:
    std::vector<double> matrix_;  // one member's, kept to save an allocation a frame
    std::vector<double> gains_;
};

}  // namespace ac4::detail

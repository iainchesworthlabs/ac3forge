#pragma once

#include <cstdint>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/meta/mixing.hpp"

// Where a rendered Table E2.5 location seats once a programme with no §7.8
// fold of its own - a wide layout such as 7.1.4 or 5.1.2 - has to be reduced
// to the nearest acmod before §7.8 can fold it at all.
//
// This IS an extension beyond §7.8, and worth being plain about: §7.8 defines
// folds FROM the eight AC-3 acmods and says nothing about the wide layouts
// Annex E's chanmap can express - a 7.1.4 program has no §7.8 fold, because
// §7.8 predates anything that could code one. Reducing first is the least
// invented thing available: every extra location has an obvious §7.8 seat (a
// wide left is a left, a rear surround is a surround, a top front left is a
// left), and once it is in that seat the actual downmix is the spec's own,
// with the stream's own levels. The alternative - dropping the channels §7.8
// cannot name - would silently discard the whole height layer.
//
// -3 dB on every secondary contribution, so two locations sharing one seat
// sum to the power of one. A location already IN the reduced layout arrives
// at unity, which makes the reduction an exact identity for every plain
// acmod bed - the overwhelmingly common case, and the one that must not
// change by so much as a bit.
//
// Shared between ac3::OutputStage's rendered-layout fold (decoder/output.cpp,
// a decoder rendering a wide programme for a listener) and
// ac3::eac3::AccessUnitEncoder's whole-programme §7.7.2 peak measurement
// (encoder/eac3_frame.cpp, an encoder deciding what ceiling to promise for
// one) - one definition of which seat a location falls into is what keeps an
// encoder's promise and a decoder's protection describing the same fold.

namespace ac3::eac3::seat {

enum class Seat : std::uint8_t { kLeft, kCentre, kRight, kLeftSurround, kRightSurround, kLfe };

struct SeatMix {
    Seat first = Seat::kLeft;
    double first_gain = 1.0;
    // A second seat, for the locations with no side of their own: a mono
    // surround or a top surround belongs equally to both surrounds.
    bool has_second = false;
    Seat second = Seat::kRight;
    double second_gain = 0.0;
};

[[nodiscard]] constexpr SeatMix seat_of(chanmap::Location location) {
    using L = chanmap::Location;
    constexpr double kHalfPower = meta::level::kMinus3dB;
    switch (location) {
        case L::kLeft: return {.first = Seat::kLeft};
        case L::kCentre: return {.first = Seat::kCentre};
        case L::kRight: return {.first = Seat::kRight};
        case L::kLeftSurround: return {.first = Seat::kLeftSurround};
        case L::kRightSurround: return {.first = Seat::kRightSurround};
        case L::kLfe: return {.first = Seat::kLfe};
        // Front pairs inside the mains, the wides and the front heights: all
        // left or right, at the shared-seat level.
        case L::kLc:
        case L::kLw:
        case L::kVhl: return {.first = Seat::kLeft, .first_gain = kHalfPower};
        case L::kRc:
        case L::kRw:
        case L::kVhr: return {.first = Seat::kRight, .first_gain = kHalfPower};
        case L::kVhc: return {.first = Seat::kCentre, .first_gain = kHalfPower};
        // Rear, side and top surrounds keep their side.
        case L::kLrs:
        case L::kLsd:
        case L::kLts: return {.first = Seat::kLeftSurround, .first_gain = kHalfPower};
        case L::kRrs:
        case L::kRsd:
        case L::kRts: return {.first = Seat::kRightSurround, .first_gain = kHalfPower};
        // A mono surround and a top (overhead centre) surround have no side,
        // so they go to both - which for the 2/1 and 3/1 beds reproduces
        // §7.8's own single-surround branch exactly, since slev then reaches
        // each front through this -3 dB rather than through the branch's own.
        case L::kCs:
        case L::kTs:
            return {.first = Seat::kLeftSurround,
                    .first_gain = kHalfPower,
                    .has_second = true,
                    .second = Seat::kRightSurround,
                    .second_gain = kHalfPower};
        // §E2.3.1.8's second LFE joins the first.
        case L::kLfe2: return {.first = Seat::kLfe, .first_gain = kHalfPower};
    }
    return {.first = Seat::kLeft, .first_gain = 0.0};
}

// Which acmod the occupied seats amount to, so §7.8's own coefficients and
// normalisation apply to the layout that is actually there rather than to a
// 3/2 with silent channels in it.
[[nodiscard]] constexpr Acmod reduced_acmod(bool centre, bool mains, bool surrounds) {
    if (!mains) {
        return Acmod::k1_0;  // a centre-only program; nothing else can reach here
    }
    if (centre) {
        return surrounds ? Acmod::k3_2 : Acmod::k3_0;
    }
    return surrounds ? Acmod::k2_2 : Acmod::k2_0;
}

}  // namespace ac3::eac3::seat

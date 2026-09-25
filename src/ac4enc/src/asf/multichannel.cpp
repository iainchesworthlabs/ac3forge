#include "asf/multichannel.hpp"

#include <cstddef>
#include <utility>

namespace ac4::detail {
namespace {

// Table 178, chel_matsel 0 to 11, as read off the printed entries: for 0,
// "a0a1 b0a1 b1 | c0 d0 0 | a0c1 b0c1 d1" is step 1 on (I0, I1), Y = O1, and
// step 2 on (X, I2) giving O0 and O2.
constexpr std::array<ThreeChannelCascade, 12> kTable178 = {{
    {.p = 0, .q = 1, .x_out = false, .out = 1, .step2_out = {0, 2}, .r = 2},
    {.p = 1, .q = 0, .x_out = false, .out = 0, .step2_out = {1, 2}, .r = 2},
    {.p = 0, .q = 2, .x_out = false, .out = 2, .step2_out = {0, 1}, .r = 1},
    {.p = 1, .q = 2, .x_out = true, .out = 1, .step2_out = {0, 2}, .r = 0},
    {.p = 0, .q = 2, .x_out = true, .out = 0, .step2_out = {1, 2}, .r = 1},
    {.p = 2, .q = 1, .x_out = true, .out = 2, .step2_out = {0, 1}, .r = 0},
    {.p = 1, .q = 0, .x_out = true, .out = 1, .step2_out = {2, 0}, .r = 2},
    {.p = 0, .q = 1, .x_out = true, .out = 0, .step2_out = {2, 1}, .r = 2},
    {.p = 2, .q = 0, .x_out = true, .out = 2, .step2_out = {1, 0}, .r = 1},
    {.p = 2, .q = 1, .x_out = false, .out = 1, .step2_out = {2, 0}, .r = 0},
    {.p = 2, .q = 0, .x_out = false, .out = 0, .step2_out = {2, 1}, .r = 1},
    {.p = 1, .q = 2, .x_out = false, .out = 2, .step2_out = {1, 0}, .r = 0},
}};

// One step undone: `left` and `right`, the step's first and second rows,
// become its first and second inputs.
[[nodiscard]] StereoChoice undo_step(Channel& left, Channel& right, std::span<const StereoChoice> forced,
                                     std::size_t set) {
    if (forced.empty()) {
        return choose_stereo(left.grouped, right.grouped, left.allowed, right.allowed);
    }
    apply_stereo(left.grouped, right.grouped, left.allowed, right.allowed, forced[set]);
    return forced[set];
}

// The tracks' perceptual entropy, with the unit's side information.
template <std::size_t N>
void count_bits(UnitChoice& choice, const std::array<Channel*, N>& tracks, bool matsel) {
    choice.bits = matsel ? 4.0 : 0.0;
    for (const StereoChoice& set : choice.sets) {
        choice.bits += static_cast<double>(chparam_info_bits(set));
    }
    for (const Channel* track : tracks) {
        choice.bits += perceptual_entropy(track->grouped, track->allowed);
    }
}

// Table 178's cascade undone, without counting bits.
void undo_cascade(const ThreeChannelCascade& k, std::array<Channel*, 3> unit, std::span<const StereoChoice> forced,
                  UnitChoice& choice) {
    Channel first = std::move(*unit[static_cast<std::size_t>(k.step2_out[0])]);
    Channel second = std::move(*unit[static_cast<std::size_t>(k.step2_out[1])]);
    Channel direct = std::move(*unit[static_cast<std::size_t>(k.out)]);
    // Step 2: its rows back to its inputs, (X, Ir) or (Ir, Y).
    choice.sets[1] = undo_step(first, second, forced, 1);
    Channel& x = k.x_out ? direct : first;
    Channel& y = k.x_out ? second : direct;
    Channel& ir = k.x_out ? first : second;
    // Step 1: X and Y back to Ip and Iq.
    choice.sets[0] = undo_step(x, y, forced, 0);
    *unit[static_cast<std::size_t>(k.p)] = std::move(x);
    *unit[static_cast<std::size_t>(k.q)] = std::move(y);
    *unit[static_cast<std::size_t>(k.r)] = std::move(ir);
}

}  // namespace

const ThreeChannelCascade& three_channel_cascade(int chel_matsel) {
    return kTable178.at(static_cast<std::size_t>(chel_matsel));
}

UnitChoice undo_pair(std::array<Channel*, 2> unit, std::span<const StereoChoice> forced) {
    UnitChoice choice;
    choice.sets.push_back(undo_step(*unit[0], *unit[1], forced, 0));
    count_bits(choice, unit, false);
    return choice;
}

UnitChoice undo_three(int chel_matsel, std::array<Channel*, 3> unit, std::span<const StereoChoice> forced) {
    UnitChoice choice;
    choice.chel_matsel = chel_matsel;
    choice.sets.resize(2);
    undo_cascade(three_channel_cascade(chel_matsel), unit, forced, choice);
    count_bits(choice, unit, true);
    return choice;
}

UnitChoice undo_four(std::array<Channel*, 4> unit, std::span<const StereoChoice> forced) {
    UnitChoice choice;
    choice.sets.resize(4);
    // Sets 2 and 3: (O0, O2) back to (P0, Q0), (O1, O3) to (P1, Q1), each
    // left where its output was.
    choice.sets[2] = undo_step(*unit[0], *unit[2], forced, 2);
    choice.sets[3] = undo_step(*unit[1], *unit[3], forced, 3);
    // Sets 0 and 1: (P0, P1) back to (I0, I1), (Q0, Q1) to (I2, I3).
    choice.sets[0] = undo_step(*unit[0], *unit[1], forced, 0);
    choice.sets[1] = undo_step(*unit[2], *unit[3], forced, 1);
    count_bits(choice, unit, false);
    return choice;
}

UnitChoice undo_five(int chel_matsel, std::array<Channel*, 5> unit, std::span<const StereoChoice> forced) {
    UnitChoice choice;
    choice.chel_matsel = chel_matsel;
    choice.sets.resize(5);
    // Sets 3 and 4: (O0, O3) back to (T0, U0), (O1, O4) to (T1, U1).
    choice.sets[3] = undo_step(*unit[0], *unit[3], forced, 3);
    choice.sets[4] = undo_step(*unit[1], *unit[4], forced, 4);
    // Set 2: (U0, U1) back to (I3, I4).
    choice.sets[2] = undo_step(*unit[3], *unit[4], forced, 2);
    // Table 178's cascade: (T0, T1, T2 = O2) back to (I0, I1, I2).
    undo_cascade(three_channel_cascade(chel_matsel), {unit[0], unit[1], unit[2]},
                 forced.empty() ? forced : forced.first(2), choice);
    count_bits(choice, unit, true);
    return choice;
}

}  // namespace ac4::detail

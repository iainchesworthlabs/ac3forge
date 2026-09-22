#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "ac3/core/exponents.hpp"
#include "ac3/core/tables.hpp"

// Exponent-run planning, shared by both encoders' §8.2.8 reuse-span decisions
// (AC-3's encoder.cpp and E-AC-3's eac3_frame.cpp).
//
// The strategy a chosen span earns under Table E2.10 is a spec rule and lives
// with the rest of the exponent pipeline (ac3::strategy_for_span,
// core/exponents.hpp). What follows is the other half of the plan - WHERE the
// spans end - and that is a judgement about cost rather than anything the
// standard states, which is why it sits here in the encoder's own headers.
//
// Internal to src/forge/src/encoder/ on purpose, the same way snr_search.hpp
// is: plumbing between the two encoder translation units, not library surface.
// tests/encoder/test_exp_strategy.cpp includes it directly.

namespace ac3::internal {

// §8.2.8: "when the variation exceeds a threshold, new exponents will be
// sent".
//
// The threshold is a judgement about COST, so it is not one number. A full-
// bandwidth channel's set is 4 + 7*ngrps bits - about 590 at D15 over a
// 250-coefficient band - and spending that mid-frame has to buy back more
// than it costs, so it waits for the exponents to have really moved: a mean
// change above two steps, 12 dB per bin.
//
// The LFE's set is always two groups, 18 bits, thirty times cheaper. Holding
// it to the same bar means almost never refreshing it, and the frame's one
// set is then the per-bin minimum across six blocks - a scale chosen by the
// loudest of them. Any block quieter than that is quantized against the wrong
// scale for the sake of not spending 18 bits. So the LFE refreshes as soon as
// its exponents move at all, which is the trade its own cost argues for.
//
// This is the AC-3 encoder's rule, kept because that is the encoder it was
// measured on. plan_exponent_runs below replaces it for E-AC-3 with an
// explicit accounting of the same trade - see its own comment for why a fixed
// threshold turned out not to travel.
[[nodiscard]] inline bool needs_new_exponents(std::span<const std::uint8_t> current,
                                              std::span<const std::uint8_t> reference,
                                              bool is_lfe) {
    long long diff = 0;
    for (std::size_t i = 0; i < current.size(); ++i) {
        diff += std::abs(static_cast<int>(current[i]) - static_cast<int>(reference[i]));
    }
    return diff > (is_lfe ? 0 : 2 * static_cast<long long>(current.size()));
}

// The largest number of blocks a plan covers: a full six-block syncframe.
inline constexpr int kMaxPlanBlocks = kBlocksPerFrame;

// One stream's plan. runs tile [0, blocks): run i covers starts[i] up to
// starts[i + 1], and states strategy[i].
struct ExponentRunPlan {
    int count = 0;
    std::array<int, kMaxPlanBlocks + 1> starts{};
    std::array<ExpStrategy, kMaxPlanBlocks> strategy{};
    // What the plan costs, in bits: the exponent sets it transmits plus the
    // mantissa precision it gives up. Comparable across plans of the same
    // stream, and (bar the per-stream strategy field, which does not depend
    // on the plan) across streams too.
    long long score = 0;
};

// What the planner needs to know about one stream.
struct ExponentRunInput {
    // [block][bin], row stride `bins`, indexed from the stream's own start
    // bin. These are the RAW per-block exponents, before any grouping.
    std::span<const std::uint8_t> exps;
    int bins = 0;
    int blocks = kMaxPlanBlocks;
    // Per bin, from a provisional allocation: how many bits of precision that
    // bin's mantissa actually resolves. It bounds how much a coarser exponent
    // set can cost the bin - see the waste model in plan_exponent_runs. Empty
    // leaves it unbounded, which is only useful for tests.
    std::span<const std::uint8_t> precision;
    // Blocks that must start a run whatever it costs. §7.9's block-switched
    // block is isolated on both sides: its 256 coefficients are two
    // interleaved short spectra, so one exponent set cannot describe it and a
    // long block together.
    std::array<bool, kMaxPlanBlocks> boundary{};
    // §5.4.3.25: the coupling channel groups from its first coded bin and its
    // group count must divide the region exactly. A full-bandwidth or LFE
    // channel keeps bin 0 as its absolute exponent and groups from bin 1
    // (§7.1.3).
    bool coupling = false;
    // §5.4.3.15 makes lfeexpstr one bit - D15 or reuse - so the LFE has no
    // banding to choose.
    bool lfe = false;
    // Table E2.10 fixes each run's strategy from its span, so a frame that
    // hoists its strategies (expstre == 0) has no strategy choice left once
    // the partition is picked. Only the per-block form leaves it free.
    bool free_strategy = false;
};

// The exponent set a run would transmit, as the decoder would reconstruct it:
// exactly what encode_exponents followed by decode_exponents produces for the
// per-bin minimum over the run's blocks, without building either side's
// vectors. §8.2.10's whole preprocessing chain is here, not just the grouping
// minimum, because the part it would be tempting to leave out is the part that
// decides between the strategies: differentials are limited to +-2 and the
// limiter only ever DECREASES exponents, so a spectrum that falls steeply
// costs a D45 set - whose groups are four bins apart - four times the ground a
// D15 set gives up over the same span. Modelling the grouping alone makes the
// coarse strategies look nearly free, and they are not.
inline void banded_run_exponents(std::span<const std::uint8_t> run_min, ExpStrategy strategy,
                                 bool coupling, std::span<std::uint8_t> out) {
    const auto bins = static_cast<int>(run_min.size());
    const int group = exponent_group_size(strategy);
    // pre[0] is the transmitted absolute exponent; pre[1 + i] covers the i-th
    // group of `group` bins. A coupling channel's reference does not
    // correspond to a coefficient and must stay even (§5.4.3.25), so its
    // groups start at bin 0; a full-bandwidth or LFE channel's reference IS
    // bin 0 and its groups start at bin 1 (§7.1.3).
    std::array<int, 257> pre{};
    const int first = coupling ? 0 : 1;
    const int real_diffs = (bins - first + group - 1) / group;
    for (int i = 0; i < real_diffs; ++i) {
        const int begin = first + i * group;
        int value = kMaxExponent;
        for (int bin = begin; bin < begin + group && bin < bins; ++bin) {
            value = std::min(value, static_cast<int>(run_min[static_cast<std::size_t>(bin)]));
        }
        pre[static_cast<std::size_t>(i) + 1] = value;
    }
    pre[0] = coupling ? std::clamp(pre[1] & ~1, 0, kMaxExponent)
                      : std::min<int>(run_min[0], kMaxAbsoluteExponent);
    for (int i = 1; i <= real_diffs; ++i) {
        pre[static_cast<std::size_t>(i)] =
            std::min(pre[static_cast<std::size_t>(i)], pre[static_cast<std::size_t>(i) - 1] + 2);
    }
    for (int i = real_diffs; i-- > 0;) {
        pre[static_cast<std::size_t>(i)] =
            std::min(pre[static_cast<std::size_t>(i)], pre[static_cast<std::size_t>(i) + 1] + 2);
        if (i == 0 && coupling) {
            pre[0] &= ~1;  // the transmitted reference stays even
        }
    }
    if (!coupling) {
        out[0] = static_cast<std::uint8_t>(pre[0]);
    }
    for (int i = 0; i < real_diffs; ++i) {
        const int begin = first + i * group;
        for (int bin = begin; bin < begin + group && bin < bins; ++bin) {
            out[static_cast<std::size_t>(bin)] =
                static_cast<std::uint8_t>(pre[static_cast<std::size_t>(i) + 1]);
        }
    }
}

// The best plan for one stream, by exhaustive dynamic programming over every
// partition of its blocks and (where the frame form allows it) every strategy
// for every run.
//
// Both halves of the trade are counted in bits, which is what makes them
// comparable at all:
//
//   cost   - the exponent set itself, 4 + 7 * ngrps.
//   waste  - the mantissa precision the set gives up. One exponent step is a
//            factor of two of scale, so a bin quantized against a set one step
//            coarser than its own block wanted carries about 6 dB more
//            quantization noise - one bit of accuracy.
//
// The waste is bounded, per bin, by what that bin was actually given
// (`precision`). Without that bound the model is badly wrong in the direction
// that matters: a quiet block's bin sits 18 exponent steps below the loud
// block that set the run's scale, and counting all 18 as recoverable would
// value a refresh at thousands of bits when the bin is allocated three. It
// cannot lose precision it never had - a bin with no bits at all loses
// nothing, and reconstructs to zero either way.
//
// That bound is the whole reason this is not a fixed threshold. At 192 kbit/s
// stereo a frame has roughly 1.5 mantissa bits per bin-block to spend, so most
// bins are allocated nothing; a rule that counts every bin's headroom as a
// saving will happily spend a tenth of the frame on exponent sets that buy
// silence. The AC-3 encoder's fixed "mean two steps" threshold is right for
// AC-3, which is where it was measured - this is not a claim that it is wrong
// there, only that it does not travel to a different rate regime by itself.
//
// Complexity is blocks^2 * strategies passes over the bins - about 32k
// operations for a 253-bin channel, once per stream per frame.
[[nodiscard]] inline ExponentRunPlan plan_exponent_runs(const ExponentRunInput& in) {
    const auto bins = static_cast<std::size_t>(in.bins);
    const int blocks = in.blocks;
    std::array<ExpStrategy, 3> candidates{ExpStrategy::kD15, ExpStrategy::kD25,
                                          ExpStrategy::kD45};

    // best[b]: the cheapest way to cover blocks [0, b) with whole runs.
    // from[b]/via[b]: the run that ends at b in that plan.
    constexpr long long kUnreachable = (1LL << 60);
    std::array<long long, kMaxPlanBlocks + 1> best{};
    std::array<int, kMaxPlanBlocks + 1> from{};
    std::array<ExpStrategy, kMaxPlanBlocks + 1> via{};
    best.fill(kUnreachable);
    best[0] = 0;

    std::array<std::uint8_t, 256> run_min{};
    std::array<std::uint8_t, 256> banded{};

    for (int a = 0; a < blocks; ++a) {
        if (best[static_cast<std::size_t>(a)] >= kUnreachable) {
            continue;
        }
        for (std::size_t bin = 0; bin < bins; ++bin) {
            run_min[bin] = kMaxExponent;
        }
        for (int b = a + 1; b <= blocks; ++b) {
            // Extend the run to cover block b - 1.
            const auto row = in.exps.subspan(static_cast<std::size_t>(b - 1) * bins, bins);
            for (std::size_t bin = 0; bin < bins; ++bin) {
                run_min[bin] = std::min(run_min[bin], row[bin]);
            }
            // A forced boundary inside the run rules it out - but only after
            // the running minimum has been extended, so the next b still sees
            // the right state.
            if (b - 1 > a && in.boundary[static_cast<std::size_t>(b - 1)]) {
                break;
            }
            const int span = b - a;
            for (const auto strategy : candidates) {
                if (in.lfe) {
                    // lfeexpstr states D15 or nothing, in either frame form -
                    // the LFE has no Table E2.10 code of its own, so the
                    // hoisted form does not constrain its run layout either.
                    if (strategy != ExpStrategy::kD15) {
                        continue;
                    }
                } else if (!in.free_strategy && strategy != strategy_for_span(span)) {
                    continue;  // Table E2.10 fixes it from the span
                }
                const int group = exponent_group_size(strategy);
                int ngrps = 0;
                if (in.coupling) {
                    // §5.4.3.25: ncplgrps covers the region exactly, with none
                    // of §7.1.3's round-up slack, so a strategy that does not
                    // divide it cannot be stated at all.
                    if (in.bins % (3 * group) != 0) {
                        continue;
                    }
                    ngrps = in.bins / (3 * group);
                } else {
                    ngrps = exponent_group_count(strategy, in.bins);
                }
                banded_run_exponents(std::span{run_min}.first(bins), strategy, in.coupling,
                                     std::span{banded}.first(bins));
                long long waste = 0;
                for (std::size_t bin = 0; bin < bins; ++bin) {
                    const int cap = in.precision.empty() ? kMaxExponent : in.precision[bin];
                    if (cap == 0) {
                        continue;
                    }
                    for (int blk = a; blk < b; ++blk) {
                        const int lost =
                            in.exps[static_cast<std::size_t>(blk) * bins + bin] - banded[bin];
                        waste += std::min(lost, cap);
                    }
                }
                const long long score =
                    best[static_cast<std::size_t>(a)] + waste + 4 + 7LL * ngrps;
                if (score < best[static_cast<std::size_t>(b)]) {
                    best[static_cast<std::size_t>(b)] = score;
                    from[static_cast<std::size_t>(b)] = a;
                    via[static_cast<std::size_t>(b)] = strategy;
                }
            }
        }
    }

    // Walk the choices back. The DP always reaches `blocks`: a single run over
    // every block is legal for every stream (D15 divides the coupling region
    // whatever its width, and no forced boundary can sit inside a run that
    // starts at block 0 and never stops).
    ExponentRunPlan plan;
    std::array<int, kMaxPlanBlocks + 1> reversed{};
    std::array<ExpStrategy, kMaxPlanBlocks> reversed_strategy{};
    int count = 0;
    for (int b = blocks; b > 0; b = from[static_cast<std::size_t>(b)]) {
        reversed_strategy[static_cast<std::size_t>(count)] = via[static_cast<std::size_t>(b)];
        reversed[static_cast<std::size_t>(count)] = from[static_cast<std::size_t>(b)];
        ++count;
    }
    plan.count = count;
    for (int i = 0; i < count; ++i) {
        plan.starts[static_cast<std::size_t>(i)] =
            reversed[static_cast<std::size_t>(count - 1 - i)];
        plan.strategy[static_cast<std::size_t>(i)] =
            reversed_strategy[static_cast<std::size_t>(count - 1 - i)];
    }
    plan.starts[static_cast<std::size_t>(count)] = blocks;
    plan.score = best[static_cast<std::size_t>(blocks)];
    return plan;
}

}  // namespace ac3::internal

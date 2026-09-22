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
// Annex E has two frame forms, and the encoder wants the best plan under
// each: hoisted (expstre == 0, every run's strategy fixed by Table E2.10 from
// its span) and per-block (every strategy free). Both are the same dynamic
// programme over the same candidate runs, differing only in which strategies
// a run may take, so plan_exponent_runs_both scores both in one pass: each
// candidate's exponent set and its waste are computed once and offered to
// whichever form admits it. What each form is offered, and in what order, is
// exactly what its own pass would have seen, so the plans are the plans the
// separate passes found - the tie-breaking is the same strict comparison in
// the same sequence.
//
// Four things keep the pass short without changing its answer. A candidate's
// waste is never negative, so a run whose exponent set alone already costs
// as much as the best plan found for its end block cannot improve on it:
// the set's cost is the lower bound, and a candidate below it is skipped
// before its set is built. A bin allocated no precision loses none, so only
// the coded bins are summed - at low rates a minority of them. A run's
// banded set depends only on its per-bin minimum, so while extending the run
// by a block moves no minimum the set stands, and the extended run's waste is
// the waste so far plus the new block's row. And that row is summed along
// the block's own exponents rather than bin by bin down a column, which is
// the order they are laid out in.
//
// Complexity was blocks^2 * strategies passes over the bins per form - about
// 32k operations for a 253-bin channel, twice per stream per frame.
struct ExponentRunPlans {
    ExponentRunPlan hoisted;
    ExponentRunPlan per_block;
};

[[nodiscard]] inline ExponentRunPlans plan_exponent_runs_both(const ExponentRunInput& in) {
    const auto bins = static_cast<std::size_t>(in.bins);
    const int blocks = in.blocks;
    constexpr std::array<ExpStrategy, 3> kCandidates{ExpStrategy::kD15, ExpStrategy::kD25,
                                                     ExpStrategy::kD45};

    // One table per form. best[b]: the cheapest way to cover blocks [0, b)
    // with whole runs; from[b]/via[b]: the run that ends at b in that plan.
    constexpr long long kUnreachable = (1LL << 60);
    struct Table {
        std::array<long long, kMaxPlanBlocks + 1> best{};
        std::array<int, kMaxPlanBlocks + 1> from{};
        std::array<ExpStrategy, kMaxPlanBlocks + 1> via{};
    };
    Table hoisted;
    Table per_block;
    hoisted.best.fill(kUnreachable);
    per_block.best.fill(kUnreachable);
    hoisted.best[0] = 0;
    per_block.best[0] = 0;

    // The bins whose waste can be non-zero: those allocated at least one bit
    // of precision. A bin with none loses nothing whatever the set, so it is
    // left out of every waste sum - and at low rates that is most of them.
    std::array<std::uint16_t, 256> coded{};
    std::size_t coded_count = 0;
    std::array<std::uint8_t, 256> cap{};
    for (std::size_t bin = 0; bin < bins; ++bin) {
        const int c = in.precision.empty() ? kMaxExponent : in.precision[bin];
        if (c > 0) {
            coded[coded_count++] = static_cast<std::uint16_t>(bin);
            cap[bin] = static_cast<std::uint8_t>(c);
        }
    }
    // One block's contribution to a run's waste under a banded set.
    const auto row_waste = [&](int blk, const std::array<std::uint8_t, 256>& banded) {
        const auto exps = in.exps.subspan(static_cast<std::size_t>(blk) * bins, bins);
        int waste = 0;
        for (std::size_t i = 0; i < coded_count; ++i) {
            const std::size_t bin = coded[i];
            waste += std::min(static_cast<int>(exps[bin]) - banded[bin], static_cast<int>(cap[bin]));
        }
        return waste;
    };

    std::array<std::uint8_t, 256> run_min{};
    // Per strategy, the banded set and the waste for the run as it stood the
    // last time the strategy was scored from this start block: banded
    // depends only on run_min, so while the minimum has not moved it holds,
    // and the waste of the extended run is the waste so far plus the new
    // rows. `covered` is how many blocks from `a` the cached waste spans.
    struct StrategyCache {
        std::array<std::uint8_t, 256> banded{};
        long long waste = 0;
        int covered = 0;
        bool valid = false;
    };
    std::array<StrategyCache, 3> cache{};

    for (int a = 0; a < blocks; ++a) {
        const auto ua = static_cast<std::size_t>(a);
        const bool hoisted_open = hoisted.best[ua] < kUnreachable;
        const bool per_block_open = per_block.best[ua] < kUnreachable;
        if (!hoisted_open && !per_block_open) {
            continue;
        }
        for (std::size_t bin = 0; bin < bins; ++bin) {
            run_min[bin] = kMaxExponent;
        }
        for (auto& c : cache) {
            c.valid = false;
        }
        for (int b = a + 1; b <= blocks; ++b) {
            const auto ub = static_cast<std::size_t>(b);
            // Extend the run to cover block b - 1, noting whether any bin's
            // minimum moved (then every cached set is stale).
            const auto row = in.exps.subspan(static_cast<std::size_t>(b - 1) * bins, bins);
            bool moved = false;
            for (std::size_t bin = 0; bin < bins; ++bin) {
                if (row[bin] < run_min[bin]) {
                    run_min[bin] = row[bin];
                    moved = true;
                }
            }
            if (moved) {
                for (auto& c : cache) {
                    c.valid = false;
                }
            }
            // A forced boundary inside the run rules it out - but only after
            // the running minimum has been extended, so the next b still sees
            // the right state.
            if (b - 1 > a && in.boundary[static_cast<std::size_t>(b - 1)]) {
                break;
            }
            const int span = b - a;
            const ExpStrategy fixed = strategy_for_span(span);
            for (std::size_t k = 0; k < kCandidates.size(); ++k) {
                const ExpStrategy strategy = kCandidates[k];
                if (in.lfe) {
                    // lfeexpstr states D15 or nothing, in either frame form -
                    // the LFE has no Table E2.10 code of its own, so the
                    // hoisted form does not constrain its run layout either.
                    if (strategy != ExpStrategy::kD15) {
                        continue;
                    }
                }
                // Which forms admit this strategy for this span. The hoisted
                // form takes only Table E2.10's; the per-block form takes any;
                // the LFE's D15 is the one strategy either form has for it.
                const bool for_hoisted = hoisted_open && (in.lfe || strategy == fixed);
                const bool for_per_block = per_block_open;
                if (!for_hoisted && !for_per_block) {
                    continue;
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
                const long long set_cost = 4 + 7LL * ngrps;
                // The lower bound: with no waste at all this candidate would
                // score best[a] + set_cost, and it has to beat best[b]
                // strictly to be taken.
                const bool hoisted_can_win =
                    for_hoisted && hoisted.best[ua] + set_cost < hoisted.best[ub];
                const bool per_block_can_win =
                    for_per_block && per_block.best[ua] + set_cost < per_block.best[ub];
                if (!hoisted_can_win && !per_block_can_win) {
                    continue;
                }
                StrategyCache& c = cache[k];
                if (!c.valid) {
                    banded_run_exponents(std::span{run_min}.first(bins), strategy, in.coupling,
                                         std::span{c.banded}.first(bins));
                    c.waste = 0;
                    c.covered = 0;
                    c.valid = true;
                }
                for (int blk = a + c.covered; blk < b; ++blk) {
                    c.waste += row_waste(blk, c.banded);
                }
                c.covered = span;
                const long long waste = c.waste;
                if (hoisted_can_win) {
                    const long long score = hoisted.best[ua] + waste + set_cost;
                    if (score < hoisted.best[ub]) {
                        hoisted.best[ub] = score;
                        hoisted.from[ub] = a;
                        hoisted.via[ub] = strategy;
                    }
                }
                if (per_block_can_win) {
                    const long long score = per_block.best[ua] + waste + set_cost;
                    if (score < per_block.best[ub]) {
                        per_block.best[ub] = score;
                        per_block.from[ub] = a;
                        per_block.via[ub] = strategy;
                    }
                }
            }
        }
    }

    // Walk each form's choices back. The DP always reaches `blocks`: a single
    // run over every block is legal for every stream (D15 divides the
    // coupling region whatever its width, and no forced boundary can sit
    // inside a run that starts at block 0 and never stops).
    const auto unwind = [blocks](const Table& table) {
        ExponentRunPlan plan;
        std::array<int, kMaxPlanBlocks + 1> reversed{};
        std::array<ExpStrategy, kMaxPlanBlocks> reversed_strategy{};
        int count = 0;
        for (int b = blocks; b > 0; b = table.from[static_cast<std::size_t>(b)]) {
            reversed_strategy[static_cast<std::size_t>(count)] =
                table.via[static_cast<std::size_t>(b)];
            reversed[static_cast<std::size_t>(count)] = table.from[static_cast<std::size_t>(b)];
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
        plan.score = table.best[static_cast<std::size_t>(blocks)];
        return plan;
    };
    return {.hoisted = unwind(hoisted), .per_block = unwind(per_block)};
}

// One form on its own, for callers and tests that want just that:
// free_strategy picks the per-block form, otherwise the hoisted one.
[[nodiscard]] inline ExponentRunPlan plan_exponent_runs(const ExponentRunInput& in) {
    const ExponentRunPlans both = plan_exponent_runs_both(in);
    return in.free_strategy ? both.per_block : both.hoisted;
}

}  // namespace ac3::internal

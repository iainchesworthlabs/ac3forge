#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/tables.hpp"
#include "exp_strategy.hpp"

// The exponent-run planner (src/forge/src/encoder/exp_strategy.hpp), tested
// directly rather than through the encoder: what it decides is a cost
// judgement, and a judgement is much easier to hold to account on inputs
// chosen to make the right answer obvious than on real program material where
// several decisions move at once.

namespace {

constexpr int kBins = 253;  // a full-bandwidth channel at chbwcod 60

// One stream's per-block exponents, laid out the way ExponentRunInput wants
// them: [block][bin] with a row stride of `stride`.
struct Exps {
    std::vector<std::uint8_t> data;
    int stride = 0;

    Exps(int blocks, int stride_bins, std::uint8_t fill)
        : data(static_cast<std::size_t>(blocks) * static_cast<std::size_t>(stride_bins), fill),
          stride(stride_bins) {}

    void set_block(int blk, std::uint8_t value, int bins) {
        for (int bin = 0; bin < bins; ++bin) {
            data[static_cast<std::size_t>(blk) * static_cast<std::size_t>(stride) +
                 static_cast<std::size_t>(bin)] = value;
        }
    }
};

ac3::internal::ExponentRunInput input_for(const Exps& exps, int bins,
                                          std::span<const std::uint8_t> precision = {}) {
    return ac3::internal::ExponentRunInput{.exps = exps.data,
                                           .bins = bins,
                                           .blocks = ac3::kBlocksPerFrame,
                                           .precision = precision,
                                           .boundary = {},
                                           .coupling = false,
                                           .lfe = false,
                                           .free_strategy = false};
}

}  // namespace

TEST_CASE("stationary exponents plan one set for the frame", "[eac3][exponents]") {
    // Nothing moves, so a second set would buy nothing and cost 4 + 7*84 bits.
    Exps exps{ac3::kBlocksPerFrame, kBins, 6};
    const auto plan = ac3::internal::plan_exponent_runs(input_for(exps, kBins));
    CHECK(plan.count == 1);
    CHECK(plan.starts[0] == 0);
    CHECK(plan.starts[1] == ac3::kBlocksPerFrame);
    CHECK(plan.strategy[0] == ac3::ExpStrategy::kD15);
}

TEST_CASE("a loud block on its own does not drag the frame's scale down",
          "[eac3][exponents]") {
    // Block 3 is 8 exponent steps louder than the rest. One set for the frame
    // takes the per-bin MINIMUM, so every other block would be quantized 8
    // steps coarse - 253 bins * 5 blocks * 8 steps of wasted precision against
    // the ~592 bits a second D15 set costs. The planner has to split.
    Exps exps{ac3::kBlocksPerFrame, kBins, 14};
    exps.set_block(3, 6, kBins);
    const auto plan = ac3::internal::plan_exponent_runs(input_for(exps, kBins));
    CHECK(plan.count > 1);
    // Block 3 starts a run of its own, and the blocks after it start another:
    // its exponents describe nothing but itself.
    bool block3_is_fresh = false;
    for (int i = 0; i < plan.count; ++i) {
        block3_is_fresh = block3_is_fresh || plan.starts[i] == 3;
    }
    CHECK(block3_is_fresh);
}

TEST_CASE("headroom on bins nobody codes is not worth an exponent set",
          "[eac3][exponents]") {
    // Block 3 is eight exponent steps louder than the rest, which is a large
    // move by any measure - and the allocation reaches none of these bins, so
    // every one of them reconstructs to zero whichever scale it is quantized
    // against. There is nothing to recover, and a second set is never free.
    Exps exps{ac3::kBlocksPerFrame, kBins, 14};
    exps.set_block(3, 6, kBins);
    const std::vector<std::uint8_t> precision(static_cast<std::size_t>(kBins), 0);
    const auto plan = ac3::internal::plan_exponent_runs(input_for(exps, kBins, precision));
    CHECK(plan.count == 1);
}

TEST_CASE("the same move DOES pay once the bins are coded", "[eac3][exponents]") {
    // Identical exponents to the test above; the only change is that the
    // allocation now reaches every bin, so the same eight steps are real
    // precision rather than headroom on silence. This pair is the whole reason
    // the planner is told what each bin was given.
    Exps exps{ac3::kBlocksPerFrame, kBins, 14};
    exps.set_block(3, 6, kBins);
    const auto plan = ac3::internal::plan_exponent_runs(input_for(exps, kBins));
    CHECK(plan.count > 1);
}

TEST_CASE("a block-switched block is isolated whatever it costs", "[eac3][exponents]") {
    // §7.9: a short-transform block's 256 coefficients are two interleaved
    // 128-bin spectra, so one exponent set cannot describe it and a long block
    // together - the boundary is not a cost judgement.
    Exps exps{ac3::kBlocksPerFrame, kBins, 6};
    auto input = input_for(exps, kBins);
    input.boundary[2] = true;
    input.boundary[3] = true;
    const auto plan = ac3::internal::plan_exponent_runs(input);
    REQUIRE(plan.count == 3);
    CHECK(plan.starts[0] == 0);
    CHECK(plan.starts[1] == 2);
    CHECK(plan.starts[2] == 3);
    CHECK(plan.starts[3] == ac3::kBlocksPerFrame);
}

TEST_CASE("the hoisted form only ever states Table E2.10's own strategies",
          "[eac3][exponents]") {
    // With expstre == 0 the run layout IS the strategy: whatever partition the
    // planner picks, every run's strategy has to be the one
    // frame_exp_strategy_code's own table attaches to that span, or the code
    // written into audfrm would describe a different set from the one encoded.
    Exps exps{ac3::kBlocksPerFrame, kBins, 20};
    exps.set_block(1, 4, kBins);
    exps.set_block(4, 8, kBins);
    const auto plan = ac3::internal::plan_exponent_runs(input_for(exps, kBins));
    std::array<bool, ac3::kBlocksPerFrame> fresh{};
    for (int i = 0; i < plan.count; ++i) {
        fresh[static_cast<std::size_t>(plan.starts[i])] = true;
        const int span = plan.starts[i + 1] - plan.starts[i];
        CHECK(plan.strategy[static_cast<std::size_t>(i)] == ac3::strategy_for_span(span));
    }
    const int code = ac3::eac3::frame_exp_strategy_code(fresh);
    for (int i = 0; i < plan.count; ++i) {
        for (int blk = plan.starts[i]; blk < plan.starts[i + 1]; ++blk) {
            const auto expected = blk == plan.starts[i]
                                      ? plan.strategy[static_cast<std::size_t>(i)]
                                      : ac3::ExpStrategy::kReuse;
            CHECK(ac3::eac3::frame_exp_strategy(code, blk) == expected);
        }
    }
}

TEST_CASE("the per-block form is never a worse plan than the hoisted one",
          "[eac3][exponents]") {
    // expstre == 1 can state anything expstre == 0 can and more, so its best
    // plan can never score worse. Worth pinning: the frame pays seven bits a
    // stream for the freedom, and the only reason to know whether that is
    // money well spent is that this comparison is meaningful in the first
    // place.
    for (int loud = 0; loud < ac3::kBlocksPerFrame; ++loud) {
        Exps exps{ac3::kBlocksPerFrame, kBins, 17};
        exps.set_block(loud, 5, kBins);
        auto input = input_for(exps, kBins);
        const auto hoisted = ac3::internal::plan_exponent_runs(input);
        input.free_strategy = true;
        const auto free_form = ac3::internal::plan_exponent_runs(input);
        CHECK(free_form.score <= hoisted.score);
    }
}

TEST_CASE("the per-block form states D15 where the table would force D45",
          "[eac3][exponents]") {
    // Table E2.10 gives a single-block run D45 - four bins to an exponent, and
    // differentials four bins apart still limited to +-2 steps (§8.2.10). On a
    // spectrum that falls a step a bin that limit bites hard: the transmitted
    // set cannot keep up with the real one and every bin past the first few is
    // quantized against a scale far too coarse. D15 follows the same fall
    // exactly. Over a narrow band - a channel whose high end is coupled or
    // extended away, so its own set is cheap - the finer set is worth its
    // extra bits, and only the per-block form can state it.
    constexpr int kNarrow = 37;  // fbw endmant with coupling from sub-band 0
    Exps exps{ac3::kBlocksPerFrame, kNarrow, 24};
    for (int bin = 0; bin < kNarrow; ++bin) {
        exps.data[static_cast<std::size_t>(3) * static_cast<std::size_t>(kNarrow) +
                  static_cast<std::size_t>(bin)] = static_cast<std::uint8_t>(bin);
    }
    auto input = input_for(exps, kNarrow);
    const auto hoisted = ac3::internal::plan_exponent_runs(input);
    input.free_strategy = true;
    const auto free_form = ac3::internal::plan_exponent_runs(input);
    CHECK(free_form.score < hoisted.score);
    bool states_d15_on_a_short_run = false;
    for (int i = 0; i < free_form.count; ++i) {
        const int span = free_form.starts[i + 1] - free_form.starts[i];
        states_d15_on_a_short_run =
            states_d15_on_a_short_run ||
            (span < 4 && free_form.strategy[static_cast<std::size_t>(i)] == ac3::ExpStrategy::kD15);
    }
    CHECK(states_d15_on_a_short_run);
}

TEST_CASE("the coupling channel never states a strategy its region cannot carry",
          "[eac3][exponents]") {
    // §5.4.3.25: ncplgrps covers the coupling region exactly, with none of
    // §7.1.3's round-up slack, so the bin count must divide by three times the
    // group size. Enhanced coupling's Table E3.9 sub-bands are 6 bins wide at
    // the bottom, so a region an odd number of those wide cannot carry D45 at
    // all - and the planner must not pick it however well it would score.
    constexpr int kEcplBins = 234;  // 253 - 19: an odd number of 6-bin sub-bands
    Exps exps{ac3::kBlocksPerFrame, kEcplBins, 20};
    exps.set_block(1, 2, kEcplBins);
    exps.set_block(3, 2, kEcplBins);
    exps.set_block(5, 2, kEcplBins);
    auto input = input_for(exps, kEcplBins);
    input.coupling = true;
    input.free_strategy = true;
    const auto plan = ac3::internal::plan_exponent_runs(input);
    REQUIRE(plan.count >= 1);
    for (int i = 0; i < plan.count; ++i) {
        const int group = ac3::exponent_group_size(plan.strategy[static_cast<std::size_t>(i)]);
        CHECK(kEcplBins % (3 * group) == 0);
    }
}

TEST_CASE("the LFE only ever states D15", "[eac3][exponents]") {
    // §5.4.3.15 makes lfeexpstr a single bit - a set is present or it is not -
    // so there is no banding to choose. Its set is two groups, 18 bits, which
    // is cheap enough that it should refresh readily.
    Exps exps{ac3::kBlocksPerFrame, ac3::kLfeEndmant, 12};
    exps.set_block(2, 4, ac3::kLfeEndmant);
    auto input = input_for(exps, ac3::kLfeEndmant);
    input.lfe = true;
    const auto plan = ac3::internal::plan_exponent_runs(input);
    CHECK(plan.count > 1);
    for (int i = 0; i < plan.count; ++i) {
        CHECK(plan.strategy[static_cast<std::size_t>(i)] == ac3::ExpStrategy::kD15);
    }
}

TEST_CASE("every plan tiles the frame exactly once", "[eac3][exponents]") {
    // The runs are what run_of_block is built from, so a gap or an overlap is
    // a block reading exponents that were never sent.
    for (int loud = 0; loud < ac3::kBlocksPerFrame; ++loud) {
        Exps exps{ac3::kBlocksPerFrame, kBins, 18};
        exps.set_block(loud, 3, kBins);
        for (const bool free_strategy : {false, true}) {
            auto input = input_for(exps, kBins);
            input.free_strategy = free_strategy;
            const auto plan = ac3::internal::plan_exponent_runs(input);
            REQUIRE(plan.count >= 1);
            CHECK(plan.starts[0] == 0);
            CHECK(plan.starts[plan.count] == ac3::kBlocksPerFrame);
            for (int i = 1; i < plan.count; ++i) {
                CHECK(plan.starts[i] > plan.starts[i - 1]);
            }
        }
    }
}

TEST_CASE("the planner's exponent model is the real encode, exactly",
          "[eac3][exponents]") {
    // banded_run_exponents predicts what encode_exponents followed by
    // decode_exponents would produce, without building either side's vectors -
    // the planner calls it a few hundred times a frame. If the two ever drift
    // apart the planner is scoring a set nobody transmits, so this pins them
    // together on shapes chosen to exercise the parts that are easy to get
    // wrong: a steep fall (slew limiting bites), a flat run (it does not), and
    // an absolute exponent above the 4-bit field's ceiling.
    std::vector<std::vector<std::uint8_t>> shapes;
    {
        std::vector<std::uint8_t> steep(static_cast<std::size_t>(kBins));
        for (std::size_t bin = 0; bin < steep.size(); ++bin) {
            steep[bin] = static_cast<std::uint8_t>(std::min<std::size_t>(bin / 4, 24));
        }
        shapes.push_back(steep);
        std::vector<std::uint8_t> flat(static_cast<std::size_t>(kBins), 9);
        shapes.push_back(flat);
        std::vector<std::uint8_t> high(static_cast<std::size_t>(kBins), 24);
        high[0] = 24;
        shapes.push_back(high);
        std::vector<std::uint8_t> ragged(static_cast<std::size_t>(kBins));
        for (std::size_t bin = 0; bin < ragged.size(); ++bin) {
            ragged[bin] = static_cast<std::uint8_t>((bin * 7 + bin / 3) % 25);
        }
        shapes.push_back(ragged);
    }
    for (const auto& shape : shapes) {
        for (const auto strategy :
             {ac3::ExpStrategy::kD15, ac3::ExpStrategy::kD25, ac3::ExpStrategy::kD45}) {
            std::vector<std::uint8_t> modelled(shape.size());
            ac3::internal::banded_run_exponents(shape, strategy, false, modelled);
            const auto coded = ac3::encode_exponents(shape, strategy);
            std::vector<std::uint8_t> decoded(shape.size());
            ac3::decode_exponents(coded.absolute, coded.groups, strategy, decoded);
            CHECK(modelled == decoded);
        }
    }
    // The coupling channel's own shape: no bin-0 absolute, an even reference,
    // and a bin count that divides by three times the group size.
    constexpr int kCplBins = 240;
    std::vector<std::uint8_t> cpl(static_cast<std::size_t>(kCplBins));
    for (std::size_t bin = 0; bin < cpl.size(); ++bin) {
        cpl[bin] = static_cast<std::uint8_t>(std::min<std::size_t>(3 + bin / 5, 24));
    }
    for (const auto strategy :
         {ac3::ExpStrategy::kD15, ac3::ExpStrategy::kD25, ac3::ExpStrategy::kD45}) {
        std::vector<std::uint8_t> modelled(cpl.size());
        ac3::internal::banded_run_exponents(cpl, strategy, true, modelled);
        const auto coded = ac3::encode_coupling_exponents(cpl, strategy);
        std::vector<std::uint8_t> decoded(cpl.size());
        ac3::decode_coupling_exponents(coded.cplabsexp, coded.groups, strategy, decoded);
        CHECK(modelled == decoded);
    }
}

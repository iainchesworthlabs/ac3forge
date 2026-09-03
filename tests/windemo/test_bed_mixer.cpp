#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "bed_mixer.hpp"

using ac3::windemo::add_to_bed;
using ac3::windemo::BedChannel;
using ac3::windemo::BedMix;
using ac3::windemo::fold_to_mono;
using Catch::Approx;

namespace {

float bed(const BedMix& mix, BedChannel channel, std::size_t frame) {
    return mix.slots[static_cast<std::size_t>(channel)][frame];
}

}  // namespace

TEST_CASE("stereo folds to the average and lands on L and R of the bed", "[windemo]") {
    const std::vector<float> stereo = {0.5F, -0.5F, 1.0F, 0.0F};  // two frames
    std::vector<float> mono(2);
    fold_to_mono(stereo, 2, mono);
    CHECK(mono[0] == Approx(0.0F));
    CHECK(mono[1] == Approx(0.5F));

    BedMix mix;
    mix.resize(2);
    add_to_bed(stereo, 2, 1.0F, mix);
    CHECK(bed(mix, BedChannel::kL, 0) == Approx(0.5F));
    CHECK(bed(mix, BedChannel::kR, 0) == Approx(-0.5F));
    CHECK(bed(mix, BedChannel::kC, 0) == 0.0F);
    CHECK(bed(mix, BedChannel::kLs, 1) == 0.0F);
}

TEST_CASE("bed contributions accumulate and take the gain", "[windemo]") {
    const std::vector<float> a = {1.0F, 1.0F};
    const std::vector<float> b = {0.5F, 0.25F};
    BedMix mix;
    mix.resize(1);
    add_to_bed(a, 2, 0.5F, mix);
    add_to_bed(b, 2, 1.0F, mix);
    CHECK(bed(mix, BedChannel::kL, 0) == Approx(1.0F));
    CHECK(bed(mix, BedChannel::kR, 0) == Approx(0.75F));
    mix.clear();
    CHECK(bed(mix, BedChannel::kL, 0) == 0.0F);
}

TEST_CASE("7.1 maps by channel, folds the rear pairs at -3 dB and drops the LFE", "[windemo]") {
    // L R C LFE Lss Rss Lrs Rrs
    const std::vector<float> frame = {0.1F, 0.2F, 0.3F, 0.9F, 0.4F, 0.5F, 0.4F, 0.5F};
    BedMix mix;
    mix.resize(1);
    add_to_bed(frame, 8, 1.0F, mix);
    CHECK(bed(mix, BedChannel::kL, 0) == Approx(0.1F));
    CHECK(bed(mix, BedChannel::kR, 0) == Approx(0.2F));
    CHECK(bed(mix, BedChannel::kC, 0) == Approx(0.3F));
    CHECK(bed(mix, BedChannel::kLs, 0) == Approx(0.70710678F * 0.8F));
    CHECK(bed(mix, BedChannel::kRs, 0) == Approx(0.70710678F * 1.0F));
    // Nothing of the 0.9 LFE reached any slot.
    float total = 0.0F;
    for (const auto& slot : mix.slots) total += slot[0];
    CHECK(total == Approx(0.1F + 0.2F + 0.3F + 0.70710678F * 1.8F));
}

TEST_CASE("5.1 maps by channel and drops the LFE", "[windemo]") {
    const std::vector<float> frame = {0.1F, 0.2F, 0.3F, 0.9F, 0.4F, 0.5F};
    BedMix mix;
    mix.resize(1);
    add_to_bed(frame, 6, 1.0F, mix);
    CHECK(bed(mix, BedChannel::kLs, 0) == Approx(0.4F));
    CHECK(bed(mix, BedChannel::kRs, 0) == Approx(0.5F));
    CHECK(bed(mix, BedChannel::kC, 0) == Approx(0.3F));
}

TEST_CASE("a mono fold of a fully driven surround signal does not clip", "[windemo]") {
    const std::vector<float> full(8, 1.0F);
    std::vector<float> mono(1);
    fold_to_mono(full, 8, mono);
    CHECK(mono[0] <= 1.0F);
    CHECK(mono[0] > 0.9F);
    const std::vector<float> full6(6, 1.0F);
    fold_to_mono(full6, 6, mono);
    CHECK(mono[0] <= 1.0F);
}

TEST_CASE("an unknown channel count is spread front left and right, never dropped", "[windemo]") {
    const std::vector<float> three = {0.3F, 0.3F, 0.3F};
    BedMix mix;
    mix.resize(1);
    add_to_bed(three, 3, 1.0F, mix);
    CHECK(bed(mix, BedChannel::kL, 0) > 0.0F);
    CHECK(bed(mix, BedChannel::kL, 0) == Approx(bed(mix, BedChannel::kR, 0)));
    std::vector<float> mono(1);
    fold_to_mono(three, 3, mono);
    CHECK(mono[0] == Approx(0.3F));
}

TEST_CASE("a short input leaves the tail of the output silent", "[windemo]") {
    const std::vector<float> one_frame = {0.5F, 0.5F};
    std::vector<float> mono(4, 9.0F);
    fold_to_mono(one_frame, 2, mono);
    CHECK(mono[0] == Approx(0.5F));
    CHECK(mono[1] == 0.0F);
    CHECK(mono[3] == 0.0F);
}

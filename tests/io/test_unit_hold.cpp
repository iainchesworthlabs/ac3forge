// The player's hold on a play's first unit, tested on the host.
//
// ac3forge/unit_hold.hpp copies a unit's blocks as the decoder delivers them
// and hands them back when the next unit starts or the play ends. What can go
// wrong is heard and reported nowhere else: a block back out of order, a block
// of the next unit taken into the hold, or a copy that still points at storage
// the decoder has reused.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <span>
#include <vector>

#include "ac3forge/unit_hold.hpp"

using ac3forge::UnitHold;

namespace {

// A block of `channels` channels and `objects` objects of `samples` samples:
// every sample of channel c is (block * 100) + c and of object o is
// (block * 100) + 50 + o, so where a sample came from can be read off it.
class TestBlock {
   public:
    TestBlock(std::size_t block, std::size_t channels, std::size_t objects, std::size_t samples) {
        for (std::size_t c = 0; c < channels; ++c) {
            data_.emplace_back(samples, static_cast<float>((block * 100) + c));
        }
        for (std::size_t o = 0; o < objects; ++o) {
            data_.emplace_back(samples, static_cast<float>((block * 100) + 50 + o));
        }
        for (std::size_t c = 0; c < channels; ++c) {
            channels_.emplace_back(data_[c]);
        }
        for (std::size_t o = 0; o < objects; ++o) {
            objects_.emplace_back(data_[channels + o]);
        }
    }
    TestBlock(const TestBlock&) = delete;
    TestBlock& operator=(const TestBlock&) = delete;

    [[nodiscard]] std::span<const std::span<const float>> channels() const { return channels_; }
    [[nodiscard]] std::span<const std::span<const float>> objects() const { return objects_; }

   private:
    std::vector<std::vector<float>> data_;
    std::vector<std::span<const float>> channels_;
    std::vector<std::span<const float>> objects_;
};

// What release() handed over for one block: every span's first sample,
// channels then objects, and the shape of the block.
struct Played {
    std::size_t position = 0;
    int index = 0;
    std::size_t channels = 0;
    std::size_t objects = 0;
    std::size_t samples = 0;
    std::vector<float> firsts;
};

std::vector<Played> release_all(UnitHold& hold) {
    std::vector<Played> played;
    hold.release([&](std::size_t position, int index,
                     std::span<const std::span<const float>> channels,
                     std::span<const std::span<const float>> objects) {
        Played p{.position = position,
                 .index = index,
                 .channels = channels.size(),
                 .objects = objects.size(),
                 .samples = channels.empty() ? 0 : channels.front().size(),
                 .firsts = {}};
        for (const auto span : channels) {
            p.firsts.push_back(span.front());
        }
        for (const auto span : objects) {
            p.firsts.push_back(span.front());
        }
        played.push_back(p);
    });
    return played;
}

}  // namespace

TEST_CASE("a unit is held and comes back in the order it was offered", "[io][unit_hold]") {
    constexpr std::size_t kChannels = 2;
    constexpr std::size_t kSamples = 4;
    std::vector<float> storage(UnitHold::storage_floats(kChannels, kSamples));
    UnitHold hold;
    REQUIRE(hold.arm(storage, kChannels, 0, kSamples));
    // Each block goes out of scope once offered, as the decoder's views do.
    for (std::size_t b = 0; b < UnitHold::kBlocks; ++b) {
        const TestBlock block(b, kChannels, 0, kSamples);
        REQUIRE(hold.offer(static_cast<int>(b), block.channels(), block.objects()));
    }
    REQUIRE(hold.held() == UnitHold::kBlocks);
    // The next unit's first block is not taken: the caller plays the held
    // unit first.
    const TestBlock next(UnitHold::kBlocks, kChannels, 0, kSamples);
    REQUIRE_FALSE(hold.offer(0, next.channels(), next.objects()));

    const auto played = release_all(hold);
    REQUIRE(played.size() == UnitHold::kBlocks);
    for (std::size_t p = 0; p < played.size(); ++p) {
        REQUIRE(played[p].position == p);
        REQUIRE(played[p].index == static_cast<int>(p));
        REQUIRE(played[p].channels == kChannels);
        REQUIRE(played[p].objects == 0);
        REQUIRE(played[p].samples == kSamples);
        for (std::size_t c = 0; c < kChannels; ++c) {
            REQUIRE(played[p].firsts[c] == static_cast<float>((p * 100) + c));
        }
    }
    REQUIRE_FALSE(hold.armed());
    REQUIRE(hold.held() == 0);
    // Released, it takes nothing more until it is armed again.
    REQUIRE_FALSE(hold.offer(1, next.channels(), next.objects()));
}

TEST_CASE("what is held is a copy, not the decoder's storage", "[io][unit_hold]") {
    std::vector<float> storage(UnitHold::storage_floats(1, 3));
    UnitHold hold;
    REQUIRE(hold.arm(storage, 1, 0, 3));
    std::array<float, 3> decoder{1.0F, 2.0F, 3.0F};
    const std::array<std::span<const float>, 1> views{std::span<const float>(decoder)};
    REQUIRE(hold.offer(0, views, {}));
    // The decoder writes the next block over its storage as soon as the call
    // that delivered this one returns.
    decoder = {9.0F, 9.0F, 9.0F};
    std::vector<float> got;
    hold.release([&](std::size_t /*position*/, int /*index*/,
                     std::span<const std::span<const float>> channels,
                     std::span<const std::span<const float>> /*objects*/) {
        got.assign(channels[0].begin(), channels[0].end());
    });
    REQUIRE(got == std::vector<float>{1.0F, 2.0F, 3.0F});
}

TEST_CASE("objects are held after the channels and come back as objects", "[io][unit_hold]") {
    std::vector<float> storage(UnitHold::storage_floats(2 + 3, 4));
    UnitHold hold;
    REQUIRE(hold.arm(storage, 2, 3, 4));
    {
        const TestBlock block(0, 2, 3, 4);
        REQUIRE(hold.offer(0, block.channels(), block.objects()));
    }
    {
        // Fewer than it was armed for is fine.
        const TestBlock block(1, 1, 2, 4);
        REQUIRE(hold.offer(1, block.channels(), block.objects()));
    }
    const auto played = release_all(hold);
    REQUIRE(played.size() == 2);
    REQUIRE(played[0].channels == 2);
    REQUIRE(played[0].objects == 3);
    REQUIRE(played[0].firsts == std::vector<float>{0.0F, 1.0F, 50.0F, 51.0F, 52.0F});
    REQUIRE(played[1].channels == 1);
    REQUIRE(played[1].objects == 2);
    REQUIRE(played[1].firsts == std::vector<float>{100.0F, 150.0F, 151.0F});
}

TEST_CASE("a block that does not fit is not held", "[io][unit_hold]") {
    std::vector<float> storage(UnitHold::storage_floats(2, 4));
    UnitHold hold;
    REQUIRE(hold.arm(storage, 2, 0, 4));

    const TestBlock wide(0, 3, 0, 4);
    REQUIRE_FALSE(hold.offer(0, wide.channels(), wide.objects()));
    const TestBlock with_objects(0, 2, 1, 4);
    REQUIRE_FALSE(hold.offer(0, with_objects.channels(), with_objects.objects()));
    const TestBlock too_long(0, 2, 0, 5);
    REQUIRE_FALSE(hold.offer(0, too_long.channels(), too_long.objects()));
    const std::array<float, 4> four{};
    const std::array<float, 3> three{};
    const std::array<std::span<const float>, 2> uneven{std::span<const float>(four),
                                                       std::span<const float>(three)};
    REQUIRE_FALSE(hold.offer(0, uneven, {}));
    REQUIRE(hold.held() == 0);

    // A unit's six blocks fit, and a seventh in the same unit does not.
    for (std::size_t b = 0; b < UnitHold::kBlocks; ++b) {
        const TestBlock block(b, 2, 0, 4);
        REQUIRE(hold.offer(static_cast<int>(b), block.channels(), block.objects()));
    }
    const TestBlock seventh(UnitHold::kBlocks, 2, 0, 4);
    REQUIRE_FALSE(hold.offer(static_cast<int>(UnitHold::kBlocks), seventh.channels(),
                             seventh.objects()));
    REQUIRE(hold.held() == UnitHold::kBlocks);
}

TEST_CASE("a play that ends inside its first unit gets back what was held", "[io][unit_hold]") {
    std::vector<float> storage(UnitHold::storage_floats(2, 4));
    UnitHold hold;
    REQUIRE(hold.arm(storage, 2, 0, 4));
    for (std::size_t b = 0; b < 3; ++b) {
        const TestBlock block(b, 2, 0, 4);
        REQUIRE(hold.offer(static_cast<int>(b), block.channels(), block.objects()));
    }
    const auto played = release_all(hold);
    REQUIRE(played.size() == 3);
    for (std::size_t p = 0; p < played.size(); ++p) {
        REQUIRE(played[p].index == static_cast<int>(p));
        REQUIRE(played[p].firsts[0] == static_cast<float>(p * 100));
    }
}

TEST_CASE("clear drops what is held without playing it", "[io][unit_hold]") {
    std::vector<float> storage(UnitHold::storage_floats(1, 2));
    UnitHold hold;
    REQUIRE(hold.arm(storage, 1, 0, 2));
    const TestBlock block(0, 1, 0, 2);
    REQUIRE(hold.offer(0, block.channels(), block.objects()));
    hold.clear();
    REQUIRE_FALSE(hold.armed());
    REQUIRE(hold.held() == 0);
    REQUIRE(release_all(hold).empty());
}

TEST_CASE("arm refuses more spans than a block can have, or too little storage",
          "[io][unit_hold]") {
    std::vector<float> storage(UnitHold::storage_floats(UnitHold::kMaxSpans + 1, 1));
    UnitHold hold;
    REQUIRE_FALSE(hold.arm(storage, UnitHold::kMaxSpans, 1, 1));
    REQUIRE_FALSE(hold.armed());
    std::vector<float> small(UnitHold::storage_floats(2, 4) - 1);
    REQUIRE_FALSE(hold.arm(small, 2, 0, 4));
    REQUIRE_FALSE(hold.armed());
    REQUIRE(hold.arm(storage, UnitHold::kMaxSpans, 0, 1));
    REQUIRE(hold.armed());
}

TEST_CASE("a hold armed again after a release starts empty", "[io][unit_hold]") {
    std::vector<float> storage(UnitHold::storage_floats(1, 2));
    UnitHold hold;
    REQUIRE(hold.arm(storage, 1, 0, 2));
    for (std::size_t b = 0; b < 2; ++b) {
        const TestBlock block(b, 1, 0, 2);
        REQUIRE(hold.offer(static_cast<int>(b), block.channels(), block.objects()));
    }
    REQUIRE(release_all(hold).size() == 2);
    REQUIRE(hold.arm(storage, 1, 0, 2));
    REQUIRE(hold.held() == 0);
    const TestBlock first(7, 1, 0, 2);
    REQUIRE(hold.offer(0, first.channels(), first.objects()));
    const auto played = release_all(hold);
    REQUIRE(played.size() == 1);
    REQUIRE(played[0].firsts[0] == 700.0F);
}

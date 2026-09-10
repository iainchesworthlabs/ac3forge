// The player's output layouts and the block renderer over them, on the host.
//
// Both headers live in the ESP-IDF component and include nothing from ESP-IDF,
// which is what makes this possible - the same arrangement test_interleave.cpp
// has. The panner's geometry is tests/spatial/'s business; what is checked
// here is the indexing between coded channels, objects and slots, where a
// swapped subscript puts the centre channel in the subwoofer and nothing
// complains.

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/oba/oamd.hpp"

#include "ac3forge/layout.hpp"
#include "ac3forge/render.hpp"

namespace {

using ac3forge::LayoutRenderer;
using ac3forge::OutputLayout;
using ac3forge::Speaker;
using Location = ac3::eac3::chanmap::Location;
using Catch::Approx;

std::vector<Location> locations_of(const OutputLayout& layout) {
    std::vector<Location> out;
    for (const Speaker& speaker : layout.speakers()) {
        REQUIRE(speaker.location.has_value());
        out.push_back(*speaker.location);
    }
    return out;
}

ac3::eac3::chanmap::Layout coded(std::uint16_t map) { return ac3::eac3::chanmap::expand(map); }

constexpr std::uint16_t k51 = ac3::eac3::chanmap::acmod_map(ac3::Acmod::k3_2, true);
constexpr std::uint16_t k71 = k51 | ac3::eac3::chanmap::k71Rear;

// A PcmBlock over constant-valued channels and objects, long enough to own
// the storage the block's spans view.
struct Block {
    std::vector<std::vector<float>> channel_storage;
    std::vector<std::vector<float>> object_storage;
    std::vector<std::span<const float>> channels;
    std::vector<std::span<const float>> objects;

    Block(std::initializer_list<float> channel_levels, std::initializer_list<float> object_levels,
          std::size_t samples = 8) {
        for (const float level : channel_levels) {
            channel_storage.emplace_back(samples, level);
        }
        for (const float level : object_levels) {
            object_storage.emplace_back(samples, level);
        }
        for (const auto& plane : channel_storage) {
            channels.emplace_back(plane);
        }
        for (const auto& plane : object_storage) {
            objects.emplace_back(plane);
        }
    }

    [[nodiscard]] ac3::PcmBlock block() const {
        return ac3::PcmBlock{.index = 0,
                             .blocks = 6,
                             .channels = channels,
                             .objects = objects,
                             .object_indices = {},
                             .object_metadata = nullptr};
    }
};

// Output storage for `slots` slots, prefilled so an unwritten slot shows.
struct Out {
    std::vector<std::vector<float>> storage;
    std::vector<std::span<float>> spans;

    explicit Out(std::size_t slots, std::size_t samples = 8, float prefill = 99.0F) {
        for (std::size_t i = 0; i < slots; ++i) {
            storage.emplace_back(samples, prefill);
        }
        for (auto& plane : storage) {
            spans.emplace_back(plane);
        }
    }

    [[nodiscard]] float at(std::size_t slot) const { return storage[slot][3]; }
};

ac3::oba::DisplayObject object_at(double x, double y, double z, double gain_db = 0.0,
                                  bool active = true) {
    ac3::oba::DisplayObject object;
    object.position = {.x = x, .y = y, .z = z};
    object.gain_db = gain_db;
    object.active = active;
    return object;
}

}  // namespace

TEST_CASE("a named layout comes out ring, heights, LFE, in Table E2.5 order", "[io][layout]") {
    const auto layout = OutputLayout::parse("7.1.4");
    REQUIRE(layout.has_value());
    REQUIRE(layout->slots() == 12);
    REQUIRE(locations_of(*layout) ==
            std::vector<Location>{Location::kLeft, Location::kCentre, Location::kRight,
                                  Location::kLeftSurround, Location::kRightSurround,
                                  Location::kLrs, Location::kRrs, Location::kVhl, Location::kVhr,
                                  Location::kLts, Location::kRts, Location::kLfe});
    REQUIRE(layout->speaker_count() == 11);
    REQUIRE(layout->lfe_count() == 1);
    REQUIRE(layout->has_height());
    REQUIRE(layout->index_of(Location::kLfe) == 11);
    REQUIRE(layout->text() == "7.1.4");
    // A name is the §7.8 stage's business only when it is 2.0 or 1.0.
    REQUIRE_FALSE(layout->fold(ac3::DownmixTarget::kLoRo).has_value());

    // With rears present the surrounds sit at the sides, as BS.2051 has 7.1.
    REQUIRE(layout->slot(3).direction.azimuth_deg == Approx(90.0));
    REQUIRE(layout->slot(7).direction.elevation_deg == Approx(ac3::spatial::kHeightElevationDeg));
    const auto five_one = OutputLayout::parse("5.1");
    REQUIRE(five_one.has_value());
    REQUIRE(five_one->slot(3).direction.azimuth_deg == Approx(110.0));
    REQUIRE_FALSE(five_one->has_height());
}

TEST_CASE("the named layouts that exist, and the ones that do not", "[io][layout]") {
    const auto wide = OutputLayout::parse("9.2.4");
    REQUIRE(wide.has_value());
    REQUIRE(wide->slots() == 15);
    REQUIRE(wide->slot(13).location == Location::kLfe);
    REQUIRE(wide->slot(14).location == Location::kLfe2);
    REQUIRE(wide->slot(7).location == Location::kLw);

    const auto no_lfe = OutputLayout::parse("5.0.4");
    REQUIRE(no_lfe.has_value());
    REQUIRE(no_lfe->slots() == 9);
    REQUIRE(no_lfe->lfe_count() == 0);

    const auto mono = OutputLayout::parse("1.0");
    REQUIRE(mono.has_value());
    REQUIRE(mono->slots() == 1);
    REQUIRE(mono->slot(0).location == Location::kCentre);
    REQUIRE(mono->fold(ac3::DownmixTarget::kLoRo) == ac3::DownmixTarget::kMono);

    const auto six_heights = OutputLayout::parse("7.1.6");
    REQUIRE(six_heights.has_value());
    REQUIRE(six_heights->slots() == 14);

    for (const std::string_view bad : {"", "5", "6.1", "5.3", "5.1.3", "5.1.", ".1", "5..1",
                                        "51", "abc", "5.1.4.2"}) {
        CAPTURE(bad);
        REQUIRE_FALSE(OutputLayout::named(bad).has_value());
    }
    // Whitespace around a name is a configuration file's, not the name's.
    REQUIRE(OutputLayout::parse(" 5.1 \n").has_value());
}

TEST_CASE("2.0 and 1.0 fold in the decoder; anything else is rendered", "[io][layout]") {
    const auto stereo = OutputLayout::stereo();
    REQUIRE(stereo.slots() == 2);
    REQUIRE(stereo.fold(ac3::DownmixTarget::kLoRo) == ac3::DownmixTarget::kLoRo);
    REQUIRE(stereo.fold(ac3::DownmixTarget::kLtRt) == ac3::DownmixTarget::kLtRt);
    REQUIRE(stereo.text() == "2.0");

    // §7.8 has no fold that keeps an LFE or places a height, so these render.
    for (const std::string_view rendered : {"2.1", "5.1", "2.0.2", "3.0", "L,R,LFE", "30/0,-30/0,lfe"}) {
        CAPTURE(rendered);
        const auto layout = OutputLayout::parse(rendered);
        REQUIRE(layout.has_value());
        REQUIRE_FALSE(layout->fold(ac3::DownmixTarget::kLoRo).has_value());
    }
    // Two speakers by angle alone are still a stereo pair.
    const auto angled = OutputLayout::parse("30/0,-30/0");
    REQUIRE(angled.has_value());
    REQUIRE(angled->fold(ac3::DownmixTarget::kLoRo) == ac3::DownmixTarget::kLoRo);
    // And a stereo DAC wired the other way round is still stereo.
    const auto swapped = OutputLayout::parse("R,L");
    REQUIRE(swapped.has_value());
    REQUIRE(swapped->fold(ac3::DownmixTarget::kLoRo) == ac3::DownmixTarget::kLoRo);
}

TEST_CASE("a speaker list is one token per slot, in slot order", "[io][layout]") {
    // A 5.1 DAC wired in WAV order rather than AC-3 order.
    const auto wav_order = OutputLayout::parse("L,R,C,LFE,Ls,Rs");
    REQUIRE(wav_order.has_value());
    REQUIRE(wav_order->slots() == 6);
    REQUIRE(wav_order->index_of(Location::kCentre) == 2);
    REQUIRE(wav_order->slot(3).kind == Speaker::Kind::kLfe);
    REQUIRE(wav_order->slot(3).location == Location::kLfe);
    REQUIRE(wav_order->text() == "L,R,C,LFE,Ls,Rs");
    REQUIRE(wav_order->slot(4).direction.azimuth_deg == Approx(110.0));

    const auto by_angle = OutputLayout::parse("30/0, -30/0 ,lfe,-");
    REQUIRE(by_angle.has_value());
    REQUIRE(by_angle->slots() == 4);
    REQUIRE(by_angle->slot(0).kind == Speaker::Kind::kSpeaker);
    REQUIRE(by_angle->slot(0).direction.azimuth_deg == Approx(30.0));
    REQUIRE_FALSE(by_angle->slot(0).location.has_value());
    REQUIRE(by_angle->slot(1).direction.azimuth_deg == Approx(-30.0));
    REQUIRE(by_angle->slot(2).kind == Speaker::Kind::kLfe);
    REQUIRE(by_angle->slot(3).kind == Speaker::Kind::kEmpty);
    REQUIRE(by_angle->speaker_count() == 2);
    REQUIRE(by_angle->lfe_count() == 1);

    const auto heights = OutputLayout::parse("45/45,-45/45");
    REQUIRE(heights.has_value());
    REQUIRE(heights->has_height());

    // Case and spacing are the configuration's, not the layout's.
    const auto loose = OutputLayout::parse(" l , r ");
    REQUIRE(loose.has_value());
    REQUIRE(loose->index_of(Location::kRight) == 1);
    REQUIRE(OutputLayout::parse("vhl,VHR,lts,Rts")->slots() == 4);

    for (const std::string_view bad :
         {"L,L", "L,,R", "foo", "30/", "/0", "1/2/3", "30/95", "-", "-,-", "L,R,"}) {
        CAPTURE(bad);
        REQUIRE_FALSE(OutputLayout::parse(bad).has_value());
    }
    // Seventeen slots is one more than a TDM line, a rendered programme or
    // the panner allows.
    REQUIRE(OutputLayout::parse("-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,L").has_value());
    REQUIRE_FALSE(OutputLayout::parse("-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,L").has_value());
}

TEST_CASE("a bed whose locations the layout has goes to them exactly", "[io][layout][render]") {
    LayoutRenderer same{*OutputLayout::parse("5.1")};
    same.set_bed(coded(k51));
    REQUIRE(same.bed_channels() == 6);
    for (std::size_t c = 0; c < 6; ++c) {
        for (std::size_t slot = 0; slot < 6; ++slot) {
            CAPTURE(c, slot);
            REQUIRE(same.bed_gain(c, slot) == (c == slot ? 1.0F : 0.0F));
        }
    }

    // The same 5.1 onto a DAC wired in WAV order: a permutation, still exact.
    LayoutRenderer permuted{*OutputLayout::parse("L,R,C,LFE,Ls,Rs")};
    permuted.set_bed(coded(k51));
    const std::array<std::size_t, 6> expected_slot = {0, 2, 1, 4, 5, 3};  // L C R Ls Rs LFE
    for (std::size_t c = 0; c < 6; ++c) {
        for (std::size_t slot = 0; slot < 6; ++slot) {
            CAPTURE(c, slot);
            REQUIRE(permuted.bed_gain(c, slot) == (slot == expected_slot[c] ? 1.0F : 0.0F));
        }
    }
}

TEST_CASE("a channel the layout lacks is panned at unit power, never into the LFE",
          "[io][layout][render]") {
    // 7.1 into a 5.1 room: L C R Ls Rs Lrs Rrs LFE. The five the room has go
    // exactly; the rears spread over the surrounds.
    LayoutRenderer renderer{*OutputLayout::parse("5.1")};
    renderer.set_bed(coded(k71));
    REQUIRE(renderer.bed_channels() == 8);
    REQUIRE(renderer.bed_gain(3, 3) == 1.0F);  // Ls to Ls, though 7.1 puts it at 90 and 5.1 at 110
    REQUIRE(renderer.bed_gain(7, 5) == 1.0F);  // LFE to LFE
    for (const std::size_t rear : {std::size_t{5}, std::size_t{6}}) {
        CAPTURE(rear);
        double power = 0.0;
        for (std::size_t slot = 0; slot < 6; ++slot) {
            power += static_cast<double>(renderer.bed_gain(rear, slot)) *
                     static_cast<double>(renderer.bed_gain(rear, slot));
        }
        REQUIRE(power == Approx(1.0).margin(1e-5));
        REQUIRE(renderer.bed_gain(rear, 5) == 0.0F);  // nothing panned reaches the LFE
        REQUIRE(renderer.bed_gain(rear, 1) == 0.0F);  // and a rear never reaches the centre
    }
    // Lrs at 150 degrees lies between Ls (110) and Rs (-110) on the ring, and
    // nearer Ls.
    REQUIRE(renderer.bed_gain(5, 3) > renderer.bed_gain(5, 4));
    REQUIRE(renderer.bed_gain(5, 4) > 0.0F);
    REQUIRE(renderer.bed_gain(6, 4) > renderer.bed_gain(6, 3));

    // A room with no speaker at all for a channel by angle: 5.1 onto two
    // angled speakers, every channel spread over the pair at unit power.
    LayoutRenderer pair{*OutputLayout::parse("30/0,-30/0,lfe")};
    pair.set_bed(coded(k51));
    for (std::size_t c = 0; c < 5; ++c) {
        CAPTURE(c);
        const auto left = static_cast<double>(pair.bed_gain(c, 0));
        const auto right = static_cast<double>(pair.bed_gain(c, 1));
        const double power = (left * left) + (right * right);
        REQUIRE(power == Approx(1.0).margin(1e-5));
        REQUIRE(pair.bed_gain(c, 2) == 0.0F);
    }
    REQUIRE(pair.bed_gain(5, 2) == 1.0F);
    REQUIRE(pair.bed_gain(0, 0) == Approx(1.0F));  // L at 30 is exactly the first speaker
}

TEST_CASE("the LFE feeds", "[io][layout][render]") {
    // Two feeds in the room, one coded: the first feed gets it, LFE2 does not.
    LayoutRenderer two_feeds{*OutputLayout::parse("9.2.4")};
    two_feeds.set_bed(coded(k51));
    REQUIRE(two_feeds.bed_gain(5, 13) == 1.0F);
    REQUIRE(two_feeds.bed_gain(5, 14) == 0.0F);

    // Two coded, two feeds: each to its own.
    two_feeds.set_bed(coded(static_cast<std::uint16_t>(k51 | ac3::eac3::chanmap::kLfe2Bit)));
    const int lfe2 = two_feeds.layout().index_of(Location::kLfe2);
    REQUIRE(lfe2 == 14);
    // Coded order puts LFE2 before LFE (bits 14 and 15).
    REQUIRE(two_feeds.bed_gain(5, 14) == 1.0F);
    REQUIRE(two_feeds.bed_gain(5, 13) == 0.0F);
    REQUIRE(two_feeds.bed_gain(6, 13) == 1.0F);
    REQUIRE(two_feeds.bed_gain(6, 14) == 0.0F);

    // Two coded, one feed: both arrive on it.
    LayoutRenderer one_feed{*OutputLayout::parse("5.1")};
    one_feed.set_bed(coded(static_cast<std::uint16_t>(k51 | ac3::eac3::chanmap::kLfe2Bit)));
    REQUIRE(one_feed.bed_gain(5, 5) == 1.0F);
    REQUIRE(one_feed.bed_gain(6, 5) == 1.0F);

    // An unnamed feed ("lfe" in a list) takes the LFE like a named one.
    LayoutRenderer unnamed{*OutputLayout::parse("L,R,lfe")};
    unnamed.set_bed(coded(k51));
    REQUIRE(unnamed.bed_gain(5, 2) == 1.0F);
}

TEST_CASE("objects are placed by their positions and their gains", "[io][layout][render]") {
    LayoutRenderer renderer{*OutputLayout::parse("5.1.4")};
    // Slots: L C R Ls Rs Vhl Vhr Lts Rts LFE.
    const std::array<ac3::oba::DisplayObject, 4> objects = {
        object_at(0.5, 0.0, 0.0),          // the front wall's centre: C, exactly
        object_at(0.5, 0.5, 1.0),          // the ceiling's centre: the two front heights
        object_at(0.5, 0.0, 0.0, -6.0206), // as the first, 6 dB down
        object_at(0.5, 0.0, 0.0, 0.0, false),  // inactive: silent
    };
    renderer.set_objects(objects);
    REQUIRE(renderer.object_count() == 4);

    REQUIRE(renderer.object_gain(0, 1) == Approx(1.0F));
    for (const std::size_t other : {0U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U}) {
        CAPTURE(other);
        REQUIRE(renderer.object_gain(0, other) == Approx(0.0F).margin(1e-6));
    }
    // Directly overhead reads as the front of the upper ring, between Vhl and
    // Vhr, at constant power.
    REQUIRE(renderer.object_gain(1, 5) == Approx(0.70710678F).margin(1e-5));
    REQUIRE(renderer.object_gain(1, 6) == Approx(0.70710678F).margin(1e-5));
    REQUIRE(renderer.object_gain(1, 1) == Approx(0.0F).margin(1e-6));
    REQUIRE(renderer.object_gain(1, 9) == 0.0F);  // never the LFE
    REQUIRE(renderer.object_gain(2, 1) == Approx(0.5F).margin(1e-4));
    for (std::size_t slot = 0; slot < 10; ++slot) {
        REQUIRE(renderer.object_gain(3, slot) == 0.0F);
    }

    // Seventeen described: the sixteen JOC can carry are placed, no more.
    std::vector<ac3::oba::DisplayObject> many(17, object_at(0.5, 0.0, 0.0));
    renderer.set_objects(many);
    REQUIRE(renderer.object_count() == 16);
}

TEST_CASE("render sums the objects into the slots and passes the bed's LFE", "[io][layout][render]") {
    LayoutRenderer renderer{*OutputLayout::parse("5.1.4")};
    renderer.set_bed(coded(k51));
    const std::array<ac3::oba::DisplayObject, 2> objects = {
        object_at(0.5, 0.0, 0.0),  // C
        object_at(0.5, 0.5, 1.0),  // Vhl and Vhr at 0.7071
    };
    renderer.set_objects(objects);

    // Bed channels L C R Ls Rs at 0.3, LFE at 0.1; objects at 0.5 and 0.2.
    const Block source({0.3F, 0.3F, 0.3F, 0.3F, 0.3F, 0.1F}, {0.5F, 0.2F});

    Out with_objects(10);
    renderer.render(source.block(), true, 1.0F, with_objects.spans);
    REQUIRE(with_objects.at(1) == Approx(0.5F));                 // C: the first object
    REQUIRE(with_objects.at(5) == Approx(0.2F * 0.70710678F));   // Vhl: the second
    REQUIRE(with_objects.at(6) == Approx(0.2F * 0.70710678F));   // Vhr
    REQUIRE(with_objects.at(9) == Approx(0.1F));                 // the bed's LFE, through
    REQUIRE(with_objects.at(0) == 0.0F);  // the bed's L is NOT added: the bed is the objects' fold
    REQUIRE(with_objects.at(3) == 0.0F);
    REQUIRE(with_objects.at(7) == 0.0F);

    // The same block with the objects declined: the bed, placed.
    Out bed_only(10);
    renderer.render(source.block(), false, 1.0F, bed_only.spans);
    for (std::size_t slot = 0; slot < 5; ++slot) {
        CAPTURE(slot);
        REQUIRE(bed_only.at(slot) == Approx(0.3F));
    }
    REQUIRE(bed_only.at(9) == Approx(0.1F));
    for (std::size_t slot = 5; slot < 9; ++slot) {
        REQUIRE(bed_only.at(slot) == 0.0F);  // heights: nothing coded reaches them
    }

    // A unit without objects renders the bed whatever was asked for.
    const Block no_objects({0.3F, 0.3F, 0.3F, 0.3F, 0.3F, 0.1F}, {});
    Out asked_anyway(10);
    renderer.render(no_objects.block(), true, 1.0F, asked_anyway.spans);
    REQUIRE(asked_anyway.at(0) == Approx(0.3F));
    REQUIRE(asked_anyway.at(9) == Approx(0.1F));

    // The gain applies to everything, objects and LFE alike.
    Out quieter(10);
    renderer.render(source.block(), true, 0.5F, quieter.spans);
    REQUIRE(quieter.at(1) == Approx(0.25F));
    REQUIRE(quieter.at(9) == Approx(0.05F));
}

TEST_CASE("every slot is written, including the ones nothing reaches", "[io][layout][render]") {
    // A bus with an unconnected slot in the middle: the prefill must not
    // survive, or the DAC clocks out whatever the last block left there.
    LayoutRenderer renderer{*OutputLayout::parse("L,-,R,LFE")};
    renderer.set_bed(coded(ac3::eac3::chanmap::acmod_map(ac3::Acmod::k2_0, false)));
    const Block stereo({0.4F, 0.6F}, {});
    Out out(4);
    renderer.render(stereo.block(), false, 1.0F, out.spans);
    REQUIRE(out.at(0) == Approx(0.4F));
    REQUIRE(out.at(1) == 0.0F);
    REQUIRE(out.at(2) == Approx(0.6F));
    REQUIRE(out.at(3) == 0.0F);  // an LFE feed with nothing coded for it
}

TEST_CASE("a folded block goes to the speakers by name, then by order", "[io][layout][render]") {
    // A stereo DAC wired R then L still plays the right way round.
    LayoutRenderer swapped{*OutputLayout::parse("R,L")};
    const Block stereo({0.1F, 0.2F}, {});
    Out out(2);
    swapped.render_folded(stereo.block(), 1.0F, out.spans);
    REQUIRE(out.at(0) == Approx(0.2F));
    REQUIRE(out.at(1) == Approx(0.1F));

    // Angles carry no names, so the channels go in slot order.
    LayoutRenderer angled{*OutputLayout::parse("30/0,-30/0")};
    Out by_order(2);
    angled.render_folded(stereo.block(), 1.0F, by_order.spans);
    REQUIRE(by_order.at(0) == Approx(0.1F));
    REQUIRE(by_order.at(1) == Approx(0.2F));

    // Mono: the one speaker.
    LayoutRenderer mono{*OutputLayout::parse("1.0")};
    const Block one({0.7F}, {});
    Out single(1);
    mono.render_folded(one.block(), 0.5F, single.spans);
    REQUIRE(single.at(0) == Approx(0.35F));

    // A layout with a gap: the fold's two channels skip the empty slot and it
    // is still zeroed.
    LayoutRenderer gapped{*OutputLayout::parse("30/0,-,-30/0")};
    Out three(3);
    gapped.render_folded(stereo.block(), 1.0F, three.spans);
    REQUIRE(three.at(0) == Approx(0.1F));
    REQUIRE(three.at(1) == 0.0F);
    REQUIRE(three.at(2) == Approx(0.2F));
}

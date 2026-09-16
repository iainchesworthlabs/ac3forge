#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include "ac3/audio/speakers.hpp"
#include "ac3/core/eac3_tables.hpp"

// ac3::audio's speaker mask: WAVEFORMATEXTENSIBLE's positions against the
// renderer's locations (src/audio/src/speakers.cpp).
//
// Nothing here touches a device. The one judgement worth testing at all is the
// pair of names that depends on the company it keeps: SPEAKER_BACK_LEFT and
// SPEAKER_BACK_RIGHT are a 5.1 ring's surrounds on their own and a 7.1 room's
// rear surrounds beside SPEAKER_SIDE_*, which is how ITU-R BS.2051 lays 7.1
// out - read the wrong way round, a 7.1 device would be handed its side
// channels to its rear speakers.

namespace {

using ac3::eac3::chanmap::Location;

std::vector<Location> locations(std::uint32_t mask) {
    return ac3::audio::locations_of(mask);
}

}  // namespace

TEST_CASE("speakers: a mask's positions are the renderer's locations, in interleave order",
          "[audio-backend][speakers]") {
    using namespace ac3::audio;

    CHECK(speaker_count(0) == 0);
    CHECK(speaker_count(kSpeakersStereo) == 2);
    CHECK(speaker_count(kSpeakers5_1) == 6);
    CHECK(speaker_count(kSpeakers7_1) == 8);
    CHECK(speaker_count(kSpeakers7_1_4) == 12);

    // WAVE's own order: FL FR FC LFE BL BR.
    CHECK(locations(kSpeakersStereo) == std::vector{Location::kLeft, Location::kRight});
    CHECK(locations(kSpeakers5_1) == std::vector{Location::kLeft, Location::kRight, Location::kCentre,
                                                 Location::kLfe, Location::kLeftSurround,
                                                 Location::kRightSurround});
    // With the sides present the backs are the rears, and the surrounds move
    // to the sides: FL FR FC LFE BL BR SL SR.
    CHECK(locations(kSpeakers7_1) ==
          std::vector{Location::kLeft, Location::kRight, Location::kCentre, Location::kLfe,
                      Location::kLrs, Location::kRrs, Location::kLeftSurround,
                      Location::kRightSurround});
    CHECK(locations(kSpeakers7_1_4) ==
          std::vector{Location::kLeft, Location::kRight, Location::kCentre, Location::kLfe,
                      Location::kLrs, Location::kRrs, Location::kLeftSurround,
                      Location::kRightSurround, Location::kVhl, Location::kVhr, Location::kLts,
                      Location::kRts});
    CHECK(locations(kSpeakers5_1_4) ==
          std::vector{Location::kLeft, Location::kRight, Location::kCentre, Location::kLfe,
                      Location::kLeftSurround, Location::kRightSurround, Location::kVhl,
                      Location::kVhr, Location::kLts, Location::kRts});
    CHECK(locations(0).empty());
}

TEST_CASE("speakers: one bit at a time, and the bits with no location", "[audio-backend][speakers]") {
    using namespace ac3::audio;

    CHECK(location_of(kSpeakerFrontLeft) == Location::kLeft);
    CHECK(location_of(kSpeakerFrontCentre) == Location::kCentre);
    CHECK(location_of(kSpeakerLowFrequency) == Location::kLfe);
    CHECK(location_of(kSpeakerBackCentre) == Location::kCs);
    CHECK(location_of(kSpeakerTopCentre) == Location::kTs);
    CHECK(location_of(kSpeakerFrontRightOfCentre) == Location::kRc);
    // A 5.1 ring's surrounds on their own.
    CHECK(location_of(kSpeakerBackLeft) == Location::kLeftSurround);

    // Table E2.5 has no position above and behind, and SPEAKER_ALL names every
    // speaker without naming one.
    CHECK_FALSE(location_of(kSpeakerTopBackCentre).has_value());
    CHECK_FALSE(location_of(0).has_value());
    CHECK_FALSE(location_of(kSpeakersStereo).has_value());
    CHECK_FALSE(location_of(0x80000000).has_value());

    // The positions WAVEFORMATEXTENSIBLE cannot name.
    CHECK(speaker_of(Location::kLw) == 0);
    CHECK(speaker_of(Location::kRw) == 0);
    CHECK(speaker_of(Location::kLsd) == 0);
    CHECK(speaker_of(Location::kRsd) == 0);
    CHECK(speaker_of(Location::kLfe2) == 0);
    CHECK(speaker_of(Location::kLeft) == kSpeakerFrontLeft);
    CHECK(speaker_of(Location::kLrs) == kSpeakerBackLeft);
}

TEST_CASE("speakers: a layout's locations make the mask that reads them back",
          "[audio-backend][speakers]") {
    using namespace ac3::audio;

    const std::vector<Location> five_one{Location::kLeft,          Location::kRight,
                                         Location::kCentre,        Location::kLfe,
                                         Location::kLeftSurround,  Location::kRightSurround};
    CHECK(speakers_of(five_one) == kSpeakers5_1);
    CHECK(locations_of(speakers_of(five_one)) == five_one);

    // A room with rear surrounds puts the surrounds at the sides, so eight
    // locations still make eight bits rather than colliding on the back pair.
    std::vector<Location> seven_one = five_one;
    seven_one.push_back(Location::kLrs);
    seven_one.push_back(Location::kRrs);
    CHECK(speakers_of(seven_one) == kSpeakers7_1);
    CHECK(speaker_count(speakers_of(seven_one)) == 8);

    // Locations with no position of their own leave the mask alone.
    CHECK(speakers_of(std::vector{Location::kLw, Location::kRw}) == 0);
    CHECK(speakers_of({}) == 0);
}

TEST_CASE("speakers: the arrangement a width implies, and the widths it does not",
          "[audio-backend][speakers]") {
    using namespace ac3::audio;

    CHECK(default_speakers(1) == kSpeakersMono);
    CHECK(default_speakers(2) == kSpeakersStereo);
    CHECK(default_speakers(4) == kSpeakersQuad);
    CHECK(default_speakers(6) == kSpeakers5_1);
    CHECK(default_speakers(8) == kSpeakers7_1);
    CHECK(default_speakers(12) == kSpeakers7_1_4);
    for (const std::uint16_t width : std::array<std::uint16_t, 9>{0, 3, 5, 7, 9, 10, 11, 14, 16}) {
        // Ten channels is 5.1.4 or 7.1.2 and fourteen 7.1.6 or 9.1.4; a device
        // that does not say cannot be guessed at.
        CHECK(default_speakers(width) == 0);
    }
    CHECK(speaker_count(default_speakers(6)) == 6);
    CHECK(speaker_count(default_speakers(8)) == 8);
    CHECK(speaker_count(default_speakers(12)) == 12);
}

TEST_CASE("speakers: a mask reads back as the names a report shows", "[audio-backend][speakers]") {
    using namespace ac3::audio;

    CHECK(describe_speakers(kSpeakersStereo) == "L R");
    CHECK(describe_speakers(kSpeakers5_1) == "L R C LFE Ls Rs");
    CHECK(describe_speakers(kSpeakers7_1) == "L R C LFE Lrs Rrs Ls Rs");
    CHECK(describe_speakers(kSpeakers7_1_4) == "L R C LFE Lrs Rrs Ls Rs Vhl Vhr Lts Rts");
    CHECK(describe_speakers(0).empty());
    // A position with no location is counted rather than dropped silently.
    CHECK(describe_speakers(kSpeakerTopBackCentre) == "+1");
    CHECK(describe_speakers(kSpeakersStereo | kSpeakerTopBackCentre) == "L R +1");
}

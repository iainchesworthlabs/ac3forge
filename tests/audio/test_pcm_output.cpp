#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ac3/audio/pcm_output.hpp"
#include "ac3/audio/speakers.hpp"

// ac3::audio::PcmOutput's two decisions, against fake device records
// (src/audio/src/pcm_output.cpp): how wide to open the stream, and which
// output each rendered channel goes to.
//
// No device is opened here. Both decisions are made from a RenderDeviceInfo,
// which is a plain record the enumeration fills in, so a fake one puts every
// case - an 8-channel HDMI endpoint, a stereo jack, a backend that cannot say
// how many channels it has - in front of the same code the real enumeration
// feeds. The scatter itself belongs to ac3::render::Routing and is tested in
// tests/render/test_routing.cpp; playing through a real device is
// test_monitor_live.cpp's [.][monitor-live] case and the receiver.

namespace {

using ac3::audio::PcmOutput;
using ac3::audio::RenderDeviceInfo;
using ac3::audio::speaker_routing;
using ac3::eac3::chanmap::Location;
using ac3::render::OutputLayout;
using ac3::render::Routing;

RenderDeviceInfo device_of(std::uint16_t channels, std::uint32_t speakers) {
    RenderDeviceInfo device;
    device.id = "fake";
    device.name = "Fake output";
    device.channels = channels;
    device.speakers = speakers;
    return device;
}

// The location of the slot patched to `output`, to read a patch back the way
// a listener hears it: which speaker of the room a given output drives.
std::optional<Location> at_output(const Routing& patch, const OutputLayout& layout,
                                  std::size_t output) {
    const int slot = patch.channel_of(output);
    if (slot == Routing::kUnassigned) {
        return std::nullopt;
    }
    return layout.slot(static_cast<std::size_t>(slot)).location;
}

}  // namespace

TEST_CASE("pcm output: the stream is as wide as the device says it is",
          "[audio-backend][pcm-output]") {
    using ac3::audio::output_width;

    // An 8-channel HDMI endpoint playing a 5.1 programme: eight outputs, not
    // six handed to a mixer to spread.
    CHECK(output_width(device_of(8, ac3::audio::kSpeakers7_1), 6) == 8);
    // And narrower than the programme, which is the fold the caller has
    // already been told about (RenderDeviceInfo::channels' own comment).
    CHECK(output_width(device_of(2, ac3::audio::kSpeakersStereo), 6) == 2);
    // 0 is "the backend cannot say", not "no channels": the caller's own
    // width is then the only number there is.
    CHECK(output_width(device_of(0, 0), 6) == 6);
    CHECK(output_width(device_of(0, 0), 2) == 2);
}

TEST_CASE("pcm output: the default patch puts each channel on its own speaker's output",
          "[audio-backend][pcm-output]") {
    const auto layout = OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    REQUIRE(layout->slots() == 6);

    // A 7.1 device, and the case that makes counting outputs off from zero
    // the wrong answer: a 5.1 programme's surrounds are the room's SIDE pair
    // (speakers.hpp promotes SPEAKER_BACK_* to the rear surrounds when
    // SPEAKER_SIDE_* is there beside it, which is how BS.2051 lays 7.1 out),
    // and in the mask's own bit order the side pair comes AFTER the rear
    // pair - outputs 6 and 7, not 4 and 5. Patched by index, a 5.1 stream
    // would come out of this device's rear speakers.
    const auto patch = speaker_routing(*layout, ac3::audio::kSpeakers7_1, 8);
    CHECK(patch.channels() == 6);
    CHECK(patch.outputs() == 8);
    CHECK(at_output(patch, *layout, 0) == Location::kLeft);
    CHECK(at_output(patch, *layout, 1) == Location::kRight);
    CHECK(at_output(patch, *layout, 2) == Location::kCentre);
    CHECK(at_output(patch, *layout, 3) == Location::kLfe);
    CHECK(at_output(patch, *layout, 6) == Location::kLeftSurround);
    CHECK(at_output(patch, *layout, 7) == Location::kRightSurround);
    // The rear surrounds a 5.1 programme has nothing for stay silent.
    CHECK_FALSE(at_output(patch, *layout, 4).has_value());
    CHECK_FALSE(at_output(patch, *layout, 5).has_value());
    // Nothing rendered is dropped: every slot found an output.
    CHECK(patch.unpatched_channels() == 0);
}

TEST_CASE("pcm output: a speaker the device has not got is left unpatched",
          "[audio-backend][pcm-output]") {
    const auto layout = OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());

    // A stereo endpoint: the front pair plays, and the centre, LFE and
    // surrounds have nowhere to go. Audible silence is the wrong answer for a
    // player, which is why the caller is told - it should fold to stereo
    // first (§7.8), and RenderDeviceInfo::channels is what tells it to.
    const auto patch = speaker_routing(*layout, ac3::audio::kSpeakersStereo, 2);
    CHECK(at_output(patch, *layout, 0) == Location::kLeft);
    CHECK(at_output(patch, *layout, 1) == Location::kRight);
    // Read in the layout's own slot order, which is the coded one - L C R Ls
    // Rs LFE - so the two that play are slots 0 and 2, not 0 and 1.
    CHECK(patch.unpatched_channels() == 0b111010);
}

TEST_CASE("pcm output: with no mask the width's own arrangement is assumed",
          "[audio-backend][pcm-output]") {
    const auto layout = OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());

    // A backend that cannot say which speakers a six-channel device has: 5.1
    // is the only arrangement six channels fits, so the patch comes out the
    // same as the mask would have made it. Counting outputs off from zero
    // instead would send the centre to the right speaker and the LFE feed to
    // a surround, the layout's slots being in the coded order and a device's
    // outputs in WAVEFORMATEXTENSIBLE's.
    const auto patch = speaker_routing(*layout, /*speakers=*/0, 6);
    CHECK(at_output(patch, *layout, 0) == Location::kLeft);
    CHECK(at_output(patch, *layout, 1) == Location::kRight);
    CHECK(at_output(patch, *layout, 2) == Location::kCentre);
    CHECK(at_output(patch, *layout, 3) == Location::kLfe);
    CHECK(at_output(patch, *layout, 4) == Location::kLeftSurround);
    CHECK(at_output(patch, *layout, 5) == Location::kRightSurround);
    CHECK(patch.unpatched_channels() == 0);
    CHECK(patch == speaker_routing(*layout, ac3::audio::kSpeakers5_1, 6));

    // Ten channels is 5.1.4 or 7.1.2 and a bare width does not say which, so
    // there is nothing to match against and the identity is what is left.
    const auto wide = OutputLayout::parse("7.1.2");
    REQUIRE(wide.has_value());
    REQUIRE(wide->slots() == 10);
    const auto guessed = speaker_routing(*wide, /*speakers=*/0, 10);
    for (std::size_t channel = 0; channel < guessed.channels(); ++channel) {
        CHECK(guessed.output_of(channel) == static_cast<int>(channel));
    }
}

TEST_CASE("pcm output: a slot placed by angle is the caller's patch to make",
          "[audio-backend][pcm-output]") {
    // Two named speakers and one placed by angle: the named pair is matched
    // by speaker, and the third is left for a patch of the caller's own
    // rather than given an output on a guess.
    const auto layout = OutputLayout::parse("L,R,110/0");
    REQUIRE(layout.has_value());
    REQUIRE(layout->slots() == 3);

    const auto patch = speaker_routing(*layout, ac3::audio::kSpeakers5_1, 6);
    CHECK(at_output(patch, *layout, 0) == Location::kLeft);
    CHECK(at_output(patch, *layout, 1) == Location::kRight);
    CHECK(patch.output_of(2) == Routing::kUnassigned);
    CHECK(patch.unpatched_channels() == 0b100);
}

TEST_CASE("pcm output: nothing is submitted or patched while it is closed",
          "[audio-backend][pcm-output]") {
    PcmOutput output;
    CHECK_FALSE(output.running());
    CHECK_FALSE(output.paused());
    CHECK_FALSE(output.position().has_value());
    CHECK(output.info().outputs == 0);
    CHECK(output.stats().frames_submitted == 0);

    std::vector<float> left(480, 0.5F);
    std::vector<float> right(480, -0.5F);
    const std::array<std::span<const float>, 2> rendered{left, right};
    CHECK_FALSE(output.submit(rendered, 480));

    // A patch cannot be installed before there is a stream for it to be
    // about: its output count has to match one.
    const auto patch = Routing::identity(2, 2);
    REQUIRE(patch.has_value());
    CHECK_FALSE(output.set_routing(*patch));

    // flush() and stop() on a closed output do nothing rather than fail.
    output.flush();
    output.stop();
    CHECK_FALSE(output.running());
}

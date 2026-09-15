// How a layout is served (ac3/render/serving.hpp): the decoder's fold or the
// renderer, and whether objects are reconstructed - the ESP32 player's policy,
// now shared with every player that renders.

#include <catch2/catch_test_macros.hpp>
#include <string_view>

#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/render/serving.hpp"

namespace {

using ac3::DownmixTarget;
using ac3::render::ObjectsPolicy;
using ac3::render::OutputLayout;
using ac3::render::serve;

}  // namespace

TEST_CASE("a stereo or mono room folds and never reconstructs", "[render][serving]") {
    for (const ObjectsPolicy policy :
         {ObjectsPolicy::kAuto, ObjectsPolicy::kNever, ObjectsPolicy::kAlways}) {
        CAPTURE(static_cast<int>(policy));
        const auto lo_ro = serve(OutputLayout::stereo(), DownmixTarget::kLoRo, policy);
        REQUIRE(lo_ro.fold == DownmixTarget::kLoRo);
        REQUIRE_FALSE(lo_ro.reconstruct);

        const auto lt_rt = serve(*OutputLayout::parse("R,L"), DownmixTarget::kLtRt, policy);
        REQUIRE(lt_rt.fold == DownmixTarget::kLtRt);
        REQUIRE_FALSE(lt_rt.reconstruct);

        const auto mono = serve(*OutputLayout::parse("1.0"), DownmixTarget::kLoRo, policy);
        REQUIRE(mono.fold == DownmixTarget::kMono);
        REQUIRE_FALSE(mono.reconstruct);
    }
}

TEST_CASE("a rendered room reconstructs objects by its policy", "[render][serving]") {
    const auto five_one = *OutputLayout::parse("5.1");
    const auto height = *OutputLayout::parse("5.1.4");

    // kAuto: only where the bed cannot serve, which is a room with heights.
    REQUIRE_FALSE(serve(five_one, DownmixTarget::kLoRo, ObjectsPolicy::kAuto).reconstruct);
    REQUIRE(serve(height, DownmixTarget::kLoRo, ObjectsPolicy::kAuto).reconstruct);
    REQUIRE_FALSE(serve(five_one, DownmixTarget::kLoRo, ObjectsPolicy::kAuto).fold.has_value());

    REQUIRE(serve(five_one, DownmixTarget::kLoRo, ObjectsPolicy::kAlways).reconstruct);
    REQUIRE_FALSE(serve(height, DownmixTarget::kLoRo, ObjectsPolicy::kNever).reconstruct);

    // Two speakers with an LFE are rendered, not folded (§7.8 keeps no LFE).
    const auto two_one =
        serve(*OutputLayout::parse("2.1"), DownmixTarget::kLoRo, ObjectsPolicy::kAlways);
    REQUIRE_FALSE(two_one.fold.has_value());
    REQUIRE(two_one.reconstruct);
}

TEST_CASE("the serving decision sets the decoder's target and object switch", "[render][serving]") {
    ac3::DecoderConfig config;
    config.output.mode = ac3::OperatingMode::kLine;
    config.drc_scale = 0.5;

    ac3::render::configure_decoder(
        serve(OutputLayout::stereo(), DownmixTarget::kLtRt, ObjectsPolicy::kAuto), config);
    REQUIRE(config.output.target == DownmixTarget::kLtRt);
    REQUIRE(config.skip_object_reconstruction);

    ac3::render::configure_decoder(
        serve(*OutputLayout::parse("7.1.4"), DownmixTarget::kLoRo, ObjectsPolicy::kAuto), config);
    REQUIRE(config.output.target == DownmixTarget::kAsCoded);
    REQUIRE_FALSE(config.skip_object_reconstruction);

    // Everything else is the caller's.
    REQUIRE(config.output.mode == ac3::OperatingMode::kLine);
    REQUIRE(config.drc_scale == 0.5);
}

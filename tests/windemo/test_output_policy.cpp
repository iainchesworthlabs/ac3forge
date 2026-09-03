#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "output_policy.hpp"

// The mode table from docs/platforms/windows-demo.md ("Output modes and hot
// switching"), including the S1 rule that a bitstream mode is never chosen
// on the endpoint applications render to.

using ac3::windemo::choose_output;
using ac3::windemo::EndpointFacts;
using ac3::windemo::OutputMode;
using ac3::windemo::OutputPolicyInput;

namespace {

EndpointFacts avr(bool is_default = false) {
    return {.id = "avr", .name = "Denon AVR", .is_default = is_default, .accepts_eac3 = true,
            .accepts_ac3 = true, .shared_channels = 8};
}

EndpointFacts dd_only() {
    return {.id = "spdif", .name = "Optical", .accepts_ac3 = true, .shared_channels = 2};
}

EndpointFacts tv() {
    return {.id = "tv", .name = "TV", .shared_channels = 6};
}

EndpointFacts headphones(bool spatial = true, bool is_default = false) {
    return {.id = "hp", .name = "Realtek", .is_default = is_default, .shared_channels = 2,
            .spatial = spatial, .spatial_max_objects = spatial ? 16U : 0U};
}

EndpointFacts null_sink(bool is_default = true) {
    return {.id = "null", .name = "Desktop Atmos Speakers", .is_default = is_default,
            .is_null_sink = true, .shared_channels = 8};
}

}  // namespace

TEST_CASE("an Atmos receiver with a key gets Atmos", "[windemo]") {
    const std::vector<EndpointFacts> endpoints = {null_sink(), avr(), headphones()};
    const auto choice = choose_output({.endpoints = endpoints, .signing_key_loaded = true, .pinned = std::nullopt});
    CHECK(choice.mode == OutputMode::kAtmos);
    CHECK(choice.endpoint_id == "avr");
    CHECK_FALSE(choice.reason.empty());
}

TEST_CASE("the same receiver without a key gets DD+ 5.1 and is told why", "[windemo]") {
    const std::vector<EndpointFacts> endpoints = {null_sink(), avr()};
    const auto choice = choose_output({.endpoints = endpoints, .signing_key_loaded = false, .pinned = std::nullopt});
    CHECK(choice.mode == OutputMode::kDdPlus51);
    CHECK(choice.reason.find("key") != std::string::npos);
}

TEST_CASE("a Dolby Digital only sink gets AC-3", "[windemo]") {
    const std::vector<EndpointFacts> endpoints = {null_sink(), dd_only(), headphones()};
    const auto choice = choose_output({.endpoints = endpoints, .signing_key_loaded = true, .pinned = std::nullopt});
    CHECK(choice.mode == OutputMode::kDd51);
    CHECK(choice.endpoint_id == "spdif");
}

TEST_CASE("a receiver on the default endpoint is never opened exclusively", "[windemo]") {
    // The S1 rule: applications render to the default; an exclusive open
    // there is refused and kills their streams. Headphones win, and the
    // reason names the receiver and the fix.
    const std::vector<EndpointFacts> endpoints = {avr(/*is_default=*/true), headphones()};
    const auto choice = choose_output({.endpoints = endpoints, .signing_key_loaded = true, .pinned = std::nullopt});
    CHECK(choice.mode == OutputMode::kHeadphones);
    CHECK(choice.reason.find("Denon AVR") != std::string::npos);
    CHECK(choice.reason.find("default") != std::string::npos);
}

TEST_CASE("with only the default receiver present there is nothing to choose but PCM on it", "[windemo]") {
    const std::vector<EndpointFacts> endpoints = {avr(/*is_default=*/true)};
    const auto choice = choose_output({.endpoints = endpoints, .signing_key_loaded = true, .pinned = std::nullopt});
    // Shared-mode surround on the default is allowed (the user hears the
    // direct mix too, and is told).
    CHECK(choice.mode == OutputMode::kPcmSurround);
    CHECK(choice.reason.find("alongside") != std::string::npos);
}

TEST_CASE("a TV takes decoded surround PCM", "[windemo]") {
    const std::vector<EndpointFacts> endpoints = {null_sink(), tv(), headphones(false)};
    const auto choice = choose_output({.endpoints = endpoints, .signing_key_loaded = true, .pinned = std::nullopt});
    CHECK(choice.mode == OutputMode::kPcmSurround);
    CHECK(choice.endpoint_id == "tv");
}

TEST_CASE("headphones with a spatial format beat plain stereo", "[windemo]") {
    const std::vector<EndpointFacts> endpoints = {null_sink(), headphones(true)};
    const auto choice = choose_output({.endpoints = endpoints, .signing_key_loaded = true, .pinned = std::nullopt});
    CHECK(choice.mode == OutputMode::kHeadphones);
}

TEST_CASE("headphones without a signing key fall to stereo", "[windemo]") {
    // No key means no object container, so nothing for the spatial renderer
    // to place; the stream is a 5.1 bed and stereo is its honest fold.
    const std::vector<EndpointFacts> endpoints = {null_sink(), headphones(true)};
    const auto choice = choose_output({.endpoints = endpoints, .signing_key_loaded = false, .pinned = std::nullopt});
    CHECK(choice.mode == OutputMode::kStereo);
}

TEST_CASE("headphones without a spatial format fall to stereo", "[windemo]") {
    const std::vector<EndpointFacts> endpoints = {null_sink(), headphones(false)};
    const auto choice = choose_output({.endpoints = endpoints, .signing_key_loaded = true, .pinned = std::nullopt});
    CHECK(choice.mode == OutputMode::kStereo);
}

TEST_CASE("the null sink is never an output", "[windemo]") {
    const std::vector<EndpointFacts> endpoints = {null_sink()};
    const auto choice = choose_output({.endpoints = endpoints, .signing_key_loaded = true, .pinned = std::nullopt});
    CHECK(choice.mode == OutputMode::kNone);
    CHECK_FALSE(choice.reason.empty());
}

TEST_CASE("no endpoints at all is a named nothing", "[windemo]") {
    const auto choice = choose_output({.endpoints = {}, .signing_key_loaded = false, .pinned = std::nullopt});
    CHECK(choice.mode == OutputMode::kNone);
    CHECK(choice.endpoint_id.empty());
    CHECK_FALSE(choice.reason.empty());
}

TEST_CASE("a pinned mode is honoured when feasible", "[windemo]") {
    const std::vector<EndpointFacts> endpoints = {null_sink(), avr(), headphones()};
    const auto choice = choose_output(
        {.endpoints = endpoints, .signing_key_loaded = true, .pinned = OutputMode::kHeadphones});
    CHECK(choice.mode == OutputMode::kHeadphones);
    CHECK(choice.reason.find("pinned") != std::string::npos);
}

TEST_CASE("an infeasible pin falls back and says so", "[windemo]") {
    const std::vector<EndpointFacts> endpoints = {null_sink(), headphones()};
    const auto choice = choose_output(
        {.endpoints = endpoints, .signing_key_loaded = true, .pinned = OutputMode::kAtmos});
    CHECK(choice.mode == OutputMode::kHeadphones);
    CHECK(choice.reason.find("pinned") != std::string::npos);
    CHECK(choice.reason.find("falling back") != std::string::npos);
}

TEST_CASE("a non-default endpoint is preferred for shared-mode output", "[windemo]") {
    // Two stereo endpoints, one the default: the other one keeps the direct
    // mix out of the user's ears.
    const std::vector<EndpointFacts> endpoints = {headphones(false, /*is_default=*/true),
                                                  {.id = "usb", .name = "USB DAC", .shared_channels = 2}};
    const auto choice = choose_output({.endpoints = endpoints, .signing_key_loaded = false, .pinned = std::nullopt});
    CHECK(choice.mode == OutputMode::kStereo);
    CHECK(choice.endpoint_id == "usb");
}

TEST_CASE("every output mode describes itself", "[windemo]") {
    for (const auto mode : {OutputMode::kAtmos, OutputMode::kDdPlus51, OutputMode::kDd51,
                            OutputMode::kPcmSurround, OutputMode::kHeadphones, OutputMode::kStereo,
                            OutputMode::kNone}) {
        CHECK_FALSE(ac3::windemo::describe(mode).empty());
        CHECK(ac3::windemo::describe(mode) != "unknown output mode");
    }
}

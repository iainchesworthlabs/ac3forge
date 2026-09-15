// The patch from rendered channels to a device's outputs (ac3/render/routing.hpp).
//
// What matters is that a channel lands on the output the patch names and on no
// other, that an output nobody is patched to is silent rather than holding
// whatever the buffer had, and that a patch which would put two channels on
// one output cannot be built.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/render/routing.hpp"

namespace {

using ac3::render::Routing;

// Planar blocks of constant value, one per channel: channel c holds c + 1.
struct Rendered {
    std::vector<std::vector<float>> storage;
    std::vector<std::span<const float>> spans;

    explicit Rendered(std::size_t channels, std::size_t samples = 4) {
        for (std::size_t c = 0; c < channels; ++c) {
            storage.emplace_back(samples, static_cast<float>(c + 1));
        }
        for (const auto& plane : storage) {
            spans.emplace_back(plane);
        }
    }
};

struct Outputs {
    std::vector<std::vector<float>> storage;
    std::vector<std::span<float>> spans;

    explicit Outputs(std::size_t outputs, std::size_t samples = 4, float prefill = 99.0F) {
        for (std::size_t o = 0; o < outputs; ++o) {
            storage.emplace_back(samples, prefill);
        }
        for (auto& plane : storage) {
            spans.emplace_back(plane);
        }
    }
};

std::string_view text_of(const Routing& routing, std::span<char> buffer) {
    const std::size_t n = routing.format(buffer);
    return std::string_view{buffer.data(), n};
}

}  // namespace

TEST_CASE("an identity patch puts channel n on output n", "[render][routing]") {
    const auto six_on_eight = Routing::identity(6, 8);
    REQUIRE(six_on_eight.has_value());
    REQUIRE(six_on_eight->channels() == 6);
    REQUIRE(six_on_eight->outputs() == 8);
    for (std::size_t c = 0; c < 6; ++c) {
        CAPTURE(c);
        REQUIRE(six_on_eight->output_of(c) == static_cast<int>(c));
        REQUIRE(six_on_eight->channel_of(c) == static_cast<int>(c));
    }
    REQUIRE(six_on_eight->channel_of(6) == Routing::kUnassigned);
    REQUIRE(six_on_eight->patched_outputs() == 0x3F);
    REQUIRE(six_on_eight->unpatched_channels() == 0);

    // More channels than outputs: the extra ones reach nothing, and say so.
    const auto twelve_on_eight = Routing::identity(12, 8);
    REQUIRE(twelve_on_eight.has_value());
    REQUIRE(twelve_on_eight->output_of(8) == Routing::kUnassigned);
    REQUIRE(twelve_on_eight->unpatched_channels() == 0x0F00);

    REQUIRE_FALSE(Routing::identity(Routing::kMaxChannels + 1, 8).has_value());
    REQUIRE_FALSE(Routing::identity(2, Routing::kMaxOutputs + 1).has_value());
    REQUIRE(Routing::identity(Routing::kMaxChannels, Routing::kMaxOutputs).has_value());
}

TEST_CASE("a patch refuses two channels on one output", "[render][routing]") {
    // L C R Ls Rs LFE onto an HDMI endpoint's FL FR FC LFE BL BR.
    const std::array<int, 6> wav_order = {0, 2, 1, 4, 5, 3};
    const auto patch = Routing::from_outputs(wav_order, 8);
    REQUIRE(patch.has_value());
    REQUIRE(patch->output_of(1) == 2);
    REQUIRE(patch->channel_of(3) == 5);

    const std::array<int, 3> twice = {0, 1, 1};
    REQUIRE_FALSE(Routing::from_outputs(twice, 8).has_value());
    const std::array<int, 2> out_of_range = {0, 8};
    REQUIRE_FALSE(Routing::from_outputs(out_of_range, 8).has_value());
    const std::array<int, 2> negative = {0, -2};
    REQUIRE_FALSE(Routing::from_outputs(negative, 8).has_value());

    // Unassigned more than once is fine: nothing is doubled up.
    const std::array<int, 4> gaps = {Routing::kUnassigned, 1, Routing::kUnassigned, 0};
    const auto gapped = Routing::from_outputs(gaps, 2);
    REQUIRE(gapped.has_value());
    REQUIRE(gapped->unpatched_channels() == 0b0101);
}

TEST_CASE("assign moves one channel and never steals an output", "[render][routing]") {
    auto patch = *Routing::identity(3, 4);
    REQUIRE(patch.assign(0, 3));
    REQUIRE(patch.output_of(0) == 3);
    REQUIRE(patch.channel_of(0) == Routing::kUnassigned);

    // Output 1 is channel 1's: refused, and nothing changes.
    REQUIRE_FALSE(patch.assign(2, 1));
    REQUIRE(patch.output_of(2) == 2);
    REQUIRE(patch.output_of(1) == 1);

    // Reassigning a channel to the output it already has is not a conflict.
    REQUIRE(patch.assign(1, 1));

    REQUIRE_FALSE(patch.assign(3, 0));   // no such channel
    REQUIRE_FALSE(patch.assign(0, 4));   // no such output
    REQUIRE_FALSE(patch.assign(0, -5));  // not an output, not unassigned

    REQUIRE(patch.assign(2, Routing::kUnassigned));
    REQUIRE(patch.output_of(2) == Routing::kUnassigned);
    REQUIRE(patch.assign(2, 0));  // output 0 is free again since channel 0 left it

    // swap exchanges, including with an unassigned channel.
    REQUIRE(patch.swap(0, 2));
    REQUIRE(patch.output_of(0) == 0);
    REQUIRE(patch.output_of(2) == 3);
    REQUIRE(patch.assign(1, Routing::kUnassigned));
    REQUIRE(patch.swap(1, 2));
    REQUIRE(patch.output_of(1) == 3);
    REQUIRE(patch.output_of(2) == Routing::kUnassigned);
    REQUIRE_FALSE(patch.swap(0, 3));
}

TEST_CASE("a patch's text form round-trips", "[render][routing]") {
    std::array<char, Routing::kTextBytes> buffer{};

    const auto parsed = Routing::parse(" 0, 2 ,1,-,5,4 ", 8);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->channels() == 6);
    REQUIRE(parsed->output_of(1) == 2);
    REQUIRE(parsed->output_of(3) == Routing::kUnassigned);
    REQUIRE(text_of(*parsed, buffer) == "0,2,1,-,5,4");
    REQUIRE(Routing::parse(text_of(*parsed, buffer), 8) == parsed);

    // Two-digit outputs, the widest patch there is.
    const auto wide = *Routing::identity(Routing::kMaxChannels, Routing::kMaxOutputs);
    auto moved = wide;
    REQUIRE(moved.assign(15, 31));
    REQUIRE(text_of(moved, buffer) == "0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,31");
    REQUIRE(Routing::parse(text_of(moved, buffer), Routing::kMaxOutputs) == moved);

    for (const std::string_view bad :
         {"", "0,,1", "0,1,", "a", "0,1,1", "8", "-1", "1.5", "0 1", "999999999999"}) {
        CAPTURE(bad);
        REQUIRE_FALSE(Routing::parse(bad, 8).has_value());
    }
    REQUIRE_FALSE(Routing::parse("0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16", 32).has_value());

    // A buffer too small for the whole text gets nothing rather than a prefix
    // that would parse as a different patch.
    std::array<char, 4> small{};
    REQUIRE(parsed->format(small) == 0);
    REQUIRE(small[0] == '\0');
    REQUIRE(Routing{}.format(buffer) == 0);
}

TEST_CASE("apply writes each output from its channel, and silence elsewhere",
          "[render][routing]") {
    // Three channels onto four outputs: 0 -> 2, 1 -> nothing, 2 -> 0; output
    // 1 and output 3 have no channel.
    const std::array<int, 3> map = {2, Routing::kUnassigned, 0};
    const auto patch = *Routing::from_outputs(map, 4);
    const Rendered rendered(3);
    Outputs out(4);
    patch.apply(rendered.spans, out.spans);
    REQUIRE(out.storage[0] == std::vector<float>(4, 3.0F));
    REQUIRE(out.storage[1] == std::vector<float>(4, 0.0F));
    REQUIRE(out.storage[2] == std::vector<float>(4, 1.0F));
    REQUIRE(out.storage[3] == std::vector<float>(4, 0.0F));

    // An output past the patch's own count is left alone.
    Outputs more(5);
    patch.apply(rendered.spans, more.spans);
    REQUIRE(more.storage[4] == std::vector<float>(4, 99.0F));

    // A channel the caller did not supply is silence on its output.
    Outputs short_input(4);
    const std::span<const std::span<const float>> first_two(rendered.spans.data(), 2);
    patch.apply(first_two, short_input.spans);
    REQUIRE(short_input.storage[0] == std::vector<float>(4, 0.0F));
    REQUIRE(short_input.storage[2] == std::vector<float>(4, 1.0F));

    // The shortest span decides how much is written.
    const Rendered longer(3, 8);
    Outputs shorter(4, 3);
    patch.apply(longer.spans, shorter.spans);
    REQUIRE(shorter.storage[2] == std::vector<float>(3, 1.0F));
}

TEST_CASE("apply_interleaved lays the outputs out frame by frame", "[render][routing]") {
    // Two channels swapped onto three outputs.
    const std::array<int, 2> map = {1, 0};
    const auto patch = *Routing::from_outputs(map, 3);
    const Rendered rendered(2, 5);
    std::vector<float> frames(3 * 4, 99.0F);
    REQUIRE(patch.apply_interleaved(rendered.spans, frames, 4) == 4);
    for (std::size_t k = 0; k < 4; ++k) {
        CAPTURE(k);
        REQUIRE(frames[k * 3] == 2.0F);
        REQUIRE(frames[(k * 3) + 1] == 1.0F);
        REQUIRE(frames[(k * 3) + 2] == 0.0F);
    }

    // Bounded by the buffer, and by the rendered block.
    std::vector<float> two_frames(3 * 2, 99.0F);
    REQUIRE(patch.apply_interleaved(rendered.spans, two_frames, 4) == 2);
    std::vector<float> roomy(3 * 10, 99.0F);
    REQUIRE(patch.apply_interleaved(rendered.spans, roomy, 10) == 5);
    REQUIRE(roomy[3 * 5] == 99.0F);

    REQUIRE(Routing{}.apply_interleaved(rendered.spans, roomy, 10) == 0);
}

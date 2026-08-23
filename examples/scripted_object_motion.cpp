// Drive Atmos objects from an authored scene instead of hand-rolled trig.
//
// atmos_objects.cpp computes each object's position with its own per-frame
// sin/cos math. ac3::oba::ObjectScene is the shared layer that replaces that:
// named objects with position/gain automation, each segment saying how it is
// traversed (hold, linear, smooth), evaluated once per frame into the
// ObjectPlacement span AtmosEncoder::encode_frame wants. A scene also has a
// text form - see the to_json() call at the end - so the same motion can be
// saved, edited by hand and fed back to `ac3cli atmos-path`.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/scene.hpp"

int main() {
    constexpr int kObjects = 2;

    using ac3::oba::Interpolation;
    auto built = ac3::oba::ObjectScene::create({
        // A slow sweep from the left wall to the right, easing in and out of
        // the front-centre cue so the pan does not corner where the segments
        // meet - that is what kSmooth buys over a straight line.
        {.name = "flyby",
         .automation = {{.time_s = 0.0,
                         .position = {.x = 0.0, .y = 0.5, .z = 0.5},
                         .gain = 0.6,
                         .interp = Interpolation::kSmooth},
                        {.time_s = 1.5,
                         .position = {.x = 0.5, .y = 0.1, .z = 0.5},
                         .gain = 0.6,
                         .interp = Interpolation::kSmooth},
                        {.time_s = 3.0,
                         .position = {.x = 1.0, .y = 0.5, .z = 0.5},
                         .gain = 0.6}}},
        // Three authored cues: enters silent at the left wall, swells to full
        // gain crossing the front centre, fades out exiting right. Before 0.0s
        // and after 1.6s it holds at its nearest cue rather than extrapolating
        // off into the wall or going silent.
        {.name = "voice",
         .automation = {{.time_s = 0.0, .position = {.x = 0.0, .y = 0.5, .z = 0.0}, .gain = 0.0},
                        {.time_s = 0.8, .position = {.x = 0.5, .y = 0.9, .z = 0.0}, .gain = 0.8},
                        {.time_s = 1.6, .position = {.x = 1.0, .y = 0.5, .z = 0.0}, .gain = 0.0}}},
    });
    if (!built) {
        std::printf("ObjectScene::create failed: %s\n", built.error().message.c_str());
        return 1;
    }
    const auto& scene = *built;

    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, kObjects};

    std::vector<std::vector<float>> sources(kObjects, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views;
    for (const auto& source : sources) {
        views.emplace_back(source);
    }
    constexpr std::array<double, kObjects> tones{440.0, 880.0};

    // Filled in place once per frame rather than reallocated - evaluate_into
    // is the allocation-free form for exactly this loop.
    std::vector<ac3::oba::ObjectPlacement> placement(kObjects);
    std::vector<std::byte> stream;
    for (int frame = 0; frame < 93; ++frame) {  // three seconds
        for (std::size_t obj = 0; obj < kObjects; ++obj) {
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                const double t = (frame * ac3::kSamplesPerFrame + n) / 48000.0;
                sources[obj][static_cast<std::size_t>(n)] =
                    static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * tones[obj] * t));
            }
        }

        const double seconds = frame * ac3::kSamplesPerFrame / 48000.0;
        scene.evaluate_into(seconds, placement);

        const auto unit = encoder.encode_frame(views, placement);
        if (!unit) {
            std::printf("atmos encode failed: %d\n", std::to_underlying(unit.error()));
            return 1;
        }
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }

    std::printf("%zu bytes of DD+ with %d scripted objects over a 5.1 bed\n", stream.size(),
                encoder.dynamic_object_count());

    // The same scene as text. Save this next to the stream and `ac3cli
    // atmos-path out.ec3 scene.json` reproduces the motion from the file -
    // and so does the keyframe grammar, which that command still reads.
    const auto text = ac3::oba::to_json(scene);
    std::printf("scene serialises to %zu bytes of JSON, %.1f s long\n", text.size(),
                scene.duration_s());
    return 0;
}

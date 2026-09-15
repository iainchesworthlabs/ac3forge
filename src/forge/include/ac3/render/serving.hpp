#pragma once

#include <cstdint>
#include <optional>

#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/render/layout.hpp"

// How a player serves its layout: which of the decoder's own §7.8 folds it
// asks for, if any, and whether the decoder reconstructs the stream's objects
// for the renderer to place.
//
// A stereo or mono room is the decoder's fold (OutputLayout::fold); anything
// wider is rendered as coded (ac3/render/render.hpp). Objects are worth their
// cost only when the layout asks for what the bed cannot give. This is the
// ESP32 player's policy, moved beside the renderer so that every player that
// renders - the boards, the desktop player's engine and its test sink - makes
// the same decision from the same inputs (planning/hearth-reference-player.md,
// "Decoder configuration").

namespace ac3::render {

// kAuto reconstructs exactly when the layout has height speakers, which is
// the case the bed cannot serve; kAlways does so for any rendered layout;
// kNever plays the bed, the objects' own 5.1 fold, which is the complete mix
// for a stereo or 5.1 room. A layout that folds never reconstructs, whatever
// this says. On an ESP32-S3 reconstruction costs about 10 ms of every 32 ms
// frame and, under the QMF domain, about 233 KB of heap.
enum class ObjectsPolicy : std::uint8_t { kAuto, kNever, kAlways };

struct Serving {
    // The decoder's fold for a stereo or mono layout; std::nullopt when the
    // renderer places the coded channels (or the objects) itself.
    std::optional<ac3::DownmixTarget> fold;
    // Whether the decoder reconstructs the object layer, when a stream has one.
    bool reconstruct = false;
};

// `stereo_fold` is the fold a two-speaker layout gets, kLoRo or kLtRt; a
// one-speaker layout folds to mono whatever it says.
[[nodiscard]] inline Serving serve(const OutputLayout& layout, ac3::DownmixTarget stereo_fold,
                                   ObjectsPolicy objects) {
    Serving out;
    out.fold = layout.fold(stereo_fold);
    switch (objects) {
        case ObjectsPolicy::kNever: out.reconstruct = false; break;
        case ObjectsPolicy::kAlways: out.reconstruct = !out.fold.has_value(); break;
        case ObjectsPolicy::kAuto:
            out.reconstruct = !out.fold.has_value() && layout.has_height();
            break;
    }
    return out;
}

// The two decoder settings a Serving decides, written into `config`: the
// output stage's target and whether object reconstruction is skipped. Every
// other field is the caller's.
inline void configure_decoder(const Serving& serving, ac3::DecoderConfig& config) {
    config.output.target = serving.fold.value_or(ac3::DownmixTarget::kAsCoded);
    config.skip_object_reconstruction = !serving.reconstruct;
}

}  // namespace ac3::render

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ac3/render/layout.hpp"
#include "ac3/render/render.hpp"
#include "ac4dec/decoder.hpp"

// An AC-4 presentation's objects rendered to speakers by the renderer Hearth
// plays E-AC-3's objects through, ac3::render::LayoutRenderer. The AC-4
// decoder hands each object over with the properties its metadata sets (ETSI
// TS 103 190-2 Annex F) for an application to render; this is ac3cli's
// rendering, kept apart from apps/cli so that a test holds it and so that
// Hearth's engine can take it up when it decodes AC-4. Compiled straight into
// ac3cli and ac3tests, as stream_playback.cpp beside it is.
//
// A dynamic object is panned from its position (X from the left wall to the
// right, Y from the front wall to the back, Z from the floor through the
// screen's height to the ceiling, which is the system of TS 103 420 that
// ac3::spatial::position_direction reads) at its gain, silent while inactive;
// a bed object, the LFE and the frame's own channels (a presentation's
// channel-coded substreams, or the intermediate spatial format the decoder
// rendered) go to their speakers, panned onto the layout where it lacks one.
// An update takes effect at its sample and each speaker's gain moves to it
// linearly over the update's ramp. Width, divergence, zones, snapping and the
// screen factor are not rendered, as the layout renderer renders none of them
// for E-AC-3.

namespace ac3::apps {

class Ac4ObjectRenderer {
   public:
    // Renders to the layout `target` names, the LFE with the fullband
    // speakers: 7.1.4 as coded, 7.1.2, 7.1, 5.1.4, 5.1.2 and 5.1 by name, two
    // channels for the stereo targets and one for mono.
    explicit Ac4ObjectRenderer(ac4::DownmixTarget target, std::uint32_t sample_rate_hz = 48000);

    // The output's speakers, in the order render() puts them out.
    [[nodiscard]] std::span<const ac4::Speaker> speakers() const noexcept;

    // Renders `frame` - its channels and its objects - into `out`, one vector
    // per speaker, each `frame.samples` long. The objects' gains carry on from
    // one frame to the next, so a stream is rendered frame after frame by one
    // renderer.
    void render(const ac4::DecodedFrame& frame, std::vector<std::vector<float>>& out);

    // The gain at each speaker, in speakers() order, of a dynamic object with
    // `properties`: its position panned by the layout renderer, times its
    // gain.
    [[nodiscard]] std::vector<float> object_gains(const ac4::ObjectProperties& properties);

    // Forgets the objects' gains, for a stream started afresh.
    void reset();

   private:
    static constexpr std::size_t kMaxSlots = render::OutputLayout::kMaxSlots;
    using Gains = std::array<float, kMaxSlots>;

    // One object's gains, moving to its last update's over its ramp.
    struct Track {
        Gains gains{};
        Gains target{};
        Gains step{};
        int left = 0;
    };

    // The gains of `object` with `properties`, at the speakers.
    [[nodiscard]] Gains gains_of(const ac4::DecodedObject& object,
                                 const ac4::ObjectProperties& properties);
    // The gains of a channel at `speaker`, through the layout renderer's bed.
    [[nodiscard]] Gains speaker_gains(ac4::Speaker speaker);

    std::vector<ac4::Speaker> speakers_;
    render::LayoutRenderer renderer_;
    std::vector<Track> tracks_;
};

}  // namespace ac3::apps

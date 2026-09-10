#pragma once

#include <array>
#include <cstdint>

// What apps/baremetal/probe.cpp's eac3_atmos_render row expects: the
// eac3_atmos_height stream's objects (fixture.hpp; three of them raised to the
// ceiling and one half way, tools/generators/atmos_height_scene.txt) rendered
// onto 7.1.4 by their own OAMD positions, one level per output slot (roadmap
// PF7).
//
// Hand-maintained, unlike fixture.hpp, and for the same reason
// encode_fixture.hpp is: nothing outside the probe produces the reference.
// gen_baremetal_fixture.py measures levels off a WAV ac3cli decode writes, and
// no ac3cli path writes a rendered layout to a WAV - `qc objects=714` renders
// the same way but meters loudness, and keeps no samples. So these are the
// host shape's own numbers, and the row is a REGRESSION reference: it says
// the target places the objects exactly as the host does, not that either
// places them correctly. tests/spatial/ is what says the panner is correct,
// on the host, where it can be checked against geometry.
//
// To regenerate: build the host shape of the decoder profile and run it. It
// prints every eac3_atmos_render.rms[n] line below, and it is the same code
// the target runs.
//
//     cmake --preset config-linux-gcc-minimal
//     cmake --build --preset build-linux-gcc-minimal
//     ./build/config-linux-gcc-minimal/bin/ac3probe
//
// The sums are float (see render_eac3), and every target this profile runs on
// rounds float the same way with contraction off, so host and target agree to
// the digit rather than merely within the level check's slack.
//
// Twelve slots, in Table E2.5 order with the LFE last: L, C, R, Ls, Rs, Lrs,
// Rrs, Vhl, Vhr, Lts, Rts - the eleven pan_targets keeps - then the bed's LFE
// passed through. Scaled by 1e6, as fixture.hpp's levels are.

namespace ac3probe {

inline constexpr std::array<std::int32_t, 12> kEac3AtmosRenderRms{{
    0, 0, 7867, 0, 104, 20201,
    20201, 82431, 97537, 102185, 58776, 0,
}};

}  // namespace ac3probe

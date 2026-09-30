#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/objects/motion.hpp"
#include "iclforge/audio/audio_backend.hpp"

// IAB reader bridge, phase 3 - the narrow seam
// between main.cpp's 'atmos-iab' command and iclforge::iab/iclforge::admbridge, exactly the same
// "library-linked-or-not is a build-time FILE choice, never a preprocessor conditional" shape
// adm/atmos_adm.hpp already uses for 'atmos-adm' - see that header's own top comment for the full
// reasoning (tools/checks/check_platform_macros.ps1's own #ifdef ban is what rules out the obvious
// alternative). 'atmos-iab' needs iclforge::admbridge specifically (its own IAB mapping, gated by
// AC3FORGE_BUILD_ADM the same as everything else in that module - see
// src/admbridge/CMakeLists.txt's own header comment) - NOT AC3FORGE_BUILD_IAB alone, which
// defaults ON and is not the gating question here: iclforge::iab by itself has nothing that can
// drive AtmosEncoder, only iclforge::admbridge's build_iab() does that, and that function only exists
// when AC3FORGE_BUILD_ADM turned admbridge on. So this command reuses adm/atmos_adm.hpp's own
// Needs::kAdm/adm_capability() gate rather than asking a new question - the availability test is
// identical either way.
//
// apps/cli/CMakeLists.txt adds exactly one of adm/enabled/atmos_iab.cpp or
// adm/disabled/atmos_iab.cpp to the ac3cli target; main.cpp calls load_iab_atmos_source below
// completely unconditionally either way, the same pattern atmos_adm.hpp's own
// load_adm_atmos_source already establishes.
namespace ac3cli {

// Everything run_atmos_iab (apps/cli/commands/atmos.cpp) needs from one parsed-and-bridged IAB
// source, expressed purely in iclforge::oba terms - the same shape AdmAtmosSource (atmos_adm.hpp)
// already uses for ADM, for the identical reason: main.cpp never needs iclforge::iab::IabError or
// iclforge::admbridge::BridgeError, only text to print. `handle` owns whatever `pcm`'s spans actually
// borrow from - for IAB this is the IabBridgeResult itself (iclforge::admbridge::build_iab), since its
// own `pcm` is OWNED storage rather than a borrow from a separate document object (see
// ac3/admbridge/iab_bridge.hpp's own top comment on why); keep an IabAtmosSource alive for exactly
// as long as its `pcm` spans are read.
struct IabAtmosSource {
    std::uint32_t sample_rate = 0;
    std::vector<bool> is_bed;                 // parallel to paths/pcm; true = bed speaker feed
    std::vector<iclforge::oba::ObjectPath> paths;  // pass directly to iclforge::oba::evaluate_placements
    std::vector<std::span<const float>> pcm;  // one mono span per channel; see `handle` above
    std::shared_ptr<void> handle;             // opaque - owns the bridged result, if any

    [[nodiscard]] std::size_t channel_count() const { return paths.size(); }
};

// Reads `path` - a bare elementary `.iab` file or a real MXF IAB Track File, sniffed by its first
// byte (an MXF Partition Pack Key starts 06h; an elementary IABitstream's own PreambleTag is 01h -
// see the enabled implementation's own comment) - and bridges it onto AtmosEncoder's input shape
// (iclforge::admbridge::build_iab), or a single diagnostic string already run through both IabError's
// and BridgeError's own describe() - so main.cpp never needs either error enum's type.
[[nodiscard]] std::expected<IabAtmosSource, std::string> load_iab_atmos_source(std::string_view path);

}  // namespace ac3cli

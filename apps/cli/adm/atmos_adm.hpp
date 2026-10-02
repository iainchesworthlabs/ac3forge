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

// The ADM BWF reader, phase 3 of 3 (feeding the JOC encoder) - the narrow
// seam between main.cpp's 'atmos-adm' command and iclforge::adm/iclforge::admbridge, this project's
// one opt-in, non-default library (ICLFORGE_BUILD_ADM, default OFF - see root CMakeLists.txt's
// own option() for why: libadm's Boost dependency).
//
// main.cpp cannot #include "ac3adm/ac3adm.hpp" or "ac3/admbridge/bridge.hpp" itself, not even
// behind a preprocessor guard: this project's tools/checks/check_platform_macros.ps1 (CI-enforced,
// see .github/workflows/ci.yml's own "Check for preprocessor conditionals in src/" job) refuses ANY
// #if/#ifdef/#ifndef under src/ - deliberately stricter than "no OS macros"; that script's own
// header comment says a feature-flag #ifdef is "just as unwelcome as a platform one". So whether
// iclforge::adm/iclforge::admbridge exist in this particular build has to be a build-time FILE
// choice, the same "exactly one implementation, selected by CMake" shape
// apps/cli/platform/{windows,posix}/ stdio_binary.cpp and src/audio's own src/backend/<os>/
// directory already use for an OS difference - here for a library-linked-or-not difference instead.
// apps/cli/CMakeLists.txt adds exactly one of adm/enabled/atmos_adm.cpp or
// adm/disabled/atmos_adm.cpp to the forge target; main.cpp calls the two functions below
// completely unconditionally either way.
//
// The functions below are declared entirely in terms of iclforge::oba's own types (always available
// - iclforge::oba is part of iclforge::ac3, unconditionally built) and plain strings, never
// iclforge::adm::AdmDocument/AdmError or iclforge::admbridge::BridgeResult/BridgeError - so this
// header itself never needs those two modules' own headers, and main.cpp (which includes this one)
// never gains a hard dependency on them either. adm/enabled/atmos_adm.cpp is the one place both
// meet.
namespace forge_cli {

// Whether THIS build's forge can run 'atmos-adm' at all - the same "is this available here?"
// question main.cpp's existing Needs::kCapture/kPassthrough/kMonitor already ask of
// iclforge::audio::audio_backend() (see that header's own top comment on why a per-platform TU,
// not a conditional, answers it); Needs::kAdm (main.cpp's kCommands table) asks this instead,
// and unmet() refuses the command before run_atmos_adm is ever called when it reports
// unavailable - reusing iclforge::audio::Capability's {available, reason} shape rather than
// inventing a second one for what is structurally the identical question.
[[nodiscard]] const iclforge::audio::Capability& adm_capability();

// Everything run_atmos_adm (main.cpp) needs from one parsed-and-bridged ADM BWF master, expressed
// purely in iclforge::oba terms. `handle` owns whatever `pcm`'s spans actually borrow from (an
// iclforge::adm::AdmDocument, in the real implementation) - keep an AdmAtmosSource alive for
// exactly as long as its `pcm` spans are read, the same lifetime contract
// iclforge::admbridge::BridgeResult itself documents for its own `pcm` field.
struct AdmAtmosSource {
    std::uint32_t sample_rate = 0;
    std::vector<bool> is_bed;                 // parallel to paths/pcm; true = bed speaker feed
    std::vector<iclforge::oba::ObjectPath>
        paths;                                // pass directly to iclforge::oba::evaluate_placements
    std::vector<std::span<const float>> pcm;  // one mono span per channel; see `handle` above
    std::shared_ptr<void> handle;             // opaque - owns the parsed document, if any

    [[nodiscard]] std::size_t channel_count() const { return paths.size(); }
};

// Parses `path` (iclforge::adm::parse_bw64) and bridges it onto AtmosEncoder's input shape
// (iclforge::admbridge::build), or a single diagnostic string already run through both AdmError's
// and BridgeError's own describe() - so main.cpp never needs either error enum's type, only text to
// print. Empty `programme_id` means "the file's own default (lowest-ID) audioProgramme", the same
// default iclforge::admbridge::build itself documents.
[[nodiscard]] std::expected<AdmAtmosSource, std::string> load_adm_atmos_source(
    std::string_view path, std::string_view programme_id);

}  // namespace forge_cli

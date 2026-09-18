#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/oba/oamd.hpp"

// Roadmap item IM2 ("JOC -> ADM BWF writer") - the write-direction sibling of atmos_adm.hpp's
// AdmAtmosSource/load_adm_atmos_source. Same reason for existing: decode.cpp cannot
// #include "ac3adm/ac3adm.hpp" or "ac3/admbridge/bridge.hpp" itself, not even behind a
// preprocessor guard (tools/checks/check_platform_macros.ps1 refuses ANY #if/#ifdef/#ifndef
// under src/ or apps/cli/commands - see atmos_adm.hpp's own top comment for the full reasoning),
// so this header is declared entirely in terms of ac3::oba's own types (always available) and
// plain strings, and apps/cli/CMakeLists.txt selects exactly one of adm/enabled/decode_adm.cpp or
// adm/disabled/decode_adm.cpp to implement it - decode.cpp calls the function below
// unconditionally either way, gating only on ac3cli::adm_capability() (declared in
// atmos_adm.hpp, reused here rather than duplicated - "is ADM available in this build?" is the
// same question regardless of which direction a command needs it for).
namespace ac3cli {

// One OAMD update to a dynamic object's DynamicObject state, timestamped in absolute samples from
// the start of the whole decode - decode.cpp builds this by walking every decoded access unit's
// own object_metadata->blocks in file order and adding each block's own sample_offset to a
// running total of samples already emitted (object_audio's own length each access unit, since
// that is the count actually written, not numblkscod*256 - see decode.cpp's own accumulation for
// why). Mirrors ac3::admbridge::WriteObjectUpdate field for field; kept as its own type rather
// than reused directly for the same reason AdmAtmosSource mirrors BridgeResult rather than
// including bridge.hpp - see this header's own top comment.
struct AdmObjectUpdate {
    std::uint64_t sample_offset = 0;
    int ramp_duration_samples = 0;
    ac3::oba::DynamicObject state;
};

// One channel of the master being written - a bed channel (`bed_label` set, pinned at its own
// room position, `updates` unused) or a JOC-reconstructed dynamic object (`bed_label` empty,
// positioned by `updates`). Mirrors ac3::admbridge::WriteChannel.
struct AdmMasterChannel {
    std::string name;
    std::vector<float> pcm;
    std::optional<ac3::oba::BedLabel> bed_label{};
    std::vector<AdmObjectUpdate> updates{};
};

struct AdmMasterInput {
    std::uint32_t sample_rate = 0;
    std::vector<AdmMasterChannel> channels;
};

// Writes `input` to `path` as a Dolby Atmos Master ADM Profile BW64 file (ac3::admbridge::write()
// builds the ac3adm::AdmDocument, ac3adm::write_bw64() writes it) - or a single diagnostic string
// already run through both BridgeError's and AdmWriteError's own describe(), the same
// "main.cpp/decode.cpp never needs either error enum's type" convention
// load_adm_atmos_source's own doc comment states for the read direction. Caller checks
// ac3cli::adm_capability() first, same as run_atmos_adm does.
[[nodiscard]] std::expected<void, std::string> write_adm_atmos_master(std::string_view path,
                                                                       const AdmMasterInput& input);

}  // namespace ac3cli

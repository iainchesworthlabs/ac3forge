#include <string>

#include <fmt/format.h>

#include "iclforge/ac3/meta/bsi.hpp"

// format_timecode alone, split out of bsi.cpp.
//
// It was that file's only fmt user, and fmt is a dependency the
// minimum-footprint profile does not carry - the repo root skips include(Fmt)
// entirely under ICLFORGE_MINIMAL_DECODER. Everything else in bsi.cpp is
// validation and enum naming that an ENCODER needs (valid_bsi_info,
// valid_alternate_bsi), so one presentation function was keeping the whole
// translation unit out of a build that has no console to format for.
//
// Found by compiling the encoder for arm-none-eabi, which had never been done:
// the decoder does not reach this file.

namespace iclforge::ac3::meta {

std::string format_timecode(const TimeCodeCoarse& coarse, const TimeCodeFine& fine) {
    return fmt::format("{:02}:{:02}:{:02}:{:02}.{}", coarse.hours, coarse.minutes,
                       coarse.eight_seconds * 8 + fine.seconds, fine.frames,
                       fine.sixty_fourths);
}

}  // namespace iclforge::ac3::meta

#include "ac3/audio/sink_capabilities.hpp"

// The ALSA EDID/ELD backend (roadmap UX9). CMake compiles this directory's
// sink_capabilities.cpp on the same host that gets passthrough.cpp's real
// implementation, so there is no #ifdef - the file's path is what says
// "ALSA".
//
// The HD-audio kernel driver populates /proc/asound/<card_id>/eld#<dev>.<port>
// per HDMI/DisplayPort PCM with the sink's own EDID-carried CEA-861 Short
// Audio Descriptors, already decoded into text fields (monitor_present,
// eld_valid, sad_count, sad<i>_coding_type/_channels/_rates/...) - a
// documented, stable interface, not a private one this reaches around. See
// eld_proc.hpp for the text parser and candidates.hpp for the card/device
// walk this shares with passthrough.cpp's own enumeration.
//
// Not verified against real HDMI hardware this session (no Linux box
// attached) - the same epistemic status PassthroughSink's own docs carry for
// parts of its path; see docs/platforms/linux.md.

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include <fmt/format.h>

#include "candidates.hpp"
#include "eld_proc.hpp"

namespace ac3::audio {

namespace {

namespace fs = std::filesystem;

// The eld# file for `candidate`'s RAW hardware device, if this machine has
// one. The port suffix after the dot is not predictable from here (and does
// not need to be - a PCM has exactly one), so this looks for whichever file
// starts with "eld#<device>." rather than assuming a port number.
[[nodiscard]] std::optional<fs::path> find_eld_file(const alsa::Candidate& candidate) {
    std::error_code ec;
    const fs::path card_dir = fs::path("/proc/asound") / candidate.card_id;
    const std::string prefix = fmt::format("eld#{}.", candidate.device);
    fs::directory_iterator it(card_dir, ec);
    if (ec) {
        return std::nullopt;
    }
    for (const auto& entry : it) {
        if (entry.path().filename().string().starts_with(prefix)) {
            return entry.path();
        }
    }
    return std::nullopt;
}

}  // namespace

std::expected<SinkAudioCapabilities, EdidError> read_sink_capabilities(
    const std::string& device_id) {
    for (const auto& candidate : alsa::find_candidates()) {
        if (candidate.name != device_id) {
            continue;
        }
        const auto eld_file = find_eld_file(candidate);
        if (!eld_file) {
            // A real, expected outcome: the port has no display/AVR attached
            // (or the driver has not populated an eld# file for it yet, e.g.
            // an S/PDIF output that carries no ELD at all), not a failure -
            // see EdidError::kNoEdid's own doc comment.
            return std::unexpected(EdidError::kNoEdid);
        }
        std::ifstream in(*eld_file);
        if (!in) {
            return std::unexpected(EdidError::kNoEdid);
        }
        std::ostringstream contents;
        contents << in.rdbuf();
        return alsa::parse_eld_proc_text(contents.str());
    }
    return std::unexpected(EdidError::kDeviceNotFound);
}

}  // namespace ac3::audio

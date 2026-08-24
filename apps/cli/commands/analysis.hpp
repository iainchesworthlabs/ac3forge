#pragma once

#include <optional>
#include <string>
#include <string_view>

// The measurement/transform commands that need no audio hardware: qc, levels, loudness, and
// spdif/unspdif. spdif is not itself a measurement command, but by this point in the H4 split it sits
// textually adjacent to loudness/levels/qc (the audio-hardware and container commands that used
// to separate them have already moved to their own files) - grouped here by that physical
// adjacency rather than forced into a cleaner-sounding but artificial category, the same rationale
// commands/containers.hpp gives for excluding spdif from itself.
// Split out of main.cpp as part of the repo-structure review's H4 monolith split.
namespace ac3cli::commands {

// `rendered_layout` is layout=bed (false, the default) or layout=rendered
// (true) - see Options::qc_rendered_layout.
//
// `want_programme` is the §E2.3.1.2 substreamid of the independent substream
// whose programme to measure - std::nullopt takes the first the stream
// carries, which for a single-programme stream (all of them, before a second
// independent substream is authored in) is the only one there is. Ignored for
// AC-3, which has no substream layer.
int run_qc(std::string_view in_path, const std::optional<std::string>& preset_arg,
           bool rendered_layout, std::optional<int> want_programme = std::nullopt);
int run_levels(std::string_view in_path, std::optional<int> want_programme = std::nullopt);
int run_loudness(std::string_view in_path);
int run_spdif(std::string_view in_path, std::string_view out_path);
int run_unspdif(std::string_view in_path, std::string_view out_path, bool keep_partial);

}  // namespace ac3cli::commands

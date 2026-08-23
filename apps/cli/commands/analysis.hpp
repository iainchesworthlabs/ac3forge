#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// The measurement/transform commands that need no audio hardware: qc, levels, loudness, and
// spdif. spdif is not itself a measurement command, but by this point in the H4 split it sits
// textually adjacent to loudness/levels/qc (the audio-hardware and container commands that used
// to separate them have already moved to their own files) - grouped here by that physical
// adjacency rather than forced into a cleaner-sounding but artificial category, the same rationale
// commands/containers.hpp gives for excluding spdif from itself.
// Split out of main.cpp as part of the repo-structure review's H4 monolith split.
namespace ac3cli::commands {

// The BS.1770-4 integrated loudness of an already-encoded stream - the
// measurement half of `qc`, without the report or the gate. `normalize`
// (commands/stream_tools.hpp) needs exactly this figure and nothing else, and
// re-implementing the decode-and-meter loop beside it would give this project
// two answers to the same question.
//
// 1+1 dual mono has no whole-programme figure: Ch1 and Ch2 are unrelated
// programmes sharing one syncframe (§E1.3, no downmix between them), so a
// single BS.1770 pass across both would measure a blend of two different
// things. Those streams report ch1_lkfs/ch2_lkfs and leave integrated_lkfs
// unset; every other layout does the opposite.
//
// A std::nullopt RESULT means the stream could not be decoded (already
// reported on stderr). A result whose figures are all unset means it decoded
// but held no audio above the -70 LKFS absolute gate.
struct StreamLoudness {
    std::optional<double> integrated_lkfs;
    std::optional<double> ch1_lkfs;
    std::optional<double> ch2_lkfs;
};

std::optional<StreamLoudness> measure_stream_loudness(std::span<const std::byte> stream);

int run_qc(std::string_view in_path, const std::optional<std::string>& preset_arg);
int run_levels(std::string_view in_path);
int run_loudness(std::string_view in_path);
int run_spdif(std::string_view in_path, std::string_view out_path);

}  // namespace ac3cli::commands

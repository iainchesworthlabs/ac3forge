#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "../support.hpp"
#include "ac3/encoder/plan.hpp"

// The measurement/transform commands that need no audio hardware: qc, levels, loudness, and
// spdif/unspdif. spdif is not itself a measurement command, but by this point in the H4 split it sits
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
    // Explicit defaults for the same -Wmissing-field-initializers reason
    // QcProgrammeResult (analysis.cpp) spells its own out.
    std::optional<double> integrated_lkfs = std::nullopt;
    std::optional<double> ch1_lkfs = std::nullopt;
    std::optional<double> ch2_lkfs = std::nullopt;
};

std::optional<StreamLoudness> measure_stream_loudness(std::span<const std::byte> stream);

// The same figure for an AC-4 stream: the integrated loudness of the
// presentation decode's options choose, as the stream codes it
// (ac4_coded_config()), over its 1/0, 2/0, 3/0 or 3/2 bed as `loudness`
// measures it. What transcode's dialnorm=auto takes from an AC-4 source.
std::optional<StreamLoudness> measure_ac4_loudness(std::span<const std::byte> stream,
                                                   std::string_view in_path, const Options& meta);

// qc reads its own options off `meta`: preset= (Options::qc_preset); layout=bed,
// the default, or layout=rendered (Options::qc_rendered_layout); programme=,
// the §E2.3.1.2 substreamid of the independent substream whose programme to
// measure, the first the stream carries where it is unset - which for a
// single-programme stream is the only one there is - and ignored for AC-3,
// which has no substream layer; and objects=<name> (legacy item IO12,
// Options::qc_objects_layout), which re-renders dynamic objects by their own
// position onto that layout and meters them through BS.1770-5 Annex 4,
// independently of layout=. An AC-4 stream's presentation is the one decode's
// options choose (ac4_coded_config()), and programme= and objects= are refused
// for it.
int run_qc(std::string_view in_path, const Options& meta);
// Per-channel levels of a WAV file or a stream: an E-AC-3 stream's programme=,
// and an AC-4 stream's presentation as decode's options choose it.
int run_levels(std::string_view in_path, const Options& meta);
// BS.1770-4 loudness, and the dialnorm it implies, of a WAV file or of a
// stream's audio as coded - an AC-3 or E-AC-3 stream's first programme, and an
// AC-4 stream's presentation as decode's options choose it - beside the
// dialnorm the stream carries.
int run_loudness(std::string_view in_path, const Options& meta);
int run_spdif(std::string_view in_path, std::string_view out_path);
int run_unspdif(std::string_view in_path, std::string_view out_path, bool keep_partial);

}  // namespace ac3cli::commands

#pragma once

#include <string_view>

#include "../support.hpp"

// `ac3cli probe` (roadmap IO1): what an elementary stream declares about
// itself, as a human-readable table or as the JSON document docs/cli/
// commands.md documents as a stable contract.
//
// Its own file rather than another entry in commands/analysis.cpp, which is
// where the other read-only commands live: those all DECODE - levels, qc and
// loudness exist to measure audio, and every one of them reconstructs the
// programme to do it. probe reads the bitstream and never the audio (see
// ac3/io/probe.hpp), which is a different job with a different cost, and it
// is the only command here whose output is meant to be consumed by another
// program.
namespace ac3cli::commands {

int run_probe(std::string_view in_path, const Options& meta);

}  // namespace ac3cli::commands

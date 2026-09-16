#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diagnostic_log.hpp"
#include "engine_thread.hpp"

// The diagnostics file (the Settings page's "Save diagnostics"): named facts,
// an engine snapshot and the ring (diagnostic_log.hpp), as plain text - what
// Hearth has done: the outputs it opened, the items it played, and every
// error.
//
// The page says pairing keys, codes and file paths are left out. The rule is
// held as Crucible's is (apps/crucible/engine/diagnostics.hpp): first by what
// the report reads, then by a scrub of what it wrote.
// - The facts are named fields, and none of them is a path, a key or a code.
// - Of the engine's snapshot, the report never reads an item's path except
//   to withhold it, nor the free-text note and error: they can quote a path
//   an item loader was given, for an item that may have left the queue
//   since. Every error is in the ring instead, withheld as it was written.
// - A setting under one of kWithheldSettings is written as withheld whatever
//   value arrived with it.
// - The finished text is scrubbed of the folders of every item in the queue,
//   and of the caller's secrets: the settings folder, the person's home
//   folder, a pairing code on screen.
//
// Pairing and the network (A4's sinks, found and paired) join the file when
// the engine drives them; their records belong under "pairing/".

namespace ac3::hearth {

// The settings the pairing records and the saved queue live under: keys,
// codes and paths.
inline constexpr std::array<std::string_view, 2> kWithheldSettings{"pairing/", "queue/"};

// The lists in the file keep this many entries: the last items played, the
// first items that cannot be played.
inline constexpr std::size_t kReportListLimit = 50;

struct ReportFacts {
    std::string written_at;      // ISO 8601, formatted by the caller
    std::string log_started_at;  // likewise
    std::string version;         // ac3::version_details()
    std::vector<std::pair<std::string, std::string>> platform;  // name/value rows, in order
    // The output the window chose (output_decision.hpp), by the endpoint's
    // name, and the decision's reason.
    std::string output_name;
    std::string output_reason;
    // Chosen by the caller, in order. Any key under kWithheldSettings is
    // written as withheld here whatever its value.
    std::vector<std::pair<std::string, std::string>> settings;
};

// The file. Reads the snapshot's named fields only - never an item's path,
// the note or the error - and runs the finished text through scrub() with
// the queue's folders added to `secrets` before returning it.
[[nodiscard]] std::string render_report(const ReportFacts& facts, const EngineStatus& engine,
                                        const DiagnosticLog& log, const Secrets& secrets);

}  // namespace ac3::hearth

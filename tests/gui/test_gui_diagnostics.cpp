#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <vector>

#include "gui_diagnostics.hpp"

// ac3gui's diagnostics module (apps/gui/gui_diagnostics.hpp): the ring keeps
// the newest lines in order and counts what it dropped, a note is one line no
// longer than the cap, and the report never carries the signing key, the path
// to a key file, or one byte of a loaded source - the rule
// docs/gui/accessibility.md promises under "Saving a diagnostics file".
//
// Deliberately the plain Catch2 suite rather than the Qt Quick one: the module
// is Qt-free so this runs on every CI leg, including the ones that build no
// window. The Qt Quick suite covers the other half - that the controller fills
// these fields from what the window is actually showing.

using ac3gui::MessageLog;
using ac3gui::ReportFacts;
using ac3gui::RunFacts;
using ac3gui::Secrets;
using ac3gui::SourceFacts;

namespace {

std::string lowercase(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

bool has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

bool stamped(const std::string& line) {
    // "+ssss.mmm " and then the note.
    if (line.size() <= MessageLog::kStampBytes || line[0] != '+' || line[5] != '.' ||
        line[9] != ' ') {
        return false;
    }
    for (const std::size_t i : {1U, 2U, 3U, 4U, 6U, 7U, 8U}) {
        if (std::isdigit(static_cast<unsigned char>(line[i])) == 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("the gui message ring keeps the newest lines in order and counts what it dropped",
          "[gui][diagnostics]") {
    MessageLog log(4);
    for (int i = 1; i <= 6; ++i) {
        log.note("line " + std::to_string(i));
    }
    const auto lines = log.lines();
    REQUIRE(lines.size() == 4);
    REQUIRE(log.dropped() == 2);
    // Oldest first, and the two that fell off are the two oldest.
    REQUIRE(has(lines.front(), "line 3"));
    REQUIRE(has(lines.back(), "line 6"));
    for (const auto& line : lines) {
        REQUIRE(stamped(line));
    }
}

TEST_CASE("a gui note is one line and no longer than the cap", "[gui][diagnostics]") {
    MessageLog log(4);
    log.note("first\r\nsecond\tthird");
    // A note past the cap, arranged so the cut lands INSIDE a multi-byte
    // character: the two bytes of a pound sign straddle it, and a naive cut
    // would leave half a character in the file. Written as explicit bytes
    // rather than as a \u escape so the test does not depend on the
    // compiler's execution character set.
    log.note(std::string(MessageLog::kMaxLine - 5, 'x') + "\xC2\xA3" + std::string(64, 'y'));
    const auto lines = log.lines();
    REQUIRE(lines.size() == 2);
    REQUIRE(has(lines[0], "first second third"));
    REQUIRE(lines[0].find('\n') == std::string::npos);
    REQUIRE(lines[0].find('\r') == std::string::npos);
    // One byte short of the cap: the walk backed off the continuation byte
    // rather than cutting the pound sign in half.
    REQUIRE(lines[1].size() == MessageLog::kStampBytes + MessageLog::kMaxLine - 1);
    REQUIRE(has(lines[1], " ..."));
}

TEST_CASE("the gui report carries the named facts and no signing value",
          "[gui][diagnostics]") {
    MessageLog log(16);
    // The kind of message that arrives through the ring rather than through a
    // named field, and the reason scrub() exists: the library's key resolver
    // names the variable it read from in its own error text.
    log.note("status: could not load the key from D:/keys/private.pem "
             "(from AC3FORGE_SIGNING_KEY_FILE)");

    ReportFacts facts;
    facts.written_at = "2026-09-06T12:00:00.000";
    facts.log_started_at = "2026-09-06T11:59:00.000";
    facts.version = "ac3forge 0.7.0";
    facts.platform.emplace_back("os", "Windows 11");
    facts.env_key_file_set = true;
    facts.env_key_inline_set = false;

    SourceFacts source;
    source.name = "stems.wav";
    source.channels = 6;
    source.rate_hz = 48000;
    source.seconds = 8.5;
    source.offset_seconds = 0.25;
    source.resample = "44.1 to 48 k";
    facts.sources.push_back(source);

    facts.plan.emplace_back("codec", "Dolby Digital Plus");
    facts.settings.emplace_back("workbench/textScale", "150");
    // Whatever a caller puts under signing/, the renderer withholds it - the
    // second half of the rule, held here so a caller that forgets cannot leak
    // through this section.
    facts.settings.emplace_back("signing/keyPath", "D:/keys/private.pem");

    RunFacts run;
    run.id = 3;
    run.status = "failed";
    run.filename = "stems.ec3";
    run.detail = "the encoder refused: 7.1.4 needs Dolby Digital Plus";
    facts.runs.push_back(run);
    facts.errors.push_back("Choose a capture device first.");

    Secrets secrets;
    secrets.strings.emplace_back("D:/keys/private.pem");

    const std::string report = ac3gui::render_report(facts, log, secrets);

    // The named facts are all there.
    REQUIRE(has(report, "AC3Forge ac3gui diagnostics"));
    REQUIRE(has(report, "ac3forge 0.7.0"));
    REQUIRE(has(report, "os: Windows 11"));
    REQUIRE(has(report, "\"stems.wav\"  6 ch  48000 Hz"));
    REQUIRE(has(report, "offset 0.25 s"));
    REQUIRE(has(report, "codec: Dolby Digital Plus"));
    REQUIRE(has(report, "workbench/textScale = 150"));
    REQUIRE(has(report, "3  \"stems.ec3\"  failed"));
    REQUIRE(has(report, "Choose a capture device first."));

    // Whether the variables are set, never what they hold.
    REQUIRE(has(report, "AC3FORGE_SIGNING_KEY_FILE: set"));
    REQUIRE(has(report, "AC3FORGE_SIGNING_KEY: not set"));

    // And the key path is gone from BOTH the whitelisted setting (structurally,
    // by the signing/ rule) and the ring's message (by the scrub).
    REQUIRE(has(report, "signing/keyPath = <withheld>"));
    REQUIRE_FALSE(has(lowercase(report), "private.pem"));
    REQUIRE(has(report, "from AC3FORGE_SIGNING_KEY_FILE"));
}

TEST_CASE("the gui report says so when there is nothing to report", "[gui][diagnostics]") {
    const MessageLog log(4);
    const ReportFacts facts;
    const std::string report = ac3gui::render_report(facts, log, Secrets{});
    // Empty sections read as empty rather than as a missing heading, which is
    // the difference between "nothing was loaded" and "the report is broken".
    REQUIRE(has(report, "# sources"));
    REQUIRE(has(report, "# runs"));
    REQUIRE(has(report, "# last errors"));
    REQUIRE(has(report, "(none)"));
    REQUIRE(has(report, "0 of 4"));
}

TEST_CASE("the gui scrub replaces every spelling and never eats its own marker",
          "[gui][diagnostics]") {
    Secrets secrets;
    secrets.strings.emplace_back("D:/keys/Private.PEM");
    // Matched case-insensitively, twice in one line, and the marker it leaves
    // behind is not itself re-matched into a growing chain.
    const std::string text = "read d:/keys/private.pem then D:/KEYS/PRIVATE.PEM again";
    const std::string out = ac3gui::scrub(text, secrets);
    REQUIRE(out == "read <withheld> then <withheld> again");

    // An empty secret matches everywhere and is ignored rather than obliterating
    // the report.
    Secrets empty;
    empty.strings.emplace_back("");
    REQUIRE(ac3gui::scrub("untouched", empty) == "untouched");
}

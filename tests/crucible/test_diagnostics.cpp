#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "diagnostics.hpp"
#include "engine.hpp"

// The diagnostics module (apps/crucible/engine/diagnostics.hpp): the ring
// keeps the newest lines in order and counts what it dropped, a note is one
// line no longer than the cap, and the report never carries the signing key,
// the path to it, the status line that names the file, or an executable's
// path - the rule docs/crucible/troubleshooting.md promises.

using ac3::crucible::AppStatus;
using ac3::crucible::DiagnosticLog;
using ac3::crucible::EngineStatus;
using ac3::crucible::KeySource;
using ac3::crucible::ReportFacts;
using ac3::crucible::Secrets;

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
    if (line.size() <= DiagnosticLog::kStampBytes || line[0] != '+' || line[5] != '.' || line[9] != ' ') {
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

TEST_CASE("the diagnostic log keeps the newest lines in order and counts what it dropped",
          "[crucible][diagnostics]") {
    DiagnosticLog log(4);
    for (int i = 1; i <= 6; ++i) {
        log.note("line " + std::to_string(i));
    }
    const auto lines = log.lines();
    REQUIRE(lines.size() == 4);
    CHECK(lines[0].ends_with("line 3"));
    CHECK(lines[1].ends_with("line 4"));
    CHECK(lines[2].ends_with("line 5"));
    CHECK(lines[3].ends_with("line 6"));
    CHECK(log.dropped() == 2);
    CHECK(log.capacity() == 4);
    for (const auto& line : lines) {
        CHECK(stamped(line));
    }
}

TEST_CASE("a note is one line and no longer than the cap", "[crucible][diagnostics]") {
    DiagnosticLog log(2);
    std::string big(2000, 'x');
    big[10] = '\r';
    big[11] = '\n';
    big[12] = '\t';
    big[13] = '\x01';
    log.note(big);
    const auto lines = log.lines();
    REQUIRE(lines.size() == 1);
    const auto& line = lines[0];
    CHECK(line.size() <= DiagnosticLog::kMaxLine + DiagnosticLog::kStampBytes);
    CHECK(line.ends_with(" ..."));
    CHECK(stamped(line));
    for (const char c : line) {
        CHECK(static_cast<unsigned char>(c) >= 0x20U);
    }
    // A short note is neither cut nor marked.
    log.note("short\r\nnote");
    const auto both = log.lines();
    REQUIRE(both.size() == 2);
    CHECK(both[1].ends_with("short  note"));
}

TEST_CASE("the report never carries the key path, the key bytes or the signing status line",
          "[crucible][diagnostics]") {
    EngineStatus status;
    status.running = true;
    status.signing = "signing key loaded from C:/Users/iain/secret/atmos.key: object container will be signed";
    status.objects_enabled = true;
    AppStatus app;
    app.app = 4242;
    app.name = "foo";
    app.description = "Foo Player";
    app.image_path = "C:/Users/iain/AppData/Local/Programs/foo/foo.exe";
    app.active = true;
    app.has_window = true;
    app.tapped = true;
    app.slot = 3;
    status.apps.push_back(app);

    DiagnosticLog log(16);
    log.note("cannot read signing key file 'C:\\Users\\iain\\secret\\atmos.key'");
    log.note("warning: key text c2VjcmV0LWtleS1ieXRlcw== decoded secret-key-bytes");

    Secrets secrets{.strings = {"C:/Users/iain/secret/atmos.key", "C:\\Users\\iain\\secret\\atmos.key",
                                "c2VjcmV0LWtleS1ieXRlcw==", "secret-key-bytes"}};
    ReportFacts facts;
    facts.signing.objects_enabled = true;
    facts.signing.source = KeySource::kFile;
    facts.settings.emplace_back("signing/keyPath", "a file is chosen");

    const std::string report = lowercase(ac3::crucible::render_report(facts, status, log, secrets));
    CHECK_FALSE(has(report, "secret/atmos.key"));
    CHECK_FALSE(has(report, "secret\\atmos.key"));
    CHECK_FALSE(has(report, "c2vjcmv0"));
    CHECK_FALSE(has(report, "secret-key-bytes"));
    CHECK_FALSE(has(report, "appdata"));
    CHECK_FALSE(has(report, "foo.exe"));
    CHECK_FALSE(has(report, "object container will be signed"));
    CHECK(has(report, "<withheld>"));
    CHECK(has(report, "objects: on"));
    CHECK(has(report, "key source: a file chosen in settings (path withheld)"));
    CHECK(has(report, "foo \"foo player\""));
    CHECK(has(report, "slot 3"));
    CHECK(has(report, "signing/keypath = <withheld>"));
    // The ring's two lines are there, with the secrets scrubbed in place.
    CHECK(has(report, "cannot read signing key file '<withheld>'"));
    CHECK(has(report, "warning: key text <withheld> decoded <withheld>"));
}

TEST_CASE("a settings key under signing/ is withheld whatever value arrives", "[crucible][diagnostics]") {
    ReportFacts facts;
    facts.settings.emplace_back("output/pinned", "auto");
    facts.settings.emplace_back("signing/keyPath", "D:/keys/k.bin");
    facts.settings.emplace_back("signing/blob", "AQID");
    const std::string report = ac3::crucible::render_report(facts, EngineStatus{}, DiagnosticLog{}, Secrets{});
    CHECK(has(report, "output/pinned = auto"));
    CHECK(has(report, "signing/keyPath = <withheld>"));
    CHECK(has(report, "signing/blob = <withheld>"));
    CHECK_FALSE(has(report, "D:/keys"));
    CHECK_FALSE(has(report, "AQID"));
}

TEST_CASE("scrub is case-insensitive and leaves other text alone", "[crucible][diagnostics]") {
    const Secrets secrets{.strings = {"C:\\K\\Key.TXT", "C:/K/Key.TXT", ""}};
    CHECK(ac3::crucible::scrub("Path C:\\K\\Key.TXT and c:/k/key.txt keyboard", secrets) ==
          "Path <withheld> and <withheld> keyboard");
    // A secret that is itself the marker's text cannot loop.
    CHECK(ac3::crucible::scrub("a <withheld> b", Secrets{.strings = {"<withheld>"}}) == "a <withheld> b");
    // Nothing to scrub leaves the text as it was.
    CHECK(ac3::crucible::scrub("plain", Secrets{}) == "plain");
}

TEST_CASE("the report renders every section with an idle engine", "[crucible][diagnostics]") {
    const std::string report = ac3::crucible::render_report(ReportFacts{}, EngineStatus{}, DiagnosticLog{}, Secrets{});
    const std::vector<std::string> sections{"# version",       "# platform",       "# signing",
                                            "# engine",        "# endpoints (last probe)",
                                            "# applications",  "# default output", "# silent device",
                                            "# settings",      "# recent messages"};
    std::size_t last = 0;
    for (const auto& section : sections) {
        const auto at = report.find("\n" + section + "\n", last);
        // The last header carries its counts on the same line.
        const auto found = at != std::string::npos ? at : report.find("\n" + section + " (", last);
        INFO(section);
        REQUIRE(found != std::string::npos);
        CHECK(found >= last);
        last = found;
    }
    CHECK(has(report, "running: no"));
    CHECK(has(report, "output: no usable output on \"\""));
    CHECK(has(report, "last error: (none)"));
    CHECK(has(report, "# recent messages (oldest first, 0 of 512; 0 dropped)"));
    CHECK(has(report, "AC3FORGE_SIGNING_KEY_FILE: not set"));
    CHECK(has(report, "AC3FORGE_SIGNING_KEY: not set"));
    CHECK(has(report, "key source: none"));
}

TEST_CASE("the process log survives and is shared", "[crucible][diagnostics]") {
    DiagnosticLog& one = ac3::crucible::process_diagnostics();
    DiagnosticLog& two = ac3::crucible::process_diagnostics();
    CHECK(&one == &two);
    const auto before = one.lines().size();
    one.note("shared note");
    const auto lines = two.lines();
    REQUIRE(lines.size() == before + 1);
    CHECK(lines.back().ends_with("shared note"));
    CHECK(one.capacity() == DiagnosticLog::kDefaultCapacity);
}

TEST_CASE("the report renders a busy engine: endpoints, placed and paired applications, and multi-line messages flattened",
          "[crucible][diagnostics]") {
    ReportFacts facts;
    facts.platform = {{"os", "test"}};
    facts.signing.objects_enabled = true;
    facts.signing.source = KeySource::kEnvironmentFile;
    facts.render_endpoints = {{.id = "a", .name = "Speakers", .is_default = false},
                              {.id = "b", .name = "Silent", .is_default = true}};
    facts.default_is_silent = true;
    facts.previous_default_name = "Speakers";
    facts.default_message = "moved\nback";
    facts.silent.detail = {"first\nsecond"};
    facts.foreground_available = false;
    facts.foreground_reason = "Wayland";

    EngineStatus engine;
    engine.running = true;
    engine.endpoints = {{.id = "hp", .name = "Headphones", .spatial = true, .spatial_max_objects = 17},
                        {.id = "avr", .name = "AVR", .accepts_eac3 = true, .accepts_ac3 = true, .shared_channels = 8}};
    AppStatus fullscreen;
    fullscreen.name = "game";
    fullscreen.fullscreen = true;
    fullscreen.packaged = true;
    fullscreen.slot = 0;
    fullscreen.width = 1;
    AppStatus pair;
    pair.name = "player";
    pair.has_window = true;
    pair.slot = 2;
    pair.width = 2;
    AppStatus custom = pair;
    custom.name = "custom";
    custom.pair_custom = true;
    AppStatus background;
    background.name = "daemon";
    background.has_session = false;
    engine.apps = {fullscreen, pair, custom, background};

    const std::string report = ac3::crucible::render_report(facts, engine, DiagnosticLog{}, Secrets{});
    CHECK(has(report, "key source: AC3FORGE_SIGNING_KEY_FILE (path withheld)"));
    CHECK(has(report, "\"Headphones\"  id=hp"));
    CHECK(has(report, "spatial=yes (max 17 objects)"));
    CHECK(has(report, "eac3=yes  ac3=yes  pcm=8ch  spatial=no"));
    CHECK(has(report, "game \"\": idle, packaged, session, not tapped, full-screen, slot 0"));
    CHECK(has(report, "width 2 size 0.00 pair "));
    CHECK(has(report, "width 2 size 0.00 custom pair "));
    CHECK(has(report, "daemon \"\": idle, background, no session, not tapped, in the bed"));
    CHECK(has(report, "\"Speakers\" id=a  \"Silent\" id=b [default]"));
    CHECK(has(report, "(silent device: yes)"));
    CHECK(has(report, "previous: \"Speakers\""));
    CHECK(has(report, "moved; back"));
    CHECK(has(report, "first; second"));
    CHECK(has(report, "unavailable: Wayland"));

    for (const auto& [source, text] :
         {std::pair{KeySource::kFile, "a file chosen in Settings (path withheld)"},
          std::pair{KeySource::kEnvironmentInline, "AC3FORGE_SIGNING_KEY (value withheld)"}}) {
        facts.signing.source = source;
        CHECK(has(ac3::crucible::render_report(facts, engine, DiagnosticLog{}, Secrets{}), text));
    }
}

TEST_CASE("a note cut at the cap never ends inside a multi-byte character", "[crucible][diagnostics]") {
    DiagnosticLog log(1);
    // Two-byte characters all the way: wherever the cut lands, it must back
    // off to a character boundary.
    std::string text;
    for (int i = 0; i < 600; ++i) {
        text += "\xC3\xA9";  // U+00E9
    }
    log.note(text);
    for (const std::string& shifted : {text, "x" + text}) {
        log.note(shifted);
        const auto line = log.lines().back();
        const auto body = line.substr(DiagnosticLog::kStampBytes);
        std::size_t lead = 0;
        std::size_t cont = 0;
        for (const char c : body) {
            const auto u = static_cast<unsigned char>(c);
            if (u == 0xC3U) {
                ++lead;
            } else if (u == 0xA9U) {
                ++cont;
            }
        }
        CHECK(lead == cont);
        CHECK(body.size() <= DiagnosticLog::kMaxLine);
    }
}

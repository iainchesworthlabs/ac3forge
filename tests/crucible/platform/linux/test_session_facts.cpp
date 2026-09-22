#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "proc_facts.hpp"
#include "session_monitor.hpp"

// The Linux SessionMonitor's bookkeeping, with no PipeWire daemon in the room
// (apps/crucible/engine/platform/linux/proc_facts.hpp, and session_monitor.cpp
// beside it for what stays behind the daemon). Four things live here and none
// of them needs a session:
//
// parse_ppid(), the /proc/<pid>/stat rule. The field a browser breaks is the
// comm, which the kernel prints in parentheses and does not escape, so the
// parse has to start from the LAST ')'. A stat line is also the one thing
// here a test can write out in full.
//
// The /proc readers, against a directory this file writes rather than against
// the machine's own /proc: a test that asserted anything about the processes
// that happen to be running would be asserting about the runner.
//
// ProcessFactsCache: that /proc is read once per process, that the identity a
// stream carries back-fills what an earlier stream left empty and never the
// other way round, and that facts for processes that have gone are dropped.
//
// The two records a refresh builds - the sounding one and the kept one - and
// the fill a second stream from one process does to the first's entry.
//
// Linux only, by tests/CMakeLists.txt, the way test_x11_foreground.cpp is:
// the file under test lives in a platform directory and is compiled nowhere
// else. What cannot be reached from here is the refresh() around it, which
// needs a graph to walk; tools/checks/crucible_platform_probe.cpp is what
// exercises that, by hand, on hardware.

namespace fs = std::filesystem;

using ac3::crucible::AppSession;
using ac3::crucible::fill_from_second_stream;
using ac3::crucible::image_path_of;
using ac3::crucible::kept_session;
using ac3::crucible::parse_ppid;
using ac3::crucible::process_alive;
using ac3::crucible::process_exe;
using ac3::crucible::process_name;
using ac3::crucible::ProcessFacts;
using ac3::crucible::ProcessFactsCache;
using ac3::crucible::ppid_of;
using ac3::crucible::sounding_session;
using ac3::crucible::StreamFacts;

namespace {

// A /proc of this case's own. AC3FORGE_TEST_SCRATCH_DIR (see
// tests/CMakeLists.txt for why it is a build-tree path) is the whole suite's
// root, and it is emptied on each use so a previous run's pids cannot pass a
// case. `name` is per CASE and not per file, deliberately: catch_discover_tests
// registers one ctest entry per test case, so two cases in this file are two
// processes that ctest -j may run at once, and a directory they shared would
// be removed under whichever of them started second.
fs::path fake_proc(const std::string& name) {
    const auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("crucible_proc_" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write_file(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

// One process in the fake /proc: comm, stat and the exe symlink, each written
// only where it is given, so a case can leave one out and get the reader's
// answer for a process that has no such file.
void write_process(const fs::path& root, std::uint32_t pid, const std::string& comm,
                   std::uint32_t parent, const std::string& exe) {
    const auto dir = root / std::to_string(pid);
    fs::create_directories(dir);
    if (!comm.empty()) {
        write_file(dir / "comm", comm + "\n");
        // The real thing carries fifty-two fields; the parse only ever reads
        // as far as the ppid, so the tail is the shape rather than the whole.
        write_file(dir / "stat", std::to_string(pid) + " (" + comm + ") S " +
                                     std::to_string(parent) + " 1 1 0 -1 4194304 0 0\n");
    }
    if (!exe.empty()) {
        std::error_code ec;
        fs::create_symlink(exe, dir / "exe", ec);
        // Not a REQUIRE on the error code alone: a filesystem that refuses
        // symlinks would make every exe case meaningless rather than fail one
        // of them, so say which it was.
        REQUIRE_FALSE(static_cast<bool>(ec));
    }
}

}  // namespace

TEST_CASE("a stat line's ppid is read from after the last close bracket",
          "[crucible][session-monitor]") {
    // The ordinary case, and the one that motivates the rule: the kernel
    // neither escapes nor quotes the comm, so a browser's renderer, a shell
    // whose binary was replaced under it, and anything a person can name a
    // process all land here.
    REQUIRE(parse_ppid("4242 (chrome) S 4200 4200 4200 0 -1 4194304 1 2 3") == 4200U);
    // A comm with a space in it, one with a bracketed suffix, and one built
    // to be as awkward as a person can make it. Splitting the line on
    // whitespace from the left takes the fourth field as the parent, which
    // for these three is "S", "S" and "c)" - which is how a browser's
    // renderer and a binary replaced under a running process both end up
    // with no parent and therefore no ancestors.
    REQUIRE(parse_ppid("7 (Web Content) S 3 3 3 0 -1 4194304") == 3U);
    REQUIRE(parse_ppid("9 (bash (deleted)) S 1 1 1") == 1U);
    REQUIRE(parse_ppid("11 (a) b) c) R 5 5") == 5U);
    // The state is read rather than assumed to be the S every line above
    // carries: this one is uninterruptible sleep, and the parent still comes
    // from the field after it.
    REQUIRE(parse_ppid("12 (wine64-preloader) D 7 12 12") == 7U);

    // Not a stat line. The empty read is what a process that went between the
    // graph walk and the open gives, and is the case that actually happens.
    REQUIRE_FALSE(parse_ppid("").has_value());
    REQUIRE_FALSE(parse_ppid("4242 chrome S 4200").has_value());
    // A line that stops at the comm, or whose next fields are not a state and
    // a number: no parent, rather than a parent of 0.
    REQUIRE_FALSE(parse_ppid("4242 (chrome)").has_value());
    REQUIRE_FALSE(parse_ppid("4242 (chrome) S").has_value());
    REQUIRE_FALSE(parse_ppid("4242 (chrome) S nothing").has_value());
}

TEST_CASE("the proc readers answer for a process and stay quiet for one that has gone",
          "[crucible][session-monitor]") {
    const auto root = fake_proc("readers");
    const std::string proc = root.generic_string();

    write_process(root, 4242, "chrome", 4200, "/opt/google/chrome/chrome");
    // A sandboxed application: comm and stat are readable, the exe link is
    // not, which is the case the binary name from PipeWire exists for.
    write_process(root, 900, "wine64-preloader", 1, "");
    // The comm the kernel writes unescaped into the middle of the stat line -
    // a space in it and a bracket of its own - read through the files rather
    // than as a literal handed to parse_ppid, so what the reader opens is
    // exercised too and not only the rule it applies afterwards.
    write_process(root, 77, "Web Content (deleted)", 42, "/usr/lib/firefox/firefox");

    CHECK(process_name(4242, proc) == "chrome");
    CHECK(process_exe(4242, proc) == "/opt/google/chrome/chrome");
    CHECK(ppid_of(4242, proc) == 4200U);
    CHECK(process_alive(4242, proc));

    CHECK(process_name(900, proc) == "wine64-preloader");
    CHECK(process_exe(900, proc).empty());
    CHECK(ppid_of(900, proc) == 1U);
    CHECK(process_alive(900, proc));

    CHECK(process_name(77, proc) == "Web Content (deleted)");
    CHECK(ppid_of(77, proc) == 42U);

    // A pid this /proc has never heard of: empty, nullopt, false - never a
    // throw and never a zero that reads like an answer.
    CHECK(process_name(31337, proc).empty());
    CHECK(process_exe(31337, proc).empty());
    CHECK_FALSE(ppid_of(31337, proc).has_value());
    CHECK_FALSE(process_alive(31337, proc));
}

TEST_CASE("process facts are read once and a later stream fills what the first left empty",
          "[crucible][session-monitor]") {
    const auto root = fake_proc("cache");
    ProcessFactsCache cache(root.generic_string());

    // 300 (a renderer) under 200 (the browser) under 100 (the shell that
    // started it): the walk stops where the executable changes, so a
    // full-screen terminal never pins the browser.
    write_process(root, 100, "bash", 1, "/bin/bash");
    write_process(root, 200, "chrome", 100, "/opt/google/chrome/chrome");
    write_process(root, 300, "chrome", 200, "/opt/google/chrome/chrome");
    write_process(root, 1, "systemd", 0, "/sbin/init");

    // The first stream from the renderer says who it is but carries no icon.
    const StreamFacts first{.binary = "chrome", .icon_name = {}, .app_id = {}};
    const ProcessFacts& facts = cache.facts_for(300, &first);
    CHECK(facts.name == "chrome");
    CHECK(facts.exe == "/opt/google/chrome/chrome");
    CHECK(facts.binary == "chrome");
    CHECK(facts.icon_name.empty());
    CHECK(facts.tree == std::vector<std::uint32_t>{300U, 200U});
    CHECK(cache.size() == 1);

    // A second stream from the same process, this one with the icon and the
    // portal id the first did not carry: they arrive, and what was already
    // there is left alone.
    const StreamFacts second{
        .binary = "chrome-from-the-second", .icon_name = "google-chrome", .app_id = "com.google.Chrome"};
    const ProcessFacts& again = cache.facts_for(300, &second);
    CHECK(again.icon_name == "google-chrome");
    CHECK(again.app_id == "com.google.Chrome");
    CHECK(again.binary == "chrome");
    CHECK(cache.size() == 1);

    // A third stream carrying nothing must not blank what the second gave.
    const StreamFacts empty{};
    CHECK(cache.facts_for(300, &empty).icon_name == "google-chrome");

    // /proc is read once, not once per stream. Asserted by changing what the
    // fake /proc says after the first read: a re-read would show the new
    // name, and the cache is exactly the thing that must not.
    write_file(root / "300" / "comm", "chrome-renamed\n");
    CHECK(cache.facts_for(300, &second).name == "chrome");

    // A process the cache has not seen is read now, with no stream to
    // identify it - the kept-application path.
    const ProcessFacts& shell = cache.facts_for(100, nullptr);
    CHECK(shell.name == "bash");
    CHECK(shell.binary.empty());
    CHECK(shell.tree == std::vector<std::uint32_t>{100U});
    CHECK(cache.size() == 2);

    // Everything for a process that is no longer listed goes; what is still
    // listed stays, with its identity intact.
    cache.forget_unless([](std::uint32_t pid) { return pid == 300U; });
    CHECK(cache.size() == 1);
    CHECK(cache.facts_for(300, nullptr).icon_name == "google-chrome");
    // 100 was forgotten, so this reads /proc again - and sees the fake tree
    // as it stands rather than what the earlier read cached.
    write_file(root / "100" / "comm", "zsh\n");
    CHECK(cache.facts_for(100, nullptr).name == "zsh");
}

TEST_CASE("a sounding application's record carries the stream and the process",
          "[crucible][session-monitor]") {
    const ProcessFacts facts{.name = "chrome",
                             .exe = "/opt/google/chrome/chrome",
                             .tree = {300U, 200U},
                             .binary = "chrome",
                             .icon_name = "google-chrome",
                             .app_id = "com.google.Chrome"};

    const AppSession app = sounding_session(300, "Chromium", facts);
    CHECK(app.app == 300U);
    // /proc's name wins over the stream's: "chrome" is what the tap and the
    // icon lookup both key on, while application.name is whatever the
    // application chose to call itself.
    CHECK(app.name == "chrome");
    CHECK(app.description == "Chromium");
    CHECK(app.image_path == "/opt/google/chrome/chrome");
    CHECK(app.icon_name == "google-chrome");
    CHECK(app.app_id == "com.google.Chrome");
    CHECK(app.active);
    CHECK(app.has_session);
    // Nothing on Linux can ask the shell whether a process owns a window, so
    // everything with a stream is listed rather than hidden.
    CHECK(app.has_window);
    CHECK_FALSE(app.packaged);
    // The full-screen rule matches the window's process against this list, so
    // the ancestors have to ride along or a browser never matches.
    CHECK(app.session_pids == std::vector<std::uint32_t>{300U, 200U});

    SECTION("a sandbox hides the executable and the binary name stands in") {
        // /proc gave no name and no exe: application.name is what a person
        // reads, and the bare binary is a degenerate path whose basename is
        // itself, which is all the icon lookup takes from it.
        const ProcessFacts sandboxed{.name = {},
                                     .exe = {},
                                     .tree = {900U},
                                     .binary = "spotify",
                                     .icon_name = {},
                                     .app_id = "com.spotify.Client"};
        CHECK(image_path_of(sandboxed) == "spotify");
        const AppSession flatpak = sounding_session(900, "Spotify", sandboxed);
        CHECK(flatpak.name == "Spotify");
        CHECK(flatpak.image_path == "spotify");
        CHECK(flatpak.app_id == "com.spotify.Client");
    }
}

TEST_CASE("a kept application is listed silent, with the same ancestors",
          "[crucible][session-monitor]") {
    const ProcessFacts facts{.name = "mpv",
                             .exe = "/usr/bin/mpv",
                             .tree = {700U},
                             .binary = {},
                             .icon_name = "mpv",
                             .app_id = {}};

    const AppSession app = kept_session(700, facts);
    CHECK(app.app == 700U);
    CHECK(app.name == "mpv");
    // No stream, so there is no application.name to describe it with: the
    // process's own name is what the room shows.
    CHECK(app.description == "mpv");
    CHECK(app.image_path == "/usr/bin/mpv");
    CHECK(app.icon_name == "mpv");
    // Not playing and nothing to tap, which is what greys the row - and the
    // whole point of the keep list: a placed application that has gone quiet
    // stays where it was put.
    CHECK_FALSE(app.active);
    CHECK_FALSE(app.has_session);
    CHECK(app.has_window);
    CHECK(app.session_pids == std::vector<std::uint32_t>{700U});
}

TEST_CASE("a second stream fills the entry the first built and overwrites none of it",
          "[crucible][session-monitor]") {
    const ProcessFacts first{.name = "chrome",
                             .exe = {},
                             .tree = {300U},
                             .binary = {},
                             .icon_name = {},
                             .app_id = {}};
    AppSession app = sounding_session(300, "Chromium", first);
    CHECK(app.icon_name.empty());
    CHECK(app.image_path.empty());

    const ProcessFacts filled{.name = "chrome",
                              .exe = "/opt/google/chrome/chrome",
                              .tree = {300U},
                              .binary = "chrome",
                              .icon_name = "google-chrome",
                              .app_id = "com.google.Chrome"};
    fill_from_second_stream(app, filled);
    // The icon shows on this refresh rather than the next one, which is the
    // whole reason the second stream is read at all.
    CHECK(app.icon_name == "google-chrome");
    CHECK(app.app_id == "com.google.Chrome");
    CHECK(app.image_path == "/opt/google/chrome/chrome");

    // A later stream never replaces what is already there: two streams from
    // one process can disagree, and the first answer is the one the room has
    // already drawn.
    const ProcessFacts other{.name = "chrome",
                             .exe = "/somewhere/else",
                             .tree = {300U},
                             .binary = "other",
                             .icon_name = "some-other-icon",
                             .app_id = "org.other"};
    fill_from_second_stream(app, other);
    CHECK(app.icon_name == "google-chrome");
    CHECK(app.app_id == "com.google.Chrome");
    CHECK(app.image_path == "/opt/google/chrome/chrome");
    // And it is a fill, not a rebuild: everything else the first stream set
    // is still the first stream's.
    CHECK(app.description == "Chromium");
    CHECK(app.active);
}

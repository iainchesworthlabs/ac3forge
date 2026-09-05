#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <vector>

#include "desktop_entries.hpp"

// The freedesktop side of the Linux application icon (apps/crucible/ui/
// desktop_entries.hpp), on every platform: the XDG data-dir list, the
// .desktop parser and its id and precedence rules, the Exec tokeniser, and
// the order the match rungs are tried in. None of it needs an icon theme
// or a display, which is why it is plain C++ and why it runs here on
// Windows as well as on the Linux leg that ships the provider.

namespace fs = std::filesystem;

using ac3::crucible::ui::AppIdentity;
using ac3::crucible::ui::DesktopEntry;
using ac3::crucible::ui::desktop_icon_for;
using ac3::crucible::ui::exec_binary_of;
using ac3::crucible::ui::icon_name_without_extension;
using ac3::crucible::ui::read_desktop_entries;
using ac3::crucible::ui::xdg_data_dirs;

namespace {

// Scratch space for this file's own tests. AC3FORGE_TEST_SCRATCH_DIR (see
// tests/CMakeLists.txt for why it is a build-tree path) is the whole
// suite's root; the leaf below is this file's own, emptied on each use so a
// previous run's files cannot pass a case.
fs::path scratch_dir() {
    const auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / "crucible_icons";
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// Written in binary mode, so the bytes - a CRLF included - are the file's.
void write_file(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

const DesktopEntry* entry_with_id(const std::vector<DesktopEntry>& entries, const std::string& id) {
    for (const auto& entry : entries) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("xdg data dirs come from the environment with the spec defaults", "[crucible][icons]") {
    const auto defaults = xdg_data_dirs("", "/home/u", "");
    REQUIRE(defaults.size() == 3);
    CHECK(defaults[0].generic_string() == "/home/u/.local/share");
    CHECK(defaults[1].generic_string() == "/usr/local/share");
    CHECK(defaults[2].generic_string() == "/usr/share");

    // An explicit data home replaces the derived one; empty pieces of the
    // list are skipped.
    const auto given = xdg_data_dirs("/x", "/home/u", "/a::/b");
    REQUIRE(given.size() == 3);
    CHECK(given[0].generic_string() == "/x");
    CHECK(given[1].generic_string() == "/a");
    CHECK(given[2].generic_string() == "/b");

    // Null is unset, which is what std::getenv hands over: no home, no data
    // home, and the two defaults.
    const auto none = xdg_data_dirs(nullptr, nullptr, nullptr);
    REQUIRE(none.size() == 2);
    CHECK(none[0].generic_string() == "/usr/local/share");
    CHECK(none[1].generic_string() == "/usr/share");
}

TEST_CASE("an exec line yields the binary name", "[crucible][icons]") {
    CHECK(exec_binary_of("env FOO=1 /usr/bin/foo --bar %U") == "foo");
    CHECK(exec_binary_of("\"/opt/My App/app\" %f") == "app");
    CHECK(exec_binary_of("sh -c \"x\"") == "sh");
    CHECK(exec_binary_of("firefox %u") == "firefox");
    CHECK(exec_binary_of("").empty());
    CHECK(exec_binary_of("env").empty());
    CHECK(exec_binary_of("FOO=1 BAR=x").empty());
    // The string escapes come first, then the quoting: an escaped space
    // inside quotes stays in the argument, and one outside them splits it.
    CHECK(exec_binary_of("\"/opt/My\\sApp/app\" %f") == "app");
    CHECK(exec_binary_of("mpv\\s--player-operation-mode=pseudo-gui -- %U") == "mpv");
    // A quote escaped inside a quoted argument is a character of it.
    CHECK(exec_binary_of("\"/opt/say\\\"hi\\\"/run\" %F") == "run");
}

TEST_CASE("desktop entries are read from every data dir with the first one winning",
          "[crucible][icons]") {
    const fs::path scratch = scratch_dir();
    const fs::path a = scratch / "a";
    const fs::path b = scratch / "b";

    write_file(a / "applications" / "x.desktop",
               "[Desktop Entry]\n"
               "Type=Application\n"
               "Name=X from A\n"
               "Name[de]=X aus A\n"
               "Exec=env FOO=1 /usr/bin/x %U\n"
               "StartupWMClass=XWin\n"
               "Icon=icon-from-a\n"
               "\n"
               "[Desktop Action New]\n"
               "Name=New window\n"
               "Icon=wrong-action-icon\n");
    write_file(b / "applications" / "x.desktop",
               "[Desktop Entry]\n"
               "Type=Application\n"
               "Name=X from B\n"
               "Icon=icon-from-b\n");
    write_file(b / "applications" / "sub" / "y.desktop",
               "# a comment first\n"
               "[Desktop Entry]\n"
               "Type = Application\n"
               "Name=Y\n"
               "TryExec=/opt/y/bin/y\n"
               "NoDisplay=true\n"
               "Icon=icon-y\n");
    write_file(a / "applications" / "link.desktop",
               "[Desktop Entry]\n"
               "Type=Link\n"
               "URL=https://example.invalid/\n"
               "Icon=link-icon\n");
    write_file(a / "applications" / "hidden.desktop",
               "[Desktop Entry]\n"
               "Type=Application\n"
               "Hidden=true\n"
               "Exec=hidden\n"
               "Icon=hidden-icon\n");
    write_file(b / "applications" / "crlf.desktop",
               "[Desktop Entry]\r\n"
               "Type=Application\r\n"
               "Exec=crlf-bin %F\r\n"
               "Icon=crlf-icon\r\n");
    write_file(a / "applications" / "notes.txt", "not a desktop file\n");

    const auto entries = read_desktop_entries({a, b, scratch / "missing"});

    // Precedence: a's x shadows b's, and a's entries come before b's.
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].id == "x");
    CHECK(entries[1].id == "crlf");
    CHECK(entries[2].id == "sub-y");

    const DesktopEntry* x = entry_with_id(entries, "x");
    REQUIRE(x != nullptr);
    CHECK(x->icon == "icon-from-a");
    CHECK(x->name == "X from A");
    CHECK(x->exec_binary == "x");
    CHECK(x->wm_class == "XWin");
    CHECK_FALSE(x->no_display);

    // A subdirectory is part of the id; TryExec is reduced to its basename;
    // blanks around '=' are tolerated.
    const DesktopEntry* y = entry_with_id(entries, "sub-y");
    REQUIRE(y != nullptr);
    CHECK(y->icon == "icon-y");
    CHECK(y->name == "Y");
    CHECK(y->try_exec_binary == "y");
    CHECK(y->no_display);

    // A CRLF file parses with no stray CR on its values.
    const DesktopEntry* crlf = entry_with_id(entries, "crlf");
    REQUIRE(crlf != nullptr);
    CHECK(crlf->icon == "crlf-icon");
    CHECK(crlf->exec_binary == "crlf-bin");

    CHECK(entry_with_id(entries, "link") == nullptr);
    CHECK(entry_with_id(entries, "hidden") == nullptr);
}

TEST_CASE("the icon rungs are tried in order", "[crucible][icons]") {
    // Listed in reverse rung order, so the answer cannot come from the
    // entries' order alone.
    DesktopEntry by_name;
    by_name.id = "n";
    by_name.name = "my app";
    by_name.icon = "by-name";
    DesktopEntry by_wm_class;
    by_wm_class.id = "w";
    by_wm_class.wm_class = "MyApp";
    by_wm_class.icon = "by-wm-class";
    DesktopEntry by_exec;
    by_exec.id = "e";
    by_exec.exec_binary = "myapp";
    by_exec.icon = "by-exec";
    DesktopEntry by_try_exec;
    by_try_exec.id = "t";
    by_try_exec.try_exec_binary = "myapp";
    by_try_exec.icon = "by-try-exec";
    DesktopEntry by_app_id;
    by_app_id.id = "org.example.MyApp";
    by_app_id.exec_binary = "flatpak";
    by_app_id.icon = "by-app-id";
    std::vector<DesktopEntry> entries{by_name, by_wm_class, by_exec, by_try_exec, by_app_id};

    AppIdentity who{.app_id = "org.example.MyApp", .binary = "myapp", .name = "My App"};
    CHECK(desktop_icon_for(entries, who) == "by-app-id");

    who.app_id.clear();
    CHECK(desktop_icon_for(entries, who) == "by-try-exec");

    entries.erase(entries.begin() + 3);  // no TryExec entry any more
    CHECK(desktop_icon_for(entries, who) == "by-exec");

    // StartupWMClass matches the name or the binary, case-insensitively.
    who.binary.clear();
    who.name = "myapp";
    CHECK(desktop_icon_for(entries, who) == "by-wm-class");
    who.name.clear();
    who.binary = "MYAPP";
    entries.erase(entries.begin() + 2);  // no Exec entry any more
    CHECK(desktop_icon_for(entries, who) == "by-wm-class");

    // Name, case-insensitively, and last.
    who.binary.clear();
    who.name = "MY APP";
    entries.erase(entries.begin() + 1);  // no StartupWMClass entry any more
    CHECK(desktop_icon_for(entries, who) == "by-name");

    // A name that matches nothing, and an empty identity, answer nothing.
    who.name = "Something Else";
    CHECK_FALSE(desktop_icon_for(entries, who).has_value());
    CHECK_FALSE(desktop_icon_for(entries, AppIdentity{.app_id = {}, .binary = {}, .name = {}})
                    .has_value());

    // A winning entry with no Icon= is no answer either.
    DesktopEntry bare;
    bare.id = "bare";
    bare.exec_binary = "bare";
    CHECK_FALSE(desktop_icon_for({bare}, AppIdentity{.app_id = {}, .binary = "bare", .name = {}})
                    .has_value());
}

TEST_CASE("a displayable entry beats a NoDisplay one on the same rung", "[crucible][icons]") {
    DesktopEntry hidden;
    hidden.id = "hidden";
    hidden.exec_binary = "myapp";
    hidden.icon = "wrong";
    hidden.no_display = true;
    DesktopEntry shown;
    shown.id = "shown";
    shown.exec_binary = "myapp";
    shown.icon = "right";
    const std::vector<DesktopEntry> entries{hidden, shown};
    CHECK(desktop_icon_for(entries, AppIdentity{.app_id = {}, .binary = "myapp", .name = {}}) ==
          "right");
    // With nothing else to choose from, a NoDisplay entry still answers.
    CHECK(desktop_icon_for({hidden}, AppIdentity{.app_id = {}, .binary = "myapp", .name = {}}) ==
          "wrong");
}

TEST_CASE("an icon value keeps its extension only when it is a path", "[crucible][icons]") {
    CHECK(icon_name_without_extension("foo.png") == "foo");
    CHECK(icon_name_without_extension("foo.svgz") == "foo");
    CHECK(icon_name_without_extension("/usr/share/pixmaps/foo.png") == "/usr/share/pixmaps/foo.png");
    CHECK(icon_name_without_extension("foo") == "foo");
    CHECK(icon_name_without_extension("org.gnome.Totem") == "org.gnome.Totem");
    CHECK(icon_name_without_extension(".png") == ".png");
}

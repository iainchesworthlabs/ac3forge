#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ac3::crucible::ui {

// The freedesktop side of the Linux application icon: which .desktop entry
// a running process belongs to, and what that entry's Icon= is. Qt-free and
// platform-neutral - std::filesystem and std::string only - so the parser
// and the match rules compile into ac3tests on every platform, where the
// icon theme itself cannot. The Linux AppIconProvider is the one caller
// (ui/platform/linux/app_icon_provider.cpp).
//
// The Desktop Entry Specification is the reference: the [Desktop Entry]
// group, and Type, Hidden, NoDisplay, Exec, TryExec, Icon, StartupWMClass
// and Name from it, are all this needs. A desktop file id is its path under
// applications/ with '/' as '-' and no .desktop.

struct DesktopEntry {
    std::string id;                // "firefox"; "org.gnome.Totem"; "kde-org.kde.dolphin" for kde/org.kde.dolphin.desktop
    std::string icon;              // Icon=, verbatim: a theme name or an absolute path; empty when absent
    std::string exec_binary;       // the basename of Exec's program, or empty
    std::string try_exec_binary;   // the basename of TryExec, or empty
    std::string wm_class;          // StartupWMClass, or empty
    std::string name;              // Name, the unlocalised key, or empty
    bool no_display = false;       // NoDisplay=true: a valid entry that menus leave out
};

// The XDG base directories for application data, most specific first:
// `data_home` when non-empty, else `<home>/.local/share`; then the ':'-
// separated `data_dirs` with empty pieces skipped, or /usr/local/share and
// /usr/share when none are given. Pure over its arguments so a test on any
// platform can pass strings; the provider passes std::getenv's answers, and
// null means unset. Flatpak's and snap's exports (.../flatpak/exports/share,
// /var/lib/snapd/desktop) are on a desktop's XDG_DATA_DIRS already, which
// is why none of them is hard-coded here.
[[nodiscard]] std::vector<std::filesystem::path> xdg_data_dirs(const char* data_home,
                                                               const char* home,
                                                               const char* data_dirs);

// The basename of the program an Exec= line runs. The specification's two
// passes, in its order: first the string escapes every desktop-file value
// may carry (\s \n \t \r \\), then the argument split on unquoted
// whitespace, a double quote enclosing an argument in which a backslash
// escapes the character after it. A leading `env` and any NAME=value
// assignments are dropped; field codes (%f %u ...) never reach the answer.
// Empty for an empty line.
[[nodiscard]] std::string exec_binary_of(std::string_view exec);

// Every Type=Application, non-Hidden entry under <dir>/applications for
// each data dir, recursively; the first data dir that carries an id owns
// it, whatever its file says. Entries from one directory come in id order,
// so the whole list is in precedence order.
[[nodiscard]] std::vector<DesktopEntry> read_desktop_entries(
    const std::vector<std::filesystem::path>& data_dirs);

// What is known about a running application, for the match.
struct AppIdentity {
    std::string app_id;   // the sandbox's application id, when there is one
    std::string binary;   // the executable's basename
    std::string name;     // what the platform calls the application
};

// The Icon= of the entry that fits `who`, tried in order: (1) id equals the
// app id; (2) TryExec's binary equals the binary; (3) Exec's binary equals
// it; (4) StartupWMClass equals the name or the binary, ignoring case;
// (5) Name equals the name, ignoring case. The first rung with any match
// answers; within it an entry menus show beats a NoDisplay one, then the
// entries' order. An empty field of `who` never matches. Nullopt when no
// rung matches or the winning entry has no Icon=.
[[nodiscard]] std::optional<std::string> desktop_icon_for(const std::vector<DesktopEntry>& entries,
                                                          const AppIdentity& who);

// An Icon= value as a theme name: a trailing .png, .svg, .svgz or .xpm is
// dropped, since the specification looks such a name up without it. An
// absolute path is returned unchanged.
[[nodiscard]] std::string_view icon_name_without_extension(std::string_view icon);

}  // namespace ac3::crucible::ui

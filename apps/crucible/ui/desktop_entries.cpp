#include "desktop_entries.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

// See desktop_entries.hpp. Everything here is plain C++ over the Desktop
// Entry Specification's text format; nothing consults a theme or Qt.

namespace ac3::crucible::ui {

namespace {

namespace fs = std::filesystem;

// ASCII case-insensitive equality, which is all a desktop-file value needs.
[[nodiscard]] bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

// Blanks off both ends, and a CR off the end: a CRLF file is legal to receive.
[[nodiscard]] std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] std::string basename_of(std::string_view program) {
    const auto slash = program.rfind('/');
    return std::string(slash == std::string_view::npos ? program : program.substr(slash + 1));
}

// A NAME=value token, as `env` and a bare assignment prefix carry: a name
// of letters, digits and underscores, then '='.
[[nodiscard]] bool is_assignment(std::string_view token) {
    const auto eq = token.find('=');
    if (eq == std::string_view::npos || eq == 0) {
        return false;
    }
    return std::ranges::all_of(token.substr(0, eq), [](char c) {
        return c == '_' || std::isalnum(static_cast<unsigned char>(c)) != 0;
    });
}

// The specification's first pass over an Exec value: the string escapes
// every desktop-file value may hold. A backslash before anything else is
// kept for the second pass, where it may be escaping a quote.
[[nodiscard]] std::string unescape_value(std::string_view exec) {
    std::string out;
    out.reserve(exec.size());
    for (std::size_t i = 0; i < exec.size(); ++i) {
        const char c = exec[i];
        if (c != '\\' || i + 1 >= exec.size()) {
            out += c;
            continue;
        }
        const char next = exec[++i];
        switch (next) {
        case 's':
            out += ' ';
            break;
        case 'n':
            out += '\n';
            break;
        case 't':
            out += '\t';
            break;
        case 'r':
            out += '\r';
            break;
        case '\\':
            out += '\\';
            break;
        default:
            out += '\\';
            out += next;
            break;
        }
    }
    return out;
}

// The second pass: the arguments. Unquoted whitespace splits; a double
// quote opens an argument in which a backslash escapes the character after
// it (the specification names " ` $ and \ as what it may escape).
[[nodiscard]] std::vector<std::string> split_arguments(std::string_view line) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_token = false;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"') {
                quoted = false;
            } else if (c == '\\' && i + 1 < line.size()) {
                current += line[++i];
            } else {
                current += c;
            }
            continue;
        }
        if (c == '"') {
            quoted = true;
            in_token = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (in_token) {
                tokens.push_back(current);
                current.clear();
                in_token = false;
            }
            continue;
        }
        in_token = true;
        current += c;
    }
    if (in_token) {
        tokens.push_back(current);
    }
    return tokens;
}

// One file's [Desktop Entry] group, or nullopt when the file is not an
// application, is Hidden (the specification's "deleted"), or has no such
// group. Only the unlocalised keys are read; Name[xx] and the like are left
// alone.
[[nodiscard]] std::optional<DesktopEntry> parse_desktop_file(const fs::path& file, std::string id) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    DesktopEntry entry;
    entry.id = std::move(id);
    bool in_group = false;
    bool application = false;
    bool hidden = false;
    std::string line;
    while (std::getline(in, line)) {
        const std::string_view text = trim(line);
        if (text.empty() || text.front() == '#') {
            continue;
        }
        if (text.front() == '[') {
            if (in_group) {
                break;  // the next group: nothing below it is the entry's
            }
            in_group = text == "[Desktop Entry]";
            continue;
        }
        if (!in_group) {
            continue;
        }
        const auto eq = text.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string_view key = trim(text.substr(0, eq));
        const std::string_view value = trim(text.substr(eq + 1));
        if (key == "Type") {
            application = value == "Application";
        } else if (key == "Hidden") {
            hidden = value == "true";
        } else if (key == "NoDisplay") {
            entry.no_display = value == "true";
        } else if (key == "Exec") {
            entry.exec_binary = exec_binary_of(value);
        } else if (key == "TryExec") {
            entry.try_exec_binary = basename_of(value);
        } else if (key == "Icon") {
            entry.icon = std::string(value);
        } else if (key == "StartupWMClass") {
            entry.wm_class = std::string(value);
        } else if (key == "Name") {
            entry.name = std::string(value);
        }
    }
    if (!application || hidden) {
        return std::nullopt;
    }
    return entry;
}

// The first entry the predicate accepts, an entry menus show beating a
// NoDisplay one, otherwise the earlier one.
template <typename Predicate>
[[nodiscard]] const DesktopEntry* best_match(const std::vector<DesktopEntry>& entries,
                                             Predicate accepts) {
    const DesktopEntry* best = nullptr;
    for (const auto& entry : entries) {
        if (!accepts(entry)) {
            continue;
        }
        if (best == nullptr || (best->no_display && !entry.no_display)) {
            best = &entry;
        }
    }
    return best;
}

}  // namespace

std::vector<fs::path> xdg_data_dirs(const char* data_home, const char* home,
                                    const char* data_dirs) {
    std::vector<fs::path> out;
    if (data_home != nullptr && data_home[0] != '\0') {
        out.emplace_back(data_home);
    } else if (home != nullptr && home[0] != '\0') {
        out.emplace_back(std::string(home) + "/.local/share");
    }
    const std::string_view list = data_dirs != nullptr ? std::string_view{data_dirs}
                                                       : std::string_view{};
    bool any = false;
    std::size_t start = 0;
    while (start <= list.size()) {
        const auto colon = list.find(':', start);
        const auto stop = colon == std::string_view::npos ? list.size() : colon;
        const auto piece = list.substr(start, stop - start);
        if (!piece.empty()) {
            out.emplace_back(piece);
            any = true;
        }
        if (colon == std::string_view::npos) {
            break;
        }
        start = colon + 1;
    }
    if (!any) {
        out.emplace_back("/usr/local/share");
        out.emplace_back("/usr/share");
    }
    return out;
}

std::string exec_binary_of(std::string_view exec) {
    const std::vector<std::string> tokens = split_arguments(unescape_value(exec));
    // `env` and NAME=value prefixes are not the program; the first token
    // after them is. (A `flatpak run ... <app id>` line yields flatpak,
    // which is why the app-id rung comes before the Exec one.)
    std::size_t first = 0;
    while (first < tokens.size() && (tokens[first] == "env" || is_assignment(tokens[first]))) {
        ++first;
    }
    if (first >= tokens.size()) {
        return {};
    }
    return basename_of(tokens[first]);
}

std::vector<DesktopEntry> read_desktop_entries(const std::vector<fs::path>& data_dirs) {
    std::vector<DesktopEntry> out;
    std::unordered_set<std::string> seen;
    for (const auto& dir : data_dirs) {
        const fs::path root = dir / "applications";
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            continue;
        }
        // Collected, then sorted by id: the iterator's order is unspecified
        // and the match rules break ties by order. The walk's own error
        // ends it (what was read so far is kept); one file's does not.
        std::vector<std::pair<std::string, fs::path>> files;
        const fs::recursive_directory_iterator end;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
             !ec && it != end; it.increment(ec)) {
            std::error_code file_ec;
            if (!it->is_regular_file(file_ec) || it->path().extension() != ".desktop") {
                continue;
            }
            std::string id = it->path().lexically_relative(root).generic_string();
            id.erase(id.size() - 8);  // ".desktop"
            std::ranges::replace(id, '/', '-');
            files.emplace_back(std::move(id), it->path());
        }
        std::ranges::sort(files, {}, &std::pair<std::string, fs::path>::first);
        for (auto& [id, file] : files) {
            // The first data dir that carries an id owns it, whatever its
            // file says: a Hidden entry in the user's directory is how the
            // specification deletes a system one.
            if (!seen.insert(id).second) {
                continue;
            }
            if (auto entry = parse_desktop_file(file, id)) {
                out.push_back(std::move(*entry));
            }
        }
    }
    return out;
}

std::optional<std::string> desktop_icon_for(const std::vector<DesktopEntry>& entries,
                                            const AppIdentity& who) {
    const DesktopEntry* hit = best_match(entries, [&who](const DesktopEntry& e) {
        return !who.app_id.empty() && e.id == who.app_id;
    });
    if (hit == nullptr) {
        hit = best_match(entries, [&who](const DesktopEntry& e) {
            return !who.binary.empty() && e.try_exec_binary == who.binary;
        });
    }
    if (hit == nullptr) {
        hit = best_match(entries, [&who](const DesktopEntry& e) {
            return !who.binary.empty() && e.exec_binary == who.binary;
        });
    }
    if (hit == nullptr) {
        hit = best_match(entries, [&who](const DesktopEntry& e) {
            return !e.wm_class.empty() && ((!who.name.empty() && iequals(e.wm_class, who.name)) ||
                                           (!who.binary.empty() && iequals(e.wm_class, who.binary)));
        });
    }
    if (hit == nullptr) {
        hit = best_match(entries, [&who](const DesktopEntry& e) {
            return !who.name.empty() && iequals(e.name, who.name);
        });
    }
    if (hit == nullptr || hit->icon.empty()) {
        return std::nullopt;
    }
    return hit->icon;
}

std::string_view icon_name_without_extension(std::string_view icon) {
    if (icon.starts_with('/')) {
        return icon;
    }
    for (const std::string_view extension : {".png", ".svg", ".svgz", ".xpm"}) {
        if (icon.size() > extension.size() && icon.ends_with(extension)) {
            return icon.substr(0, icon.size() - extension.size());
        }
    }
    return icon;
}

}  // namespace ac3::crucible::ui

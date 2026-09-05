#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The six Crucible translation catalogues (apps/crucible/translations/
// ac3crucible_<code>.ts), read as files rather than through Qt: these cases
// ride the plain ac3tests binary, so they run on a developer's own ctest and
// on every CI leg, including the ones that build no Qt at all. What they
// check is what a reader of the catalogue cannot see at a glance and what a
// hand edit or a machine pass gets wrong: a dropped placeholder, a brand name
// translated into something else, one language holding a string the other
// five do not, and an entry left empty.
//
// lupdate writes a fixed shape - one element per line, no attribute order to
// guess at - so the scan below is a plain string walk. Nothing here needs an
// XML parser, and adding one would make the gate depend on a library the
// suite does not otherwise carry.
//
// docs/crucible/localisation.md is the page these rules are written down on.

namespace {

namespace fs = std::filesystem;

constexpr std::array<std::string_view, 6> kLanguages{"ar", "de", "es", "fr", "he", "yi"};

// Words that name a product, a format or a piece of software. They read the
// same in every language, and a translation that has lost one of them has
// renamed something rather than translated it. German joins a brand into a
// compound with hyphens ("Dolby-Atmos-Szene"), which is that language's own
// orthography, so the check accepts the hyphenated form too.
constexpr std::array<std::string_view, 10> kBrandTerms{
    "AC3Forge", "Crucible", "Dolby Atmos", "E-AC-3", "AC-3",
    "JOC", "PCM", "HDMI", "PipeWire", "WirePlumber"};

// The catalogues are regenerated centrally once the source changes of this
// pass have landed, and the review that fills the new entries follows that
// regeneration (docs/crucible/localisation.md, "Regenerating the
// catalogues"). Until both have happened an unfinished entry is the expected
// state of a freshly extracted string, and a vanished one is the state of a
// string this pass reworded, so the two cases below stand written and idle.
//
// Turn this on in the commit that lands the refilled catalogues, and take
// "(idle until refilled)" out of the two case names when you do. From then
// on an entry left unfinished, or a dead entry left in the file, fails the
// build. Until then the two cases report the count and pass, and the names
// say so rather than claiming a guarantee the bodies do not give.
constexpr bool kCatalogueRefilled = false;

struct Message {
    std::string context;
    std::string source;
    // Whatever stood inside the <translation ...> tag, so a case can ask
    // whether the entry was marked unfinished or vanished.
    std::string attributes;
    std::string translation;
};

[[nodiscard]] fs::path catalogue_path(std::string_view code) {
    return fs::path{AC3FORGE_CRUCIBLE_TS_DIR} / ("ac3crucible_" + std::string{code} + ".ts");
}

[[nodiscard]] std::string read_catalogue(std::string_view code) {
    const auto path = catalogue_path(code);
    std::ifstream in(path, std::ios::binary);
    INFO("catalogue " << path.string());
    REQUIRE(in.is_open());
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// The text between `open` and the matching `close`, starting the search at
// `from`. Returns an empty string when the element is not there, which every
// caller treats as "this message does not carry one".
[[nodiscard]] std::string element(const std::string& text, std::size_t from, std::size_t until,
                                  std::string_view open, std::string_view close) {
    const auto start = text.find(open, from);
    if (start == std::string::npos || start >= until) {
        return {};
    }
    const auto body = start + open.size();
    const auto end = text.find(close, body);
    if (end == std::string::npos || end > until) {
        return {};
    }
    return text.substr(body, end - body);
}

[[nodiscard]] std::vector<Message> parse(const std::string& text) {
    std::vector<Message> messages;
    std::string context;
    std::size_t at = 0;
    while (true) {
        const auto name_at = text.find("<name>", at);
        const auto message_at = text.find("<message", at);
        // A <context> opens with its <name>; every <message> between that and
        // the next one belongs to it.
        const bool name_first = name_at != std::string::npos &&
                                (message_at == std::string::npos || name_at < message_at);
        if (name_first) {
            const auto end = text.find("</name>", name_at);
            if (end == std::string::npos) {
                break;
            }
            context = text.substr(name_at + 6, end - (name_at + 6));
            at = end;
            continue;
        }
        if (message_at == std::string::npos) {
            break;
        }
        const auto message_end = text.find("</message>", message_at);
        if (message_end == std::string::npos) {
            break;
        }
        Message message;
        message.context = context;
        message.source = element(text, message_at, message_end, "<source>", "</source>");
        // The opening tag first, so an entry marked unfinished or vanished is
        // recognised whether or not it carries any text.
        const auto tag_at = text.find("<translation", message_at);
        if (tag_at != std::string::npos && tag_at < message_end) {
            const auto tag_end = text.find('>', tag_at);
            if (tag_end != std::string::npos && tag_end < message_end) {
                message.attributes = text.substr(tag_at, tag_end - tag_at);
                if (text[tag_end - 1] != '/') {
                    const auto body_end = text.find("</translation>", tag_end);
                    if (body_end != std::string::npos && body_end <= message_end) {
                        message.translation = text.substr(tag_end + 1, body_end - (tag_end + 1));
                    }
                }
            }
        }
        messages.push_back(message);
        at = message_end;
    }
    return messages;
}

[[nodiscard]] bool marked(const Message& message, std::string_view type) {
    const std::string wanted = "type=\"" + std::string{type} + "\"";
    return message.attributes.find(wanted) != std::string::npos;
}

// An entry lupdate has just extracted, or one whose English changed under a
// translation: it holds the old text or nothing at all, and the rules below
// that read the translation itself leave it alone. The case that forbids it
// outright is the one kCatalogueRefilled turns on.
[[nodiscard]] bool awaiting_translation(const Message& message) {
    return marked(message, "unfinished");
}

[[nodiscard]] bool blank(std::string_view text) {
    return text.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

// %1 .. %9, in the order they appear, so a translation that reorders them is
// accepted and one that drops or invents one is not.
[[nodiscard]] std::vector<std::string> placeholders(const std::string& text) {
    std::vector<std::string> found;
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] == '%' && text[i + 1] >= '1' && text[i + 1] <= '9') {
            found.push_back(text.substr(i, 2));
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

[[nodiscard]] bool carries_term(const std::string& text, std::string_view term) {
    if (text.find(term) != std::string::npos) {
        return true;
    }
    std::string hyphenated{term};
    std::replace(hyphenated.begin(), hyphenated.end(), ' ', '-');
    return text.find(hyphenated) != std::string::npos;
}

// A short, quotable form of a source, for the message a failure prints.
// The cut walks back off a continuation byte first: every source and every
// translation in ar, he and yi is multi-byte UTF-8, and half a codepoint in
// the one line a reader has to read would print as a broken character.
[[nodiscard]] std::string quoted(const std::string& source) {
    constexpr std::size_t kCap = 72;
    if (source.size() <= kCap) {
        return source;
    }
    std::size_t cut = kCap;
    while (cut > 0 && (static_cast<unsigned char>(source[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return source.substr(0, cut) + "...";
}

// Every entry across the six files that carries the given type, named so a
// failure says which file and which string.
[[nodiscard]] std::vector<std::string> entries_marked(std::string_view type) {
    std::vector<std::string> found;
    for (const auto code : kLanguages) {
        for (const Message& message : parse(read_catalogue(code))) {
            if (marked(message, type)) {
                found.push_back(std::string{code} + " / " + message.context + " / " +
                                quoted(message.source));
            }
        }
    }
    return found;
}

// The first few of them, for the line a failure prints; the rest are in the
// files, and a reader who needs all of them greps for the type.
[[nodiscard]] std::string listed(const std::vector<std::string>& entries) {
    constexpr std::size_t kShown = 5;
    std::string out;
    for (std::size_t i = 0; i < entries.size() && i < kShown; ++i) {
        out += (out.empty() ? "" : "; ") + entries[i];
    }
    if (entries.size() > kShown) {
        out += "; and " + std::to_string(entries.size() - kShown) + " more";
    }
    return out.empty() ? std::string{"none"} : out;
}

}  // namespace

TEST_CASE("crucible translation catalogues carry no unfinished entry (idle until refilled)",
          "[crucible][translations]") {
    const auto unfinished = entries_marked("unfinished");
    if (!unfinished.empty()) {
        WARN("unfinished entries: " << unfinished.size() << ", first: " << unfinished.front()
             << " - this case only fails once kCatalogueRefilled is turned on, which is the"
                " commit that lands the refilled catalogues");
    }
    INFO(listed(unfinished));
    // With the catalogues refilled an unfinished entry is a failure. Before
    // that it is the expected state of a string lupdate has just extracted,
    // and the count above is what the pass still owes a translator.
    CHECK((unfinished.empty() || !kCatalogueRefilled));
}

TEST_CASE("crucible translation catalogues carry no dead entry (idle until refilled)",
          "[crucible][translations]") {
    auto dead = entries_marked("vanished");
    const auto obsolete = entries_marked("obsolete");
    dead.insert(dead.end(), obsolete.begin(), obsolete.end());
    if (!dead.empty()) {
        WARN("dead entries: " << dead.size() << ", first: " << dead.front()
             << " - this case only fails once kCatalogueRefilled is turned on, which is the"
                " commit that lands the refilled catalogues");
    }
    INFO(listed(dead));
    // The same two states as above: a string this pass reworded leaves its
    // old entry behind until the regeneration drops it.
    CHECK((dead.empty() || !kCatalogueRefilled));
}

TEST_CASE("every finished crucible translation is non-empty", "[crucible][translations]") {
    for (const auto code : kLanguages) {
        const auto messages = parse(read_catalogue(code));
        REQUIRE_FALSE(messages.empty());
        for (const Message& message : messages) {
            if (awaiting_translation(message) || blank(message.source)) {
                continue;
            }
            INFO(code << " / " << message.context << ": " << quoted(message.source));
            CHECK_FALSE(blank(message.translation));
        }
    }
}

TEST_CASE("placeholders survive translation in every crucible catalogue",
          "[crucible][translations]") {
    for (const auto code : kLanguages) {
        for (const Message& message : parse(read_catalogue(code))) {
            if (awaiting_translation(message) || blank(message.translation)) {
                continue;
            }
            INFO(code << " / " << message.context << ": " << quoted(message.source));
            INFO("translation: " << quoted(message.translation));
            CHECK(placeholders(message.source) == placeholders(message.translation));
        }
    }
}

TEST_CASE("brand terms survive translation in every crucible catalogue",
          "[crucible][translations]") {
    for (const auto code : kLanguages) {
        for (const Message& message : parse(read_catalogue(code))) {
            if (awaiting_translation(message) || blank(message.translation)) {
                continue;
            }
            for (const auto term : kBrandTerms) {
                if (!carries_term(message.source, term)) {
                    continue;
                }
                INFO(code << " / " << message.context << " [" << term << "]: "
                          << quoted(message.source));
                INFO("translation: " << quoted(message.translation));
                CHECK(carries_term(message.translation, term));
            }
        }
    }
}

TEST_CASE("all six crucible catalogues share one source set", "[crucible][translations]") {
    // Every language is generated from the same extraction, so a difference
    // here is a hand edit or a file that missed the last lupdate rather than
    // anything about the language itself.
    std::set<std::pair<std::string, std::string>> first;
    std::string_view first_code;
    for (const auto code : kLanguages) {
        std::set<std::pair<std::string, std::string>> here;
        for (const Message& message : parse(read_catalogue(code))) {
            here.emplace(message.context, message.source);
        }
        if (first.empty()) {
            first = here;
            first_code = code;
            REQUIRE_FALSE(first.empty());
            continue;
        }
        for (const auto& entry : first) {
            INFO(first_code << " has an entry " << code << " does not: " << entry.first << " / "
                            << quoted(entry.second));
            CHECK(here.contains(entry));
        }
        for (const auto& entry : here) {
            INFO(code << " has an entry " << first_code << " does not: " << entry.first << " / "
                      << quoted(entry.second));
            CHECK(first.contains(entry));
        }
    }
}

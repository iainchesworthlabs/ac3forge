#include "diagnostic_log.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// See diagnostic_log.hpp.

namespace ac3::hearth {

namespace {

constexpr std::string_view kCutMarker = " ...";
constexpr std::string_view kSeparators = "/\\";

// ASCII only, and whatever locale the window has set: a byte keeps its place,
// so a match found in the lowered copy is at the same place in the text.
[[nodiscard]] std::string lowercase(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// One note as one line: control characters become spaces, and a note longer
// than kMaxLine is cut at a UTF-8 boundary and marked. The marker counts
// against the cap, so a cut line is never longer than an uncut one can be.
[[nodiscard]] std::string one_line(std::string_view text) {
    const bool cut = text.size() > DiagnosticLog::kMaxLine;
    std::size_t keep = cut ? DiagnosticLog::kMaxLine - kCutMarker.size() : text.size();
    if (cut) {
        // Never end inside a multi-byte sequence.
        while (keep > 0 && (static_cast<unsigned char>(text[keep]) & 0xC0U) == 0x80U) {
            --keep;
        }
    }
    std::string out;
    out.reserve(keep + kCutMarker.size());
    for (std::size_t i = 0; i < keep; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        out.push_back(c < 0x20U || c == 0x7FU ? ' ' : static_cast<char>(c));
    }
    if (cut) {
        out += kCutMarker;
    }
    return out;
}

// Whether `folder` is the top of a drive or of the file system: nothing but
// separators, or a drive letter and its colon with nothing after them but
// separators.
[[nodiscard]] bool is_top(std::string_view folder) {
    const std::size_t last = folder.find_last_not_of(kSeparators);
    if (last == std::string_view::npos) {
        return true;
    }
    const std::string_view rest = folder.substr(0, last + 1);
    const auto letter = static_cast<unsigned char>(rest.front());
    const bool is_letter = (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z');
    return rest.size() == 2 && is_letter && rest[1] == ':';
}

[[nodiscard]] std::string spelled_with(std::string_view text, char separator) {
    std::string out(text);
    std::ranges::replace_if(
        out, [](char c) { return c == '/' || c == '\\'; }, separator);
    return out;
}

}  // namespace

// --- the ring -------------------------------------------------------------

DiagnosticLog::DiagnosticLog(std::size_t capacity)
    : capacity_(std::max<std::size_t>(capacity, 1)),
      start_(std::chrono::steady_clock::now()),
      wall_start_(std::chrono::system_clock::now()) {
    ring_.reserve(capacity_);
}

void DiagnosticLog::note(std::string_view line) {
    // Built before the lock is taken, so a writer holds it for one move.
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start_)
                             .count();
    std::string entry = fmt::format("+{:04}.{:03} ", elapsed / 1000, elapsed % 1000);
    entry += one_line(line);
    const std::scoped_lock lock(mutex_);
    if (ring_.size() < capacity_) {
        ring_.push_back(std::move(entry));
        return;
    }
    ring_[head_] = std::move(entry);
    head_ = (head_ + 1) % capacity_;
    ++dropped_;
}

std::vector<std::string> DiagnosticLog::lines() const {
    const std::scoped_lock lock(mutex_);
    std::vector<std::string> out;
    out.reserve(ring_.size());
    for (std::size_t i = 0; i < ring_.size(); ++i) {
        out.push_back(ring_[(head_ + i) % ring_.size()]);
    }
    return out;
}

std::uint64_t DiagnosticLog::dropped() const {
    const std::scoped_lock lock(mutex_);
    return dropped_;
}

DiagnosticLog& process_diagnostics() {
    // Never destroyed (see the header): a message written during static
    // destruction must not reach a destroyed log.
    static DiagnosticLog* const log = new DiagnosticLog(DiagnosticLog::kDefaultCapacity);  // NOLINT(cppcoreguidelines-owning-memory)
    return *log;
}

// --- keeping paths out ----------------------------------------------------

void withhold_path(Secrets& secrets, std::string_view path) {
    const auto add = [&secrets](const std::string& text) {
        secrets.strings.push_back(text);
        secrets.strings.push_back(spelled_with(text, '/'));
        secrets.strings.push_back(spelled_with(text, '\\'));
    };
    // Each folder, from the item's own outwards: the text before each
    // separator, less any separators it ends with.
    std::size_t end = path.find_last_of(kSeparators);
    while (end != std::string_view::npos) {
        const std::string_view folder = path.substr(0, end);
        if (is_top(folder)) {
            break;
        }
        const std::string_view trimmed = folder.substr(0, folder.find_last_not_of(kSeparators) + 1);
        if (trimmed.find_first_of(kSeparators) != std::string_view::npos) {
            // Unmistakably a path, wherever it appears.
            add(std::string(trimmed));
        } else {
            // The first folder of a relative path is a lone name, and a
            // folder only where a separator follows it: "music/" is
            // withheld, the word "music" is not.
            add(std::string(trimmed) + '/');
        }
        end = trimmed.find_last_of(kSeparators);
    }
}

std::string scrub(std::string text, const Secrets& secrets) {
    std::vector<std::string> needles;
    needles.reserve(secrets.strings.size());
    for (const std::string& secret : secrets.strings) {
        if (!secret.empty()) {
            needles.push_back(lowercase(secret));
        }
    }
    // Longest first, and each once: a queue's items mostly share folders.
    std::ranges::sort(needles, [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() > b.size() : a < b;
    });
    const auto repeated = std::ranges::unique(needles);
    needles.erase(repeated.begin(), repeated.end());
    if (needles.empty()) {
        return text;
    }
    // The lowered copy is patched in step with the text, and the marker is
    // lower case already, so a replacement is never matched again.
    std::string lowered = lowercase(text);
    for (const std::string& needle : needles) {
        std::size_t at = 0;
        while ((at = lowered.find(needle, at)) != std::string::npos) {
            text.replace(at, needle.size(), kWithheld);
            lowered.replace(at, needle.size(), kWithheld);
            at += kWithheld.size();
        }
    }
    return text;
}

}  // namespace ac3::hearth

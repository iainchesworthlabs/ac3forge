#include "gui_diagnostics.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ac3gui {

namespace {

constexpr std::string_view kWithheld = "<withheld>";
constexpr std::string_view kCutMarker = " ...";

std::string lowercase(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// One note as one line: control characters become spaces, and a note longer
// than kMaxLine is cut at a UTF-8 boundary and marked. The marker counts
// against the cap, so a cut line is never longer than an uncut one.
std::string one_line(std::string_view text) {
    const bool cut = text.size() > MessageLog::kMaxLine;
    std::size_t keep = cut ? MessageLog::kMaxLine - kCutMarker.size() : text.size();
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

// Sized for what the format can produce rather than for what the callers
// will: %f can reach 309 digits before the point for a double at the top of
// its range, and this is that plus a sign, a point, up to a dozen decimals
// and the terminator. Every caller here passes a small `decimals` and a
// duration or an offset, so 64 would hold every answer - but then the size
// depends on an argument no compiler can see, which is the shape that made
// the Crucible widen its own copy (apps/crucible/engine/diagnostics.cpp,
// and see MessageLog::note below for the case where it actually became a
// build error). Same size here, for the same reason.
std::string fixed(double value, int decimals) {
    std::array<char, 344> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%.*f", decimals, value);
    return buffer.data();
}

std::string quoted(const std::string& text) {
    return "\"" + text + "\"";
}

std::string or_none(const std::string& text) {
    return text.empty() ? std::string("(none)") : text;
}

// A value that may span lines (a decoder's complaint, a device's refusal),
// kept to one row.
std::string flat(std::string text) {
    for (std::size_t at = text.find('\n'); at != std::string::npos; at = text.find('\n', at + 2)) {
        text.replace(at, 1, "; ");
    }
    return text;
}

}  // namespace

// --- the ring -----------------------------------------------------------------

MessageLog::MessageLog(std::size_t capacity)
    : capacity_(std::max<std::size_t>(capacity, 1)),
      start_(std::chrono::steady_clock::now()),
      wall_start_(std::chrono::system_clock::now()) {
    ring_.reserve(capacity_);
}

void MessageLog::note(std::string_view line) {
    // The string is built before the lock is taken, so a writer on an encode
    // worker holds it for one push.
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
    // Forty-eight covers a sign, both fields at their full width, the
    // separator, the trailing space and the terminator. Sized for what the
    // format can produce, not for what the clock will say: two long longs
    // are up to twenty characters each, and the Crucible's identical helper
    // is where that stopped being theoretical - GCC 16 under the coverage
    // preset's -fno-inline cannot see that a millisecond count since this
    // object was constructed is small, and reports the truncation as an
    // error (apps/crucible/engine/diagnostics.cpp).
    std::array<char, 48> stamp{};
    std::snprintf(stamp.data(), stamp.size(), "+%04lld.%03lld ", static_cast<long long>(elapsed / 1000),
                  static_cast<long long>(elapsed % 1000));
    std::string entry = stamp.data();
    entry += one_line(line);
    const std::lock_guard<std::mutex> lock(mutex_);
    if (ring_.size() >= capacity_) {
        ring_.erase(ring_.begin());
        ++dropped_;
    }
    ring_.push_back(std::move(entry));
}

std::vector<std::string> MessageLog::lines() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return ring_;
}

std::uint64_t MessageLog::dropped() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

MessageLog& process_diagnostics() {
    // Leaked on purpose (see the header): a note emitted during static
    // destruction must never reach a destroyed log.
    static MessageLog* const log = new MessageLog(MessageLog::kDefaultCapacity);  // NOLINT(cppcoreguidelines-owning-memory)
    return *log;
}

// --- the scrub ------------------------------------------------------------------

std::string scrub(std::string text, const Secrets& secrets) {
    for (const auto& secret : secrets.strings) {
        if (secret.empty()) {
            continue;
        }
        // ASCII lowercase keeps every byte in place, so a position found in
        // the lowered copy is the same position in the text; the copy is
        // patched in step so the marker itself is never re-matched.
        const std::string needle = lowercase(secret);
        const std::string marker_lowered = lowercase(kWithheld);
        std::string lowered = lowercase(text);
        std::size_t at = 0;
        while ((at = lowered.find(needle, at)) != std::string::npos) {
            text.replace(at, needle.size(), kWithheld);
            lowered.replace(at, needle.size(), marker_lowered);
            at += kWithheld.size();
        }
    }
    return text;
}

// --- the report -----------------------------------------------------------------

std::string render_report(const ReportFacts& facts, const MessageLog& log, const Secrets& secrets) {
    std::string out;
    auto line = [&out](std::string_view text) {
        out += text;
        out += '\n';
    };
    auto row = [&out](std::string_view name, std::string_view value) {
        out += name;
        out += ": ";
        out += value;
        out += '\n';
    };

    line("AC3Forge ac3gui diagnostics");
    row("written", facts.written_at);
    row("log started", facts.log_started_at);

    line("");
    line("# version");
    line(facts.version);

    line("");
    line("# platform");
    for (const auto& [name, value] : facts.platform) {
        row(name, value);
    }

    // Whether the variables exist, never what they hold. There is no third
    // row here for a key chosen in the window, because ac3gui has no such
    // control: object signing reaches it through the library's own resolver
    // (src/signing/signing_key.hpp), which reads these two.
    line("");
    line("# signing");
    row("AC3FORGE_SIGNING_KEY_FILE", facts.env_key_file_set ? "set" : "not set");
    row("AC3FORGE_SIGNING_KEY", facts.env_key_inline_set ? "set" : "not set");

    // What was loaded, by name and shape. No folder and no sample data - see
    // the header's rule.
    line("");
    line("# sources (names only; no folder, no contents)");
    if (facts.sources.empty()) {
        line("(none)");
    }
    for (const auto& source : facts.sources) {
        std::string entry = quoted(source.name) + "  " + std::to_string(source.channels) + " ch  " +
                            std::to_string(source.rate_hz) + " Hz  " + fixed(source.seconds, 2) + " s";
        if (source.offset_seconds != 0.0) {
            entry += "  offset " + fixed(source.offset_seconds, 2) + " s";
        }
        if (!source.resample.empty()) {
            entry += "  " + source.resample;
        }
        line(entry);
    }

    line("");
    line("# plan");
    for (const auto& [name, value] : facts.plan) {
        row(name, value);
    }

    // A fixed list from the caller, and the signing/ rule applied again here
    // so a caller that forgets it cannot leak through this section.
    line("");
    line("# settings");
    for (const auto& [key, value] : facts.settings) {
        if (key.starts_with("signing/")) {
            line(key + " = " + std::string(kWithheld));
        } else {
            line(key + " = " + value);
        }
    }

    line("");
    line("# runs (newest first)");
    if (facts.runs.empty()) {
        line("(none)");
    }
    for (const auto& run : facts.runs) {
        std::string entry = std::to_string(run.id) + "  " + quoted(run.filename) + "  " + run.status;
        if (!run.rate_text.empty()) {
            entry += "  " + run.rate_text;
        }
        if (!run.duration_text.empty()) {
            entry += "  " + run.duration_text;
        }
        if (!run.detail.empty()) {
            entry += "  " + flat(run.detail);
        }
        line(entry);
    }

    line("");
    line("# last errors (oldest first)");
    if (facts.errors.empty()) {
        line("(none)");
    }
    for (const auto& error : facts.errors) {
        line(or_none(flat(error)));
    }

    line("");
    const auto notes = log.lines();
    line("# recent messages (oldest first, " + std::to_string(notes.size()) + " of " +
         std::to_string(log.capacity()) + "; " + std::to_string(log.dropped()) + " dropped)");
    for (const auto& note : notes) {
        line(note);
    }

    return scrub(std::move(out), secrets);
}

}  // namespace ac3gui

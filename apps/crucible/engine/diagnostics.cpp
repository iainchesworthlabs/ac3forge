#include "diagnostics.hpp"

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

#include "ac3/oba/oamd.hpp"
#include "default_device.hpp"
#include "engine.hpp"
#include "output_policy.hpp"
#include "virtual_device.hpp"

namespace ac3::crucible {

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

// Every caller passes a small `decimals` and a value from a clock, a level
// or a position, so 64 was the size of the answer rather than the size of
// the format. %f can reach 309 digits before the point for a double at the
// top of its range; this is that plus a sign, a point, up to a dozen
// decimals and the terminator, so no input truncates and no compiler has to
// prove anything about the callers.
std::string fixed(double value, int decimals) {
    std::array<char, 344> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%.*f", decimals, value);
    return buffer.data();
}

std::string_view yes_no(bool value) {
    return value ? "yes" : "no";
}

std::string quoted(const std::string& text) {
    return "\"" + text + "\"";
}

std::string or_none(const std::string& text) {
    return text.empty() ? std::string("(none)") : text;
}

// A value that may span lines (a driver transcript's tail), kept to one row.
std::string flat(std::string text) {
    for (std::size_t at = text.find('\n'); at != std::string::npos; at = text.find('\n', at + 2)) {
        text.replace(at, 1, "; ");
    }
    return text;
}

std::string_view describe_source(KeySource source) {
    switch (source) {
        case KeySource::kNone: return "none";
        case KeySource::kFile: return "a file chosen in Settings (path withheld)";
        case KeySource::kEnvironmentFile: return "AC3FORGE_SIGNING_KEY_FILE (path withheld)";
        case KeySource::kEnvironmentInline: return "AC3FORGE_SIGNING_KEY (value withheld)";
    }
    return "none";
}

std::string position_of(const ac3::oba::Position& p) {
    return "(" + fixed(p.x, 2) + ", " + fixed(p.y, 2) + ", " + fixed(p.z, 2) + ")";
}

}  // namespace

// --- the ring -----------------------------------------------------------------

DiagnosticLog::DiagnosticLog(std::size_t capacity)
    : capacity_(std::max<std::size_t>(capacity, 1)),
      start_(std::chrono::steady_clock::now()),
      wall_start_(std::chrono::system_clock::now()) {
    ring_.reserve(capacity_);
}

void DiagnosticLog::note(std::string_view line) {
    // The string is built before the lock is taken, so a writer on the
    // frame thread holds it for one push.
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
    // Sized for what the format can produce, not for what the clock will:
    // two long longs are up to twenty characters each, and GCC 16 under the
    // coverage preset's -fno-inline cannot see that a millisecond count
    // since this object was constructed is small, so it reports the
    // truncation as an error. Forty-eight covers sign, both fields, the
    // separator, the trailing space and the terminator.
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

std::vector<std::string> DiagnosticLog::lines() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return ring_;
}

std::uint64_t DiagnosticLog::dropped() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

DiagnosticLog& process_diagnostics() {
    // Leaked on purpose (see the header): a message emitted during static
    // destruction must never reach a destroyed log.
    static DiagnosticLog* const log = new DiagnosticLog(DiagnosticLog::kDefaultCapacity);  // NOLINT(cppcoreguidelines-owning-memory)
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

std::string render_report(const ReportFacts& facts, const EngineStatus& engine, const DiagnosticLog& log,
                          const Secrets& secrets) {
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

    line("AC3Forge Crucible diagnostics");
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

    line("");
    line("# signing");
    row("objects", facts.signing.objects_enabled ? "on" : "off");
    row("key source", describe_source(facts.signing.source));
    row("AC3FORGE_SIGNING_KEY_FILE", facts.signing.env_key_file_set ? "set" : "not set");
    row("AC3FORGE_SIGNING_KEY", facts.signing.env_key_inline_set ? "set" : "not set");

    // Named fields only: EngineStatus::signing is never read here.
    line("");
    line("# engine");
    row("running", yes_no(engine.running));
    row("output", std::string(describe(engine.mode)) + " on " + quoted(engine.endpoint_name));
    row("reason", or_none(engine.output_reason));
    line("frames: " + std::to_string(engine.frames_encoded) + "  encode " + fixed(engine.encode_ms, 2) +
         " ms  last " + fixed(engine.last_frame_ms, 1) + " ms  worst " + fixed(engine.worst_frame_ms, 1) + " ms");
    line("underruns: " + std::to_string(engine.underruns) + "  starved reads: " + std::to_string(engine.starved_reads) +
         "  catch-ups: " + std::to_string(engine.catchups) + "  tap backlog " + fixed(engine.tap_backlog_ms, 0) +
         " ms  sink queue " + fixed(engine.sink_queue_ms, 0) + " ms");
    line("taps: " + std::to_string(engine.tap_channels) + " channels  codec bypassed: " +
         std::string(yes_no(engine.codec_bypassed)));
    row("last error", or_none(engine.last_error));

    line("");
    line("# endpoints (last probe)");
    if (engine.endpoints.empty()) {
        line("(none)");
    }
    for (const auto& e : engine.endpoints) {
        std::string spatial = std::string(yes_no(e.spatial));
        if (e.spatial) {
            spatial += " (max " + std::to_string(e.spatial_max_objects) + " objects)";
        }
        line(quoted(e.name) + "  id=" + e.id + "  default=" + std::string(yes_no(e.is_default)) + "  silent=" +
             std::string(yes_no(e.is_null_sink)) + "  eac3=" + std::string(yes_no(e.accepts_eac3)) + "  ac3=" +
             std::string(yes_no(e.accepts_ac3)) + "  pcm=" + std::to_string(e.shared_channels) + "ch  spatial=" +
             spatial);
    }

    // Name and description alone: AppStatus::image_path is never read here.
    line("");
    line("# applications");
    if (engine.apps.empty()) {
        line("(none)");
    }
    for (const auto& a : engine.apps) {
        std::string entry = a.name + " " + quoted(a.description) + ": ";
        entry += a.active ? "active" : "idle";
        entry += a.packaged ? ", packaged" : (a.has_window ? ", window" : ", background");
        entry += a.has_session ? ", session" : ", no session";
        entry += a.tapped ? ", tapped" : ", not tapped";
        if (a.fullscreen) {
            entry += ", full-screen";
        }
        if (a.slot) {
            entry += ", slot " + std::to_string(*a.slot) + " " + position_of(a.position) + " width " +
                     std::to_string(a.width) + " size " + fixed(a.size, 2);
            if (a.width == 2) {
                entry += a.pair_custom ? " custom pair " : " pair ";
                entry += position_of(a.left) + " " + position_of(a.right);
            }
        } else {
            entry += ", in the bed";
        }
        entry += ", " + fixed(static_cast<double>(a.level_dbfs), 1) + " dBFS";
        line(entry);
    }

    line("");
    line("# default output");
    {
        std::string current = "(none)";
        std::string listed;
        for (const auto& endpoint : facts.render_endpoints) {
            if (endpoint.is_default) {
                current = quoted(endpoint.name);
            }
            if (!listed.empty()) {
                listed += "  ";
            }
            listed += quoted(endpoint.name) + " id=" + endpoint.id;
            if (endpoint.is_default) {
                listed += " [default]";
            }
        }
        row("current", current + "  (silent device: " + std::string(yes_no(facts.default_is_silent)) + ")");
        row("render endpoints", or_none(listed));
        row("previous", facts.previous_default_name.empty() ? std::string("(none)") : quoted(facts.previous_default_name));
        row("platform moves the default", yes_no(facts.moves_default));
        row("message", or_none(flat(facts.default_message)));
    }

    line("");
    line("# silent device");
    row("name", quoted(facts.silent_device_name) + "  from a package: " + std::string(yes_no(facts.silent_from_package)));
    row("how to get one", or_none(facts.silent_advice));
    line("needed " + std::string(yes_no(facts.silent.needed)) + "  present " + std::string(yes_no(facts.silent.present)) +
         "  in use " + std::string(yes_no(facts.silent.in_use)) + "  can install " +
         std::string(yes_no(facts.silent.can_install)));
    row("blocker", or_none(facts.silent.blocker));
    if (facts.silent.detail.empty()) {
        row("detail", "(none)");
    }
    for (const auto& detail : facts.silent.detail) {
        row("detail", flat(detail));
    }
    row("driver folder", or_none(facts.driver_dir));
    row("last driver action", or_none(flat(facts.driver_message)));
    row("full-screen rule",
        facts.foreground_available ? std::string("available") : "unavailable: " + or_none(facts.foreground_reason));

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
    const auto notes = log.lines();
    line("# recent messages (oldest first, " + std::to_string(notes.size()) + " of " + std::to_string(log.capacity()) +
         "; " + std::to_string(log.dropped()) + " dropped)");
    for (const auto& n : notes) {
        line(n);
    }

    return scrub(std::move(out), secrets);
}

}  // namespace ac3::crucible

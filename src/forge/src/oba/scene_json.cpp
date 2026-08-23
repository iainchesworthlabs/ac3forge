#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ac3/oba/oamd.hpp"
#include "ac3/oba/scene.hpp"

// The serialised form of an ObjectScene. RFC 8259 JSON, read and written here
// rather than by a dependency - see scene.hpp's own note on why this format and
// not YAML.
//
// The reader is strict about what it does not recognise: an unknown member is
// an error, not something to ignore. A hand-authored scene file's likeliest
// fault by far is a misspelled key, and silently defaulting "gian" to 1.0 would
// produce a scene that is wrong in a way nothing reports. Forward compatibility
// rides on the "ac3forge_scene" version number instead, which is what it is for.

namespace ac3::oba {

namespace {

constexpr int kFormatVersion = 1;
constexpr double kPi = 3.141592653589793238462643383279502884;

struct BedLabel {
    std::string_view name;
    std::uint16_t mask;
};

// TS 103 420 Table 12's channel labels, in the array order that clause uses
// (index 9 downwards), spelled for a JSON file rather than for C++.
constexpr std::array<BedLabel, 10> kBedLabels{{
    {"lr", bed::kLR},
    {"c", bed::kC},
    {"lfe", bed::kLfe},
    {"ls_rs", bed::kLsRs},
    {"lb_rb", bed::kLbRb},
    {"tfl_tfr", bed::kTflTfr},
    {"tsl_tsr", bed::kTslTsr},
    {"tbl_tbr", bed::kTblTbr},
    {"lw_rw", bed::kLwRw},
    {"lfe2", bed::kLfe2},
}};

constexpr std::string_view name_of(Interpolation interp) {
    switch (interp) {
        case Interpolation::kHold:
            return "hold";
        case Interpolation::kSmooth:
            return "smooth";
        case Interpolation::kLinear:
            break;
    }
    return "linear";
}

// --- Writing --------------------------------------------------------------

// Shortest round-trip: std::format's default for a floating-point value is the
// shortest decimal that reads back as the same double, which is what makes a
// scene file survive a save/load cycle bit-exactly and keeps a diff to the
// values that actually changed.
std::string number(double value) { return std::format("{}", value); }

void write_string(std::string& out, std::string_view value) {
    out += '"';
    for (const char c : value) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                // Everything below 0x20 has to be escaped and has no short
                // form; anything at or above it (UTF-8 continuation bytes
                // included) is passed through unchanged, which is what §7
                // allows and what keeps a non-ASCII object name readable in
                // the file.
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                } else {
                    out += c;
                }
                break;
        }
    }
    out += '"';
}

}  // namespace

std::string to_json(const ObjectScene& scene) {
    std::string out;
    out += std::format("{{\n  \"ac3forge_scene\": {},\n", kFormatVersion);
    const auto& orientation = scene.orientation();
    out += std::format("  \"orientation\": {{ \"yaw_rad\": {}, \"pitch_rad\": {}, "
                       "\"roll_rad\": {} }},\n",
                       number(orientation.yaw_rad), number(orientation.pitch_rad),
                       number(orientation.roll_rad));
    out += "  \"objects\": [\n";
    const auto objects = scene.objects();
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const auto& object = objects[i];
        out += "    {\n      \"name\": ";
        write_string(out, object.name);
        out += ",\n      \"bed\": [";
        bool first = true;
        for (const auto& label : kBedLabels) {
            if ((object.bed & label.mask) == 0) {
                continue;
            }
            if (!first) {
                out += ", ";
            }
            first = false;
            write_string(out, label.name);
        }
        out += "],\n      \"automation\": [\n";
        for (std::size_t k = 0; k < object.automation.size(); ++k) {
            const auto& point = object.automation[k];
            // One point per line: a scene under version control should show
            // the cue that moved, not a reflowed block.
            out += std::format(
                "        {{ \"t\": {}, \"x\": {}, \"y\": {}, \"z\": {}, \"gain\": {}, "
                "\"lfe\": {}, \"interp\": \"{}\" }}{}\n",
                number(point.time_s), number(point.position.x), number(point.position.y),
                number(point.position.z), number(point.gain), number(point.lfe_send),
                name_of(point.interp), k + 1 == object.automation.size() ? "" : ",");
        }
        out += std::format("      ]\n    }}{}\n", i + 1 == objects.size() ? "" : ",");
    }
    out += "  ]\n}\n";
    return out;
}

// --- Reading --------------------------------------------------------------

namespace {

// A recursive-descent reader driven by the schema rather than building a
// document: every member this format knows is consumed where it is expected,
// and anything else is a diagnosed error at the point it appears. There is
// deliberately no generic value-skipper - see this file's header comment on why
// an unrecognised member is refused rather than stepped over.
class Reader {
   public:
    explicit Reader(std::string_view text) : text_(text) {}

    [[nodiscard]] const SceneError& error() const { return error_; }

    // Records the first failure and returns false; every reader below
    // propagates that false straight out, so the message a caller sees is the
    // deepest one, not whatever the unwinding hit next.
    bool fail(SceneErrorKind kind, std::string message) {
        if (!failed_) {
            failed_ = true;
            error_ = {.kind = kind, .line = line_at(pos_), .message = std::move(message)};
        }
        return false;
    }

    void skip_space() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                       text_[pos_] == '\n' || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    [[nodiscard]] char peek() {
        skip_space();
        return pos_ < text_.size() ? text_[pos_] : '\0';
    }

    bool expect(char c) {
        if (peek() != c) {
            return fail(SceneErrorKind::kSyntax, std::format("expected '{}'", c));
        }
        ++pos_;
        return true;
    }

    bool at_end() {
        skip_space();
        return pos_ >= text_.size();
    }

    bool read_string(std::string& out) {
        if (!expect('"')) {
            return false;
        }
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) {
                break;
            }
            const char escape = text_[pos_++];
            switch (escape) {
                case '"':
                case '\\':
                case '/':
                    out += escape;
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    unsigned code = 0;
                    if (!read_hex4(code)) {
                        return false;
                    }
                    append_utf8(out, code);
                    break;
                }
                default:
                    return fail(SceneErrorKind::kSyntax, "unknown string escape");
            }
        }
        return fail(SceneErrorKind::kSyntax, "unterminated string");
    }

    bool read_number(double& out) {
        skip_space();
        const std::size_t begin = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) {
            ++pos_;
        }
        // Take the whole numeric-looking run and let from_chars adjudicate it,
        // rather than half-validating here and again there. Deliberately not
        // std::isdigit: that is locale-sensitive, and JSON's digits are not.
        while (pos_ < text_.size() && ((text_[pos_] >= '0' && text_[pos_] <= '9') ||
                                       text_[pos_] == '.' || text_[pos_] == 'e' ||
                                       text_[pos_] == 'E' || text_[pos_] == '+' ||
                                       text_[pos_] == '-')) {
            ++pos_;
        }
        const std::string_view token = text_.substr(begin, pos_ - begin);
        const char* first = token.data();
        if (!token.empty() && token.front() == '+') {
            ++first;
        }
        const auto result = std::from_chars(first, token.data() + token.size(), out);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
            pos_ = begin;
            return fail(SceneErrorKind::kSyntax, "expected a number");
        }
        if (!std::isfinite(out)) {
            pos_ = begin;
            return fail(SceneErrorKind::kBadValue, "numbers must be finite");
        }
        return true;
    }

   private:
    bool read_hex4(unsigned& out) {
        if (pos_ + 4 > text_.size()) {
            return fail(SceneErrorKind::kSyntax, "truncated \\u escape");
        }
        out = 0;
        const auto result = std::from_chars(text_.data() + pos_, text_.data() + pos_ + 4, out, 16);
        if (result.ec != std::errc{} || result.ptr != text_.data() + pos_ + 4) {
            return fail(SceneErrorKind::kSyntax, "malformed \\u escape");
        }
        pos_ += 4;
        return true;
    }

    // Surrogate halves are written through as the replacement character rather
    // than paired: object names are the only strings this format has, and a
    // name outside the BMP is not worth a surrogate-pairing state machine that
    // nothing else would exercise.
    static void append_utf8(std::string& out, unsigned code) {
        if (code >= 0xD800 && code <= 0xDFFF) {
            code = 0xFFFD;
        }
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    [[nodiscard]] std::size_t line_at(std::size_t pos) const {
        return 1 + static_cast<std::size_t>(
                       std::ranges::count(text_.substr(0, std::min(pos, text_.size())), '\n'));
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    bool failed_ = false;
    SceneError error_{};
};

bool read_interpolation(Reader& reader, Interpolation& out) {
    std::string name;
    if (!reader.read_string(name)) {
        return false;
    }
    if (name == "hold") {
        out = Interpolation::kHold;
    } else if (name == "linear") {
        out = Interpolation::kLinear;
    } else if (name == "smooth") {
        out = Interpolation::kSmooth;
    } else {
        return reader.fail(SceneErrorKind::kBadValue,
                           std::format("unknown interpolation '{}' (hold, linear or smooth)",
                                       name));
    }
    return true;
}

bool read_bed(Reader& reader, std::uint16_t& out) {
    if (!reader.expect('[')) {
        return false;
    }
    out = 0;
    if (reader.peek() == ']') {
        reader.expect(']');
        return true;
    }
    while (true) {
        std::string name;
        if (!reader.read_string(name)) {
            return false;
        }
        const auto label = std::ranges::find(kBedLabels, name, &BedLabel::name);
        if (label == kBedLabels.end()) {
            return reader.fail(SceneErrorKind::kBadValue,
                               std::format("unknown bed channel label '{}'", name));
        }
        out |= label->mask;
        if (reader.peek() == ',') {
            reader.expect(',');
            continue;
        }
        return reader.expect(']');
    }
}

bool read_point(Reader& reader, AutomationPoint& out) {
    if (!reader.expect('{')) {
        return false;
    }
    bool have_t = false;
    bool have_x = false;
    bool have_y = false;
    bool have_z = false;
    if (reader.peek() == '}') {
        return reader.fail(SceneErrorKind::kBadField,
                           "an automation point needs at least 't', 'x', 'y' and 'z'");
    }
    while (true) {
        std::string key;
        if (!reader.read_string(key) || !reader.expect(':')) {
            return false;
        }
        bool ok = true;
        if (key == "t") {
            ok = reader.read_number(out.time_s);
            have_t = true;
        } else if (key == "x") {
            ok = reader.read_number(out.position.x);
            have_x = true;
        } else if (key == "y") {
            ok = reader.read_number(out.position.y);
            have_y = true;
        } else if (key == "z") {
            ok = reader.read_number(out.position.z);
            have_z = true;
        } else if (key == "gain") {
            ok = reader.read_number(out.gain);
        } else if (key == "lfe") {
            ok = reader.read_number(out.lfe_send);
        } else if (key == "interp") {
            ok = read_interpolation(reader, out.interp);
        } else {
            return reader.fail(SceneErrorKind::kBadField,
                               std::format("unknown automation member '{}'", key));
        }
        if (!ok) {
            return false;
        }
        if (reader.peek() == ',') {
            reader.expect(',');
            continue;
        }
        if (!reader.expect('}')) {
            return false;
        }
        break;
    }
    if (!have_t || !have_x || !have_y || !have_z) {
        // Defaulting a missing coordinate would silently move the object to
        // room centre, which is exactly the kind of wrong-but-plausible a
        // hand-authored file must not be allowed to be.
        return reader.fail(SceneErrorKind::kBadField,
                           "an automation point needs 't', 'x', 'y' and 'z'");
    }
    return true;
}

bool read_object(Reader& reader, SceneObject& out) {
    if (!reader.expect('{')) {
        return false;
    }
    bool have_automation = false;
    if (reader.peek() == '}') {
        return reader.fail(SceneErrorKind::kBadField, "an object needs an 'automation' array");
    }
    while (true) {
        std::string key;
        if (!reader.read_string(key) || !reader.expect(':')) {
            return false;
        }
        bool ok = true;
        if (key == "name") {
            ok = reader.read_string(out.name);
        } else if (key == "bed") {
            ok = read_bed(reader, out.bed);
        } else if (key == "automation") {
            have_automation = true;
            if (!reader.expect('[')) {
                return false;
            }
            if (reader.peek() == ']') {
                reader.expect(']');
            } else {
                while (true) {
                    AutomationPoint point;
                    if (!read_point(reader, point)) {
                        return false;
                    }
                    out.automation.push_back(point);
                    if (reader.peek() == ',') {
                        reader.expect(',');
                        continue;
                    }
                    if (!reader.expect(']')) {
                        return false;
                    }
                    break;
                }
            }
        } else {
            return reader.fail(SceneErrorKind::kBadField,
                               std::format("unknown object member '{}'", key));
        }
        if (!ok) {
            return false;
        }
        if (reader.peek() == ',') {
            reader.expect(',');
            continue;
        }
        if (!reader.expect('}')) {
            return false;
        }
        break;
    }
    if (!have_automation) {
        return reader.fail(SceneErrorKind::kBadField, "an object needs an 'automation' array");
    }
    return true;
}

// yaw/pitch/roll, each accepted as either radians or degrees but never both -
// a file that gave both would leave which one is in force to whichever the
// reader happened to see last, and that is not a thing to guess at.
bool read_angle(Reader& reader, std::string_view key, double& out, bool& seen) {
    if (seen) {
        return reader.fail(SceneErrorKind::kBadField,
                           std::format("orientation gives '{}' twice, or in both units", key));
    }
    seen = true;
    if (!reader.read_number(out)) {
        return false;
    }
    if (key.ends_with("_deg")) {
        out *= kPi / 180.0;
    }
    return true;
}

bool read_orientation(Reader& reader, Orientation& out) {
    if (!reader.expect('{')) {
        return false;
    }
    bool yaw = false;
    bool pitch = false;
    bool roll = false;
    if (reader.peek() == '}') {
        reader.expect('}');
        return true;
    }
    while (true) {
        std::string key;
        if (!reader.read_string(key) || !reader.expect(':')) {
            return false;
        }
        bool ok = true;
        if (key == "yaw_rad" || key == "yaw_deg") {
            ok = read_angle(reader, key, out.yaw_rad, yaw);
        } else if (key == "pitch_rad" || key == "pitch_deg") {
            ok = read_angle(reader, key, out.pitch_rad, pitch);
        } else if (key == "roll_rad" || key == "roll_deg") {
            ok = read_angle(reader, key, out.roll_rad, roll);
        } else {
            return reader.fail(SceneErrorKind::kBadField,
                               std::format("unknown orientation member '{}'", key));
        }
        if (!ok) {
            return false;
        }
        if (reader.peek() == ',') {
            reader.expect(',');
            continue;
        }
        return reader.expect('}');
    }
}

}  // namespace

std::expected<ObjectScene, SceneError> scene_from_json(std::string_view text) {
    Reader reader{text};
    std::vector<SceneObject> objects;
    Orientation orientation{};
    bool have_version = false;
    bool have_objects = false;

    if (!reader.expect('{')) {
        return std::unexpected(reader.error());
    }
    if (reader.peek() != '}') {
        while (true) {
            std::string key;
            if (!reader.read_string(key) || !reader.expect(':')) {
                return std::unexpected(reader.error());
            }
            if (key == "ac3forge_scene") {
                double version = 0.0;
                if (!reader.read_number(version)) {
                    return std::unexpected(reader.error());
                }
                if (version != kFormatVersion) {
                    reader.fail(SceneErrorKind::kBadValue,
                                std::format("this is an ac3forge scene version {}; this build "
                                            "reads version {}",
                                            version, kFormatVersion));
                    return std::unexpected(reader.error());
                }
                have_version = true;
            } else if (key == "orientation") {
                if (!read_orientation(reader, orientation)) {
                    return std::unexpected(reader.error());
                }
            } else if (key == "objects") {
                have_objects = true;
                if (!reader.expect('[')) {
                    return std::unexpected(reader.error());
                }
                if (reader.peek() == ']') {
                    reader.expect(']');
                } else {
                    while (true) {
                        SceneObject object;
                        if (!read_object(reader, object)) {
                            return std::unexpected(reader.error());
                        }
                        objects.push_back(std::move(object));
                        if (reader.peek() == ',') {
                            reader.expect(',');
                            continue;
                        }
                        if (!reader.expect(']')) {
                            return std::unexpected(reader.error());
                        }
                        break;
                    }
                }
            } else {
                reader.fail(SceneErrorKind::kBadField,
                            std::format("unknown scene member '{}'", key));
                return std::unexpected(reader.error());
            }
            if (reader.peek() == ',') {
                reader.expect(',');
                continue;
            }
            break;
        }
    }
    if (!reader.expect('}')) {
        return std::unexpected(reader.error());
    }
    if (!reader.at_end()) {
        reader.fail(SceneErrorKind::kSyntax, "trailing text after the scene");
        return std::unexpected(reader.error());
    }
    if (!have_version) {
        reader.fail(SceneErrorKind::kBadField,
                    "not an ac3forge scene: no 'ac3forge_scene' version member");
        return std::unexpected(reader.error());
    }
    if (!have_objects) {
        reader.fail(SceneErrorKind::kBadField, "a scene needs an 'objects' array");
        return std::unexpected(reader.error());
    }
    return ObjectScene::create(std::move(objects), orientation);
}

}  // namespace ac3::oba

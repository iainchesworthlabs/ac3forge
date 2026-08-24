#include "ac3/oba/scene.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <expected>
#include <fmt/format.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ac3/oba/atmos.hpp"
#include "ac3/oba/oamd.hpp"
#include "scene_text.hpp"

namespace ac3::oba {

namespace {

// Exactly KeyframePath's own arithmetic (src/oba/motion.cpp), not a
// rearrangement of it: a scene built from a legacy keyframe file has to
// evaluate to the same doubles that file produced before this type existed,
// and "the same formula, reassociated" is not the same double.
double lerp(double a, double b, double f) { return a + (b - a) * f; }

// f*f*(3-2f): zero slope at both ends of the segment, monotone in between.
double smoothstep(double f) { return f * f * (3.0 - 2.0 * f); }

ObjectPlacement placement_of(const AutomationPoint& point) {
    return {.position = point.position, .gain = point.gain, .lfe_send = point.lfe_send};
}

ObjectPlacement interpolate(const AutomationPoint& a, const AutomationPoint& b, double time_s) {
    if (a.interp == Interpolation::kHold) {
        return placement_of(a);
    }
    double f = (time_s - a.time_s) / (b.time_s - a.time_s);
    if (a.interp == Interpolation::kSmooth) {
        f = smoothstep(f);
    }
    return {.position = {.x = lerp(a.position.x, b.position.x, f),
                         .y = lerp(a.position.y, b.position.y, f),
                         .z = lerp(a.position.z, b.position.z, f)},
            .gain = lerp(a.gain, b.gain, f),
            .lfe_send = lerp(a.lfe_send, b.lfe_send, f)};
}

bool finite(const AutomationPoint& point) {
    return std::isfinite(point.time_s) && std::isfinite(point.position.x) &&
           std::isfinite(point.position.y) && std::isfinite(point.position.z) &&
           std::isfinite(point.gain) && std::isfinite(point.lfe_send);
}

bool known(Interpolation interp) {
    return interp == Interpolation::kHold || interp == Interpolation::kLinear ||
           interp == Interpolation::kSmooth;
}

SceneError bad_value(std::string message) {
    return {.kind = SceneErrorKind::kBadValue, .line = 0, .message = std::move(message)};
}

// An object's printable identity for a diagnostic: its index always, plus its
// name when it has one, since an unnamed object (everything a legacy keyframe
// file produces) has nothing else to go on.
std::string label_of(std::size_t index, const SceneObject& object) {
    if (object.name.empty()) {
        return fmt::format("object {}", index);
    }
    return fmt::format("object {} ({})", index, object.name);
}

}  // namespace

Orientation orientation_from_degrees(double yaw_deg, double pitch_deg, double roll_deg) {
    constexpr double kPerDegree = 3.141592653589793238462643383279502884 / 180.0;
    return {.yaw_rad = yaw_deg * kPerDegree,
            .pitch_rad = pitch_deg * kPerDegree,
            .roll_rad = roll_deg * kPerDegree};
}

Position rotate(const Position& position, const Orientation& orientation) {
    // Exactly zero is the overwhelmingly common case and has to be free of
    // rounding, not merely close to it - see Orientation's own comment on why
    // an un-oriented scene must produce bit-identical positions.
    if (orientation.yaw_rad == 0.0 && orientation.pitch_rad == 0.0 &&
        orientation.roll_rad == 0.0) {
        return position;
    }
    // Into the centred cube: u right, v back, w up, each [-1, +1] about the
    // room centre. x/y are [0,1] in §4.2.1 and z is already centred.
    double u = 2.0 * position.x - 1.0;
    double v = 2.0 * position.y - 1.0;
    double w = position.z;

    // Yaw about the vertical axis, positive turning the scene clockwise seen
    // from above (an object at the front wall moves to the right wall).
    if (orientation.yaw_rad != 0.0) {
        const double c = std::cos(orientation.yaw_rad);
        const double s = std::sin(orientation.yaw_rad);
        const double ru = u * c - v * s;
        const double rv = u * s + v * c;
        u = ru;
        v = rv;
    }
    // Pitch about the left-right axis, positive raising the front of the scene.
    if (orientation.pitch_rad != 0.0) {
        const double c = std::cos(orientation.pitch_rad);
        const double s = std::sin(orientation.pitch_rad);
        const double rv = v * c + w * s;
        const double rw = -v * s + w * c;
        v = rv;
        w = rw;
    }
    // Roll about the front-back axis, positive raising the right of the scene.
    if (orientation.roll_rad != 0.0) {
        const double c = std::cos(orientation.roll_rad);
        const double s = std::sin(orientation.roll_rad);
        const double ru = u * c - w * s;
        const double rw = u * s + w * c;
        u = ru;
        w = rw;
    }

    return {.x = std::clamp(0.5 * (u + 1.0), 0.0, 1.0),
            .y = std::clamp(0.5 * (v + 1.0), 0.0, 1.0),
            .z = std::clamp(w, -1.0, 1.0)};
}

std::expected<ObjectScene, SceneError> ObjectScene::create(std::vector<SceneObject> objects,
                                                           const Orientation& orientation) {
    if (!std::isfinite(orientation.yaw_rad) || !std::isfinite(orientation.pitch_rad) ||
        !std::isfinite(orientation.roll_rad)) {
        return std::unexpected(bad_value("orientation angles must be finite"));
    }
    for (std::size_t i = 0; i < objects.size(); ++i) {
        auto& object = objects[i];
        if (object.automation.empty()) {
            return std::unexpected(
                SceneError{.kind = SceneErrorKind::kEmptyObject,
                          .line = 0,
                          .message = fmt::format("{} has no automation points", label_of(i, object))});
        }
        for (const auto& point : object.automation) {
            if (!finite(point)) {
                return std::unexpected(bad_value(
                    fmt::format("{} has a non-finite automation value", label_of(i, object))));
            }
            if (!known(point.interp)) {
                return std::unexpected(
                    bad_value(fmt::format("{} has an unknown interpolation", label_of(i, object))));
            }
        }
        std::ranges::sort(object.automation, {}, &AutomationPoint::time_s);
        const auto duplicate =
            std::ranges::adjacent_find(object.automation, [](const AutomationPoint& a,
                                                             const AutomationPoint& b) {
                return a.time_s == b.time_s;
            });
        if (duplicate != object.automation.end()) {
            return std::unexpected(SceneError{
                .kind = SceneErrorKind::kDuplicateTime,
                .line = 0,
                .message = fmt::format("{} has two automation points at t={}", label_of(i, object),
                                       duplicate->time_s)});
        }
    }
    return ObjectScene{std::move(objects), orientation};
}

double ObjectScene::duration_s() const {
    double last = 0.0;
    for (const auto& object : objects_) {
        // create() guarantees a non-empty, time-sorted automation, so back()
        // is the object's last cue.
        last = std::max(last, object.automation.back().time_s);
    }
    return last;
}

ObjectPlacement ObjectScene::evaluate(std::size_t object, double time_s) const {
    if (object >= objects_.size()) {
        return {};
    }
    const auto& automation = objects_[object].automation;
    ObjectPlacement placement{};
    if (time_s <= automation.front().time_s) {
        placement = placement_of(automation.front());
    } else if (time_s >= automation.back().time_s) {
        placement = placement_of(automation.back());
    } else {
        const auto next =
            std::ranges::upper_bound(automation, time_s, {}, &AutomationPoint::time_s);
        placement = interpolate(*(next - 1), *next, time_s);
    }
    placement.position = rotate(placement.position, orientation_);
    return placement;
}

void ObjectScene::evaluate_into(double time_s, std::span<ObjectPlacement> out) const {
    const std::size_t count = std::min(out.size(), objects_.size());
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = evaluate(i, time_s);
    }
}

std::vector<ObjectPlacement> ObjectScene::evaluate(double time_s) const {
    std::vector<ObjectPlacement> placement(objects_.size());
    evaluate_into(time_s, placement);
    return placement;
}

// --- Live -----------------------------------------------------------------

SceneCursor::SceneCursor(ObjectScene scene)
    : scene_(std::move(scene)), live_(scene_.object_count()) {}

bool SceneCursor::push(const SceneUpdate& update) {
    if (update.object >= live_.size()) {
        return false;
    }
    live_[update.object] = update.placement;
    return true;
}

void SceneCursor::release(std::size_t object) {
    if (object < live_.size()) {
        live_[object].reset();
    }
}

void SceneCursor::release_all() {
    std::ranges::fill(live_, std::nullopt);
}

bool SceneCursor::is_live(std::size_t object) const {
    return object < live_.size() && live_[object].has_value();
}

void SceneCursor::sample_into(double time_s, std::span<ObjectPlacement> out) const {
    const std::size_t count = std::min(out.size(), scene_.object_count());
    for (std::size_t i = 0; i < count; ++i) {
        // Bound once rather than indexing live_ twice: clang-tidy's
        // bugprone-unchecked-optional-access cannot see that two separate
        // live_[i] calls name the same optional, and flags the dereference
        // below as unchecked even though the branch it is in guarantees it.
        const std::optional<ObjectPlacement>* const pushed = i < live_.size() ? &live_[i] : nullptr;
        if (pushed && *pushed) {
            out[i] = **pushed;
            out[i].position = rotate(out[i].position, scene_.orientation());
            continue;
        }
        out[i] = scene_.evaluate(i, time_s);
    }
}

std::vector<ObjectPlacement> SceneCursor::sample(double time_s) const {
    std::vector<ObjectPlacement> placement(scene_.object_count());
    sample_into(time_s, placement);
    return placement;
}

// --- The keyframe grammar -------------------------------------------------

namespace {

// One whitespace-separated token, advancing `rest`. Empty when the line is
// exhausted. Written by hand rather than with istringstream because the
// parser reports a column-free line number and nothing else needs a stream.
std::string_view next_token(std::string_view& rest) {
    const auto begin = rest.find_first_not_of(" \t\r\f\v");
    if (begin == std::string_view::npos) {
        rest = {};
        return {};
    }
    rest.remove_prefix(begin);
    const auto end = rest.find_first_of(" \t\r\f\v");
    const auto token = rest.substr(0, end);
    rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
    return token;
}

bool parse_index(std::string_view token, std::size_t& out) {
    if (token.empty()) {
        return false;
    }
    const auto result = std::from_chars(token.data(), token.data() + token.size(), out);
    return result.ec == std::errc{} && result.ptr == token.data() + token.size();
}

}  // namespace

std::expected<std::vector<SceneObject>, SceneError> scene_objects_from_keyframe_text(
    std::string_view text) {
    constexpr auto kNoNewline = std::string_view::npos;
    const auto malformed = [](std::size_t lineno) {
        return SceneError{.kind = SceneErrorKind::kSyntax,
                          .line = lineno,
                          .message = "expected 'object time_s x y z gain lfe_send'"};
    };

    std::vector<SceneObject> by_object;
    std::size_t lineno = 0;
    // pos walks past the end by one so a file whose last line has no newline
    // is still visited, and an empty file is visited exactly once (as one
    // empty line) rather than not at all.
    for (std::size_t pos = 0; pos <= text.size();) {
        ++lineno;
        const auto newline = text.find('\n', pos);
        std::string_view line =
            text.substr(pos, newline == kNoNewline ? kNoNewline : newline - pos);
        pos = newline == kNoNewline ? text.size() + 1 : newline + 1;

        if (const auto hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        const auto index_token = next_token(line);
        if (index_token.empty()) {
            continue;  // blank, or comment-only, line
        }
        std::size_t object = 0;
        if (!parse_index(index_token, object)) {
            return std::unexpected(malformed(lineno));
        }
        // The index sizes the returned vector, so a typo'd one is an
        // allocation this function would otherwise attempt on a file's say-so.
        // The ceiling is far above anything an object programme can be -
        // §8.3.2.2 caps an Atmos programme at 16 objects and §5.5.2's own
        // object_count field at 31 - and exists only to keep a malformed file
        // from being a memory question.
        constexpr std::size_t kMaxObjectIndex = 1023;
        if (object > kMaxObjectIndex) {
            return std::unexpected(SceneError{
                .kind = SceneErrorKind::kBadValue,
                .line = lineno,
                .message = fmt::format("object index {} is out of range (0 to {})", object,
                                       kMaxObjectIndex)});
        }
        AutomationPoint point;
        // The seven columns in order, the leading object index already taken.
        const std::array<double*, 6> columns{&point.time_s,    &point.position.x,
                                             &point.position.y, &point.position.z,
                                             &point.gain,       &point.lfe_send};
        for (double* column : columns) {
            if (!read_double(next_token(line), *column)) {
                return std::unexpected(malformed(lineno));
            }
        }
        if (object >= by_object.size()) {
            by_object.resize(object + 1);
        }
        by_object[object].automation.push_back(point);
    }
    return by_object;
}

std::string to_keyframe_text(std::span<const SceneObject> objects) {
    std::string out;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        for (const auto& point : objects[i].automation) {
            out += fmt::format("{} {} {} {} {} {} {}\n", i, point.time_s, point.position.x,
                               point.position.y, point.position.z, point.gain, point.lfe_send);
        }
    }
    return out;
}

std::string to_keyframe_text(const ObjectScene& scene) {
    return to_keyframe_text(scene.objects());
}

std::expected<SceneContents, SceneError> read_scene(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n\f\v");
    if (first != std::string_view::npos && text[first] == '{') {
        return read_scene_json(text);
    }
    auto objects = scene_objects_from_keyframe_text(text);
    if (!objects) {
        return std::unexpected(std::move(objects.error()));
    }
    // The keyframe grammar has no column for an orientation, so a file in it
    // is always an un-turned scene.
    return SceneContents{.objects = std::move(*objects), .orientation = {}};
}

std::expected<ObjectScene, SceneError> scene_from_text(std::string_view text,
                                                       const ObjectPlacement& fallback) {
    auto contents = read_scene(text);
    if (!contents) {
        return std::unexpected(std::move(contents.error()));
    }
    for (auto& object : contents->objects) {
        if (object.automation.empty()) {
            object.automation.push_back({.time_s = 0.0,
                                         .position = fallback.position,
                                         .gain = fallback.gain,
                                         .lfe_send = fallback.lfe_send});
        }
    }
    return ObjectScene::create(std::move(contents->objects), contents->orientation);
}

}  // namespace ac3::oba

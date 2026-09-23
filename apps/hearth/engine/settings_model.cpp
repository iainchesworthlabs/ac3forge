#include "settings_model.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ac3/render/layout.hpp"
#include "ac3/render/routing.hpp"

// See settings_model.hpp.

namespace ac3::hearth {

namespace {

// A DNS label, which the mDNS instance name is.
constexpr std::size_t kLabelBytes = 63;
// The most items a saved queue is read to: a damaged count must not keep the
// walk going for ever.
constexpr std::uint64_t kMaxSavedItems = 100000;

constexpr std::string_view kGapless = "playback/gapless";
constexpr std::string_view kResumeQueue = "playback/resumeQueue";
constexpr std::string_view kOnFailure = "playback/onFailure";
constexpr std::string_view kName = "network/name";
constexpr std::string_view kDiscover = "network/discover";
constexpr std::string_view kQueue = "queue";
constexpr std::string_view kQueueSize = "queue/size";
constexpr std::string_view kQueueCurrent = "queue/current";
constexpr std::string_view kQueuePosition = "queue/positionMs";

constexpr std::string_view kSpeakers = "speakers";
constexpr std::string_view kSpeakersLayout = "speakers/layout";
constexpr std::string_view kSpeakersSlots = "speakers/slots";
constexpr std::string_view kSpeakersCrossoverHz = "speakers/crossoverHz";
constexpr std::string_view kSpeakersRouting = "speakers/routing";
constexpr std::string_view kSpeakersRoutingOutputs = "speakers/routingOutputs";

[[nodiscard]] std::string_view text_of(bool on) {
    return on ? "true" : "false";
}

[[nodiscard]] std::string_view text_of(FailurePolicy policy) {
    return policy == FailurePolicy::kStop ? "stop" : "skip";
}

// {fmt}'s default float formatting is locale-independent (CONTRIBUTING.md:
// "{fmt} only formats out") and round-trips exactly through double_of()
// below, the same pairing item_key()'s neighbours already trust for every
// other typed value here.
[[nodiscard]] std::string text_of(double value) {
    return fmt::format("{}", value);
}

[[nodiscard]] std::optional<bool> bool_of(const std::optional<std::string>& text) {
    if (text == "true") {
        return true;
    }
    if (text == "false") {
        return false;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<FailurePolicy> policy_of(const std::optional<std::string>& text) {
    if (text == "skip") {
        return FailurePolicy::kSkip;
    }
    if (text == "stop") {
        return FailurePolicy::kStop;
    }
    return std::nullopt;
}

// A whole number of decimal digits and nothing else.
[[nodiscard]] std::optional<std::uint64_t> number_of(const std::optional<std::string>& text) {
    if (!text || text->empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const char* const end = text->data() + text->size();
    const auto [at, error] = std::from_chars(text->data(), end, value);
    if (error != std::errc{} || at != end) {
        return std::nullopt;
    }
    return value;
}

// A signed decimal number, read the same locale-independent way as
// number_of() above - not strtod, whose result depends on the process
// locale, which Qt sets from the user's (ac3::sendspin::json's own header
// comment). std::from_chars for floating point is unavailable only on
// Android and at the macOS wheel's deployment target (CONTRIBUTING.md) -
// neither is a target apps/hearth's CMakeLists.txt builds for (WIN32 OR
// APPLE OR LINUX, desktop only), the same platform set apps/cli/support.cpp
// already relies on this for.
[[nodiscard]] std::optional<double> double_of(const std::optional<std::string>& text) {
    if (!text || text->empty()) {
        return std::nullopt;
    }
    double value = 0.0;
    const char* const end = text->data() + text->size();
    const auto [at, error] = std::from_chars(text->data(), end, value);
    if (error != std::errc{} || at != end) {
        return std::nullopt;
    }
    return value;
}

// QSettings' own array layout: counted from 1.
[[nodiscard]] std::string item_key(std::size_t index, std::string_view field) {
    return fmt::format("{}/{}/{}", kQueue, index + 1, field);
}

// The same array layout for the speaker setup's own two flat lists -
// "speakers/trimDb/1", not the queue's "queue/1/path", since a slot has no
// second field to group it with the way a queue item's path and title
// share one index for.
[[nodiscard]] std::string speaker_key(std::string_view field, std::size_t index) {
    return fmt::format("{}/{}/{}", kSpeakers, field, index + 1);
}

// What the queue shows for an item until metadata has been read: the file's
// own name.
[[nodiscard]] std::string name_of(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return std::string{slash == std::string_view::npos ? path : path.substr(slash + 1)};
}

[[nodiscard]] std::string trimmed(std::string text) {
    const std::size_t first = text.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(' ');
    return text.substr(first, last - first + 1);
}

}  // namespace

// --- the store ------------------------------------------------------------

MemorySettingsStore::MemorySettingsStore(Values values) : values_(values), synced_(std::move(values)) {}

bool MemorySettingsStore::sync() {
    if (sync_fails_) {
        return false;
    }
    synced_ = values_;
    return true;
}

std::optional<std::string> MemorySettingsStore::value(std::string_view key) const {
    const auto found = values_.find(key);
    if (found == values_.end()) {
        return std::nullopt;
    }
    return found->second;
}

void MemorySettingsStore::set_value(std::string_view key, std::string_view value) {
    values_.insert_or_assign(std::string{key}, std::string{value});
}

void MemorySettingsStore::remove_group(std::string_view group) {
    // As QSettings::remove() does: the key itself, and every key under it.
    const std::string prefix = std::string{group} + "/";
    auto at = values_.lower_bound(prefix);
    while (at != values_.end() && at->first.starts_with(prefix)) {
        at = values_.erase(at);
    }
    const auto same = values_.find(group);
    if (same != values_.end()) {
        values_.erase(same);
    }
}

// --- names ----------------------------------------------------------------

std::string network_name(std::string_view typed) {
    std::string kept;
    kept.reserve(typed.size());
    for (const char c : typed) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte >= 0x20U && byte != 0x7FU) {
            kept.push_back(c);
        }
    }
    kept = trimmed(std::move(kept));
    if (kept.size() > kLabelBytes) {
        std::size_t keep = kLabelBytes;
        // Never end inside a multi-byte sequence.
        while (keep > 0 && (static_cast<unsigned char>(kept[keep]) & 0xC0U) == 0x80U) {
            --keep;
        }
        kept.resize(keep);
        kept = trimmed(std::move(kept));
    }
    return kept;
}

std::string default_network_name(std::string_view host) {
    const std::string name = network_name(host);
    return network_name(name.empty() ? std::string{"Hearth"} : "Hearth on " + name);
}

// --- the settings ---------------------------------------------------------

EngineSettings load_settings(const SettingsStore& store, std::string_view host) {
    EngineSettings out;
    PlaybackSettings& playback = out.playback;
    playback.gapless = bool_of(store.value(kGapless)).value_or(playback.gapless);
    playback.resume_queue = bool_of(store.value(kResumeQueue)).value_or(playback.resume_queue);
    playback.on_failure = policy_of(store.value(kOnFailure)).value_or(playback.on_failure);
    NetworkSettings& network = out.network;
    network.name = network_name(store.value(kName).value_or(std::string{}));
    if (network.name.empty()) {
        network.name = default_network_name(host);
    }
    network.discover = bool_of(store.value(kDiscover)).value_or(network.discover);
    return out;
}

void save_settings(const EngineSettings& settings, SettingsStore& store) {
    for (const auto& [key, value] : settings_rows(settings)) {
        store.set_value(key, value);
    }
}

std::vector<std::pair<std::string, std::string>> settings_rows(const EngineSettings& settings) {
    std::vector<std::pair<std::string, std::string>> rows;
    rows.emplace_back(kGapless, text_of(settings.playback.gapless));
    rows.emplace_back(kResumeQueue, text_of(settings.playback.resume_queue));
    rows.emplace_back(kOnFailure, text_of(settings.playback.on_failure));
    rows.emplace_back(kName, network_name(settings.network.name));
    rows.emplace_back(kDiscover, text_of(settings.network.discover));
    return rows;
}

// --- the saved queue ------------------------------------------------------

SavedQueue saved_queue(const EngineStatus& status, const PlayPosition& position) {
    SavedQueue out;
    out.items.reserve(status.queue.size());
    for (const QueueItem& item : status.queue) {
        QueueItem kept;
        kept.path = item.path;
        kept.title = item.title;
        out.items.push_back(std::move(kept));
    }
    if (position.item < out.items.size()) {
        out.current = position.item;
        out.position = std::max(position.heard, std::chrono::milliseconds{0});
    } else if (status.current < out.items.size()) {
        out.current = status.current;
    }
    return out;
}

void save_queue(const SavedQueue& saved, SettingsStore& store) {
    store.remove_group(kQueue);
    store.set_value(kQueueSize, std::to_string(saved.items.size()));
    for (std::size_t i = 0; i < saved.items.size(); ++i) {
        store.set_value(item_key(i, "path"), saved.items[i].path);
        store.set_value(item_key(i, "title"), saved.items[i].title);
    }
    if (saved.current < saved.items.size()) {
        store.set_value(kQueueCurrent, std::to_string(saved.current + 1));
        const auto ms = std::max<std::chrono::milliseconds::rep>(saved.position.count(), 0);
        store.set_value(kQueuePosition, std::to_string(ms));
    }
}

SavedQueue load_queue(const SettingsStore& store) {
    SavedQueue out;
    const auto size = number_of(store.value(kQueueSize));
    if (!size) {
        return out;
    }
    const std::uint64_t count = std::min(*size, kMaxSavedItems);
    const auto current = number_of(store.value(kQueueCurrent));
    for (std::size_t i = 0; i < count; ++i) {
        auto path = store.value(item_key(i, "path"));
        if (!path || path->empty()) {
            continue;
        }
        if (current == i + 1) {
            out.current = out.items.size();
        }
        QueueItem item;
        item.title = store.value(item_key(i, "title")).value_or(std::string{});
        if (item.title.empty()) {
            item.title = name_of(*path);
        }
        item.path = std::move(*path);
        out.items.push_back(std::move(item));
    }
    const auto ms = number_of(store.value(kQueuePosition));
    const auto most = static_cast<std::uint64_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max());
    if (out.current != Queue::kNone && ms && *ms <= most) {
        out.position = std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(*ms)};
    }
    return out;
}

// --- the saved speaker setup ------------------------------------------

SavedSpeakerSetup saved_speaker_setup(const EngineStatus& status) {
    SavedSpeakerSetup out;
    out.layout = std::string(status.layout.text());
    out.trim_db = status.trim_db;
    out.delay_ms = status.delay_ms;
    out.crossover_hz = status.crossover_hz;
    std::array<char, ac3::render::Routing::kTextBytes> routing_text{};
    const std::size_t written = status.routing.format(routing_text);
    out.routing.assign(routing_text.data(), written);
    out.routing_outputs = status.routing.outputs();
    return out;
}

void save_speaker_setup(const SavedSpeakerSetup& saved, SettingsStore& store) {
    store.remove_group(kSpeakers);
    store.set_value(kSpeakersLayout, saved.layout);
    // trim_db and delay_ms are always the same length (SavedSpeakerSetup's
    // own header comment) - one count covers both.
    store.set_value(kSpeakersSlots, std::to_string(saved.trim_db.size()));
    for (std::size_t i = 0; i < saved.trim_db.size(); ++i) {
        store.set_value(speaker_key("trimDb", i), text_of(saved.trim_db[i]));
        store.set_value(speaker_key("delayMs", i), text_of(saved.delay_ms[i]));
    }
    store.set_value(kSpeakersCrossoverHz, text_of(saved.crossover_hz));
    if (!saved.routing.empty()) {
        store.set_value(kSpeakersRouting, saved.routing);
        store.set_value(kSpeakersRoutingOutputs, std::to_string(saved.routing_outputs));
    }
}

SavedSpeakerSetup load_speaker_setup(const SettingsStore& store) {
    SavedSpeakerSetup out;
    out.layout = store.value(kSpeakersLayout).value_or(std::string{});
    if (const auto slots = number_of(store.value(kSpeakersSlots))) {
        const std::uint64_t count =
            std::min(*slots, static_cast<std::uint64_t>(ac3::render::OutputLayout::kMaxSlots));
        out.trim_db.reserve(count);
        out.delay_ms.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            out.trim_db.push_back(double_of(store.value(speaker_key("trimDb", i))).value_or(0.0));
            out.delay_ms.push_back(double_of(store.value(speaker_key("delayMs", i))).value_or(0.0));
        }
    }
    out.crossover_hz = double_of(store.value(kSpeakersCrossoverHz)).value_or(out.crossover_hz);
    out.routing = store.value(kSpeakersRouting).value_or(std::string{});
    if (!out.routing.empty()) {
        out.routing_outputs =
            static_cast<std::size_t>(number_of(store.value(kSpeakersRoutingOutputs)).value_or(0));
    }
    return out;
}

}  // namespace ac3::hearth

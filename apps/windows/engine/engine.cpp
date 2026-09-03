#include "engine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "ac3/audio/device_watcher.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/oba/atmos.hpp"
#include "bed_mixer.hpp"
#include "output_stage.hpp"
#include "placement.hpp"
#include "platform/windows/foreground.hpp"
#include "platform/windows/session_monitor.hpp"
#include "signing_hook.hpp"
#include "tap_pool.hpp"

namespace ac3::windemo {

namespace {

constexpr auto kSessionRefresh = std::chrono::milliseconds(500);
constexpr int kTapWaitMs = 80;

float dbfs(std::span<const float> interleaved) {
    if (interleaved.empty()) {
        return -120.0F;
    }
    double sum = 0.0;
    for (const float v : interleaved) {
        sum += static_cast<double>(v) * v;
    }
    const double rms = std::sqrt(sum / static_cast<double>(interleaved.size()));
    return rms > 0.0 ? static_cast<float>(20.0 * std::log10(rms)) : -120.0F;
}

}  // namespace

struct Engine::Impl {
    EngineConfig config;

    std::jthread worker;
    std::atomic_bool running{false};
    std::atomic_bool want_reprobe{true};

    mutable std::mutex mutex;  // commands and the status snapshot
    std::vector<std::function<void()>> commands;
    EngineStatus snapshot;

    // Frame-thread state.
    SessionMonitor sessions;
    TapPool taps{2};
    SlotAllocator slots;
    PlacementSmoother placement;
    BedMix bed;
    SigningHook signing;
    std::unique_ptr<OutputStage> output;
    std::unique_ptr<ac3::oba::AtmosEncoder> encoder;
    ac3::audio::DeviceWatcher watcher;
    std::unordered_map<AppId, ac3::oba::Position> wanted_positions;
    std::unordered_map<AppId, AppSession> known;
    std::unordered_map<AppId, float> levels;
    std::vector<std::vector<float>> objects;
    std::vector<std::span<const float>> views;
    std::vector<ac3::oba::ObjectPlacement> placements;
    std::vector<std::span<const float>> bed_views;
    std::vector<std::byte> unit_bytes;
    std::size_t frames_per = 0;
    std::string signing_status;
    double worst_ms = 0.0;
    std::uint64_t frames_encoded = 0;
    std::uint64_t starved = 0;

    explicit Impl(EngineConfig c) : config(std::move(c)), taps(c.tap_channels) {}

    void post(std::function<void()> command) {
        const std::lock_guard<std::mutex> lock(mutex);
        commands.push_back(std::move(command));
    }

    void build_encoder() {
        ac3::oba::AtmosConfig atmos;
        atmos.numblkscod = config.low_latency ? 0 : 3;
        atmos.bitrate_kbps =
            config.bitrate_kbps != 0 ? config.bitrate_kbps : (config.low_latency ? 1536U : 448U);
        atmos.emit_object_metadata = signing.available();
        encoder = std::make_unique<ac3::oba::AtmosEncoder>(atmos, kObjectSlots);
        const int blocks = config.low_latency ? 1 : ac3::kBlocksPerFrame;
        frames_per = static_cast<std::size_t>(blocks * ac3::kSamplesPerBlock);
        objects.assign(kObjectSlots, std::vector<float>(frames_per, 0.0F));
        views.resize(kObjectSlots);
        placements.resize(kObjectSlots);
        bed.resize(frames_per);
        bed_views.resize(6);
    }

    void refresh_sessions() {
        auto apps = sessions.refresh();
        std::vector<AppId> ids;
        ids.reserve(apps.size());
        known.clear();
        for (auto& app : apps) {
            ids.push_back(app.app);
            slots.add(app.app);
            known.emplace(app.app, std::move(app));
        }
        // Forget applications that left.
        std::vector<AppId> gone;
        for (const auto& slot : slots.apps()) {
            if (!known.contains(slot.app)) {
                gone.push_back(slot.app);
            }
        }
        for (const AppId app : gone) {
            slots.remove(app);
            wanted_positions.erase(app);
            levels.erase(app);
        }
        taps.sync(ids);

        // The full-screen rule: the foreground pid may be a window process
        // rather than the one with the session, so match it against every
        // pid in each application's tree that we know about.
        std::optional<AppId> fullscreen;
        if (const auto pid = fullscreen_foreground_pid()) {
            for (const auto& [id, app] : known) {
                if (id == *pid || std::ranges::find(app.session_pids, *pid) != app.session_pids.end()) {
                    fullscreen = id;
                    break;
                }
            }
        }
        slots.set_fullscreen(fullscreen);
        apply_slot_changes();
    }

    // Placement targets follow the allocator: a slot that just got an
    // application snaps to that application's wanted position and fades
    // in; a freed slot fades out where it is.
    std::unordered_map<int, AppId> slot_owner;
    void apply_slot_changes() {
        std::unordered_map<int, AppId> now;
        for (const auto& app : slots.apps()) {
            if (app.positioned) {
                now[*app.positioned] = app.app;
            }
        }
        for (int slot = 0; slot < kPositionedSlots; ++slot) {
            const auto before = slot_owner.find(slot);
            const auto after = now.find(slot);
            if (after == now.end()) {
                if (before != slot_owner.end()) {
                    placement.set_gain(slot, 0.0);
                }
                continue;
            }
            const auto wanted = wanted_positions.find(after->second);
            const ac3::oba::Position where =
                wanted == wanted_positions.end() ? ac3::oba::Position{0.5, 0.5, 0.0} : wanted->second;
            if (before == slot_owner.end() || before->second != after->second) {
                placement.set_target(slot, {.position = where, .gain = 0.0});
                placement.snap(slot);
            }
            placement.set_target(slot, {.position = where, .gain = 1.0});
        }
        slot_owner = std::move(now);
    }

    void publish_status() {
        EngineStatus s;
        s.running = true;
        for (const auto& slot : slots.apps()) {
            AppStatus a;
            a.app = slot.app;
            if (const auto it = known.find(slot.app); it != known.end()) {
                a.name = it->second.name;
                a.image_path = it->second.image_path;
                a.active = it->second.active;
            }
            a.tapped = taps.has(slot.app);
            a.fullscreen = slot.fullscreen;
            a.slot = slot.positioned;
            if (slot.positioned) {
                a.position = placement.current(*slot.positioned).position;
            }
            if (const auto level = levels.find(slot.app); level != levels.end()) {
                a.level_dbfs = level->second;
            }
            s.apps.push_back(std::move(a));
        }
        const auto& out = output->status();
        s.mode = out.mode;
        s.endpoint_name = out.endpoint_name;
        s.output_reason = out.reason;
        s.endpoints = out.endpoints;
        s.underruns = out.underruns;
        s.signing = signing_status;
        s.objects_enabled = signing.available();
        s.frames_encoded = frames_encoded;
        s.starved_reads = starved;
        s.worst_frame_ms = worst_ms;
        const std::lock_guard<std::mutex> lock(mutex);
        s.last_frame_ms = snapshot.last_frame_ms;
        s.last_error = snapshot.last_error;
        snapshot = std::move(s);
    }

    void loop(const std::stop_token& stop) {
        output = std::make_unique<OutputStage>(OutputStageConfig{
            .null_sink_substring = config.null_sink_substring, .pinned = config.pinned});
        signing_status = signing.load(config.signing_key_path);
        build_encoder();
        std::ignore = watcher.start([this](const ac3::audio::DeviceChangeEvent&) {
            want_reprobe.store(true, std::memory_order_release);
        });

        auto last_refresh = std::chrono::steady_clock::time_point{};
        const auto frame_duration =
            std::chrono::microseconds(static_cast<long long>(1e6 * frames_per / 48000.0));

        while (!stop.stop_requested()) {
            const auto frame_start = std::chrono::steady_clock::now();
            {
                std::vector<std::function<void()>> pending;
                {
                    const std::lock_guard<std::mutex> lock(mutex);
                    pending.swap(commands);
                }
                for (auto& command : pending) {
                    command();
                }
            }
            if (frame_start - last_refresh >= kSessionRefresh) {
                refresh_sessions();
                last_refresh = frame_start;
            }
            if (want_reprobe.exchange(false, std::memory_order_acq_rel)) {
                output->reprobe(signing.available());
            }

            // Taps in, slots out.
            for (auto& object : objects) {
                std::ranges::fill(object, 0.0F);
            }
            bed.clear();
            if (taps.size() == 0) {
                // Nothing to tap: keep the stream alive at real time anyway.
                std::this_thread::sleep_for(frame_duration);
            } else {
                for (const auto& read : taps.read(frames_per, kTapWaitMs)) {
                    if (read.starved) {
                        ++starved;
                    }
                    levels[read.app] = dbfs(read.interleaved);
                    if (const auto slot = slots.slot_of(read.app)) {
                        fold_to_mono(read.interleaved, taps.channels(),
                                     objects[static_cast<std::size_t>(*slot)]);
                    } else {
                        add_to_bed(read.interleaved, taps.channels(), 1.0F, bed);
                    }
                }
            }
            for (int channel = 0; channel < kBedSlots; ++channel) {
                objects[static_cast<std::size_t>(kPositionedSlots + channel)] =
                    bed.slots[static_cast<std::size_t>(channel)];
            }
            for (int slot = 0; slot < kObjectSlots; ++slot) {
                views[static_cast<std::size_t>(slot)] = objects[static_cast<std::size_t>(slot)];
            }
            placement.step(placements);

            auto unit = encoder->encode_frame(views, placements);
            if (!unit) {
                const std::lock_guard<std::mutex> lock(mutex);
                snapshot.last_error = "encode_frame refused a frame";
                continue;
            }
            unit_bytes = std::move(unit->bytes);
            if (signing.available()) {
                std::ignore = signing.sign(unit_bytes);
            }
            const auto bed_channels = encoder->bed();
            for (std::size_t ch = 0; ch < 6 && ch < bed_channels.size(); ++ch) {
                bed_views[ch] = bed_channels[ch];
            }
            output->submit(unit_bytes, bed_views);
            ++frames_encoded;

            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - frame_start)
                                  .count();
            worst_ms = std::max(worst_ms, ms);
            {
                const std::lock_guard<std::mutex> lock(mutex);
                snapshot.last_frame_ms = ms;
            }
            if ((frames_encoded % 8) == 0) {
                publish_status();
            }
        }

        watcher.stop();
        output->stop();
        taps.sync({});
    }
};

Engine::Engine(EngineConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Engine::~Engine() {
    stop();
}

std::expected<void, std::string> Engine::start() {
    if (impl_->running.exchange(true)) {
        return std::unexpected("already running");
    }
    impl_->worker = std::jthread([this](const std::stop_token& stop) { impl_->loop(stop); });
    return {};
}

void Engine::stop() {
    if (!impl_->running.exchange(false)) {
        return;
    }
    impl_->worker.request_stop();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->snapshot.running = false;
}

void Engine::position(AppId app, ac3::oba::Position where) {
    impl_->post([this, app, where] {
        impl_->wanted_positions[app] = where;
        std::ignore = impl_->slots.position(app);
        impl_->apply_slot_changes();
    });
}

void Engine::unposition(AppId app) {
    impl_->post([this, app] {
        impl_->slots.unposition(app);
        impl_->apply_slot_changes();
    });
}

void Engine::pin(std::optional<OutputMode> mode) {
    impl_->post([this, mode] {
        impl_->output->set_pinned(mode);
        impl_->want_reprobe.store(true, std::memory_order_release);
    });
}

void Engine::reprobe() {
    impl_->want_reprobe.store(true, std::memory_order_release);
}

void Engine::load_signing_key(std::string path) {
    impl_->post([this, path = std::move(path)] {
        impl_->signing_status = impl_->signing.load(path);
        impl_->build_encoder();
        impl_->want_reprobe.store(true, std::memory_order_release);
    });
}

void Engine::clear_signing_key() {
    impl_->post([this] {
        impl_->signing.clear();
        impl_->signing_status = "signing key cleared: objects off, streaming the 5.1 bed only";
        impl_->build_encoder();
        impl_->want_reprobe.store(true, std::memory_order_release);
    });
}

EngineStatus Engine::status() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->snapshot;
}

}  // namespace ac3::windemo

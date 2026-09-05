#include "engine.hpp"

#include "ac3/internal/profiling.hpp"

#include <algorithm>
#include <array>
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
#include "platform_services.hpp"
#include "signing_hook.hpp"
#include "tap_pool.hpp"

namespace ac3::crucible {

namespace {

constexpr auto kSessionRefresh = std::chrono::milliseconds(500);
constexpr int kTapWaitMs = 80;
// The PCM sink's queue is allowed this many frames of the encoder's frame
// length (never less than 30 ms: three of the sink's 10 ms periods, under
// which one-block frames oscillate across the line and get dropped
// needlessly) before the loop drops tap audio to catch up. The sink
// queues up to a second; without a bound whatever offset the pipeline
// started with is the session's latency (spike S5). Two frames, not one:
// the queue holds the frame just submitted while the sink drains it, so a
// one-frame bound is crossed at every submit and the loop dropped audio
// twice a second for no gain in latency (S5 measured both).
constexpr std::size_t kMaxSinkQueueFrames = 2;
constexpr std::size_t kMinSinkQueueBound = 1440;

float dbfs(std::span<const float> interleaved) {
    if (interleaved.empty()) {
        return -120.0F;
    }
    double sum = 0.0;
    for (const float v : interleaved) {
        sum += static_cast<double>(v) * static_cast<double>(v);
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
    std::shared_ptr<SessionMonitor> sessions;
    std::shared_ptr<Foreground> foreground;
    // The monitor's thread and its latest list; the frame loop takes the
    // list when one is fresh and never waits for the monitor.
    std::jthread session_thread;
    std::mutex session_mutex;
    std::vector<AppSession> latest_sessions;
    bool sessions_fresh = false;
    // The probe's thread, the same shape as the monitor's: it runs the slow
    // enumeration and leaves the facts here; the frame loop applies them at
    // its next boundary and never waits. `probing` keeps one enumeration in
    // flight at a time, since a second request while one runs would only
    // repeat it.
    std::jthread probe_thread;
    std::mutex probe_mutex;
    std::optional<std::vector<EndpointFacts>> probe_result;
    std::atomic_bool probing{false};
    std::vector<AppId> keep_ids;  // placed applications, for the monitor to keep listed
    std::shared_ptr<AudioDevices> devices;
    TapPool taps;
    SlotAllocator slots;
    PlacementSmoother placement;
    BedMix bed;
    SigningHook signing;
    std::unique_ptr<OutputStage> output;
    std::unique_ptr<ac3::oba::AtmosEncoder> encoder;
    ac3::audio::DeviceWatcher watcher;
    std::unordered_map<AppId, ac3::oba::Position> wanted_positions;
    std::unordered_map<AppId, bool> split_choice;  // per-app override of split_by_default
    std::unordered_map<AppId, double> sizes;       // per-app object extent, default a point
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
    double tap_backlog_ms = 0.0;
    std::uint64_t catchups = 0;

    explicit Impl(EngineConfig c)
        : config(std::move(c)),
          sessions(config.sessions ? config.sessions : platform_session_monitor()),
          foreground(config.foreground ? config.foreground : platform_foreground()),
          devices(config.devices ? config.devices : platform_audio_devices()),
          taps(devices, config.tap_channels) {}

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

    // Taps are opened at the null sink's own width, so what a surround
    // application renders into it arrives by channel. A change closes every
    // tap; the next session refresh reopens them at the new width.
    void follow_null_sink_width() {
        std::uint16_t want = taps.channels();
        for (const auto& e : output->status().endpoints) {
            if (e.is_null_sink && (e.shared_channels == 2 || e.shared_channels == 6 || e.shared_channels == 8)) {
                want = e.shared_channels;
                break;
            }
        }
        if (want != taps.channels()) {
            taps = TapPool{devices, want};
            refresh_sessions();
        }
    }

    // Registers an application in the slot plan at the width its split
    // choice (or the default) asks for. Commands can name an application
    // before the first session refresh has listed it - a test, or a UI that
    // remembers - so this is the one place applications enter the plan.
    void ensure_in_plan(AppId app) {
        if (slots.known(app)) {
            return;
        }
        slots.add(app);
        const auto choice = split_choice.find(app);
        const bool split = choice != split_choice.end() ? choice->second : config.split_by_default;
        slots.set_width(app, split ? 2 : 1);
    }

    // Called every frame: nothing to do unless the monitor's thread has a
    // new list.
    void refresh_sessions() {
        std::vector<AppSession> apps;
        {
            const std::lock_guard<std::mutex> lock(session_mutex);
            if (!sessions_fresh) {
                return;
            }
            apps = std::move(latest_sessions);
            sessions_fresh = false;
        }
        AC3_ZONE_SCOPED_N("take sessions");
        std::vector<AppId> ids;  // what to tap: applications with a session
        ids.reserve(apps.size());
        known.clear();
        for (auto& app : apps) {
            if (app.has_session) {
                ids.push_back(app.app);
            }
            ensure_in_plan(app.app);
            known.emplace(app.app, std::move(app));
        }
        {
            // What the monitor should keep listed next time: whatever is
            // placed, so a silent spell does not empty the room.
            std::vector<AppId> placed;
            for (const auto& slot : slots.apps()) {
                if (slot.positioned) {
                    placed.push_back(slot.app);
                }
            }
            const std::lock_guard<std::mutex> lock(session_mutex);
            keep_ids = std::move(placed);
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
            split_choice.erase(app);
            sizes.erase(app);
            levels.erase(app);
        }
        taps.sync(ids);

        // The full-screen rule: the foreground pid may be a window process
        // rather than the one with the session, so match it against every
        // pid in each application's tree that we know about.
        std::optional<AppId> fullscreen;
        if (const auto pid = foreground->fullscreen_pid()) {
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
    // Custom pair positions: left and right, when a side has been placed.
    std::unordered_map<AppId, std::array<ac3::oba::Position, 2>> pair_positions;
    // The pair's two positions as they stand: custom, or the spread.
    std::array<ac3::oba::Position, 2> pair_of(AppId app) const {
        if (const auto custom = pair_positions.find(app); custom != pair_positions.end()) {
            return custom->second;
        }
        const auto wanted = wanted_positions.find(app);
        const ac3::oba::Position centre =
            wanted == wanted_positions.end() ? ac3::oba::Position{0.5, 0.5, 0.0} : wanted->second;
        ac3::oba::Position left = centre;
        ac3::oba::Position right = centre;
        left.x = std::clamp(centre.x - config.split_spread, 0.0, 1.0);
        right.x = std::clamp(centre.x + config.split_spread, 0.0, 1.0);
        return {left, right};
    }
    void apply_slot_changes() {
        std::unordered_map<int, AppId> now;
        std::unordered_map<int, double> side;  // -1 left, +1 right, 0 mono
        for (const auto& app : slots.apps()) {
            if (app.positioned) {
                for (int i = 0; i < app.width; ++i) {
                    now[*app.positioned + i] = app.app;
                    side[*app.positioned + i] = app.width == 2 ? (i == 0 ? -1.0 : 1.0) : 0.0;
                }
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
            ac3::oba::Position where =
                wanted == wanted_positions.end() ? ac3::oba::Position{0.5, 0.5, 0.0} : wanted->second;
            // A split pair's objects sit where the pair puts them: at the
            // standard spread either side of the placed position, or where
            // each was dragged to.
            if (side[slot] != 0.0) {
                where = pair_of(after->second)[side[slot] < 0 ? 0 : 1];
            }
            const auto sized = sizes.find(after->second);
            const double size = sized == sizes.end() ? 0.0 : sized->second;
            if (before == slot_owner.end() || before->second != after->second) {
                placement.set_target(slot, {.position = where, .gain = 0.0, .size = size});
                placement.snap(slot);
            }
            placement.set_target(slot, {.position = where, .gain = 1.0, .size = size});
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
                a.description = it->second.description;
                a.active = it->second.active;
                a.has_window = it->second.has_window;
                a.packaged = it->second.packaged;
                a.has_session = it->second.has_session;
            }
            a.tapped = taps.has(slot.app);
            a.fullscreen = slot.fullscreen;
            a.slot = slot.positioned;
            a.width = slot.width;
            {
                const auto pair = pair_of(slot.app);
                a.left = pair[0];
                a.right = pair[1];
                a.pair_custom = pair_positions.contains(slot.app);
            }
            if (const auto sized = sizes.find(slot.app); sized != sizes.end()) {
                a.size = sized->second;
            }
            if (slot.positioned) {
                a.position = placement.current(*slot.positioned).position;
                if (slot.width == 2) {
                    // Report the pair's centre, which is what the user placed.
                    a.position.x = 0.5 * (a.position.x + placement.current(*slot.positioned + 1).position.x);
                }
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
        s.tap_channels = taps.channels();
        s.codec_bypassed = out.bypassed;
        s.tap_backlog_ms = tap_backlog_ms;
        s.sink_queue_ms = 1000.0 * static_cast<double>(out.sink_queue_frames) / 48000.0;
        s.catchups = catchups;
        s.underruns = out.underruns;
        s.signing = signing_status;
        s.objects_enabled = signing.available();
        s.frames_encoded = frames_encoded;
        s.starved_reads = starved;
        s.worst_frame_ms = worst_ms;
        s.encode_ms = snapshot.encode_ms;
        const std::lock_guard<std::mutex> lock(mutex);
        s.last_frame_ms = snapshot.last_frame_ms;
        s.last_error = snapshot.last_error;
        snapshot = std::move(s);
    }

    void loop(const std::stop_token& stop) {
        output = std::make_unique<OutputStage>(OutputStageConfig{
            .devices = devices,
            .bypass_codec = config.bypass_codec,
            .low_latency = config.low_latency,
            .null_sink_substring = config.null_sink_substring,
            .pinned = config.pinned,
            .preferred_endpoint_id = config.preferred_endpoint_id});
        signing_status = signing.load(config.signing_key_path);
        build_encoder();
        std::ignore = watcher.start([this](const ac3::audio::DeviceChangeEvent&) {
            want_reprobe.store(true, std::memory_order_release);
        });

        // The session monitor, on its own thread, for as long as the loop
        // runs (the guard joins it on the way out).
        session_thread = std::jthread([this](const std::stop_token& monitor_stop) {
            while (!monitor_stop.stop_requested()) {
                std::vector<AppSession> apps;
                std::vector<AppId> keep;
                {
                    const std::lock_guard<std::mutex> lock(session_mutex);
                    keep = keep_ids;
                }
                {
                    AC3_ZONE_SCOPED_N("session monitor");
                    apps = sessions->refresh(keep);
                }
                {
                    const std::lock_guard<std::mutex> lock(session_mutex);
                    latest_sessions = std::move(apps);
                    sessions_fresh = true;
                }
                for (int i = 0; i < 10 && !monitor_stop.stop_requested(); ++i) {
                    std::this_thread::sleep_for(kSessionRefresh / 10);
                }
            }
        });
        struct StopMonitor {
            std::jthread& thread;
            ~StopMonitor() {
                thread.request_stop();
                if (thread.joinable()) {
                    thread.join();
                }
            }
        } stop_monitor{session_thread};
        const auto frame_duration =
            std::chrono::microseconds(static_cast<long long>(1e6 * static_cast<double>(frames_per) / 48000.0));

        while (!stop.stop_requested()) {
            AC3_ZONE_SCOPED_N("crucible frame");
            const auto frame_start = std::chrono::steady_clock::now();
            {
                AC3_ZONE_SCOPED_N("commands");
                std::vector<std::function<void()>> pending;
                {
                    const std::lock_guard<std::mutex> lock(mutex);
                    pending.swap(commands);
                }
                for (auto& command : pending) {
                    command();
                }
            }
            refresh_sessions();
            // The probe: asked for on the frame thread, run off it. A request
            // while one is already in flight is simply absorbed - the running
            // enumeration will be as fresh as any it could start.
            if (want_reprobe.exchange(false, std::memory_order_acq_rel) &&
                !probing.exchange(true, std::memory_order_acq_rel)) {
                if (probe_thread.joinable()) {
                    probe_thread.join();
                }
                probe_thread = std::jthread([this] {
                    AC3_ZONE_SCOPED_N("probe (off-thread)");
                    auto facts = output->enumerate();
                    {
                        const std::lock_guard<std::mutex> lock(probe_mutex);
                        probe_result = std::move(facts);
                    }
                    probing.store(false, std::memory_order_release);
                });
            }
            // Facts that arrived since the last frame are applied here, where
            // starting and stopping sinks is allowed.
            {
                std::optional<std::vector<EndpointFacts>> facts;
                {
                    const std::lock_guard<std::mutex> lock(probe_mutex);
                    facts.swap(probe_result);
                }
                if (facts) {
                    AC3_ZONE_SCOPED_N("apply probe");
                    const auto before = output->status().mode;
                    const auto before_endpoint = output->status().endpoint_id;
                    output->apply(std::move(*facts), signing.available());
                    follow_null_sink_width();
                    if (output->status().mode != before ||
                        output->status().endpoint_id != before_endpoint) {
                        // A sink took time to open; what the taps gathered
                        // meanwhile would sit in its queue for good.
                        taps.flush();
                    }
                }
            }

            // Taps in, slots out.
            for (auto& object : objects) {
                std::ranges::fill(object, 0.0F);
            }
            bed.clear();
            {
                // Over the bound: drop down to half of it in one go, so a
                // correction is one audible event rather than a run of them
                // while the queue oscillates around the line.
                const std::size_t bound = std::max(kMaxSinkQueueFrames * frames_per, kMinSinkQueueBound);
                const std::size_t queued = output->status().sink_queue_frames;
                if (queued > bound && taps.size() > 0) {
                    std::size_t to_drop = queued - bound / 2;
                    while (to_drop > 0) {
                        std::ignore = taps.read(std::min(to_drop, frames_per), 0);
                        to_drop -= std::min(to_drop, frames_per);
                    }
                    ++catchups;
                }
            }
            if (taps.size() == 0) {
                // Nothing to tap: keep the stream alive at real time anyway.
                AC3_ZONE_SCOPED_N("idle");
                std::this_thread::sleep_for(frame_duration);
            } else {
                AC3_ZONE_SCOPED_N("taps");
                for (const auto& read : taps.read(frames_per, kTapWaitMs)) {
                    if (read.starved) {
                        ++starved;
                    }
                    levels[read.app] = dbfs(read.interleaved);
                    if (const auto slot = slots.slot_of(read.app)) {
                        if (slots.width_of(read.app) == 2) {
                            fold_to_pair(read.interleaved, taps.channels(),
                                         objects[static_cast<std::size_t>(*slot)],
                                         objects[static_cast<std::size_t>(*slot + 1)]);
                        } else {
                            fold_to_mono(read.interleaved, taps.channels(),
                                         objects[static_cast<std::size_t>(*slot)]);
                        }
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

            const auto encode_start = std::chrono::steady_clock::now();
            AC3_ZONE_BEGIN(encode_zone, "encode");
            auto unit = encoder->encode_frame(views, placements);
            AC3_ZONE_END(encode_zone);
            const double encode_ms = std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - encode_start)
                                         .count();
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
            {
                AC3_ZONE_SCOPED_N("submit");
                output->submit(unit_bytes, RawFrame{.objects = views, .placements = placements, .bed = bed_views});
            }
            ++frames_encoded;
            AC3_FRAME_MARK();

            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - frame_start)
                                  .count();
            worst_ms = std::max(worst_ms, ms);
            {
                const std::lock_guard<std::mutex> lock(mutex);
                snapshot.last_frame_ms = ms;
                snapshot.encode_ms = encode_ms;
            }
            // Every other frame: the meters are read from this, and eight
            // frames (a quarter of a second) stepped visibly.
            if ((frames_encoded % 2) == 0) {
                tap_backlog_ms = 1000.0 * static_cast<double>(taps.backlog_frames()) / 48000.0;
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
        // A custom pair moves as one: both objects by the same amount.
        if (const auto custom = impl_->pair_positions.find(app); custom != impl_->pair_positions.end()) {
            const auto old = impl_->wanted_positions.find(app);
            const ac3::oba::Position from =
                old == impl_->wanted_positions.end() ? ac3::oba::Position{0.5, 0.5, 0.0} : old->second;
            for (auto& p : custom->second) {
                p.x = std::clamp(p.x + (where.x - from.x), 0.0, 1.0);
                p.y = std::clamp(p.y + (where.y - from.y), 0.0, 1.0);
                p.z = std::clamp(p.z + (where.z - from.z), -1.0, 1.0);
            }
        }
        impl_->wanted_positions[app] = where;
        impl_->ensure_in_plan(app);
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

void Engine::prefer_endpoint(std::string id) {
    impl_->post([this, id = std::move(id)] {
        impl_->output->set_preferred_endpoint(id);
        impl_->want_reprobe.store(true, std::memory_order_release);
    });
}

void Engine::set_split(AppId app, bool split) {
    impl_->post([this, app, split] {
        impl_->split_choice[app] = split;
        impl_->ensure_in_plan(app);
        impl_->slots.set_width(app, split ? 2 : 1);
        impl_->apply_slot_changes();
    });
}

void Engine::position_side(AppId app, int side, ac3::oba::Position where) {
    impl_->post([this, app, side, where] {
        if (side != 0 && side != 1) {
            return;
        }
        auto& pair = impl_->pair_positions.try_emplace(app, impl_->pair_of(app)).first->second;
        pair[static_cast<std::size_t>(side)] = where;
        // The pair's centre follows, so the plan's marker stays between them.
        auto& centre = impl_->wanted_positions[app];
        centre.x = (pair[0].x + pair[1].x) / 2.0;
        centre.y = (pair[0].y + pair[1].y) / 2.0;
        centre.z = (pair[0].z + pair[1].z) / 2.0;
        impl_->ensure_in_plan(app);
        impl_->slots.set_width(app, 2);
        std::ignore = impl_->slots.position(app);
        impl_->apply_slot_changes();
    });
}

void Engine::reset_pair(AppId app) {
    impl_->post([this, app] {
        impl_->pair_positions.erase(app);
        impl_->apply_slot_changes();
    });
}

void Engine::set_size(AppId app, double size) {
    impl_->post([this, app, size] {
        impl_->sizes[app] = std::clamp(size, 0.0, 1.0);
        impl_->apply_slot_changes();
    });
}

void Engine::set_bypass(bool on) {
    impl_->post([this, on] { impl_->output->set_bypass(on); });
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

}  // namespace ac3::crucible

#include "engine_thread.hpp"

#include <utility>

// See engine_thread.hpp.

namespace ac3::hearth {

Engine::Engine(std::unique_ptr<PcmSink> sink, ItemLoader loader, const render::OutputLayout& layout,
               const DecoderSettings& settings, const EngineTiming& timing)
    : timing_(timing), player_(std::move(sink), std::move(loader), layout, settings) {
    status_.settings = settings;
    thread_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

Engine::~Engine() {
    thread_.request_stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Engine::post(Command command) {
    {
        const std::scoped_lock lock(mutex_);
        commands_.push_back(std::move(command));
        ++posted_;
    }
    wake_.notify_one();
}

void Engine::play() {
    post([](Player& player) { return player.play().note; });
}

void Engine::pause() {
    post([](Player& player) { return player.pause().note; });
}

void Engine::stop() {
    post([](Player& player) { return player.stop().note; });
}

void Engine::next() {
    post([](Player& player) { return player.next().note; });
}

void Engine::previous() {
    post([](Player& player) { return player.previous().note; });
}

void Engine::seek(std::chrono::milliseconds to) {
    post([to](Player& player) { return player.seek(to).note; });
}

void Engine::add(std::vector<QueueItem> items) {
    post([items = std::move(items)](Player& player) {
        for (const QueueItem& item : items) {
            player.add(item);
        }
        return std::string{};
    });
}

void Engine::insert(std::size_t index, QueueItem item) {
    post([index, item = std::move(item)](Player& player) {
        player.insert(index, item);
        return std::string{};
    });
}

void Engine::remove(std::size_t index) {
    post([index](Player& player) {
        player.remove(index);
        return std::string{};
    });
}

void Engine::move(std::size_t from, std::size_t to) {
    post([from, to](Player& player) {
        player.move(from, to);
        return std::string{};
    });
}

void Engine::clear() {
    post([](Player& player) {
        player.clear();
        return std::string{};
    });
}

void Engine::play_item(std::size_t index) {
    post([index](Player& player) { return player.play_item(index).note; });
}

void Engine::set_decoder_settings(const DecoderSettings& settings) {
    post([settings](Player& player) {
        player.set_decoder_settings(settings);
        return std::string{};
    });
}

void Engine::set_gapless(bool on) {
    post([on](Player& player) {
        player.set_gapless(on);
        return std::string{};
    });
}

void Engine::set_repeat(bool on) {
    post([on](Player& player) {
        player.set_repeat(on);
        return std::string{};
    });
}

void Engine::sync() {
    std::unique_lock lock(mutex_);
    const std::uint64_t made = posted_;
    published_cv_.wait(lock, [this, made] { return published_ >= made; });
}

EngineStatus Engine::status() const {
    const std::scoped_lock lock(mutex_);
    return status_;
}

PlayPosition Engine::position() const {
    const std::scoped_lock lock(mutex_);
    return position_;
}

std::optional<MeterSnapshot> Engine::meters() const {
    const std::scoped_lock lock(mutex_);
    if (!has_meters_) {
        return std::nullopt;
    }
    return meters_;
}

std::optional<UnitReport> Engine::unit_report() const {
    const std::scoped_lock lock(mutex_);
    if (!has_report_) {
        return std::nullopt;
    }
    return report_;
}

void Engine::on_change(std::function<void(const EngineStatus&)> callback) {
    const std::scoped_lock lock(mutex_);
    on_change_ = std::move(callback);
}

void Engine::publish(const std::string& note, std::uint64_t carried) {
    // Read on the engine thread, which alone touches the player, and only
    // then stored under the lock readers take.
    EngineStatus next;
    next.state = player_.transport().state();
    const auto items = player_.queue().items();
    next.queue.assign(items.begin(), items.end());
    next.current = player_.queue().current_index();
    next.gapless = player_.transport().gapless();
    next.repeat = player_.transport().repeat();
    next.settings = player_.decoder_settings();
    next.output_opens = player_.output_opens();
    next.history = player_.history();
    next.error = player_.last_error();
    const PlayPosition position = player_.position();

    std::function<void(const EngineStatus&)> callback;
    {
        const std::scoped_lock lock(mutex_);
        next.generation = status_.generation + 1;
        next.note = note.empty() ? status_.note : note;
        status_ = next;
        position_ = position;
        published_ = carried;
        callback = on_change_;
    }
    published_cv_.notify_all();
    if (callback) {
        callback(next);
    }
}

void Engine::run(const std::stop_token& stop) {
    std::uint64_t carried = 0;
    publish(std::string{}, carried);
    std::unique_lock lock(mutex_);
    while (!stop.stop_requested()) {
        // Every command made so far, in order, then one publication for the
        // lot.
        std::string note;
        bool ran = false;
        while (!commands_.empty()) {
            Command command = std::move(commands_.front());
            commands_.pop_front();
            lock.unlock();
            std::string said = command(player_);
            if (!said.empty()) {
                note = std::move(said);
            }
            lock.lock();
            ++carried;
            ran = true;
        }
        lock.unlock();

        PumpReport report;
        const bool active = player_.active();
        if (active) {
            report = player_.pump(timing_.budget);
        }
        const bool metered = active && player_.meters(meter_scratch_);
        const bool reported = active && player_.unit_report(report_scratch_);
        {
            const std::scoped_lock meter_lock(mutex_);
            if (metered) {
                meters_.output_frame = meter_scratch_.output_frame;
                meters_.levels.assign(meter_scratch_.levels.begin(), meter_scratch_.levels.end());
                meters_.momentary_lkfs = meter_scratch_.momentary_lkfs;
                meters_.short_term_lkfs = meter_scratch_.short_term_lkfs;
                meters_.integrated_lkfs = meter_scratch_.integrated_lkfs;
                meters_.loudness_range = meter_scratch_.loudness_range;
                meters_.true_peak_dbtp = meter_scratch_.true_peak_dbtp;
                has_meters_ = true;
            }
            if (reported) {
                report_ = report_scratch_;
                has_report_ = true;
            }
            if (!player_.active()) {
                has_meters_ = false;
                has_report_ = false;
            }
        }
        if (ran || report.item_started || report.output_reopened || report.stopped ||
            !report.note.empty()) {
            publish(report.note.empty() ? note : report.note, carried);
        } else if (active) {
            const PlayPosition position = player_.position();
            const std::scoped_lock position_lock(mutex_);
            position_ = position;
        }

        lock.lock();
        if (stop.stop_requested()) {
            break;
        }
        // An open output wants pumping each period; with none, only a command
        // can give the engine anything to do. `active` is re-read, since what
        // the pump just did may have opened or closed an output.
        const auto has_work = [this] { return !commands_.empty(); };
        if (player_.active()) {
            wake_.wait_for(lock, stop, timing_.period, has_work);
        } else {
            wake_.wait(lock, stop, has_work);
        }
    }
    lock.unlock();
    // Stopped with the thread, so the sink closes before the player goes.
    player_.stop();
}

}  // namespace ac3::hearth

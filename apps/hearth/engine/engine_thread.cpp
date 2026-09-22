#include "engine_thread.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

// See engine_thread.hpp.

namespace ac3::hearth {

namespace {

[[nodiscard]] std::string_view items_word(std::size_t count) {
    return count == 1 ? "item" : "items";
}

// A queue item named for a note, or just its place when the queue has no
// such item.
[[nodiscard]] std::string named(const Player& player, std::size_t index) {
    const auto items = player.queue().items();
    return index < items.size() ? describe_item(index, items[index].title)
                                : fmt::format("item {} (no such item)", index + 1);
}

}  // namespace

Engine::Engine(std::unique_ptr<PcmSink> sink, ItemLoader loader, const render::OutputLayout& layout,
               const DecoderSettings& settings, const EngineTiming& timing,
               DiagnosticLog* diagnostics)
    : Engine(PlayerOutputs{.pcm = std::move(sink), .bitstream = {}, .choose = {}},
             std::move(loader), layout, settings, timing, diagnostics) {}

// selector_ is declared before player_, so its initializer - which reads
// whether there is a passthrough output - runs before player_'s moves it.
Engine::Engine(EngineOutputs outputs, ItemLoader loader, const render::OutputLayout& layout,
               const DecoderSettings& settings, const EngineTiming& timing,
               DiagnosticLog* diagnostics)
    : timing_(timing),
      diagnostics_(diagnostics),
      selector_(std::make_unique<OutputSelector>(std::move(outputs.endpoints),
                                                 /*bitstream_output=*/outputs.bitstream != nullptr)),
      player_(PlayerOutputs{.pcm = std::move(outputs.pcm),
                            .bitstream = std::move(outputs.bitstream),
                            .choose = [chooser = selector_.get()](const ItemFacts& facts,
                                                                  const HeldOutput& held) {
                                return chooser->choose(facts, held);
                            }},
              std::move(loader), layout, settings, diagnostics) {
    start(layout, settings);
}

Engine::Engine(PlayerOutputs outputs, ItemLoader loader, const render::OutputLayout& layout,
               const DecoderSettings& settings, const EngineTiming& timing,
               DiagnosticLog* diagnostics)
    : timing_(timing),
      diagnostics_(diagnostics),
      player_(std::move(outputs), std::move(loader), layout, settings, diagnostics) {
    start(layout, settings);
}

void Engine::start(const render::OutputLayout& layout, const DecoderSettings& settings) {
    status_.settings = settings;
    note(fmt::format("engine started: layout {} ({} slots), {}", layout.text(), layout.slots(),
                     describe(settings)));
    thread_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

Engine::~Engine() {
    thread_.request_stop();
    if (thread_.joinable()) {
        thread_.join();
    }
    note("engine stopped");
}

void Engine::note(std::string_view line) const {
    if (diagnostics_ != nullptr) {
        diagnostics_->note(line);
    }
}

std::string Engine::transport_said(const TransportOutcome& outcome) const {
    if (diagnostics_ != nullptr && !outcome.note.empty()) {
        // A note about an item can quote why it cannot be played, which can
        // quote its path; the item it is about is the outcome's.
        Secrets secrets;
        const auto items = player_.queue().items();
        if (outcome.item < items.size()) {
            withhold_path(secrets, items[outcome.item].path);
        }
        diagnostics_->note(scrub(fmt::format("transport: {}", outcome.note), secrets));
    }
    return outcome.note;
}

void Engine::post(Command command) {
    {
        const std::scoped_lock lock(mutex_);
        commands_.push_back(std::move(command));
        ++posted_;
    }
    wake_.notify_one();
}

// Each command is noted as the engine thread starts to carry it out, so that
// what the player notes while carrying it out follows it.

void Engine::play() {
    post([this](Player& player) {
        note("play");
        return transport_said(player.play());
    });
}

void Engine::pause() {
    post([this](Player& player) {
        note("pause");
        return transport_said(player.pause());
    });
}

void Engine::stop() {
    post([this](Player& player) {
        note("stop");
        return transport_said(player.stop());
    });
}

void Engine::next() {
    post([this](Player& player) {
        note("next");
        return transport_said(player.next());
    });
}

void Engine::previous() {
    post([this](Player& player) {
        note("previous");
        return transport_said(player.previous());
    });
}

void Engine::seek(std::chrono::milliseconds to) {
    post([this, to](Player& player) {
        note(fmt::format("seek to {:.3f} s", static_cast<double>(to.count()) / 1000.0));
        return transport_said(player.seek(to));
    });
}

void Engine::add(std::vector<QueueItem> items) {
    post([this, items = std::move(items)](Player& player) {
        note(fmt::format("add {} {} to a queue of {}", items.size(), items_word(items.size()),
                         player.queue().size()));
        for (const QueueItem& item : items) {
            player.add(item);
        }
        return std::string{};
    });
}

void Engine::insert(std::size_t index, QueueItem item) {
    post([this, index, item = std::move(item)](Player& player) {
        const std::size_t at = std::min(index, player.queue().size());
        note(fmt::format("insert {} into a queue of {}", describe_item(at, item.title),
                         player.queue().size()));
        player.insert(index, item);
        return std::string{};
    });
}

void Engine::remove(std::size_t index) {
    post([this, index](Player& player) {
        note(fmt::format("remove {}", named(player, index)));
        player.remove(index);
        return std::string{};
    });
}

void Engine::move(std::size_t from, std::size_t to) {
    post([this, from, to](Player& player) {
        if (to < player.queue().size()) {
            note(fmt::format("move {} to {}", named(player, from), to + 1));
        } else {
            note(fmt::format("move {} to {} (no such place)", named(player, from), to + 1));
        }
        player.move(from, to);
        return std::string{};
    });
}

void Engine::clear() {
    post([this](Player& player) {
        note(fmt::format("clear a queue of {} {}", player.queue().size(),
                         items_word(player.queue().size())));
        player.clear();
        return std::string{};
    });
}

void Engine::play_item(std::size_t index) {
    post([this, index](Player& player) {
        note(fmt::format("play {}", named(player, index)));
        return transport_said(player.play_item(index));
    });
}

void Engine::set_decoder_settings(const DecoderSettings& settings) {
    post([this, settings](Player& player) {
        if (settings != player.decoder_settings()) {
            note(fmt::format("decoder settings: {}", describe(settings)));
        }
        player.set_decoder_settings(settings);
        return std::string{};
    });
}

void Engine::set_trim_db(std::size_t slot, double db) {
    post([this, slot, db](Player& player) {
        if (!player.set_trim_db(slot, db)) {
            return fmt::format("trim refused: slot {} at {:.1f} dB", slot, db);
        }
        note(fmt::format("trim: slot {} {:.1f} dB", slot, db));
        return std::string{};
    });
}

void Engine::set_delay_ms(std::size_t slot, double ms) {
    post([this, slot, ms](Player& player) {
        if (!player.set_delay_ms(slot, ms)) {
            return fmt::format("delay refused: slot {} at {:.1f} ms", slot, ms);
        }
        note(fmt::format("delay: slot {} {:.1f} ms", slot, ms));
        return std::string{};
    });
}

void Engine::set_crossover_hz(double hz) {
    post([this, hz](Player& player) {
        if (!player.set_crossover_hz(hz)) {
            return fmt::format("crossover refused: {:.0f} Hz", hz);
        }
        note(fmt::format("crossover: {:.0f} Hz", hz));
        return std::string{};
    });
}

void Engine::set_routing(const render::Routing& routing) {
    post([this, routing](Player& player) {
        std::array<char, render::Routing::kTextBytes> text{};
        routing.format(text);
        if (!player.set_routing(routing)) {
            return fmt::format("routing refused: {}", text.data());
        }
        note(fmt::format("routing: {}", text.data()));
        return std::string{};
    });
}

void Engine::set_volume_db(double db) {
    post([this, db](Player& player) {
        if (!player.set_volume_db(db)) {
            return fmt::format("volume refused: {:.1f} dB", db);
        }
        note(fmt::format("volume: {:.1f} dB", db));
        return std::string{};
    });
}

void Engine::set_gapless(bool on) {
    post([this, on](Player& player) {
        if (on != player.transport().gapless()) {
            note(on ? "gapless on" : "gapless off");
        }
        player.set_gapless(on);
        return std::string{};
    });
}

void Engine::set_repeat(bool on) {
    post([this, on](Player& player) {
        if (on != player.transport().repeat()) {
            note(on ? "repeat on" : "repeat off");
        }
        player.set_repeat(on);
        return std::string{};
    });
}

void Engine::set_on_failure(FailurePolicy policy) {
    post([this, policy](Player& player) {
        if (policy != player.transport().on_failure()) {
            note(fmt::format("an item that fails: {}", describe(policy)));
        }
        player.set_on_failure(policy);
        return std::string{};
    });
}

void Engine::restore(std::vector<QueueItem> items, std::size_t current,
                     std::chrono::milliseconds position) {
    post([this, items = std::move(items), current, position](Player& player) {
        note(fmt::format("restore a queue of {} {}, {}", items.size(), items_word(items.size()),
                         current < items.size()
                             ? fmt::format("item {} current, {:.3f} s in", current + 1,
                                           static_cast<double>(position.count()) / 1000.0)
                             : std::string{"none current"}));
        player.clear();
        for (const QueueItem& item : items) {
            player.add(item);
        }
        if (!player.select(current) || position <= std::chrono::milliseconds{0}) {
            return std::string{};
        }
        return transport_said(player.seek(position));
    });
}

void Engine::set_output_preferences(OutputPreferences preferences) {
    post([this, preferences = std::move(preferences)](Player& player) {
        if (!selector_) {
            note("output choices refused: this engine was given its outputs' decisions");
            return std::string{"This engine's outputs are chosen by its owner, not here."};
        }
        if (preferences == selector_->preferences()) {
            return std::string{};
        }
        note(fmt::format("output choices: mode {}, endpoint {}, follow the sink {}",
                         preferences.pinned ? describe(*preferences.pinned) : "automatic",
                         preferences.endpoint_id.empty() ? "automatic" : preferences.endpoint_id,
                         preferences.follow_sink ? "on" : "off"));
        selector_->set_preferences(preferences);
        return player.refollow();
    });
}

void Engine::refresh_outputs() {
    post([this](Player& player) {
        if (!selector_) {
            return std::string{};
        }
        note("outputs changed: reading them again");
        selector_->refresh();
        return player.refollow();
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
    next.volume_db = player_.volume_db();
    next.on_failure = player_.transport().on_failure();
    next.settings = player_.decoder_settings();
    next.settings_note = player_.settings_note();
    next.output = player_.transport().open_format();
    next.output_reason = player_.output_choice().reason;
    if (selector_) {
        next.output_preferences = selector_->preferences();
    }
    next.output_opens = player_.output_opens();
    next.history = player_.history();
    next.error = player_.last_error();
    const std::size_t slots = player_.layout().slots();
    next.trim_db.resize(slots);
    next.delay_ms.resize(slots);
    for (std::size_t slot = 0; slot < slots; ++slot) {
        next.trim_db[slot] = player_.trim_db(slot);
        next.delay_ms[slot] = player_.delay_ms(slot);
    }
    next.crossover_hz = player_.crossover_hz();
    next.routing = player_.routing();
    next.device_name = player_.device_name();
    next.speaker_mask = player_.speaker_mask();
    next.layout = player_.layout();
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

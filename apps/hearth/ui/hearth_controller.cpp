#include "hearth_controller.hpp"

#include <QFileInfo>

#include <chrono>

// hearth_controller.hpp's Qt headers define `slots` as a macro for the
// classic SIGNAL/SLOT syntax (unless QT_NO_KEYWORDS is set, which this
// project's Qt targets do not - CrucibleController and EncoderController
// both rely on the bare `signals:` keyword this class also uses). Hearth's
// engine is Qt-free by design and apps/hearth/ui is the first place in the
// tree to include both Qt and ac3::render::OutputLayout in one translation
// unit, so this is the first place the collision can happen:
// OutputLayout::slots() is a real method name, and left alone the macro
// rewrites its declaration into nonsense. Undefined here, before anything
// that spells the word - nothing below still needs Qt's old-style slots:
// poll() connects through the modern function-pointer connect(), which
// needs no access-specifier keyword at all.
#undef slots

#include "ac3/render/layout.hpp"
#include "decoder_settings.hpp"
#include "engine_thread.hpp"
#include "item_loader.hpp"
#include "pcm_sink.hpp"
#include "queue.hpp"
#include "transport.hpp"

namespace ac3::hearth::ui {

namespace {

// CrucibleController's own rate: fast enough that a command's effect shows
// up within a frame or two, slow enough that a snapshot copy sixteen times a
// second costs nothing worth measuring.
constexpr int kPollMs = 60;

[[nodiscard]] QString transport_state_name(ac3::hearth::TransportState state) {
    switch (state) {
        case ac3::hearth::TransportState::kPlaying:
            return QStringLiteral("playing");
        case ac3::hearth::TransportState::kPaused:
            return QStringLiteral("paused");
        case ac3::hearth::TransportState::kStopped:
        default:
            return QStringLiteral("stopped");
    }
}

[[nodiscard]] QString stream_kind_name(const ac3::hearth::ItemFacts& facts) {
    if (!facts.stream.has_value()) {
        return QString();
    }
    return facts.has_objects ? QStringLiteral("E-AC-3 JOC") : QStringLiteral("AC-3/E-AC-3");
}

// --- DecoderSettings <-> QVariantMap, field by field (decoder_settings.hpp) ---
// Only the controls the Decoder page can set without stream-dependent
// information: not mix_levels (the design's "From the stream/Set here"
// choice needs the stream's own levels, which this controller does not read
// yet) and not programme (Session's choice of units, not part of
// DecoderSettings at all).

[[nodiscard]] QString mode_name(ac3::OperatingMode mode) {
    switch (mode) {
        case ac3::OperatingMode::kLine:
            return QStringLiteral("line");
        case ac3::OperatingMode::kRf:
            return QStringLiteral("rf");
        case ac3::OperatingMode::kCustom:
        default:
            return QStringLiteral("custom");
    }
}

[[nodiscard]] ac3::OperatingMode mode_from_name(const QString& name) {
    if (name == QLatin1String("line")) {
        return ac3::OperatingMode::kLine;
    }
    if (name == QLatin1String("rf")) {
        return ac3::OperatingMode::kRf;
    }
    return ac3::OperatingMode::kCustom;
}

[[nodiscard]] QString downmix_name(ac3::DownmixTarget target) {
    return target == ac3::DownmixTarget::kLtRt ? QStringLiteral("ltrt") : QStringLiteral("loro");
}

[[nodiscard]] ac3::DownmixTarget downmix_from_name(const QString& name) {
    return name == QLatin1String("ltrt") ? ac3::DownmixTarget::kLtRt : ac3::DownmixTarget::kLoRo;
}

[[nodiscard]] QString dual_mono_name(ac3::hearth::DualMonoChoice choice) {
    switch (choice) {
        case ac3::hearth::DualMonoChoice::kFirst:
            return QStringLiteral("first");
        case ac3::hearth::DualMonoChoice::kSecond:
            return QStringLiteral("second");
        case ac3::hearth::DualMonoChoice::kBoth:
        default:
            return QStringLiteral("both");
    }
}

[[nodiscard]] ac3::hearth::DualMonoChoice dual_mono_from_name(const QString& name) {
    if (name == QLatin1String("first")) {
        return ac3::hearth::DualMonoChoice::kFirst;
    }
    if (name == QLatin1String("second")) {
        return ac3::hearth::DualMonoChoice::kSecond;
    }
    return ac3::hearth::DualMonoChoice::kBoth;
}

[[nodiscard]] QString objects_policy_name(ac3::render::ObjectsPolicy policy) {
    switch (policy) {
        case ac3::render::ObjectsPolicy::kNever:
            return QStringLiteral("never");
        case ac3::render::ObjectsPolicy::kAlways:
            return QStringLiteral("always");
        case ac3::render::ObjectsPolicy::kAuto:
        default:
            return QStringLiteral("auto");
    }
}

[[nodiscard]] ac3::render::ObjectsPolicy objects_policy_from_name(const QString& name) {
    if (name == QLatin1String("never")) {
        return ac3::render::ObjectsPolicy::kNever;
    }
    if (name == QLatin1String("always")) {
        return ac3::render::ObjectsPolicy::kAlways;
    }
    return ac3::render::ObjectsPolicy::kAuto;
}

[[nodiscard]] QString concealment_name(ac3::ConcealmentPolicy policy) {
    switch (policy) {
        case ac3::ConcealmentPolicy::kNone:
            return QStringLiteral("stop");
        case ac3::ConcealmentPolicy::kMute:
            return QStringLiteral("mute");
        case ac3::ConcealmentPolicy::kRepeatFade:
        default:
            return QStringLiteral("repeatFade");
    }
}

[[nodiscard]] ac3::ConcealmentPolicy concealment_from_name(const QString& name) {
    if (name == QLatin1String("stop")) {
        return ac3::ConcealmentPolicy::kNone;
    }
    if (name == QLatin1String("mute")) {
        return ac3::ConcealmentPolicy::kMute;
    }
    return ac3::ConcealmentPolicy::kRepeatFade;
}

[[nodiscard]] QVariantMap decoder_settings_to_map(const ac3::hearth::DecoderSettings& settings) {
    QVariantMap map;
    map[QStringLiteral("mode")] = mode_name(settings.mode);
    map[QStringLiteral("rfCeilingDb")] = settings.rf_ceiling_db;
    map[QStringLiteral("drcCut")] = settings.drc_cut;
    map[QStringLiteral("drcBoost")] = settings.drc_boost;
    map[QStringLiteral("heavyCompression")] = settings.heavy_compression;
    map[QStringLiteral("normaliseDialogue")] = settings.normalise_dialogue;
    map[QStringLiteral("stereoFold")] = downmix_name(settings.stereo_fold);
    map[QStringLiteral("ltrtPhaseShift")] = settings.ltrt_phase_shift;
    map[QStringLiteral("mixLfe")] = settings.mix_lfe;
    map[QStringLiteral("dualMono")] = dual_mono_name(settings.dual_mono);
    map[QStringLiteral("objects")] = objects_policy_name(settings.objects);
    map[QStringLiteral("concealment")] = concealment_name(settings.concealment);
    return map;
}

// Starts from `base` (the engine's last-known settings) so a key this map
// does not carry - or a caller that reads decoderSettings(), changes one
// key and writes the rest back unmodified - keeps its value rather than
// resetting to DecoderSettings{}'s defaults.
[[nodiscard]] ac3::hearth::DecoderSettings decoder_settings_from_map(
    const QVariantMap& map, const ac3::hearth::DecoderSettings& base) {
    ac3::hearth::DecoderSettings out = base;
    if (map.contains(QStringLiteral("mode"))) {
        out.mode = mode_from_name(map[QStringLiteral("mode")].toString());
    }
    if (map.contains(QStringLiteral("rfCeilingDb"))) {
        out.rf_ceiling_db = map[QStringLiteral("rfCeilingDb")].toDouble();
    }
    if (map.contains(QStringLiteral("drcCut"))) {
        out.drc_cut = map[QStringLiteral("drcCut")].toDouble();
    }
    if (map.contains(QStringLiteral("drcBoost"))) {
        out.drc_boost = map[QStringLiteral("drcBoost")].toDouble();
    }
    if (map.contains(QStringLiteral("heavyCompression"))) {
        out.heavy_compression = map[QStringLiteral("heavyCompression")].toBool();
    }
    if (map.contains(QStringLiteral("normaliseDialogue"))) {
        out.normalise_dialogue = map[QStringLiteral("normaliseDialogue")].toBool();
    }
    if (map.contains(QStringLiteral("stereoFold"))) {
        out.stereo_fold = downmix_from_name(map[QStringLiteral("stereoFold")].toString());
    }
    if (map.contains(QStringLiteral("ltrtPhaseShift"))) {
        out.ltrt_phase_shift = map[QStringLiteral("ltrtPhaseShift")].toBool();
    }
    if (map.contains(QStringLiteral("mixLfe"))) {
        out.mix_lfe = map[QStringLiteral("mixLfe")].toBool();
    }
    if (map.contains(QStringLiteral("dualMono"))) {
        out.dual_mono = dual_mono_from_name(map[QStringLiteral("dualMono")].toString());
    }
    if (map.contains(QStringLiteral("objects"))) {
        out.objects = objects_policy_from_name(map[QStringLiteral("objects")].toString());
    }
    if (map.contains(QStringLiteral("concealment"))) {
        out.concealment = concealment_from_name(map[QStringLiteral("concealment")].toString());
    }
    return out;
}

[[nodiscard]] QString realization_name(ac3::render::Speaker::Realization realization) {
    switch (realization) {
        case ac3::render::Speaker::Realization::kTop:
            return QStringLiteral("ceiling");
        case ac3::render::Speaker::Realization::kUpFiring:
            return QStringLiteral("upfiring");
        case ac3::render::Speaker::Realization::kDefault:
        case ac3::render::Speaker::Realization::kHeight:
        default:
            // kHeight (a wall-mounted, angled speaker) reads the same as
            // kDefault: layout.hpp's own header comment says the two render
            // identically, and "wall" is what setHeights("wall") writes -
            // kDefault, never kHeight - so a round trip through this control
            // is stable.
            return QStringLiteral("wall");
    }
}

[[nodiscard]] ac3::render::Speaker::Realization realization_from_name(const QString& name) {
    if (name == QLatin1String("ceiling")) {
        return ac3::render::Speaker::Realization::kTop;
    }
    if (name == QLatin1String("upfiring")) {
        return ac3::render::Speaker::Realization::kUpFiring;
    }
    return ac3::render::Speaker::Realization::kDefault;
}

[[nodiscard]] QVariantMap queue_row(const ac3::hearth::QueueItem& item, bool current) {
    QVariantMap row;
    row[QStringLiteral("path")] = QString::fromStdString(item.path);
    row[QStringLiteral("title")] = QString::fromStdString(item.title);
    row[QStringLiteral("playable")] = item.playable();
    row[QStringLiteral("note")] = QString::fromStdString(
        item.playable() ? item.facts.note : item.facts.unplayable_because);
    row[QStringLiteral("durationMs")] =
        item.facts.duration.has_value() ? static_cast<qlonglong>(item.facts.duration->count()) : qlonglong{0};
    row[QStringLiteral("channels")] = item.facts.channels;
    row[QStringLiteral("sampleRate")] = item.facts.sample_rate;
    row[QStringLiteral("hasObjects")] = item.facts.has_objects;
    row[QStringLiteral("streamKind")] = stream_kind_name(item.facts);
    row[QStringLiteral("current")] = current;
    return row;
}

}  // namespace

HearthController::HearthController(QObject* parent) : QObject(parent) {
    poll_timer_.setInterval(kPollMs);
    connect(&poll_timer_, &QTimer::timeout, this, &HearthController::poll);
}

HearthController::~HearthController() = default;

void HearthController::start() {
    if (engine_) {
        return;
    }
    // "2.0" until the Speakers page (A5, following this slice) makes the
    // layout a setting; a literal this application writes always parses.
    const std::optional<ac3::render::OutputLayout> layout = ac3::render::OutputLayout::parse("2.0");
    engine_ = std::make_unique<ac3::hearth::Engine>(
        ac3::hearth::make_device_sink(std::string()), ac3::hearth::ui::make_file_item_loader(), *layout,
        ac3::hearth::DecoderSettings{});
    poll_timer_.start();
    poll();
}

void HearthController::setGapless(bool on) {
    if (gapless_ == on) {
        return;
    }
    gapless_ = on;
    if (engine_) {
        engine_->set_gapless(on);
    }
    emit stateChanged();
}

void HearthController::play() {
    if (engine_) {
        engine_->play();
    }
}

void HearthController::pause() {
    if (engine_) {
        engine_->pause();
    }
}

void HearthController::stop() {
    if (engine_) {
        engine_->stop();
    }
}

void HearthController::next() {
    if (engine_) {
        engine_->next();
    }
}

void HearthController::previous() {
    if (engine_) {
        engine_->previous();
    }
}

void HearthController::seek(qlonglong ms) {
    if (engine_ && ms >= 0) {
        engine_->seek(std::chrono::milliseconds(ms));
    }
}

void HearthController::playItem(int index) {
    if (engine_ && index >= 0) {
        engine_->play_item(static_cast<std::size_t>(index));
    }
}

void HearthController::removeAt(int index) {
    if (engine_ && index >= 0) {
        engine_->remove(static_cast<std::size_t>(index));
    }
}

void HearthController::addFiles(const QStringList& paths) {
    if (!engine_ || paths.isEmpty()) {
        return;
    }
    std::vector<ac3::hearth::QueueItem> items;
    items.reserve(static_cast<std::size_t>(paths.size()));
    for (const QString& path : paths) {
        ac3::hearth::QueueItem item;
        item.path = path.toStdString();
        item.title = QFileInfo(path).fileName().toStdString();
        items.push_back(std::move(item));
    }
    engine_->add(std::move(items));
}

void HearthController::poll() {
    if (!engine_) {
        return;
    }
    const ac3::hearth::EngineStatus status = engine_->status();

    QVariantList rows;
    rows.reserve(static_cast<qsizetype>(status.queue.size()));
    for (std::size_t i = 0; i < status.queue.size(); ++i) {
        rows.push_back(queue_row(status.queue[i], i == status.current));
    }
    const bool queue_changed = rows != queue_;
    queue_ = std::move(rows);
    const int new_current =
        status.current == ac3::hearth::Queue::kNone ? -1 : static_cast<int>(status.current);
    if (queue_changed || new_current != current_index_) {
        current_index_ = new_current;
        emit queueChanged();
    }

    const QString new_state = transport_state_name(status.state);
    const QString new_output_reason = QString::fromStdString(status.output_reason);
    const QString new_note = QString::fromStdString(status.note);
    const QString new_error = QString::fromStdString(status.error);
    if (new_state != state_ || status.gapless != gapless_ || new_output_reason != output_reason_ ||
        new_note != note_ || new_error != error_) {
        state_ = new_state;
        gapless_ = status.gapless;
        output_reason_ = new_output_reason;
        note_ = new_note;
        error_ = new_error;
        emit stateChanged();
    }

    // Read apart from status() - Engine::position()'s own comment says why -
    // and on its own signal, so the scrubber does not have to sit through
    // queue-row rebuilding sixty times a second just to hear it move.
    const ac3::hearth::PlayPosition position = engine_->position();
    const qlonglong new_position_ms = static_cast<qlonglong>(position.heard.count());
    const qlonglong new_duration_ms = static_cast<qlonglong>(position.duration.count());
    if (new_position_ms != position_ms_ || new_duration_ms != duration_ms_) {
        position_ms_ = new_position_ms;
        duration_ms_ = new_duration_ms;
        emit positionChanged();
    }

    const QVariantMap new_decoder_settings = decoder_settings_to_map(status.settings);
    if (new_decoder_settings != decoder_settings_) {
        decoder_settings_ = new_decoder_settings;
        emit decoderSettingsChanged();
    }

    const std::size_t slots = status.layout.slots();
    // The layout can change now (setLayoutText()/setHeights()/
    // setSpeakerSmall()), not just appear once, so everything keyed by slot
    // index - the labels, which are small, which is LFE, the Heights
    // reading - is recomputed whenever the text says it changed, not only
    // the first time slots appear.
    const QString new_layout_text = QString::fromStdString(std::string(status.layout.text()));
    const bool layout_changed = new_layout_text != layout_text_;
    QStringList new_labels;
    QVariantList new_small;
    QVariantList new_is_lfe;
    bool new_has_height = layout_has_height_;
    QString new_heights = heights_realization_;
    if (layout_changed) {
        new_labels.reserve(static_cast<qsizetype>(slots));
        new_small.reserve(static_cast<qsizetype>(slots));
        new_is_lfe.reserve(static_cast<qsizetype>(slots));
        bool has_height = false;
        bool heights_mixed = false;
        std::optional<ac3::render::Speaker::Realization> shared_realization;
        for (std::size_t slot = 0; slot < slots; ++slot) {
            std::array<char, 32> name{};
            status.layout.slot_name(slot, name);
            const ac3::render::Speaker& speaker = status.layout.slot(slot);
            new_labels.push_back(QString::fromLatin1(name.data()));
            new_small.push_back(speaker.small);
            new_is_lfe.push_back(speaker.kind == ac3::render::Speaker::Kind::kLfe);
            if (speaker.location.has_value() &&
                ac3::render::OutputLayout::is_realizable_height(*speaker.location)) {
                has_height = true;
                if (!shared_realization) {
                    shared_realization = speaker.realization;
                } else if (*shared_realization != speaker.realization) {
                    heights_mixed = true;
                }
            }
        }
        new_has_height = has_height;
        new_heights =
            (has_height && !heights_mixed) ? realization_name(*shared_realization) : QString();
    }

    QVariantList new_trim_db;
    QVariantList new_delay_ms;
    new_trim_db.reserve(static_cast<qsizetype>(status.trim_db.size()));
    new_delay_ms.reserve(static_cast<qsizetype>(status.delay_ms.size()));
    for (const double db : status.trim_db) {
        new_trim_db.push_back(db);
    }
    for (const double ms : status.delay_ms) {
        new_delay_ms.push_back(ms);
    }
    QVariantList new_routing;
    new_routing.reserve(static_cast<qsizetype>(slots));
    for (std::size_t slot = 0; slot < slots; ++slot) {
        new_routing.push_back(status.routing.output_of(slot));
    }
    const QString new_device_name = QString::fromStdString(status.device_name);
    const bool new_has_lfe = status.layout.lfe_count() > 0;
    if (layout_changed || new_trim_db != trim_db_ || new_delay_ms != delay_ms_ ||
        status.crossover_hz != crossover_hz_ || new_routing != routing_ ||
        static_cast<int>(status.routing.outputs()) != routing_outputs_ ||
        new_device_name != device_name_ || new_has_lfe != layout_has_lfe_) {
        trim_db_ = std::move(new_trim_db);
        delay_ms_ = std::move(new_delay_ms);
        crossover_hz_ = status.crossover_hz;
        routing_ = std::move(new_routing);
        routing_outputs_ = static_cast<int>(status.routing.outputs());
        device_name_ = new_device_name;
        layout_has_lfe_ = new_has_lfe;
        if (layout_changed) {
            layout_text_ = new_layout_text;
            speaker_labels_ = new_labels;
            speaker_small_ = new_small;
            speaker_is_lfe_ = new_is_lfe;
            layout_has_height_ = new_has_height;
            heights_realization_ = new_heights;
        }
        emit speakerSetupChanged();
    }
}

void HearthController::setDecoderSettings(const QVariantMap& settings) {
    if (!engine_) {
        return;
    }
    // A fresh read rather than a cached struct: engine_->status() is already
    // a cheap, synchronous snapshot (poll() calls it every tick), and this
    // avoids hearth_controller.hpp itself needing ac3::hearth::DecoderSettings
    // by value, which would pull in ac3::render::OutputLayout (through
    // decoder_settings.hpp) ahead of this header's own Qt includes and
    // reintroduce the slots-macro collision hearth_controller.cpp's own
    // #undef only guards its own translation unit against.
    engine_->set_decoder_settings(decoder_settings_from_map(settings, engine_->status().settings));
}

void HearthController::setTrimDb(int slot, double db) {
    if (engine_ && slot >= 0) {
        engine_->set_trim_db(static_cast<std::size_t>(slot), db);
    }
}

void HearthController::setDelayMs(int slot, double ms) {
    if (engine_ && slot >= 0) {
        engine_->set_delay_ms(static_cast<std::size_t>(slot), ms);
    }
}

void HearthController::setCrossoverHz(double hz) {
    if (engine_) {
        engine_->set_crossover_hz(hz);
    }
}

void HearthController::setRoutingAssignment(int slot, int output) {
    if (!engine_ || slot < 0) {
        return;
    }
    ac3::render::Routing patch = ac3::render::Routing::identity(
                                     static_cast<std::size_t>(routing_.size()),
                                     static_cast<std::size_t>(routing_outputs_))
                                     .value_or(ac3::render::Routing{});
    for (qsizetype i = 0; i < routing_.size(); ++i) {
        patch.assign(static_cast<std::size_t>(i), routing_[i].toInt());
    }
    patch.assign(static_cast<std::size_t>(slot), output);
    engine_->set_routing(patch);
}

void HearthController::clearRouting() {
    if (!engine_) {
        return;
    }
    const auto patch = ac3::render::Routing::identity(static_cast<std::size_t>(routing_.size()), 0);
    engine_->set_routing(patch.value_or(ac3::render::Routing{}));
}

void HearthController::useDeviceOrder() {
    if (!engine_) {
        return;
    }
    const auto patch = ac3::render::Routing::identity(static_cast<std::size_t>(routing_.size()),
                                                       static_cast<std::size_t>(routing_outputs_));
    if (patch) {
        engine_->set_routing(*patch);
    }
}

void HearthController::setLayoutText(const QString& text) {
    if (!engine_) {
        return;
    }
    const auto layout = ac3::render::OutputLayout::parse(text.toStdString());
    if (layout) {
        engine_->set_layout(*layout);
    }
}

void HearthController::setHeights(const QString& realization) {
    if (!engine_) {
        return;
    }
    // A fresh read, not the (possibly one poll stale) layoutText property -
    // same reasoning as setDecoderSettings()'s own comment.
    const ac3::render::OutputLayout layout = engine_->status().layout;
    engine_->set_layout(layout.with_realization(realization_from_name(realization)));
}

void HearthController::setSpeakerSmall(int slot, bool small) {
    if (!engine_ || slot < 0) {
        return;
    }
    const ac3::render::OutputLayout layout = engine_->status().layout;
    const auto changed = layout.with_small(static_cast<std::size_t>(slot), small);
    if (changed) {
        engine_->set_layout(*changed);
    }
}

bool HearthController::firstRunSeen() const {
    return settings_.value(QStringLiteral("firstRun/seen"), false).toBool();
}

void HearthController::setFirstRunSeen(bool seen) {
    if (seen == firstRunSeen()) {
        return;
    }
    if (seen) {
        settings_.setValue(QStringLiteral("firstRun/seen"), true);
    } else {
        settings_.remove(QStringLiteral("firstRun/seen"));
    }
    settings_.sync();  // survive a hard exit
    emit firstRunSeenChanged();
}

}  // namespace ac3::hearth::ui

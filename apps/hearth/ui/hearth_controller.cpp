#include "hearth_controller.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>

#include "ac3/version.hpp"

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

#include "ac3/audio/passthrough.hpp"
#include "ac3/audio/speakers.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/sendspin/crypto.hpp"
#include "decoder_settings.hpp"
#include "diagnostics_report.hpp"
#include "engine_thread.hpp"
#include "item_loader.hpp"
#include "output_decision.hpp"
#include "output_selector.hpp"
#include "pairing_store.hpp"
#include "pcm_sink.hpp"
#include "queue.hpp"
#include "settings_model.hpp"
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

[[nodiscard]] QString joc_domain_name(ac3::oba::joc::Domain domain) {
    return domain == ac3::oba::joc::Domain::kMdctBand ? QStringLiteral("mdct") : QStringLiteral("qmf");
}

[[nodiscard]] ac3::oba::joc::Domain joc_domain_from_name(const QString& name) {
    return name == QLatin1String("mdct") ? ac3::oba::joc::Domain::kMdctBand
                                          : ac3::oba::joc::Domain::kQmf;
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
    map[QStringLiteral("jocDomain")] = joc_domain_name(settings.joc_domain);
    map[QStringLiteral("concealment")] = concealment_name(settings.concealment);
    map[QStringLiteral("fastInverseTransform")] = settings.fast_inverse_transform;
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
    if (map.contains(QStringLiteral("jocDomain"))) {
        out.joc_domain = joc_domain_from_name(map[QStringLiteral("jocDomain")].toString());
    }
    if (map.contains(QStringLiteral("concealment"))) {
        out.concealment = concealment_from_name(map[QStringLiteral("concealment")].toString());
    }
    if (map.contains(QStringLiteral("fastInverseTransform"))) {
        out.fast_inverse_transform = map[QStringLiteral("fastInverseTransform")].toBool();
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

[[nodiscard]] ac3::hearth::QueueItem queue_item_from_path(const QString& path) {
    ac3::hearth::QueueItem item;
    item.path = path.toStdString();
    item.title = QFileInfo(path).fileName().toStdString();
    return item;
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

// One row of the output picker's "this computer" list: OutputPicker.qml
// reads channels/speakers/sampleRates for the PCM section and
// supportsAc3/supportsEac3 for the passthrough section, from the one
// enumeration both come from - a device that cannot bitstream simply has
// both flags false, which is why the passthrough section shows only some of
// these rows rather than needing a second list.
[[nodiscard]] QVariantMap output_device_row(const ac3::audio::RenderDeviceInfo& device) {
    QVariantMap row;
    row[QStringLiteral("id")] = QString::fromStdString(device.id);
    row[QStringLiteral("name")] = QString::fromStdString(device.name);
    row[QStringLiteral("isDefault")] = device.is_default;
    // 0 is "not reported", not "no channels" (RenderDeviceInfo's own
    // comment) - QML reads a zero channel count that way too.
    row[QStringLiteral("channels")] = device.channels;
    row[QStringLiteral("speakers")] = QString::fromStdString(ac3::audio::describe_speakers(device.speakers));
    QVariantList rates;
    rates.reserve(static_cast<qsizetype>(device.sample_rates.size()));
    for (const std::uint32_t rate : device.sample_rates) {
        rates.push_back(static_cast<uint>(rate));
    }
    row[QStringLiteral("sampleRates")] = rates;
    row[QStringLiteral("supportsAc3")] = device.supports_ac3_passthrough;
    row[QStringLiteral("supportsEac3")] = device.supports_eac3_passthrough;
    return row;
}

// --- settings (the Settings page) ------------------------------------------

// ac3::hearth::SettingsStore over QSettings (planning/hearth-reference-
// player.md, A5: "it stores its settings through QSettings"). The key
// strings settings_model.cpp and pairing_store.cpp already compose
// ("playback/gapless", "queue/1/path", "pairing/1/client", ...) are plain
// QSettings keys, so this is a thin pass-through that does not have to know
// what any of them mean - MemorySettingsStore (settings_model.cpp) is the
// test suites' own version of the same three methods.
class QSettingsStore final : public ac3::hearth::SettingsStore {
public:
    explicit QSettingsStore(QSettings& settings) : settings_(settings) {}

    [[nodiscard]] std::optional<std::string> value(std::string_view key) const override {
        const QVariant found =
            settings_.value(QString::fromUtf8(key.data(), static_cast<qsizetype>(key.size())));
        if (!found.isValid()) {
            return std::nullopt;
        }
        return found.toString().toStdString();
    }

    void set_value(std::string_view key, std::string_view value) override {
        settings_.setValue(QString::fromUtf8(key.data(), static_cast<qsizetype>(key.size())),
                           QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())));
    }

    void remove_group(std::string_view group) override {
        // QSettings::remove() already removes the key itself and everything
        // under it; MemorySettingsStore::remove_group() only says so in a
        // comment because it has to do that walk by hand.
        settings_.remove(QString::fromUtf8(group.data(), static_cast<qsizetype>(group.size())));
    }

    [[nodiscard]] bool sync() override {
        settings_.sync();
        return settings_.status() == QSettings::NoError;
    }

private:
    QSettings& settings_;
};

[[nodiscard]] QString failure_policy_name(ac3::hearth::FailurePolicy policy) {
    return policy == ac3::hearth::FailurePolicy::kStop ? QStringLiteral("stop") : QStringLiteral("skip");
}

[[nodiscard]] ac3::hearth::FailurePolicy failure_policy_from_name(const QString& name) {
    return name == QLatin1String("stop") ? ac3::hearth::FailurePolicy::kStop
                                         : ac3::hearth::FailurePolicy::kSkip;
}

// Read fresh rather than cached, every time - hearth_controller.hpp's own
// comment on why EngineSettings is never a member here applies the same way
// to a local variable that would outlive one call.
[[nodiscard]] ac3::hearth::EngineSettings current_settings(const ac3::hearth::SettingsStore& store) {
    return ac3::hearth::load_settings(store, QSysInfo::machineHostName().toStdString());
}

// sync() is [[nodiscard]] (settings_model.hpp: "False when that failed: what
// was kept before is what a later start reads") - noted rather than silently
// dropped, since a full disk or a read-only settings folder is exactly the
// kind of thing a diagnostics export exists to have caught.
void sync_store(ac3::hearth::SettingsStore& store, ac3::hearth::DiagnosticLog& log) {
    if (!store.sync()) {
        log.note("settings: could not save to disk");
    }
}

}  // namespace

HearthController::HearthController(QObject* parent)
    : QObject(parent),
      log_(ac3::hearth::process_diagnostics()),
      settings_(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("ac3forge"),
               QStringLiteral("Hearth")),
      store_(std::make_unique<QSettingsStore>(settings_)),
      pairing_(std::make_unique<ac3::hearth::PairingStore>(
          *store_, [] { return QDate::currentDate().toString(Qt::ISODate).toStdString(); })) {
    poll_timer_.setInterval(kPollMs);
    connect(&poll_timer_, &QTimer::timeout, this, &HearthController::poll);
    // A window close runs this; a session logout or a killed process does
    // not, which is the same trade every setter below already makes by
    // syncing only on a change rather than continuously (save_on_quit()'s
    // own comment in the header).
    if (auto* application = QCoreApplication::instance()) {
        connect(application, &QCoreApplication::aboutToQuit, this, &HearthController::save_on_quit);
    }
}

HearthController::~HearthController() = default;

QString HearthController::versionDetails() const {
    return QString::fromStdString(ac3::version_details());
}

QString HearthController::licenceNotices() const {
    // The same file the package installs, embedded by
    // apps/hearth/notices/notices.cmake once ac3hearth exists for it to
    // embed into, so the dialog cannot say something the package does not.
    // A binary built without the embedding gets a sentence that says so
    // rather than an empty view - the same fallback
    // CrucibleController::licenceNotices() uses.
    QFile file(QStringLiteral(":/notices/NOTICES.txt"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return tr("This build carries no embedded notices file (:/notices/NOTICES.txt was not "
                  "compiled in); the NOTICES.txt beside the application and the repository's "
                  "LICENSE say what it ships.");
    }
    return QString::fromUtf8(file.readAll());
}

void HearthController::start() {
    if (engine_) {
        return;
    }
    // "2.0" until the Speakers page makes the layout a setting; a literal
    // this application writes always parses.
    const std::optional<ac3::render::OutputLayout> layout = ac3::render::OutputLayout::parse("2.0");
    // EngineOutputs, not a bare PcmSink: given every render endpoint
    // (device_endpoints(), output_selector.hpp), the engine decides each
    // item's output itself and set_output_preferences() - the output
    // picker's "Play here" - has something to act on. No bitstream sink yet,
    // so OutputSelector::endpoints() reads every endpoint's passthrough
    // flags as false regardless of what the device reports
    // (bitstream_output=false in engine_thread.cpp's EngineOutputs
    // constructor) and every item still decodes to PCM - the same outcome
    // the old bare-PcmSink construction gave, on the same default device
    // (best_for_pcm() picks it the same way DeviceSink::open() did with an
    // empty device id), until a dialog row pins a different one.
    ac3::hearth::EngineOutputs outputs{.pcm = ac3::hearth::make_device_sink(std::string()),
                                       .bitstream = {},
                                       .endpoints = ac3::hearth::device_endpoints()};
    const ac3::hearth::EngineSettings loaded = current_settings(*store_);
    engine_ = std::make_unique<ac3::hearth::Engine>(
        std::move(outputs), ac3::hearth::ui::make_file_item_loader(), *layout,
        ac3::hearth::DecoderSettings{}, ac3::hearth::EngineTiming{}, &log_);
    engine_->set_gapless(loaded.playback.gapless);
    engine_->set_on_failure(loaded.playback.on_failure);
    if (loaded.playback.resume_queue) {
        const ac3::hearth::SavedQueue saved = ac3::hearth::load_queue(*store_);
        if (!saved.items.empty()) {
            engine_->restore(saved.items, saved.current, saved.position);
        }
    }
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
    ac3::hearth::EngineSettings settings = current_settings(*store_);
    settings.playback.gapless = on;
    ac3::hearth::save_settings(settings, *store_);
    sync_store(*store_, log_);
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
        items.push_back(queue_item_from_path(path));
    }
    engine_->add(std::move(items));
}

void HearthController::addFolder(const QString& path) {
    if (!engine_ || path.isEmpty()) {
        return;
    }
    const std::vector<std::string> found = ac3::hearth::ui::list_folder_items(path.toStdString());
    if (found.empty()) {
        return;
    }
    std::vector<ac3::hearth::QueueItem> items;
    items.reserve(found.size());
    for (const std::string& item_path : found) {
        items.push_back(queue_item_from_path(QString::fromStdString(item_path)));
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
    const QString new_device_id = QString::fromStdString(status.device_id);
    const bool new_has_lfe = status.layout.lfe_count() > 0;
    const int new_identify_slot = status.identify_slot == ac3::hearth::Queue::kNone
                                      ? -1
                                      : static_cast<int>(status.identify_slot);
    if (layout_changed || new_trim_db != trim_db_ || new_delay_ms != delay_ms_ ||
        status.crossover_hz != crossover_hz_ || new_routing != routing_ ||
        static_cast<int>(status.routing.outputs()) != routing_outputs_ ||
        new_device_name != device_name_ || new_device_id != device_id_ || new_has_lfe != layout_has_lfe_ ||
        status.identify_level_db != identify_level_db_ || new_identify_slot != identify_slot_) {
        trim_db_ = std::move(new_trim_db);
        delay_ms_ = std::move(new_delay_ms);
        crossover_hz_ = status.crossover_hz;
        routing_ = std::move(new_routing);
        routing_outputs_ = static_cast<int>(status.routing.outputs());
        device_name_ = new_device_name;
        device_id_ = new_device_id;
        layout_has_lfe_ = new_has_lfe;
        if (layout_changed) {
            layout_text_ = new_layout_text;
            speaker_labels_ = new_labels;
            speaker_small_ = new_small;
            speaker_is_lfe_ = new_is_lfe;
            layout_has_height_ = new_has_height;
            heights_realization_ = new_heights;
        }
        identify_level_db_ = status.identify_level_db;
        identify_slot_ = new_identify_slot;
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

void HearthController::refreshOutputDevices() {
    QVariantList rows;
    const auto devices = ac3::audio::enumerate_render_devices();
    if (devices.has_value()) {
        rows.reserve(static_cast<qsizetype>(devices->size()));
        for (const auto& device : *devices) {
            rows.push_back(output_device_row(device));
        }
    }
    // A failed enumeration (kNoBackend, say) empties the list rather than
    // keeping whatever an earlier, working refresh found - a row from a
    // probe this machine can no longer repeat should not sit there
    // clickable as if it still could.
    output_devices_ = std::move(rows);
    emit outputDevicesChanged();
}

void HearthController::selectOutputDevice(const QString& deviceId) {
    if (!engine_ || deviceId.isEmpty()) {
        return;
    }
    engine_->set_output_preferences(
        ac3::hearth::OutputPreferences{.pinned = ac3::hearth::OutputMode::kLocalPcm,
                                       .endpoint_id = deviceId.toStdString(),
                                       .follow_sink = true});
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

void HearthController::setIdentifyLevelDb(double db) {
    if (engine_) {
        engine_->set_identify_level_db(db);
    }
}

void HearthController::startIdentify(int slot) {
    if (engine_ && slot >= 0) {
        engine_->identify_start(static_cast<std::size_t>(slot));
    }
}

void HearthController::stopIdentify() {
    if (engine_) {
        engine_->identify_stop();
    }
}

// --- first run ---------------------------------------------------------

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

// --- settings (the Settings page) ------------------------------------------

bool HearthController::resumeQueue() const {
    return current_settings(*store_).playback.resume_queue;
}

void HearthController::setResumeQueue(bool on) {
    ac3::hearth::EngineSettings settings = current_settings(*store_);
    if (settings.playback.resume_queue == on) {
        return;
    }
    settings.playback.resume_queue = on;
    ac3::hearth::save_settings(settings, *store_);
    sync_store(*store_, log_);
    emit settingsChanged();
}

QString HearthController::onFailure() const {
    return failure_policy_name(current_settings(*store_).playback.on_failure);
}

void HearthController::setOnFailure(const QString& policy) {
    const ac3::hearth::FailurePolicy wanted = failure_policy_from_name(policy);
    ac3::hearth::EngineSettings settings = current_settings(*store_);
    if (settings.playback.on_failure == wanted) {
        return;
    }
    settings.playback.on_failure = wanted;
    ac3::hearth::save_settings(settings, *store_);
    sync_store(*store_, log_);
    if (engine_) {
        engine_->set_on_failure(wanted);
    }
    emit settingsChanged();
}

QString HearthController::networkName() const {
    return QString::fromStdString(current_settings(*store_).network.name);
}

void HearthController::setNetworkName(const QString& name) {
    ac3::hearth::EngineSettings settings = current_settings(*store_);
    const std::string wanted = name.toStdString();
    if (settings.network.name == wanted) {
        return;
    }
    settings.network.name = wanted;
    // settings_rows() (called from save_settings()) runs this through
    // ac3::hearth::network_name() itself before it is written, so a name
    // typed with leading/trailing spaces or past the 63-byte mDNS label
    // limit is stored trimmed either way; load_settings() re-derives the
    // same trim on every read, so the round trip agrees with what is shown.
    ac3::hearth::save_settings(settings, *store_);
    sync_store(*store_, log_);
    emit settingsChanged();
}

bool HearthController::networkDiscover() const {
    return current_settings(*store_).network.discover;
}

void HearthController::setNetworkDiscover(bool on) {
    ac3::hearth::EngineSettings settings = current_settings(*store_);
    if (settings.network.discover == on) {
        return;
    }
    settings.network.discover = on;
    ac3::hearth::save_settings(settings, *store_);
    sync_store(*store_, log_);
    emit settingsChanged();
}

QVariantList HearthController::pairingRecords() const {
    QVariantList rows;
    if (!pairing_) {
        return rows;
    }
    for (const ac3::hearth::PairingRecordView& record : pairing_->records()) {
        QVariantMap row;
        row[QStringLiteral("id")] =
            QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(record.client_key.data()),
                                           static_cast<qsizetype>(record.client_key.size()))
                                    .toHex());
        row[QStringLiteral("name")] = QString::fromStdString(record.name);
        row[QStringLiteral("pairedOn")] = QString::fromStdString(record.paired_on);
        rows.push_back(row);
    }
    return rows;
}

void HearthController::forgetPairing(const QString& id) {
    if (!pairing_) {
        return;
    }
    const QByteArray bytes = QByteArray::fromHex(id.toLatin1());
    ac3::sendspin::crypto::Key32 key{};
    if (static_cast<std::size_t>(bytes.size()) != key.size()) {
        return;
    }
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(bytes[static_cast<qsizetype>(i)]);
    }
    pairing_->forget(key);
    emit pairingChanged();
}

// --- appearance --------------------------------------------------------

QString HearthController::theme() const {
    return settings_.value(QStringLiteral("appearance/theme"), QStringLiteral("system")).toString();
}

void HearthController::setTheme(const QString& theme) {
    if (theme == this->theme()) {
        return;
    }
    settings_.setValue(QStringLiteral("appearance/theme"), theme);
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

QString HearthController::palette() const {
    return settings_.value(QStringLiteral("appearance/palette"), QStringLiteral("signal")).toString();
}

void HearthController::setPalette(const QString& palette) {
    if (palette == this->palette()) {
        return;
    }
    settings_.setValue(QStringLiteral("appearance/palette"), palette);
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

QString HearthController::textScale() const {
    const auto stored = settings_.value(QStringLiteral("appearance/textScale"), QStringLiteral("100")).toString();
    static const QStringList known{QStringLiteral("system"), QStringLiteral("100"), QStringLiteral("125"),
                                   QStringLiteral("150"), QStringLiteral("175")};
    return known.contains(stored) ? stored : QStringLiteral("100");
}

void HearthController::setTextScale(const QString& scale) {
    if (scale == textScale()) {
        return;
    }
    settings_.setValue(QStringLiteral("appearance/textScale"), scale);
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

// --- diagnostics -------------------------------------------------------

QString HearthController::diagnosticsReport() const {
    const ac3::hearth::EngineStatus status = engine_ ? engine_->status() : ac3::hearth::EngineStatus{};

    ac3::hearth::ReportFacts facts;
    facts.written_at = QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toStdString();
    const auto started_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 log_.started_at().time_since_epoch())
                                .count();
    facts.log_started_at =
        QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(started_ms)).toString(Qt::ISODateWithMs).toStdString();
    facts.version = ac3::version_details();

    auto platform_row = [&facts](const char* name, const QString& value) {
        facts.platform.emplace_back(name, value.toStdString());
    };
    platform_row("os", QSysInfo::prettyProductName());
    platform_row("kernel", QSysInfo::kernelType() + QLatin1Char(' ') + QSysInfo::kernelVersion());
    platform_row("cpu", QSysInfo::currentCpuArchitecture());
    platform_row("qt", QString::fromLatin1(qVersion()) + QStringLiteral(" (built against ") +
                           QString::fromLatin1(QT_VERSION_STR) + QLatin1Char(')'));
    platform_row("qpa", QGuiApplication::platformName());

    facts.output_name = status.device_name;
    facts.output_reason = status.output_reason;
    facts.settings = ac3::hearth::settings_rows(current_settings(*store_));

    // The caller's own secrets, beyond what render_report() already knows to
    // withhold from the engine snapshot (an item's path, kWithheldSettings):
    // where this computer's settings live, and the person's own home folder,
    // each in every spelling withhold_path()/scrub() might meet.
    ac3::hearth::Secrets secrets;
    auto add_secret = [&secrets](const QString& path) {
        if (path.isEmpty()) {
            return;
        }
        secrets.strings.push_back(path.toStdString());
        secrets.strings.push_back(QDir::fromNativeSeparators(path).toStdString());
        secrets.strings.push_back(QDir::toNativeSeparators(path).toStdString());
    };
    add_secret(QFileInfo(settings_.fileName()).absolutePath());
    add_secret(QStandardPaths::writableLocation(QStandardPaths::HomeLocation));

    return QString::fromStdString(ac3::hearth::render_report(facts, status, log_, secrets));
}

QString HearthController::suggestedDiagnosticsFile() const {
    QString folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (folder.isEmpty()) {
        folder = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    }
    const QString name = QStringLiteral("hearth-diagnostics-") +
                         QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")) +
                         QStringLiteral(".txt");
    return QUrl::fromLocalFile(QDir(folder).filePath(name)).toString();
}

bool HearthController::exportDiagnostics(const QString& fileUrl) {
    // The GUI's rule for a dialog's answer: a file: URL becomes a local
    // path, anything else is taken as one already.
    const QUrl url(fileUrl);
    const QString path = url.isLocalFile() ? url.toLocalFile() : fileUrl;
    const QString shown = QDir::toNativeSeparators(path);
    // UTF-8 with LF line endings on every platform: written as bytes, not
    // through a text-mode translation.
    const QByteArray report = diagnosticsReport().toUtf8();
    QSaveFile file(path);
    bool ok = file.open(QIODevice::WriteOnly);
    if (ok) {
        ok = file.write(report) == static_cast<qint64>(report.size()) && file.commit();
    }
    if (ok) {
        diagnostics_message_ = tr("saved to %1").arg(shown);
        log_.note("diagnostics saved");
    } else {
        diagnostics_message_ = tr("could not write %1: %2").arg(shown, file.errorString());
    }
    emit diagnosticsChanged();
    return ok;
}

void HearthController::save_on_quit() {
    if (!engine_) {
        return;
    }
    const ac3::hearth::EngineSettings settings = current_settings(*store_);
    if (settings.playback.resume_queue) {
        ac3::hearth::save_queue(ac3::hearth::saved_queue(engine_->status(), engine_->position()), *store_);
    }
    sync_store(*store_, log_);
}

}  // namespace ac3::hearth::ui

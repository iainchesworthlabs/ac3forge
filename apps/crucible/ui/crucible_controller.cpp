#include "crucible_controller.hpp"

#include "ac3/internal/profiling.hpp"

#include "ac3/version.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QVariantMap>
#include <QtLogging>

#include <QByteArray>
#include <QDateTime>
#include <QGuiApplication>
#include <QIODevice>
#include <QLocale>
#include <QSaveFile>
#include <QSysInfo>
#include <QtGlobal>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

#include "ac3/audio/audio_backend.hpp"
#include "diagnostics.hpp"
#include "output_policy.hpp"
#include "platform_services.hpp"

namespace {

using ac3::crucible::OutputMode;

constexpr int kPollMs = 60;  // the meters read at this rate; 120 stepped visibly

// The version of the first-run explanation (FirstRunDialog.qml). Bump it
// when what the dialog says changes enough to be worth showing once more,
// such as the silent device's name changing with the driver's signing.
constexpr int kFirstRunVersion = 1;

// A silent device the application makes itself appears in the graph a
// moment after the request returns; Send applications waits this long, at
// most, for it before moving the default, so one press does the whole move.
constexpr int kCreateWaitTries = 20;
constexpr unsigned long kCreateWaitStepMs = 25;

QString mode_key(OutputMode mode) {
    switch (mode) {
        case OutputMode::kAtmos: return QStringLiteral("atmos");
        case OutputMode::kDdPlus51: return QStringLiteral("ddplus");
        case OutputMode::kDd51: return QStringLiteral("dd");
        case OutputMode::kPcmSurround: return QStringLiteral("pcm");
        case OutputMode::kHeadphones: return QStringLiteral("headphones");
        case OutputMode::kStereo: return QStringLiteral("stereo");
        case OutputMode::kNone: return QStringLiteral("none");
    }
    return QStringLiteral("none");
}

std::optional<OutputMode> mode_from_key(const QString& key) {
    if (key == QLatin1String("atmos")) return OutputMode::kAtmos;
    if (key == QLatin1String("ddplus")) return OutputMode::kDdPlus51;
    if (key == QLatin1String("dd")) return OutputMode::kDd51;
    if (key == QLatin1String("pcm")) return OutputMode::kPcmSurround;
    if (key == QLatin1String("headphones")) return OutputMode::kHeadphones;
    if (key == QLatin1String("stereo")) return OutputMode::kStereo;
    return std::nullopt;
}

QString short_mode_name(OutputMode mode) {
    switch (mode) {
        case OutputMode::kAtmos: return CrucibleController::tr("Atmos");
        case OutputMode::kDdPlus51: return CrucibleController::tr("Dolby Digital Plus 5.1");
        case OutputMode::kDd51: return CrucibleController::tr("Dolby Digital 5.1");
        case OutputMode::kPcmSurround: return CrucibleController::tr("PCM surround");
        case OutputMode::kHeadphones: return CrucibleController::tr("Headphones");
        case OutputMode::kStereo: return CrucibleController::tr("Stereo");
        case OutputMode::kNone: return CrucibleController::tr("No output");
    }
    return {};
}

// Session names are executable stems ("chrome", "steam"); a capital
// first letter reads as the application's name rather than its file's.
QString display_name(const std::string& stem) {
    QString name = QString::fromUtf8(stem.data(), static_cast<qsizetype>(stem.size()));
    if (!name.isEmpty()) {
        name[0] = name[0].toUpper();
    }
    return name;
}

QString from_utf8(const std::string& s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

}  // namespace

CrucibleController::CrucibleController(QObject* parent)
    // The four-argument constructor: the two-argument one always uses the
    // native store (the registry here) whatever QSettings::setDefaultFormat
    // says, which let the test suites read and write the developer's own
    // settings for a whole afternoon. This one honours the default format,
    // so the tests' isolation (an INI file in a temporary directory) holds.
    : QObject(parent),
      default_device_(ac3::crucible::platform_default_device()),
      virtual_device_(ac3::crucible::platform_virtual_device()),
      settings_(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("ac3forge"), QStringLiteral("Crucible")),
      log_(ac3::crucible::process_diagnostics()),
      foreground_(ac3::crucible::platform_foreground()) {
    poll_timer_.setInterval(kPollMs);
    connect(&poll_timer_, &QTimer::timeout, this, &CrucibleController::poll);
    driver_timer_.setInterval(250);
    connect(&driver_timer_, &QTimer::timeout, this, &CrucibleController::poll_driver);
    // A session logout, or a close of the last path that never went through
    // quit(), still puts the default back; quit() runs the same restore
    // first and the flag makes the second call a no-op.
    if (auto* application = QCoreApplication::instance()) {
        connect(application, &QCoreApplication::aboutToQuit, this, &CrucibleController::restore_on_quit);
    }
    previous_default_id_ = default_device_->default_id();
    refreshDefault();
    refreshDriver();
}

CrucibleController::~CrucibleController() {
    stop();
}

ac3::crucible::EngineConfig CrucibleController::engine_config() const {
    ac3::crucible::EngineConfig config;
    config.null_sink_substring = nullSinkName().toStdString();
    config.signing_key_path = keyPath().toStdString();
    config.low_latency = lowLatency();
    config.bypass_codec = bypassCodec();
    config.split_by_default = splitStereo();
    config.bitrate_kbps = static_cast<std::uint32_t>(std::max(0, bitrate()));
    config.pinned = mode_from_key(pinned());
    config.preferred_endpoint_id = preferredEndpoint().toStdString();
    config.diagnostics = &log_;
    return config;
}

void CrucibleController::start() {
    if (engine_) {
        return;
    }
    engine_ = std::make_unique<ac3::crucible::Engine>(engine_config());
    if (const auto started = engine_->start(); !started) {
        last_error_ = from_utf8(started.error());
        log_.note("engine start refused: " + started.error());
        engine_.reset();
        emit stateChanged();
        return;
    }
    running_ = true;
    poll_timer_.start();
    emit stateChanged();
}

void CrucibleController::stop() {
    poll_timer_.stop();
    if (engine_) {
        log_.note("engine stop requested");
        engine_->stop();
        engine_.reset();
    }
    // The rule's state was the old engine's too: a restart on another seat
    // starts clean and states the rule until the new engine says otherwise.
    fullscreen_rule_ = false;
    fullscreen_reason_.clear();
    if (running_) {
        running_ = false;
        emit stateChanged();
    }
    // The list was the old engine's; a fresh one starts empty.
    if (!apps_.isEmpty() || placed_ != 0 || bed_ != 0) {
        for (auto* entry : entries_) {
            entry->deleteLater();
        }
        entries_.clear();
        apps_.clear();
        placed_ = 0;
        bed_ = 0;
        emit appsChanged();
    }
}

void CrucibleController::quit() {
    stop();
    restore_on_quit();
    QCoreApplication::quit();
}

void CrucibleController::restore_on_quit() {
    if (!moved_default_by_us_) {
        return;
    }
    moved_default_by_us_ = false;
    // The machine's state now, not the last poll's: a person may have moved
    // the default by hand since, and that is left where they put it.
    refreshDefault();
    if (!default_is_null_sink_ || previous_default_id_.empty()) {
        return;
    }
    if (const auto ok = default_device_->set_default(previous_default_id_); !ok) {
        qWarning("could not restore the previous default output on quit: %s", ok.error().c_str());
    }
}

void CrucibleController::restart_engine() {
    if (!engine_) {
        return;
    }
    stop();
    start();
}

void CrucibleController::poll() {
    AC3_ZONE_SCOPED_N("desk poll");
    if (!engine_) {
        return;
    }
    const auto s = engine_->status();

    // Applications: one live entry each, updated in place. The list is
    // rebuilt (and appsChanged emitted) only when an application appears,
    // leaves, or its order changes; a position or level change reaches the
    // delegates through the entry's own signals.
    int placed = 0;
    int bed = 0;
    const bool show_background = showBackgroundApps();
    const bool show_silent = showSilentApps();
    int sounding = 0;
    QList<QObject*> apps;
    bool membership_changed = false;
    for (const auto& app : s.apps) {
        // Background processes (no window of their own, not packaged) stay
        // out of the rail unless asked for; they keep their tap and their
        // place in the bed regardless.
        const bool background = !app.has_window && !app.packaged;
        if (background && !show_background) {
            continue;
        }
        // Silent and unplaced: listed only when asked for.
        if (!app.has_session && !app.slot && !show_silent) {
            continue;
        }
        if (app.has_session && app.active) {
            ++sounding;
        }
        const int id = static_cast<int>(app.app);
        auto* entry = qobject_cast<ac3::crucible::ui::AppEntry*>(entries_.value(id, nullptr));
        if (entry == nullptr) {
            entry = new ac3::crucible::ui::AppEntry(id, this);
            entries_.insert(id, entry);
            membership_changed = true;
        }
        // The executable's own description ("Steam", "Zoom") over its stem.
        entry->update(app, app.description.empty() ? display_name(app.name) : from_utf8(app.description), background);
        apps.push_back(entry);
        (app.slot ? placed : bed) += 1;
    }
    // Entries for applications that left.
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!apps.contains(it.value())) {
            it.value()->deleteLater();
            it = entries_.erase(it);
            membership_changed = true;
        } else {
            ++it;
        }
    }
    // Sound first, then a session without sound, then silent; by name
    // within each, so the rail reads top to bottom.
    std::stable_sort(apps.begin(), apps.end(), [](QObject* a, QObject* b) {
        auto* ea = qobject_cast<ac3::crucible::ui::AppEntry*>(a);
        auto* eb = qobject_cast<ac3::crucible::ui::AppEntry*>(b);
        const int ra = ea->silent() ? 2 : (ea->active() ? 0 : 1);
        const int rb = eb->silent() ? 2 : (eb->active() ? 0 : 1);
        if (ra != rb) {
            return ra < rb;
        }
        return ea->name().compare(eb->name(), Qt::CaseInsensitive) < 0;
    });
    if (sounding != sounding_) {
        sounding_ = sounding;
        membership_changed = true;
    }
    if (membership_changed || apps != apps_) {
        apps_ = std::move(apps);
        emit appsChanged();
    }
    if (placed != placed_ || bed != bed_) {
        placed_ = placed;
        bed_ = bed;
        emit appsChanged();
    }

    const QString mode_name = short_mode_name(s.mode);
    const QString key = mode_key(s.mode);
    const QString endpoint = from_utf8(s.endpoint_name);
    const QString reason = from_utf8(s.output_reason);
    const QString signing = from_utf8(s.signing);
    const QString error = from_utf8(s.last_error);
    const bool rule = s.fullscreen_rule_available;
    const QString rule_reason = from_utf8(s.fullscreen_rule_reason);
    if (mode_name != mode_name_ || key != mode_key_ || endpoint != endpoint_name_ ||
        reason != output_reason_ || signing != signing_status_ ||
        s.objects_enabled != objects_enabled_ || error != last_error_ ||
        static_cast<int>(s.tap_channels) != tap_channels_ || s.codec_bypassed != codec_bypassed_ ||
        rule != fullscreen_rule_ || rule_reason != fullscreen_reason_) {
        tap_channels_ = static_cast<int>(s.tap_channels);
        codec_bypassed_ = s.codec_bypassed;
        fullscreen_rule_ = rule;
        fullscreen_reason_ = rule_reason;
        mode_name_ = mode_name;
        mode_key_ = key;
        endpoint_name_ = endpoint;
        output_reason_ = reason;
        signing_status_ = signing;
        objects_enabled_ = s.objects_enabled;
        last_error_ = error;
        emit stateChanged();
        // A mode change usually means the endpoint list changed too.
        last_endpoint_stamp_ = 0;
    }

    frames_ = static_cast<double>(s.frames_encoded);
    underruns_ = static_cast<double>(s.underruns);
    starved_ = static_cast<double>(s.starved_reads);
    last_frame_ms_ = s.last_frame_ms;
    worst_frame_ms_ = s.worst_frame_ms;
    encode_ms_ = s.encode_ms;
    emit statsChanged();

    // The endpoint table is what the engine's last probe saw; it changes
    // rarely, so it is re-read only when the mode moved or every ~2 s.
    if (last_endpoint_stamp_ == 0 || s.frames_encoded - last_endpoint_stamp_ > 60) {
        last_endpoint_stamp_ = std::max<std::uint64_t>(s.frames_encoded, 1);
        QVariantList endpoints;
        for (const auto& e : s.endpoints) {
            QVariantMap row;
            row.insert(QStringLiteral("id"), from_utf8(e.id));
            row.insert(QStringLiteral("name"), from_utf8(e.name));
            row.insert(QStringLiteral("isDefault"), e.is_default);
            row.insert(QStringLiteral("isNullSink"), e.is_null_sink);
            row.insert(QStringLiteral("eac3"), e.accepts_eac3);
            row.insert(QStringLiteral("ac3"), e.accepts_ac3);
            row.insert(QStringLiteral("pcmChannels"), static_cast<int>(e.shared_channels));
            row.insert(QStringLiteral("spatial"), e.spatial);
            row.insert(QStringLiteral("chosen"), from_utf8(e.name) == endpoint_name_ && s.mode != OutputMode::kNone);
            row.insert(QStringLiteral("preferred"), from_utf8(e.id) == preferredEndpoint());
            endpoints.push_back(row);
        }
        if (endpoints != endpoints_) {
            endpoints_ = std::move(endpoints);
            emit endpointsChanged();
        }
        refreshDefault();
    }
}

// --- settings ---------------------------------------------------------------

QString CrucibleController::pinned() const {
    return settings_.value(QStringLiteral("output/pinned"), QStringLiteral("auto")).toString();
}

void CrucibleController::setPinned(const QString& mode) {
    if (mode == pinned()) {
        return;
    }
    settings_.setValue(QStringLiteral("output/pinned"), mode);
    log_.note("setting output/pinned = " + mode.toStdString());
    if (engine_) {
        engine_->pin(mode_from_key(mode));
    }
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

QString CrucibleController::preferredEndpoint() const {
    return settings_.value(QStringLiteral("output/endpoint")).toString();
}

void CrucibleController::setPreferredEndpoint(const QString& id) {
    if (id == preferredEndpoint()) {
        return;
    }
    settings_.setValue(QStringLiteral("output/endpoint"), id);
    log_.note("setting output/endpoint = " + (id.isEmpty() ? std::string("(automatic)") : id.toStdString()));
    if (engine_) {
        engine_->prefer_endpoint(id.toStdString());
    }
    settings_.sync();  // survive a hard exit
    last_endpoint_stamp_ = 0;  // re-read the table with the new flag at the next poll
    emit settingsChanged();
}

QString CrucibleController::keyPath() const {
    return settings_.value(QStringLiteral("signing/keyPath")).toString();
}

// The default is the driver's current device name, not the application's:
// see EngineConfig::null_sink_substring for why the two differ until
// attestation signing lands.
QString CrucibleController::nullSinkName() const {
    // The default is the platform's own name for its silent device, not a
    // literal: "Desktop Atmos" on Windows, "Crucible (silent)" on Linux.
    return settings_.value(QStringLiteral("output/nullSinkName"),
                           from_utf8(virtual_device_->device_name()))
        .toString();
}

QString CrucibleController::silentDeviceAdvice() const {
    return from_utf8(virtual_device_->how_to_get_one());
}

bool CrucibleController::silentDeviceFromPackage() const {
    return virtual_device_->from_package();
}

bool CrucibleController::silentDeviceCanCreate() const {
    return !virtual_device_->from_package() && silent_state_.can_install;
}

void CrucibleController::setNullSinkName(const QString& name) {
    if (name == nullSinkName()) {
        return;
    }
    settings_.setValue(QStringLiteral("output/nullSinkName"), name);
    log_.note("setting output/nullSinkName = " + name.toStdString());
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
    restart_engine();
    refreshDefault();
}

bool CrucibleController::lowLatency() const {
    return settings_.value(QStringLiteral("codec/lowLatency"), false).toBool();
}

void CrucibleController::setLowLatency(bool on) {
    if (on == lowLatency()) {
        return;
    }
    settings_.setValue(QStringLiteral("codec/lowLatency"), on);
    log_.note(on ? "setting codec/lowLatency = on" : "setting codec/lowLatency = off");
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
    restart_engine();
}

bool CrucibleController::bypassCodec() const {
    return settings_.value(QStringLiteral("codec/bypass"), false).toBool();
}

void CrucibleController::setBypassCodec(bool on) {
    if (on == bypassCodec()) {
        return;
    }
    settings_.setValue(QStringLiteral("codec/bypass"), on);
    log_.note(on ? "setting codec/bypass = on" : "setting codec/bypass = off");
    if (engine_) {
        engine_->set_bypass(on);
    }
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

bool CrucibleController::splitStereo() const {
    return settings_.value(QStringLiteral("codec/splitStereo"), false).toBool();
}

void CrucibleController::setSplitStereo(bool on) {
    if (on == splitStereo()) {
        return;
    }
    settings_.setValue(QStringLiteral("codec/splitStereo"), on);
    log_.note(on ? "setting codec/splitStereo = on" : "setting codec/splitStereo = off");
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
    // The default applies to applications the engine meets from now on;
    // a restart applies it to the ones it already has.
    restart_engine();
}

int CrucibleController::bitrate() const {
    return settings_.value(QStringLiteral("codec/bitrate"), 0).toInt();
}

void CrucibleController::setBitrate(int kbps) {
    if (kbps == bitrate()) {
        return;
    }
    settings_.setValue(QStringLiteral("codec/bitrate"), kbps);
    log_.note("setting codec/bitrate = " + std::to_string(kbps));
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
    restart_engine();
}

QString CrucibleController::theme() const {
    return settings_.value(QStringLiteral("appearance/theme"), QStringLiteral("system")).toString();
}

void CrucibleController::setTheme(const QString& theme) {
    if (theme == this->theme()) {
        return;
    }
    settings_.setValue(QStringLiteral("appearance/theme"), theme);
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

QString CrucibleController::palette() const {
    return settings_.value(QStringLiteral("appearance/palette"), QStringLiteral("system")).toString();
}

void CrucibleController::setPalette(const QString& palette) {
    if (palette == this->palette()) {
        return;
    }
    settings_.setValue(QStringLiteral("appearance/palette"), palette);
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

QString CrucibleController::roomView() const {
    return settings_.value(QStringLiteral("appearance/roomView"), QStringLiteral("2d")).toString();
}

void CrucibleController::setRoomView(const QString& view) {
    const QString wanted = view == QLatin1String("3d") ? QStringLiteral("3d") : QStringLiteral("2d");
    if (wanted == roomView()) {
        return;
    }
    settings_.setValue(QStringLiteral("appearance/roomView"), wanted);
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

bool CrucibleController::keepRunningWhenClosed() const {
    return settings_.value(QStringLiteral("behaviour/keepRunningWhenClosed"), true).toBool();
}

void CrucibleController::setKeepRunningWhenClosed(bool on) {
    settings_.setValue(QStringLiteral("behaviour/keepRunningWhenClosed"), on);
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

bool CrucibleController::showBackgroundApps() const {
    return settings_.value(QStringLiteral("behaviour/showBackgroundApps"), false).toBool();
}

void CrucibleController::setShowBackgroundApps(bool on) {
    if (on == showBackgroundApps()) {
        return;
    }
    settings_.setValue(QStringLiteral("behaviour/showBackgroundApps"), on);
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
    poll();
}

bool CrucibleController::showSilentApps() const {
    return settings_.value(QStringLiteral("behaviour/showSilentApps"), true).toBool();
}

void CrucibleController::setShowSilentApps(bool on) {
    if (on == showSilentApps()) {
        return;
    }
    settings_.setValue(QStringLiteral("behaviour/showSilentApps"), on);
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
    poll();
}

QString CrucibleController::roomLayout() const {
    const auto v = settings_.value(QStringLiteral("appearance/roomLayout"), QStringLiteral("auto")).toString();
    static const QStringList known{QStringLiteral("auto"), QStringLiteral("5.1"), QStringLiteral("7.1"), QStringLiteral("7.1.4")};
    return known.contains(v) ? v : QStringLiteral("auto");
}

void CrucibleController::setRoomLayout(const QString& layout) {
    if (layout == roomLayout()) {
        return;
    }
    settings_.setValue(QStringLiteral("appearance/roomLayout"), layout);
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

QString CrucibleController::versionDetails() const {
    return QString::fromStdString(ac3::version_details());
}

void CrucibleController::setDefaultOutput(const QString& id) {
    if (const auto ok = default_device_->set_default(id.toStdString()); !ok) {
        default_message_ = from_utf8(ok.error());
        log_.note("default output move refused: " + ok.error());
        emit defaultChanged();
        return;
    }
    default_message_.clear();
    refreshDefault();
    log_.note("default output moved to \"" + default_name_.toStdString() + "\"");
}

bool CrucibleController::moveDefaultOnLaunch() const {
    return settings_.value(QStringLiteral("behaviour/moveDefaultOnLaunch"), false).toBool();
}

void CrucibleController::setMoveDefaultOnLaunch(bool on) {
    settings_.setValue(QStringLiteral("behaviour/moveDefaultOnLaunch"), on);
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

bool CrucibleController::firstRunAcknowledged() const {
    return settings_.value(QStringLiteral("firstRun/acknowledgedVersion"), 0).toInt() >= kFirstRunVersion;
}

void CrucibleController::setFirstRunAcknowledged(bool on) {
    if (on == firstRunAcknowledged()) {
        return;
    }
    if (on) {
        settings_.setValue(QStringLiteral("firstRun/acknowledgedVersion"), kFirstRunVersion);
    } else {
        settings_.remove(QStringLiteral("firstRun/acknowledgedVersion"));
    }
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

bool CrucibleController::migratedFromDemo() const {
    return settings_.value(QStringLiteral("migration/fromDesktopAtmos"), false).toBool();
}

// --- commands ---------------------------------------------------------------

void CrucibleController::position(int app, double x, double y, double z) {
    if (engine_) {
        engine_->position(static_cast<ac3::crucible::AppId>(app),
                          {std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0), std::clamp(z, -1.0, 1.0)});
    }
}

void CrucibleController::unposition(int app) {
    if (engine_) {
        engine_->unposition(static_cast<ac3::crucible::AppId>(app));
    }
}

void CrucibleController::setSize(int app, double size) {
    if (engine_) {
        engine_->set_size(static_cast<ac3::crucible::AppId>(app), std::clamp(size, 0.0, 1.0));
    }
}

void CrucibleController::positionSide(int app, int side, double x, double y, double z) {
    if (engine_) {
        engine_->position_side(static_cast<ac3::crucible::AppId>(app), side,
                               {std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0), std::clamp(z, -1.0, 1.0)});
    }
}

void CrucibleController::resetPair(int app) {
    if (engine_) {
        engine_->reset_pair(static_cast<ac3::crucible::AppId>(app));
    }
}

void CrucibleController::setSplit(int app, bool split) {
    if (engine_) {
        engine_->set_split(static_cast<ac3::crucible::AppId>(app), split);
    }
}

void CrucibleController::reprobe() {
    if (engine_) {
        engine_->reprobe();
    }
    last_endpoint_stamp_ = 0;
    refreshDefault();
}

void CrucibleController::loadKey(const QString& path) {
    QString local = path;
    const QUrl url(path);
    if (url.isLocalFile()) {
        local = url.toLocalFile();
    }
    settings_.setValue(QStringLiteral("signing/keyPath"), QDir::toNativeSeparators(local));
    // The path itself never reaches the log (diagnostics.hpp).
    log_.note("signing key file chosen (path withheld)");
    if (engine_) {
        engine_->load_signing_key(local.toStdString());
    }
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

void CrucibleController::clearKey() {
    settings_.remove(QStringLiteral("signing/keyPath"));
    log_.note("signing key cleared");
    if (engine_) {
        engine_->clear_signing_key();
    }
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

void CrucibleController::refreshDefault() {
    const auto endpoints = default_device_->endpoints();
    const std::string needle = nullSinkName().toLower().toStdString();
    default_name_.clear();
    default_is_null_sink_ = false;
    null_sink_present_ = false;
    previous_default_name_.clear();
    for (const auto& endpoint : endpoints) {
        const QString name = from_utf8(endpoint.name);
        const bool is_null = !needle.empty() && name.toLower().toStdString().find(needle) != std::string::npos;
        null_sink_present_ = null_sink_present_ || is_null;
        if (endpoint.is_default) {
            default_name_ = name;
            default_is_null_sink_ = is_null;
        }
        if (endpoint.id == previous_default_id_) {
            previous_default_name_ = name;
        }
    }
    emit defaultChanged();
}

void CrucibleController::moveDefaultToNullSink() {
    // Where this application makes the silent device itself, the first Send
    // creates it, so the seam's "Crucible creates it when you send
    // applications to it" is what happens. Where the device is an installed
    // package this is skipped: an install is elevated and persistent, and
    // stays an explicit Settings action.
    if (!null_sink_present_ && silentDeviceCanCreate()) {
        if (const auto made = virtual_device_->install(); !made) {
            default_message_ = from_utf8(made.error());
            emit defaultChanged();
            return;
        }
        const std::string wanted = nullSinkName().toStdString();
        for (int tries = 0; tries < kCreateWaitTries && default_device_->find_endpoint(wanted).empty(); ++tries) {
            QThread::msleep(kCreateWaitStepMs);
        }
        refreshDefault();
        refreshDriver();
    }
    const auto id = default_device_->find_endpoint(nullSinkName().toStdString());
    if (id.empty()) {
        default_message_ = QStringLiteral("No render endpoint named like \"%1\" exists; install the virtual device or name it in Settings.").arg(nullSinkName());
        log_.note("default output move refused: no endpoint named like \"" + nullSinkName().toStdString() + "\"");
        emit defaultChanged();
        return;
    }
    const std::string current = default_device_->default_id();
    if (current != id) {
        previous_default_id_ = current;
    }
    bool moved = false;
    if (const auto ok = default_device_->set_default(id); !ok) {
        default_message_ = from_utf8(ok.error());
        log_.note("default output move refused: " + ok.error());
        default_device_->open_sound_settings();
    } else {
        default_message_.clear();
        moved = true;
        moved_default_by_us_ = true;
    }
    refreshDefault();
    if (moved) {
        log_.note("default output moved to \"" + default_name_.toStdString() + "\"");
    }
    reprobe();
}

void CrucibleController::restoreDefault() {
    if (previous_default_id_.empty()) {
        default_message_ = QStringLiteral("There is no previous default to restore.");
        log_.note("default output restore refused: no previous default");
        emit defaultChanged();
        return;
    }
    bool moved = false;
    if (const auto ok = default_device_->set_default(previous_default_id_); !ok) {
        default_message_ = from_utf8(ok.error());
        log_.note("default output restore refused: " + ok.error());
        default_device_->open_sound_settings();
    } else {
        default_message_.clear();
        moved = true;
        moved_default_by_us_ = false;
    }
    refreshDefault();
    if (moved) {
        log_.note("default output restored to \"" + default_name_.toStdString() + "\"");
    }
    reprobe();
}

void CrucibleController::openSoundSettings() {
    default_device_->open_sound_settings();
}

// --- the null-sink driver ---------------------------------------------------

QString CrucibleController::driverDir() const {
    const QString stored = settings_.value(QStringLiteral("driver/dir")).toString();
    if (!stored.isEmpty()) {
        return stored;
    }
    // Next to the executable when deployed; the source tree's driver folder
    // when run from a build (the CMake target passes it in).
    const QString beside = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("driver"));
    if (QFileInfo::exists(QDir(beside).filePath(QStringLiteral("install.ps1")))) {
        return QDir::toNativeSeparators(beside);
    }
    return QDir::toNativeSeparators(QStringLiteral(AC3DESK_DRIVER_SOURCE_DIR));
}

void CrucibleController::setDriverDir(const QString& dir) {
    if (dir == driverDir()) {
        return;
    }
    if (dir.trimmed().isEmpty()) {
        settings_.remove(QStringLiteral("driver/dir"));
    } else {
        settings_.setValue(QStringLiteral("driver/dir"), QDir::toNativeSeparators(dir.trimmed()));
    }
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
    refreshDriver();
}

QString CrucibleController::silentDeviceBlocker() const {
    return from_utf8(silent_state_.blocker);
}

QStringList CrucibleController::silentDeviceDetail() const {
    QStringList lines;
    for (const auto& line : silent_state_.detail) {
        lines.push_back(from_utf8(line));
    }
    return lines;
}

void CrucibleController::refreshDriver() {
    // Whether a silent endpoint exists, and whether it is the default, are
    // DefaultDevice's answers; the platform's own facts about installing one
    // come from VirtualDevice. Asking each for what it knows keeps the
    // Windows-only reasoning (test signing, memory integrity, a built
    // package) out of this file and out of the QML.
    virtual_device_->set_package_dir(driverDir().toStdString());
    const ac3::crucible::SilentDeviceQuery query{
        .endpoint_present = null_sink_present_,
        .endpoint_is_default = defaultIsNullSink()};
    silent_state_ = virtual_device_->state(query);
    emit driverChanged();
}

void CrucibleController::installDriver() {
    driver_verb_ = QStringLiteral("install");
    if (const auto started = virtual_device_->install(); !started) {
        driver_message_ = from_utf8(started.error());
        log_.note("silent device install refused: " + started.error());
        emit driverChanged();
        return;
    }
    driver_busy_ = true;
    driver_message_ = tr("installing, answer the elevation prompt ...");
    log_.note("silent device install started");
    driver_timer_.start();
    emit driverChanged();
}

void CrucibleController::removeDriver() {
    driver_verb_ = QStringLiteral("remove");
    if (const auto started = virtual_device_->remove(); !started) {
        driver_message_ = from_utf8(started.error());
        log_.note("silent device remove refused: " + started.error());
        emit driverChanged();
        return;
    }
    driver_busy_ = true;
    driver_message_ = tr("removing, answer the elevation prompt ...");
    log_.note("silent device remove started");
    driver_timer_.start();
    emit driverChanged();
}

void CrucibleController::poll_driver() {
    const auto status = virtual_device_->action_status();
    if (status.running || !status.exit_code) {
        return;
    }
    driver_timer_.stop();
    driver_busy_ = false;
    QStringList lines;
    for (const auto& line : status.log_tail) {
        lines.push_back(from_utf8(line));
    }
    const QString detail =
        lines.isEmpty() ? QString() : QStringLiteral("\n") + lines.join(QStringLiteral("\n"));
    if (*status.exit_code == 0) {
        driver_message_ = (driver_verb_ == QLatin1String("install") ? tr("installed") : tr("removed")) + detail;
        log_.note("silent device " + driver_verb_.toStdString() + ": " +
                  (driver_verb_ == QLatin1String("install") ? "installed" : "removed"));
    } else {
        driver_message_ = tr("%1 failed (exit code %2)").arg(driver_verb_).arg(*status.exit_code) + detail;
        log_.note("silent device " + driver_verb_.toStdString() + " failed (exit code " +
                  std::to_string(*status.exit_code) + ")");
    }
    // The transcript's tail, as the page shows it (the platform has already
    // dropped the transcript's user and machine header lines).
    for (const auto& line : status.log_tail) {
        log_.note("  action: " + line);
    }
    // Endpoints appear a moment after the device does.
    refreshDefault();
    refreshDriver();
    reprobe();
}

// --- diagnostics ------------------------------------------------------------
//
// The rule (diagnostics.hpp): the report is composed from named fields; the
// key, its path and the environment's values have none; the settings are a
// fixed list rather than settings_.allKeys(); and every spelling of the key
// path and of the inline key value is scrubbed from the finished text.

ac3::crucible::Secrets CrucibleController::secrets() const {
    ac3::crucible::Secrets out;
    auto add = [&out](const QString& value) {
        if (!value.isEmpty()) {
            out.strings.push_back(value.toStdString());
        }
    };
    // A path in every spelling it could arrive in.
    auto add_path = [&add](const QString& path) {
        add(path);
        add(QDir::fromNativeSeparators(path));
        add(QDir::toNativeSeparators(path));
        if (!path.isEmpty()) {
            const QString canonical = QFileInfo(path).canonicalFilePath();
            add(canonical);
            add(QDir::toNativeSeparators(canonical));
        }
    };
    add_path(keyPath());
    add_path(qEnvironmentVariable("AC3FORGE_SIGNING_KEY_FILE"));
    add(qEnvironmentVariable("AC3FORGE_SIGNING_KEY"));
    return out;
}

ac3::crucible::ReportFacts CrucibleController::build_report_facts() const {
    using ac3::crucible::KeySource;
    ac3::crucible::ReportFacts facts;
    facts.written_at = QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toStdString();
    const auto started_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(log_.started_at().time_since_epoch()).count();
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
    platform_row("locale", QLocale::system().name());
    // One named, harmless variable (ui/main.cpp sets it); the environment is
    // never enumerated.
    platform_row("render loop", qEnvironmentVariable("QSG_RENDER_LOOP"));
    platform_row("3d room", has3D() ? QStringLiteral("built") : QStringLiteral("not built"));
    const auto& backend = ac3::audio::audio_backend();
    auto capability_row = [&facts](const char* name, const ac3::audio::Capability& capability) {
        facts.platform.emplace_back(name, capability.available ? std::string("yes")
                                                               : "no: " + std::string(capability.reason));
    };
    capability_row("audio capture", backend.capture);
    capability_row("audio passthrough", backend.passthrough);
    capability_row("audio monitor", backend.monitor);
    capability_row("audio spatial", backend.spatial);
    capability_row("process loopback", backend.process_loopback);
    capability_row("device watch", backend.device_watch);

    // Whether the variables are set is read; their values never are.
    const bool env_key_file = qEnvironmentVariableIsSet("AC3FORGE_SIGNING_KEY_FILE");
    const bool env_key_inline = qEnvironmentVariableIsSet("AC3FORGE_SIGNING_KEY");
    facts.signing.objects_enabled = objects_enabled_;
    facts.signing.env_key_file_set = env_key_file;
    facts.signing.env_key_inline_set = env_key_inline;
    facts.signing.source = !keyPath().isEmpty() ? KeySource::kFile
                           : env_key_file       ? KeySource::kEnvironmentFile
                           : env_key_inline     ? KeySource::kEnvironmentInline
                                                : KeySource::kNone;

    facts.render_endpoints = default_device_->endpoints();
    facts.default_is_silent = default_is_null_sink_;
    facts.previous_default_name = previous_default_name_.toStdString();
    facts.moves_default = default_device_->moves_default();
    facts.default_message = default_message_.toStdString();

    facts.silent_device_name = virtual_device_->device_name();
    facts.silent_from_package = virtual_device_->from_package();
    facts.silent_advice = virtual_device_->how_to_get_one();
    facts.silent = silent_state_;
    facts.driver_dir = driverDir().toStdString();
    facts.driver_message = driver_message_.toStdString();
    const auto support = foreground_->support();
    facts.foreground_available = support.available;
    facts.foreground_reason = std::string(support.reason);

    // The fixed list, read through the same getters (and defaults) the page
    // uses. signing/keyPath is written as withheld by the renderer whatever
    // arrives here; what arrives is whether a file is chosen at all.
    auto setting = [&facts](const char* key, const QString& value) {
        facts.settings.emplace_back(key, value.toStdString());
    };
    auto flag = [](bool on) { return on ? QStringLiteral("yes") : QStringLiteral("no"); };
    setting("output/pinned", pinned());
    setting("output/endpoint", preferredEndpoint());
    setting("output/nullSinkName", nullSinkName());
    setting("codec/lowLatency", flag(lowLatency()));
    setting("codec/bypass", flag(bypassCodec()));
    setting("codec/splitStereo", flag(splitStereo()));
    setting("codec/bitrate", QString::number(bitrate()));
    setting("appearance/theme", theme());
    setting("appearance/palette", palette());
    setting("appearance/roomView", roomView());
    setting("appearance/roomLayout", roomLayout());
    setting("behaviour/keepRunningWhenClosed", flag(keepRunningWhenClosed()));
    setting("behaviour/moveDefaultOnLaunch", flag(moveDefaultOnLaunch()));
    setting("behaviour/showBackgroundApps", flag(showBackgroundApps()));
    setting("behaviour/showSilentApps", flag(showSilentApps()));
    setting("driver/dir", driverDir());
    setting("signing/keyPath", keyPath().isEmpty() ? QStringLiteral("none") : QStringLiteral("a file is chosen"));
    return facts;
}

QString CrucibleController::diagnosticsReport() const {
    const auto status = engine_ ? engine_->status() : ac3::crucible::EngineStatus{};
    return QString::fromStdString(ac3::crucible::render_report(build_report_facts(), status, log_, secrets()));
}

QString CrucibleController::suggestedDiagnosticsFile() const {
    QString folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (folder.isEmpty()) {
        folder = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    }
    const QString name = QStringLiteral("crucible-diagnostics-") +
                         QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")) +
                         QStringLiteral(".txt");
    return QUrl::fromLocalFile(QDir(folder).filePath(name)).toString();
}

bool CrucibleController::exportDiagnostics(const QString& fileUrl) {
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
        log_.note("diagnostics export failed: " + file.errorString().toStdString());
    }
    emit diagnosticsChanged();
    return ok;
}

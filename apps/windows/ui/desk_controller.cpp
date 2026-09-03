#include "desk_controller.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <string>

#include "output_policy.hpp"
#include "platform/windows/default_device.hpp"
#include "platform/windows/driver_tools.hpp"

namespace {

using ac3::windemo::OutputMode;

constexpr int kPollMs = 120;

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
        case OutputMode::kAtmos: return DeskController::tr("Atmos");
        case OutputMode::kDdPlus51: return DeskController::tr("Dolby Digital Plus 5.1");
        case OutputMode::kDd51: return DeskController::tr("Dolby Digital 5.1");
        case OutputMode::kPcmSurround: return DeskController::tr("PCM surround");
        case OutputMode::kHeadphones: return DeskController::tr("Headphones");
        case OutputMode::kStereo: return DeskController::tr("Stereo");
        case OutputMode::kNone: return DeskController::tr("No output");
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

DeskController::DeskController(QObject* parent)
    : QObject(parent), settings_(QStringLiteral("ac3forge"), QStringLiteral("DesktopAtmos")) {
    poll_timer_.setInterval(kPollMs);
    connect(&poll_timer_, &QTimer::timeout, this, &DeskController::poll);
    driver_timer_.setInterval(250);
    connect(&driver_timer_, &QTimer::timeout, this, &DeskController::poll_driver);
    previous_default_id_ = ac3::windemo::default_render_id();
    refreshDefault();
    refreshDriver();
}

DeskController::~DeskController() {
    stop();
}

ac3::windemo::EngineConfig DeskController::engine_config() const {
    ac3::windemo::EngineConfig config;
    config.null_sink_substring = nullSinkName().toStdString();
    config.signing_key_path = keyPath().toStdString();
    config.low_latency = lowLatency();
    config.bitrate_kbps = static_cast<std::uint32_t>(std::max(0, bitrate()));
    config.pinned = mode_from_key(pinned());
    return config;
}

void DeskController::start() {
    if (engine_) {
        return;
    }
    engine_ = std::make_unique<ac3::windemo::Engine>(engine_config());
    if (const auto started = engine_->start(); !started) {
        last_error_ = from_utf8(started.error());
        engine_.reset();
        emit stateChanged();
        return;
    }
    running_ = true;
    poll_timer_.start();
    emit stateChanged();
}

void DeskController::stop() {
    poll_timer_.stop();
    if (engine_) {
        engine_->stop();
        engine_.reset();
    }
    if (running_) {
        running_ = false;
        emit stateChanged();
    }
}

void DeskController::restart_engine() {
    if (!engine_) {
        return;
    }
    stop();
    start();
}

void DeskController::poll() {
    if (!engine_) {
        return;
    }
    const auto s = engine_->status();

    // Applications: rebuilt every poll (the list is short); the QML side
    // keys its delegates by app id so a rebuild does not reset a drag.
    QVariantList apps;
    int placed = 0;
    int bed = 0;
    for (const auto& app : s.apps) {
        QVariantMap row;
        row.insert(QStringLiteral("app"), static_cast<int>(app.app));
        row.insert(QStringLiteral("name"), display_name(app.name));
        row.insert(QStringLiteral("imagePath"), from_utf8(app.image_path));
        row.insert(QStringLiteral("active"), app.active);
        row.insert(QStringLiteral("tapped"), app.tapped);
        row.insert(QStringLiteral("fullscreen"), app.fullscreen);
        row.insert(QStringLiteral("slot"), app.slot ? *app.slot : -1);
        row.insert(QStringLiteral("x"), app.position.x);
        row.insert(QStringLiteral("y"), app.position.y);
        row.insert(QStringLiteral("z"), app.position.z);
        row.insert(QStringLiteral("level"), static_cast<double>(app.level_dbfs));
        apps.push_back(row);
        (app.slot ? placed : bed) += 1;
    }
    if (apps != apps_ || placed != placed_ || bed != bed_) {
        apps_ = std::move(apps);
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
    if (mode_name != mode_name_ || key != mode_key_ || endpoint != endpoint_name_ ||
        reason != output_reason_ || signing != signing_status_ ||
        s.objects_enabled != objects_enabled_ || error != last_error_ ||
        static_cast<int>(s.tap_channels) != tap_channels_) {
        tap_channels_ = static_cast<int>(s.tap_channels);
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

QString DeskController::pinned() const {
    return settings_.value(QStringLiteral("output/pinned"), QStringLiteral("auto")).toString();
}

void DeskController::setPinned(const QString& mode) {
    if (mode == pinned()) {
        return;
    }
    settings_.setValue(QStringLiteral("output/pinned"), mode);
    if (engine_) {
        engine_->pin(mode_from_key(mode));
    }
    emit settingsChanged();
}

QString DeskController::keyPath() const {
    return settings_.value(QStringLiteral("signing/keyPath")).toString();
}

QString DeskController::nullSinkName() const {
    return settings_.value(QStringLiteral("output/nullSinkName"), QStringLiteral("Desktop Atmos")).toString();
}

void DeskController::setNullSinkName(const QString& name) {
    if (name == nullSinkName()) {
        return;
    }
    settings_.setValue(QStringLiteral("output/nullSinkName"), name);
    emit settingsChanged();
    restart_engine();
    refreshDefault();
}

bool DeskController::lowLatency() const {
    return settings_.value(QStringLiteral("codec/lowLatency"), false).toBool();
}

void DeskController::setLowLatency(bool on) {
    if (on == lowLatency()) {
        return;
    }
    settings_.setValue(QStringLiteral("codec/lowLatency"), on);
    emit settingsChanged();
    restart_engine();
}

int DeskController::bitrate() const {
    return settings_.value(QStringLiteral("codec/bitrate"), 0).toInt();
}

void DeskController::setBitrate(int kbps) {
    if (kbps == bitrate()) {
        return;
    }
    settings_.setValue(QStringLiteral("codec/bitrate"), kbps);
    emit settingsChanged();
    restart_engine();
}

QString DeskController::theme() const {
    return settings_.value(QStringLiteral("appearance/theme"), QStringLiteral("system")).toString();
}

void DeskController::setTheme(const QString& theme) {
    if (theme == this->theme()) {
        return;
    }
    settings_.setValue(QStringLiteral("appearance/theme"), theme);
    emit settingsChanged();
}

QString DeskController::palette() const {
    return settings_.value(QStringLiteral("appearance/palette"), QStringLiteral("signal")).toString();
}

void DeskController::setPalette(const QString& palette) {
    if (palette == this->palette()) {
        return;
    }
    settings_.setValue(QStringLiteral("appearance/palette"), palette);
    emit settingsChanged();
}

bool DeskController::keepRunningWhenClosed() const {
    return settings_.value(QStringLiteral("behaviour/keepRunningWhenClosed"), true).toBool();
}

void DeskController::setKeepRunningWhenClosed(bool on) {
    settings_.setValue(QStringLiteral("behaviour/keepRunningWhenClosed"), on);
    emit settingsChanged();
}

bool DeskController::moveDefaultOnLaunch() const {
    return settings_.value(QStringLiteral("behaviour/moveDefaultOnLaunch"), false).toBool();
}

void DeskController::setMoveDefaultOnLaunch(bool on) {
    settings_.setValue(QStringLiteral("behaviour/moveDefaultOnLaunch"), on);
    emit settingsChanged();
}

// --- commands ---------------------------------------------------------------

void DeskController::position(int app, double x, double y, double z) {
    if (engine_) {
        engine_->position(static_cast<ac3::windemo::AppId>(app),
                          {std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0), std::clamp(z, -1.0, 1.0)});
    }
}

void DeskController::unposition(int app) {
    if (engine_) {
        engine_->unposition(static_cast<ac3::windemo::AppId>(app));
    }
}

void DeskController::reprobe() {
    if (engine_) {
        engine_->reprobe();
    }
    last_endpoint_stamp_ = 0;
    refreshDefault();
}

void DeskController::loadKey(const QString& path) {
    QString local = path;
    const QUrl url(path);
    if (url.isLocalFile()) {
        local = url.toLocalFile();
    }
    settings_.setValue(QStringLiteral("signing/keyPath"), QDir::toNativeSeparators(local));
    if (engine_) {
        engine_->load_signing_key(local.toStdString());
    }
    emit settingsChanged();
}

void DeskController::clearKey() {
    settings_.remove(QStringLiteral("signing/keyPath"));
    if (engine_) {
        engine_->clear_signing_key();
    }
    emit settingsChanged();
}

void DeskController::refreshDefault() {
    const auto endpoints = ac3::windemo::render_endpoints();
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

void DeskController::moveDefaultToNullSink() {
    const auto id = ac3::windemo::find_render_endpoint(nullSinkName().toStdString());
    if (id.empty()) {
        default_message_ = QStringLiteral("No render endpoint named like \"%1\" exists; install the virtual device or name it in Settings.").arg(nullSinkName());
        emit defaultChanged();
        return;
    }
    const std::string current = ac3::windemo::default_render_id();
    if (current != id) {
        previous_default_id_ = current;
    }
    if (const auto ok = ac3::windemo::set_default_render(id); !ok) {
        default_message_ = from_utf8(ok.error());
        ac3::windemo::open_sound_settings();
    } else {
        default_message_.clear();
    }
    refreshDefault();
    reprobe();
}

void DeskController::restoreDefault() {
    if (previous_default_id_.empty()) {
        default_message_ = QStringLiteral("There is no previous default to restore.");
        emit defaultChanged();
        return;
    }
    if (const auto ok = ac3::windemo::set_default_render(previous_default_id_); !ok) {
        default_message_ = from_utf8(ok.error());
        ac3::windemo::open_sound_settings();
    } else {
        default_message_.clear();
    }
    refreshDefault();
    reprobe();
}

void DeskController::openSoundSettings() {
    ac3::windemo::open_sound_settings();
}

// --- the null-sink driver ---------------------------------------------------

QString DeskController::driverDir() const {
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

void DeskController::setDriverDir(const QString& dir) {
    if (dir == driverDir()) {
        return;
    }
    if (dir.trimmed().isEmpty()) {
        settings_.remove(QStringLiteral("driver/dir"));
    } else {
        settings_.setValue(QStringLiteral("driver/dir"), QDir::toNativeSeparators(dir.trimmed()));
    }
    emit settingsChanged();
    refreshDriver();
}

QString DeskController::driver_log_path() const {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(base);
    return QDir::toNativeSeparators(QDir(base).filePath(QStringLiteral("driver-%1.log").arg(driver_verb_)));
}

void DeskController::refreshDriver() {
    const QDir dir(driverDir());
    const bool found = dir.exists(QStringLiteral("install.ps1")) && dir.exists(QStringLiteral("remove.ps1")) &&
                       dir.exists(QStringLiteral("Package/x64/Release/package/Ac3ForgeNullSink.inf"));
    driver_package_found_ = found;
    code_integrity_ = ac3::windemo::code_integrity_state();
    emit driverChanged();
}

void DeskController::run_driver_script(const QString& script, const QString& verb) {
    if (driver_process_.running()) {
        return;
    }
    driver_verb_ = verb;
    const QDir dir(driverDir());
    const QString script_path = QDir::toNativeSeparators(dir.filePath(script));
    const QString package = QDir::toNativeSeparators(dir.filePath(QStringLiteral("Package/x64/Release/package")));
    const QString devcon = dir.exists(QStringLiteral("devcon.exe"))
                               ? QDir::toNativeSeparators(dir.filePath(QStringLiteral("devcon.exe")))
                               : QStringLiteral("devcon.exe");
    const QString log = driver_log_path();
    QFile::remove(log);
    // One -Command so the script's output lands in a transcript this can
    // read back; the window itself is hidden. Paths are single-quoted for
    // PowerShell; the whole command is one argument to powershell.exe.
    const QString command =
        QStringLiteral("Start-Transcript -Path '%1' | Out-Null; try { & '%2' -PackageDir '%3' -Devcon '%4'; $code = 0 } "
                       "catch { Write-Host $_; $code = 1 }; Stop-Transcript | Out-Null; exit $code")
            .arg(log, script_path, package, devcon);
    const QString arguments = QStringLiteral("-NoProfile -ExecutionPolicy Bypass -Command \"%1\"").arg(command);
    auto started = ac3::windemo::ElevatedProcess::start(L"powershell.exe", arguments.toStdWString());
    if (!started) {
        driver_message_ = from_utf8(started.error());
        emit driverChanged();
        return;
    }
    driver_process_ = std::move(*started);
    driver_message_ = verb == QLatin1String("install") ? tr("installing, answer the elevation prompt ...")
                                                        : tr("removing, answer the elevation prompt ...");
    driver_timer_.start();
    emit driverChanged();
}

void DeskController::installDriver() {
    run_driver_script(QStringLiteral("install.ps1"), QStringLiteral("install"));
}

void DeskController::removeDriver() {
    run_driver_script(QStringLiteral("remove.ps1"), QStringLiteral("remove"));
}

void DeskController::poll_driver() {
    const auto code = driver_process_.poll();
    if (!code) {
        return;
    }
    driver_timer_.stop();
    const auto tail = ac3::windemo::transcript_tail(driver_log_path().toStdWString(), 3);
    QStringList lines;
    for (const auto& line : tail) {
        lines.push_back(from_utf8(line));
    }
    const QString detail = lines.isEmpty() ? QString() : QStringLiteral("\n") + lines.join(QStringLiteral("\n"));
    if (*code == 0) {
        driver_message_ = (driver_verb_ == QLatin1String("install") ? tr("installed") : tr("removed")) + detail;
    } else {
        driver_message_ = tr("%1 failed (exit code %2)").arg(driver_verb_).arg(*code) + detail;
    }
    // Endpoints appear a moment after the device does.
    refreshDriver();
    reprobe();
}

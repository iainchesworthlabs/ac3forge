#pragma once

#include <QHash>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QtQmlIntegration>

#include <memory>
#include <string>

#include "app_entry.hpp"
#include "audio_devices.hpp"
#include "engine.hpp"
#include "session_monitor.hpp"
#include "default_device.hpp"
#include <QStringList>

#include "virtual_device.hpp"

#include "diagnostics.hpp"
#include "foreground.hpp"

// The one object QML talks to: the engine's status, republished as
// properties a few times a second, and its commands, as invokables
// (docs/platforms/windows-demo.md, "UI"). Settings persist through
// QSettings under the same organisation as the GUI, so the two apps' theme
// preferences agree; the signing key's PATH is what is stored, never the
// key.

// The first-run explanation's version. A store whose
// firstRun/acknowledgedVersion is at least this has seen the current wording,
// and bumping it shows the dialog once more. The Qt Quick harness seeds it so
// that no suite instantiating the shell meets a modal it did not ask for
// (ui/tests/qml_test_main.cpp); tst_firstrun.qml clears it in its own init().
inline constexpr int kFirstRunVersion = 1;

class CrucibleController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // --- engine state -------------------------------------------------------
    Q_PROPERTY(bool running READ running NOTIFY stateChanged)
    // Live entries, one per application (AppEntry); the list itself
    // changes only when an application appears or leaves.
    Q_PROPERTY(QList<QObject*> apps READ apps NOTIFY appsChanged)
    Q_PROPERTY(QString modeName READ modeName NOTIFY stateChanged)
    Q_PROPERTY(QString modeKey READ modeKey NOTIFY stateChanged)
    Q_PROPERTY(QString endpointName READ endpointName NOTIFY stateChanged)
    Q_PROPERTY(QString outputReason READ outputReason NOTIFY stateChanged)
    Q_PROPERTY(QString signingStatus READ signingStatus NOTIFY stateChanged)
    Q_PROPERTY(bool objectsEnabled READ objectsEnabled NOTIFY stateChanged)
    Q_PROPERTY(double framesEncoded READ framesEncoded NOTIFY statsChanged)
    Q_PROPERTY(double underruns READ underruns NOTIFY statsChanged)
    Q_PROPERTY(double starvedReads READ starvedReads NOTIFY statsChanged)
    Q_PROPERTY(double lastFrameMs READ lastFrameMs NOTIFY statsChanged)
    Q_PROPERTY(double worstFrameMs READ worstFrameMs NOTIFY statsChanged)
    // The encoder's own time for the last frame, as distinct from the loop's cadence.
    Q_PROPERTY(double encodeMs READ encodeMs NOTIFY statsChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY stateChanged)
    Q_PROPERTY(QVariantList endpoints READ endpoints NOTIFY endpointsChanged)
    Q_PROPERTY(int placedCount READ placedCount NOTIFY appsChanged)
    Q_PROPERTY(int bedCount READ bedCount NOTIFY appsChanged)
    Q_PROPERTY(int tapChannels READ tapChannels NOTIFY stateChanged)
    Q_PROPERTY(bool codecBypassed READ codecBypassed NOTIFY stateChanged)
    // The full-screen rule: whether this platform can say what is full-screen
    // and, when it cannot, the engine's one-line reason. Both false/empty
    // while the engine is stopped, so the Room page states the rule until it
    // knows better.
    Q_PROPERTY(bool fullscreenRuleAvailable READ fullscreenRuleAvailable NOTIFY stateChanged)
    Q_PROPERTY(QString fullscreenRuleReason READ fullscreenRuleReason NOTIFY stateChanged)

    // --- settings -----------------------------------------------------------
    Q_PROPERTY(QString pinned READ pinned WRITE setPinned NOTIFY settingsChanged)
    // The endpoint to hear it on, by id; empty for the automatic choice.
    Q_PROPERTY(QString preferredEndpoint READ preferredEndpoint WRITE setPreferredEndpoint NOTIFY settingsChanged)
    Q_PROPERTY(QString keyPath READ keyPath NOTIFY settingsChanged)
    Q_PROPERTY(QString nullSinkName READ nullSinkName WRITE setNullSinkName NOTIFY settingsChanged)
    Q_PROPERTY(bool lowLatency READ lowLatency WRITE setLowLatency NOTIFY settingsChanged)
    Q_PROPERTY(bool bypassCodec READ bypassCodec WRITE setBypassCodec NOTIFY settingsChanged)
    Q_PROPERTY(bool splitStereo READ splitStereo WRITE setSplitStereo NOTIFY settingsChanged)
    Q_PROPERTY(int bitrate READ bitrate WRITE setBitrate NOTIFY settingsChanged)
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY settingsChanged)
    Q_PROPERTY(QString palette READ palette WRITE setPalette NOTIFY settingsChanged)
    Q_PROPERTY(QString roomView READ roomView WRITE setRoomView NOTIFY settingsChanged)  // "2d" or "3d"
    // How large the window's text is: a percentage - "100" (the default,
    // and the size the window is drawn at), "125", "150", "175" - or
    // "system", which takes the point size the platform's theme reports and
    // counts 9 pt as 100%. "system" is not the default because 9 pt is the
    // base size on Windows and several Linux desktops report 10 or 11 with
    // nothing about text size touched, so it would open the window 11 to 22%
    // larger than it is designed for and say nothing about why. Main.qml
    // turns the choice into Theme.fontScale, and every size in the window is
    // a multiple of that (docs/crucible/accessibility.md).
    Q_PROPERTY(QString textScale READ textScale WRITE setTextScale NOTIFY settingsChanged)
    Q_PROPERTY(bool keepRunningWhenClosed READ keepRunningWhenClosed WRITE setKeepRunningWhenClosed NOTIFY settingsChanged)
    // Background processes with audio sessions (no visible window, not a
    // packaged app) are hidden from the rail unless this is on.
    Q_PROPERTY(bool showBackgroundApps READ showBackgroundApps WRITE setShowBackgroundApps NOTIFY settingsChanged)
    // Running applications with no audio session are listed (greyed) so
    // they can be placed before they play; off hides them unless placed.
    Q_PROPERTY(bool showSilentApps READ showSilentApps WRITE setShowSilentApps NOTIFY settingsChanged)
    // How many listed applications have sound right now.
    Q_PROPERTY(int soundingCount READ soundingCount NOTIFY appsChanged)
    // The reference speaker layout the 3D room draws: "auto" (5.1 while
    // the stream is bed-only, 7.1.4 once objects are on), "5.1", "7.1", "7.1.4".
    Q_PROPERTY(QString roomLayout READ roomLayout WRITE setRoomLayout NOTIFY settingsChanged)
    // Version, commit and build target, for About.
    Q_PROPERTY(QString versionDetails READ versionDetails CONSTANT)
    // The third-party notices this build ships - the package's NOTICES.txt,
    // embedded at build time - for About > Licences.
    Q_PROPERTY(QString licenceNotices READ licenceNotices CONSTANT)
    Q_PROPERTY(bool moveDefaultOnLaunch READ moveDefaultOnLaunch WRITE setMoveDefaultOnLaunch NOTIFY settingsChanged)
    // Whether the first-run explanation of what this application does to the
    // sound settings has been seen (FirstRunDialog.qml). Stored as the
    // version of the explanation that was acknowledged
    // (firstRun/acknowledgedVersion), so a change to what it says can show
    // it once more.
    Q_PROPERTY(bool firstRunAcknowledged READ firstRunAcknowledged WRITE setFirstRunAcknowledged NOTIFY settingsChanged)
    // Whether this store was copied from the demo's on the first launch
    // (ui/main.cpp), for the one sentence in the first-run dialog that says so.
    Q_PROPERTY(bool migratedFromDemo READ migratedFromDemo CONSTANT)

    // --- the default output ---------------------------------------------------
    Q_PROPERTY(QString defaultOutputName READ defaultOutputName NOTIFY defaultChanged)
    Q_PROPERTY(bool defaultIsNullSink READ defaultIsNullSink NOTIFY defaultChanged)
    Q_PROPERTY(QString previousDefaultName READ previousDefaultName NOTIFY defaultChanged)
    Q_PROPERTY(bool nullSinkPresent READ nullSinkPresent NOTIFY defaultChanged)
    Q_PROPERTY(QString defaultMessage READ defaultMessage NOTIFY defaultChanged)
    // Whether moving the system default output is part of this platform's
    // model at all (DefaultDevice::moves_default). False where each
    // application is silenced at its tap instead, and the views drop the
    // move, the restore and the silent device with it.
    Q_PROPERTY(bool movesDefault READ movesDefault CONSTANT)

    // --- the null-sink driver (Phase 4) ------------------------------------
    // The folder holding install.ps1, remove.ps1 and the built package; the
    // scripts run elevated (a UAC prompt) with their output in a transcript
    // this reports the tail of.
    Q_PROPERTY(QString driverDir READ driverDir WRITE setDriverDir NOTIFY settingsChanged)
    Q_PROPERTY(bool driverPackageFound READ driverPackageFound NOTIFY driverChanged)
    Q_PROPERTY(bool driverBusy READ driverBusy NOTIFY driverChanged)
    Q_PROPERTY(QString driverMessage READ driverMessage NOTIFY driverChanged)
    // What stands between this machine and a silent device, in the
    // platform's own words: "turn test signing on ..." on Windows, a module
    // load's error on Linux, nothing at all on macOS, which needs no device.
    // Empty when nothing is in the way.
    Q_PROPERTY(QString silentDeviceBlocker READ silentDeviceBlocker NOTIFY driverChanged)
    Q_PROPERTY(QStringList silentDeviceDetail READ silentDeviceDetail NOTIFY driverChanged)
    Q_PROPERTY(bool silentDeviceNeeded READ silentDeviceNeeded NOTIFY driverChanged)
    // How a person gets a silent device on this platform, in one sentence,
    // for the signal path's warning when there is none.
    Q_PROPERTY(QString silentDeviceAdvice READ silentDeviceAdvice CONSTANT)
    // Whether the silent device is installed from a package (a driver, with
    // a folder to point at) or made by this application itself; the
    // settings page shows the driver tools only in the first case.
    Q_PROPERTY(bool silentDeviceFromPackage READ silentDeviceFromPackage CONSTANT)
    // Whether this application can make the silent device itself, right
    // now: true where the device is the application's own and it is not
    // there yet, never where it is an installed package (an elevated install
    // stays an explicit Settings action). Send applications creates it first.
    Q_PROPERTY(bool silentDeviceCanCreate READ silentDeviceCanCreate NOTIFY driverChanged)
    // Whether this build carries the room's 3D view (Qt Quick 3D found at
    // configure time; the page hides its toggle otherwise).
    Q_PROPERTY(bool has3D READ has3D CONSTANT)
    // The outcome of the last diagnostics export, for the Settings page
    // (docs/crucible/troubleshooting.md, "Saving a diagnostics file").
    Q_PROPERTY(QString diagnosticsMessage READ diagnosticsMessage NOTIFY diagnosticsChanged)

public:
    explicit CrucibleController(QObject* parent = nullptr);
    ~CrucibleController() override;

    [[nodiscard]] bool running() const { return running_; }
    [[nodiscard]] QList<QObject*> apps() const { return apps_; }
    [[nodiscard]] QString modeName() const { return mode_name_; }
    [[nodiscard]] QString modeKey() const { return mode_key_; }
    [[nodiscard]] QString endpointName() const { return endpoint_name_; }
    [[nodiscard]] QString outputReason() const { return output_reason_; }
    [[nodiscard]] QString signingStatus() const { return signing_status_; }
    [[nodiscard]] bool objectsEnabled() const { return objects_enabled_; }
    [[nodiscard]] double framesEncoded() const { return frames_; }
    [[nodiscard]] double underruns() const { return underruns_; }
    [[nodiscard]] double starvedReads() const { return starved_; }
    [[nodiscard]] double lastFrameMs() const { return last_frame_ms_; }
    [[nodiscard]] double worstFrameMs() const { return worst_frame_ms_; }
    [[nodiscard]] double encodeMs() const { return encode_ms_; }
    [[nodiscard]] QString lastError() const { return last_error_; }
    [[nodiscard]] QVariantList endpoints() const { return endpoints_; }
    [[nodiscard]] int placedCount() const { return placed_; }
    [[nodiscard]] int bedCount() const { return bed_; }
    [[nodiscard]] int tapChannels() const { return tap_channels_; }
    [[nodiscard]] bool codecBypassed() const { return codec_bypassed_; }
    [[nodiscard]] bool fullscreenRuleAvailable() const { return fullscreen_rule_; }
    [[nodiscard]] QString fullscreenRuleReason() const { return fullscreen_reason_; }

    [[nodiscard]] QString pinned() const;
    void setPinned(const QString& mode);
    [[nodiscard]] QString preferredEndpoint() const;
    void setPreferredEndpoint(const QString& id);
    [[nodiscard]] QString keyPath() const;
    [[nodiscard]] QString nullSinkName() const;
    void setNullSinkName(const QString& name);
    [[nodiscard]] bool lowLatency() const;
    void setLowLatency(bool on);
    [[nodiscard]] bool bypassCodec() const;
    void setBypassCodec(bool on);
    [[nodiscard]] bool splitStereo() const;
    void setSplitStereo(bool on);
    [[nodiscard]] int bitrate() const;
    void setBitrate(int kbps);
    [[nodiscard]] QString theme() const;
    void setTheme(const QString& theme);
    [[nodiscard]] QString palette() const;
    void setPalette(const QString& palette);
    [[nodiscard]] QString roomView() const;
    void setRoomView(const QString& view);
    [[nodiscard]] QString textScale() const;
    void setTextScale(const QString& scale);
    [[nodiscard]] bool keepRunningWhenClosed() const;
    void setKeepRunningWhenClosed(bool on);
    [[nodiscard]] bool moveDefaultOnLaunch() const;
    void setMoveDefaultOnLaunch(bool on);
    [[nodiscard]] bool firstRunAcknowledged() const;
    void setFirstRunAcknowledged(bool on);
    [[nodiscard]] bool migratedFromDemo() const;

    [[nodiscard]] QString defaultOutputName() const { return default_name_; }
    [[nodiscard]] bool defaultIsNullSink() const { return default_is_null_sink_; }
    [[nodiscard]] QString previousDefaultName() const { return previous_default_name_; }
    [[nodiscard]] bool nullSinkPresent() const { return null_sink_present_; }
    [[nodiscard]] QString defaultMessage() const { return default_message_; }
    [[nodiscard]] bool movesDefault() const { return default_device_->moves_default(); }

    [[nodiscard]] QString driverDir() const;
    void setDriverDir(const QString& dir);
    [[nodiscard]] bool driverPackageFound() const { return silent_state_.can_install; }
    [[nodiscard]] bool driverBusy() const { return driver_busy_; }
    [[nodiscard]] QString driverMessage() const { return driver_message_; }
    [[nodiscard]] QString silentDeviceBlocker() const;
    [[nodiscard]] QStringList silentDeviceDetail() const;
    [[nodiscard]] bool silentDeviceNeeded() const { return silent_state_.needed; }
    [[nodiscard]] QString silentDeviceAdvice() const;
    [[nodiscard]] bool silentDeviceFromPackage() const;
    [[nodiscard]] bool silentDeviceCanCreate() const;
    [[nodiscard]] static bool has3D() { return AC3DESK_QUICK3D != 0; }
    // Make any probed endpoint the Windows default output (the same policy
    // call the silent-device switch uses), by its endpoint id.
    Q_INVOKABLE void setDefaultOutput(const QString& id);
    [[nodiscard]] bool showBackgroundApps() const;
    [[nodiscard]] bool showSilentApps() const;
    void setShowSilentApps(bool on);
    [[nodiscard]] int soundingCount() const { return sounding_; }
    void setShowBackgroundApps(bool on);
    [[nodiscard]] QString roomLayout() const;
    void setRoomLayout(const QString& layout);
    [[nodiscard]] QString versionDetails() const;
    [[nodiscard]] QString licenceNotices() const;

    Q_INVOKABLE void installDriver();
    Q_INVOKABLE void removeDriver();
    Q_INVOKABLE void refreshDriver();

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    // stop(), then put the default output back where this application moved
    // it from, then end the process: what the tray's Quit and the window's
    // close do when it is not staying in the tray. stop() alone never
    // touches the default, so the test suites can call it freely.
    Q_INVOKABLE void quit();
    Q_INVOKABLE void position(int app, double x, double y, double z);
    Q_INVOKABLE void unposition(int app);
    Q_INVOKABLE void setSplit(int app, bool split);
    // One object of a split pair (side 0 left, 1 right) placed on its own,
    // and the way back to the standard spread.
    Q_INVOKABLE void positionSide(int app, int side, double x, double y, double z);
    Q_INVOKABLE void resetPair(int app);
    Q_INVOKABLE void setSize(int app, double size);
    Q_INVOKABLE void reprobe();
    Q_INVOKABLE void loadKey(const QString& path);
    Q_INVOKABLE void clearKey();
    Q_INVOKABLE void moveDefaultToNullSink();
    Q_INVOKABLE void restoreDefault();
    Q_INVOKABLE void openSoundSettings();
    Q_INVOKABLE void refreshDefault();
    // One line into the diagnostics ring, from the window. The shell's
    // announcer calls it for every state change it reads out, so a
    // diagnostics file carries what the window SAID as well as what the
    // engine did, and the two can be lined up (Main.qml, "what the window
    // says out loud").
    Q_INVOKABLE void note(const QString& line);

    // The diagnostics file: the report as text, composed from named facts
    // and never from the signing key, its path or an environment value
    // (diagnostics.hpp says how that is held); a suggested file: URL in the
    // Documents folder; and the export itself, which writes UTF-8 with LF
    // line endings and reports through diagnosticsMessage.
    [[nodiscard]] QString diagnosticsMessage() const { return diagnostics_message_; }
    Q_INVOKABLE QString diagnosticsReport() const;
    Q_INVOKABLE QString suggestedDiagnosticsFile() const;
    Q_INVOKABLE bool exportDiagnostics(const QString& fileUrl);

    // The five platform seams, replaced wholesale. For the Qt Quick harness
    // alone (ui/tests/qml_test_main.cpp), which scripts a room so the
    // keyboard suites can drive a real controller and a real engine over
    // fake sessions and devices instead of skipping on a machine with no
    // audio session. Deliberately neither Q_INVOKABLE nor a property:
    // nothing in QML and nothing in the shipped window can reach it.
    //
    // A null argument puts that seam back to the platform's own, so five
    // nulls hand the machine back. A suite has to do that before a case that
    // expects the machine's own answers: the default device and the silent
    // device are held here rather than passed to the engine at start(), and
    // movesDefault and silentDeviceFromPackage are CONSTANT properties whose
    // bindings never re-read after a swap.
    void set_test_services(std::shared_ptr<ac3::crucible::SessionMonitor> sessions,
                           std::shared_ptr<ac3::crucible::AudioDevices> devices,
                           std::shared_ptr<ac3::crucible::Foreground> foreground,
                           std::shared_ptr<ac3::crucible::DefaultDevice> default_device,
                           std::shared_ptr<ac3::crucible::VirtualDevice> virtual_device);

signals:
    void stateChanged();
    void appsChanged();
    void statsChanged();
    void endpointsChanged();
    void settingsChanged();
    void defaultChanged();
    void driverChanged();
    void diagnosticsChanged();

private:
    void poll();
    void restart_engine();
    [[nodiscard]] ac3::crucible::EngineConfig engine_config() const;
    void emit_restored_default();
    void poll_driver();
    [[nodiscard]] ac3::crucible::ReportFacts build_report_facts() const;
    [[nodiscard]] ac3::crucible::Secrets secrets() const;
    // The restore behind quit() and QCoreApplication::aboutToQuit: idempotent
    // through moved_default_by_us_, and it never opens the sound settings on
    // a refusal (a window opening as the application exits is worse than a
    // logged failure).
    void restore_on_quit();

    // The machine, behind the engine's seams: the system default output
    // and the silent device applications play into. Both are resolved
    // from platform_services.hpp, so nothing in this file names an
    // operating system (docs/crucible/promotion.md, Phase 2).
    std::shared_ptr<ac3::crucible::DefaultDevice> default_device_;
    QSettings settings_;
    std::unique_ptr<ac3::crucible::Engine> engine_;
    QTimer poll_timer_;

    bool running_ = false;
    QList<QObject*> apps_;  // AppEntry*, owned by this
    QHash<int, QObject*> entries_;  // by app id
    QString mode_name_, mode_key_, endpoint_name_, output_reason_, signing_status_, last_error_;
    bool objects_enabled_ = false;
    double frames_ = 0, underruns_ = 0, starved_ = 0, last_frame_ms_ = 0, worst_frame_ms_ = 0, encode_ms_ = 0;
    QVariantList endpoints_;
    int placed_ = 0, bed_ = 0, sounding_ = 0;
    int tap_channels_ = 0;
    bool codec_bypassed_ = false;
    bool fullscreen_rule_ = false;
    QString fullscreen_reason_;

    QString default_name_, previous_default_name_, default_message_;
    std::string previous_default_id_;
    bool default_is_null_sink_ = false;
    bool null_sink_present_ = false;
    // Whether this application moved the default to the silent device
    // itself, so quit puts back only what it changed.
    bool moved_default_by_us_ = false;
    std::uint64_t last_endpoint_stamp_ = 0;

    std::shared_ptr<ac3::crucible::VirtualDevice> virtual_device_;
    QTimer driver_timer_;
    ac3::crucible::SilentDeviceState silent_state_;
    bool driver_busy_ = false;
    QString driver_message_;
    QString driver_verb_;

    // The process-wide note ring (diagnostics.hpp) the engine, the window's
    // message handler and this controller share: a reference, since it
    // outlives everything including this object. The full-screen seam is
    // held for its support() line in the report only.
    ac3::crucible::DiagnosticLog& log_;
    std::shared_ptr<ac3::crucible::Foreground> foreground_;
    QString diagnostics_message_;

    // Set only by set_test_services(); null in the shipped window, which is
    // what makes engine_config() ask the platform for each of them.
    std::shared_ptr<ac3::crucible::SessionMonitor> test_sessions_;
    std::shared_ptr<ac3::crucible::AudioDevices> test_devices_;
    std::shared_ptr<ac3::crucible::Foreground> test_foreground_;
};

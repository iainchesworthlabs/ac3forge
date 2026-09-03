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
#include "engine.hpp"
#include "platform/windows/driver_tools.hpp"

// The one object QML talks to: the engine's status, republished as
// properties a few times a second, and its commands, as invokables
// (docs/platforms/windows-demo.md, "UI"). Settings persist through
// QSettings under the same organisation as the GUI, so the two apps' theme
// preferences agree; the signing key's PATH is what is stored, never the
// key.

class DeskController : public QObject {
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

    // --- settings -----------------------------------------------------------
    Q_PROPERTY(QString pinned READ pinned WRITE setPinned NOTIFY settingsChanged)
    Q_PROPERTY(QString keyPath READ keyPath NOTIFY settingsChanged)
    Q_PROPERTY(QString nullSinkName READ nullSinkName WRITE setNullSinkName NOTIFY settingsChanged)
    Q_PROPERTY(bool lowLatency READ lowLatency WRITE setLowLatency NOTIFY settingsChanged)
    Q_PROPERTY(bool bypassCodec READ bypassCodec WRITE setBypassCodec NOTIFY settingsChanged)
    Q_PROPERTY(bool splitStereo READ splitStereo WRITE setSplitStereo NOTIFY settingsChanged)
    Q_PROPERTY(int bitrate READ bitrate WRITE setBitrate NOTIFY settingsChanged)
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY settingsChanged)
    Q_PROPERTY(QString palette READ palette WRITE setPalette NOTIFY settingsChanged)
    Q_PROPERTY(QString roomView READ roomView WRITE setRoomView NOTIFY settingsChanged)  // "2d" or "3d"
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
    Q_PROPERTY(bool moveDefaultOnLaunch READ moveDefaultOnLaunch WRITE setMoveDefaultOnLaunch NOTIFY settingsChanged)

    // --- the default output ---------------------------------------------------
    Q_PROPERTY(QString defaultOutputName READ defaultOutputName NOTIFY defaultChanged)
    Q_PROPERTY(bool defaultIsNullSink READ defaultIsNullSink NOTIFY defaultChanged)
    Q_PROPERTY(QString previousDefaultName READ previousDefaultName NOTIFY defaultChanged)
    Q_PROPERTY(bool nullSinkPresent READ nullSinkPresent NOTIFY defaultChanged)
    Q_PROPERTY(QString defaultMessage READ defaultMessage NOTIFY defaultChanged)

    // --- the null-sink driver (Phase 4) ------------------------------------
    // The folder holding install.ps1, remove.ps1 and the built package; the
    // scripts run elevated (a UAC prompt) with their output in a transcript
    // this reports the tail of.
    Q_PROPERTY(QString driverDir READ driverDir WRITE setDriverDir NOTIFY settingsChanged)
    Q_PROPERTY(bool driverPackageFound READ driverPackageFound NOTIFY driverChanged)
    Q_PROPERTY(bool driverBusy READ driverBusy NOTIFY driverChanged)
    Q_PROPERTY(QString driverMessage READ driverMessage NOTIFY driverChanged)
    Q_PROPERTY(bool testSigningOn READ testSigningOn NOTIFY driverChanged)
    Q_PROPERTY(bool memoryIntegrityOn READ memoryIntegrityOn NOTIFY driverChanged)
    Q_PROPERTY(bool codeIntegrityKnown READ codeIntegrityKnown NOTIFY driverChanged)
    // Whether this build carries the room's 3D view (Qt Quick 3D found at
    // configure time; the page hides its toggle otherwise).
    Q_PROPERTY(bool has3D READ has3D CONSTANT)

public:
    explicit DeskController(QObject* parent = nullptr);
    ~DeskController() override;

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

    [[nodiscard]] QString pinned() const;
    void setPinned(const QString& mode);
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
    [[nodiscard]] bool keepRunningWhenClosed() const;
    void setKeepRunningWhenClosed(bool on);
    [[nodiscard]] bool moveDefaultOnLaunch() const;
    void setMoveDefaultOnLaunch(bool on);

    [[nodiscard]] QString defaultOutputName() const { return default_name_; }
    [[nodiscard]] bool defaultIsNullSink() const { return default_is_null_sink_; }
    [[nodiscard]] QString previousDefaultName() const { return previous_default_name_; }
    [[nodiscard]] bool nullSinkPresent() const { return null_sink_present_; }
    [[nodiscard]] QString defaultMessage() const { return default_message_; }

    [[nodiscard]] QString driverDir() const;
    void setDriverDir(const QString& dir);
    [[nodiscard]] bool driverPackageFound() const { return driver_package_found_; }
    [[nodiscard]] bool driverBusy() const { return driver_process_.running(); }
    [[nodiscard]] QString driverMessage() const { return driver_message_; }
    [[nodiscard]] bool testSigningOn() const { return code_integrity_.test_signing; }
    [[nodiscard]] bool memoryIntegrityOn() const { return code_integrity_.hvci; }
    [[nodiscard]] bool codeIntegrityKnown() const { return code_integrity_.known; }
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

    Q_INVOKABLE void installDriver();
    Q_INVOKABLE void removeDriver();
    Q_INVOKABLE void refreshDriver();

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
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

signals:
    void stateChanged();
    void appsChanged();
    void statsChanged();
    void endpointsChanged();
    void settingsChanged();
    void defaultChanged();
    void driverChanged();

private:
    void poll();
    void restart_engine();
    [[nodiscard]] ac3::windemo::EngineConfig engine_config() const;
    void emit_restored_default();
    void run_driver_script(const QString& script, const QString& verb);
    void poll_driver();
    [[nodiscard]] QString driver_log_path() const;

    QSettings settings_;
    std::unique_ptr<ac3::windemo::Engine> engine_;
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

    QString default_name_, previous_default_name_, default_message_;
    std::string previous_default_id_;
    bool default_is_null_sink_ = false;
    bool null_sink_present_ = false;
    std::uint64_t last_endpoint_stamp_ = 0;

    ac3::windemo::ElevatedProcess driver_process_;
    QTimer driver_timer_;
    ac3::windemo::CodeIntegrityState code_integrity_;
    bool driver_package_found_ = false;
    QString driver_message_;
    QString driver_verb_;
};

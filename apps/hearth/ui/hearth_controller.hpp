#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QtQmlIntegration>

#include <memory>

#include "diagnostic_log.hpp"

namespace ac3::hearth {
class Engine;
class SettingsStore;
class PairingStore;
}

// The one object QML talks to for the queue and the transport
// (planning/hearth-reference-player.md, A5: "a controller polling the
// engine's snapshot"). Everything it shows comes from Engine::status(),
// read on a timer at CrucibleController's own rate (kPollMs) rather than
// from Engine::on_change(), which runs on the engine's own thread and would
// have to hop back to the GUI thread to touch a single Qt property - polling
// does that hop once for the whole snapshot instead.
//
// This first slice owns the engine directly, with a device PCM sink
// (apps/hearth/engine/pcm_sink.hpp) and a loader that reads raw
// `.ac3`/`.ec3` files (item_loader.hpp). The output picker, the speaker
// layout and the decoder settings pages are not built yet, so the sink is
// the platform's default device and the layout is a fixed "2.0" until they
// are.

namespace ac3::hearth::ui {

class HearthController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // --- about ------------------------------------------------------------
    // Version, commit and build target, for About (ac3::version_details()).
    Q_PROPERTY(QString versionDetails READ versionDetails CONSTANT)
    // The third-party notices this build ships - the package's NOTICES.txt,
    // embedded at build time - for About > Licences.
    Q_PROPERTY(QString licenceNotices READ licenceNotices CONSTANT)

    // --- queue and transport --------------------------------------------
    // Each entry: path, title, playable (bool), note (why not, or a
    // decode-time remark), durationMs, channels, sampleRate, hasObjects,
    // streamKind ("AC-3", "E-AC-3" or "" before the item has been probed).
    Q_PROPERTY(QVariantList queue READ queue NOTIFY queueChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY queueChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
    Q_PROPERTY(bool gapless READ gapless WRITE setGapless NOTIFY stateChanged)
    // The output decision's own sentence (EngineStatus::output_reason), and
    // the latest note/error the transport or an item had to say.
    Q_PROPERTY(QString outputReason READ outputReason NOTIFY stateChanged)
    Q_PROPERTY(QString noteText READ noteText NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    // Whether FirstRunDialog.qml has been dismissed once already. Persisted
    // through QSettings under organisation "ac3forge", application "Hearth"
    // (set in main.cpp) - this window's only settings storage so far. The
    // queue/decoder/speaker settings apps/hearth/engine/settings_model.hpp
    // describes are a separate, later piece (the Settings page proper), not
    // wired to this controller yet.
    Q_PROPERTY(bool firstRunSeen READ firstRunSeen WRITE setFirstRunSeen NOTIFY firstRunSeenChanged)

    // --- position (the transport bar's scrubber) -------------------------
    // Where the item playing now has got to - Engine::position(), read apart
    // from status() so a poll sixteen times a second does not copy the whole
    // queue with it (that method's own comment). Both zero with nothing
    // current to play.
    Q_PROPERTY(qlonglong positionMs READ positionMs NOTIFY positionChanged)
    Q_PROPERTY(qlonglong durationMs READ durationMs NOTIFY positionChanged)

    // --- decoder settings (the Decoder page, AC-3 and E-AC-3) ------------
    // The whole of DecoderSettings, as one map QML reads field by field and
    // writes back through setDecoderSettings() - see that method's own
    // comment for the field names. Not every control the design shows has a
    // field here yet: the JOC domain/fast-inverse-transform switches are
    // library-level settings this app does not carry a knob for yet, so the
    // page shows them inactive.
    Q_PROPERTY(QVariantMap decoderSettings READ decoderSettings NOTIFY decoderSettingsChanged)

    // --- speaker setup (the Speakers page) -------------------------------
    // One entry per render layout slot (0 is always this engine's first
    // coded-channel slot; the routing patch below may send it to any
    // device output). Sized and ordered to match speakerLabels.
    Q_PROPERTY(QVariantList trimDb READ trimDb NOTIFY speakerSetupChanged)
    Q_PROPERTY(QVariantList delayMs READ delayMs NOTIFY speakerSetupChanged)
    Q_PROPERTY(double crossoverHz READ crossoverHz NOTIFY speakerSetupChanged)
    // routing[slot] is the device output that slot is patched to, or -1 for
    // unpatched (render::Routing::kUnassigned) - what the Speakers page's
    // routing grid draws one radio button per (slot, output) pair from.
    Q_PROPERTY(QVariantList routing READ routing NOTIFY speakerSetupChanged)
    Q_PROPERTY(int routingOutputs READ routingOutputs NOTIFY speakerSetupChanged)
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY speakerSetupChanged)
    // Each render layout slot's own speaker name ("L", "C", "LFE", ...),
    // from the layout this engine was built with - fixed for this slice
    // (Player::layout()'s own comment says why there is no live layout
    // change yet). NOTIFY, not CONSTANT, despite being fixed once set:
    // QML reads this property while building the page tree, which happens
    // before start() has posted anything to the engine thread, let alone
    // before its first status has come back - CONSTANT would tell the
    // binding engine to cache that first, empty read forever.
    Q_PROPERTY(QStringList speakerLabels READ speakerLabels NOTIFY speakerSetupChanged)
    // Each slot's own render::Speaker::small - whether its bass is
    // redirected to the LFE feed rather than reproduced there. Baked into
    // the fixed layout the same as speakerLabels, so read-only here: there
    // is no per-speaker size control in this slice, only the crossover
    // corner those small speakers share (crossoverHz). Same NOTIFY, same
    // reason as speakerLabels.
    Q_PROPERTY(QVariantList speakerSmall READ speakerSmall NOTIFY speakerSetupChanged)
    // The identify tone's IDENTIFY card: the pink-noise level every session
    // plays at (the design offers -30/-20/-12 dB; render::IdentifyTone's
    // own range is wider) and which speakerLabels slot is currently
    // sounding it, or -1 for none - the same index space as trimDb/
    // speakerLabels, not a device output.
    Q_PROPERTY(double identifyLevelDb READ identifyLevelDb NOTIFY speakerSetupChanged)
    Q_PROPERTY(int identifySlot READ identifySlot NOTIFY speakerSetupChanged)

    // --- settings (the Settings page) ------------------------------------
    // Playback and network are ac3::hearth::EngineSettings, kept through a
    // SettingsStore this controller implements over QSettings
    // (hearth_controller.cpp's own QSettingsStore) - the way
    // apps/hearth/engine/settings_model.hpp says the window has to. Read
    // fresh from the store on every call rather than cached here as
    // EngineSettings by value, for the same reason decoderSettings() above
    // takes a fresh read rather than a cached DecoderSettings: caching the
    // type by value would need settings_model.hpp in this header, which
    // pulls in engine_thread.hpp and so ac3::render::OutputLayout, ahead of
    // this header's own Qt includes - see hearth_controller.cpp's #undef
    // slots for what that collision does.
    Q_PROPERTY(bool resumeQueue READ resumeQueue WRITE setResumeQueue NOTIFY settingsChanged)
    // "skip" or "stop" (ac3::hearth::FailurePolicy).
    Q_PROPERTY(QString onFailure READ onFailure WRITE setOnFailure NOTIFY settingsChanged)
    Q_PROPERTY(QString networkName READ networkName WRITE setNetworkName NOTIFY settingsChanged)
    Q_PROPERTY(bool networkDiscover READ networkDiscover WRITE setNetworkDiscover NOTIFY settingsChanged)
    // Each entry: id (the pairing record's client key, in hex - what
    // forgetPairing() takes back), name, pairedOn. Empty until a Sendspin
    // server actually pairs a client (A6); the store and this page are real
    // now, so nothing here has to change when that server lands.
    Q_PROPERTY(QVariantList pairingRecords READ pairingRecords NOTIFY pairingChanged)

    // --- appearance --------------------------------------------------------
    // Window-level, not part of EngineSettings: kept through the same
    // QSettings this controller already opens, under "appearance/" rather
    // than through the engine's SettingsStore. "system"/"light"/"dark",
    // the palette name, and "100"/"125"/"150"/"175"/"system" - Main.qml
    // writes these straight into Theme.preference/Theme.paletteChoice/
    // Theme.fontScale, the same trio apps/crucible/ui/crucible_controller.hpp
    // exposes for the same reason, so the two windows' Settings pages behave
    // alike.
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY settingsChanged)
    Q_PROPERTY(QString palette READ palette WRITE setPalette NOTIFY settingsChanged)
    Q_PROPERTY(QString textScale READ textScale WRITE setTextScale NOTIFY settingsChanged)

    // The outcome of the last diagnostics export (the Settings page's "Save
    // diagnostics").
    Q_PROPERTY(QString diagnosticsMessage READ diagnosticsMessage NOTIFY diagnosticsChanged)

public:
    explicit HearthController(QObject* parent = nullptr);
    ~HearthController() override;

    // Starts the engine thread. Called from Main.qml's Component.onCompleted
    // - not the constructor, so a singleton QML creates before the window is
    // on screen does not open a device with nothing yet shown for it.
    Q_INVOKABLE void start();

    [[nodiscard]] QString versionDetails() const;
    [[nodiscard]] QString licenceNotices() const;

    [[nodiscard]] QVariantList queue() const { return queue_; }
    [[nodiscard]] int currentIndex() const { return current_index_; }
    [[nodiscard]] QString state() const { return state_; }
    [[nodiscard]] bool playing() const { return state_ == QStringLiteral("playing"); }
    [[nodiscard]] bool gapless() const { return gapless_; }
    void setGapless(bool on);
    [[nodiscard]] QString outputReason() const { return output_reason_; }
    [[nodiscard]] QString noteText() const { return note_; }
    [[nodiscard]] QString errorText() const { return error_; }
    [[nodiscard]] bool firstRunSeen() const;
    void setFirstRunSeen(bool seen);

    [[nodiscard]] qlonglong positionMs() const { return position_ms_; }
    [[nodiscard]] qlonglong durationMs() const { return duration_ms_; }

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    // Jumps the item playing now to `ms` from its start, clamped to it.
    // Legal whatever the transport state, and does not itself start or stop
    // playback (Player::seek()'s own comment).
    Q_INVOKABLE void seek(qlonglong ms);
    Q_INVOKABLE void playItem(int index);
    Q_INVOKABLE void removeAt(int index);
    // Each path becomes one queue item, titled by its file name.
    Q_INVOKABLE void addFiles(const QStringList& paths);
    // Every media file item_loader.hpp's list_folder_items() finds under
    // `path` (recursively), added the same way addFiles() adds a file
    // picked directly - including a container list_folder_items() lists but
    // make_file_item_loader() cannot yet open, which lands in the queue
    // unplayable with a reason, same as addFiles() already does for one.
    Q_INVOKABLE void addFolder(const QString& path);

    [[nodiscard]] QVariantMap decoderSettings() const { return decoder_settings_; }
    // Rebuilds a DecoderSettings from `settings` (every key
    // decoderSettings() reads back) and posts it whole, the way a settings
    // page's "apply what changed" always does here - Engine::
    // set_decoder_settings() takes the whole struct in any case. Unknown or
    // missing keys keep the engine's last-known value for that field.
    Q_INVOKABLE void setDecoderSettings(const QVariantMap& settings);

    [[nodiscard]] QVariantList trimDb() const { return trim_db_; }
    [[nodiscard]] QVariantList delayMs() const { return delay_ms_; }
    [[nodiscard]] double crossoverHz() const { return crossover_hz_; }
    [[nodiscard]] QVariantList routing() const { return routing_; }
    [[nodiscard]] int routingOutputs() const { return routing_outputs_; }
    [[nodiscard]] QString deviceName() const { return device_name_; }
    [[nodiscard]] QStringList speakerLabels() const { return speaker_labels_; }
    [[nodiscard]] QVariantList speakerSmall() const { return speaker_small_; }
    [[nodiscard]] double identifyLevelDb() const { return identify_level_db_; }
    [[nodiscard]] int identifySlot() const { return identify_slot_; }

    Q_INVOKABLE void setTrimDb(int slot, double db);
    Q_INVOKABLE void setDelayMs(int slot, double ms);
    Q_INVOKABLE void setCrossoverHz(double hz);
    // Patches `slot` to `output`, or unpatches it with output < 0. Refused
    // (engine-side, EngineStatus::note says so) for an output another slot
    // already has - swap or clear that one first, matching render::Routing::
    // assign()'s own rule.
    Q_INVOKABLE void setRoutingAssignment(int slot, int output);
    Q_INVOKABLE void clearRouting();
    // Patches each slot to the device's own reported order (identity, one
    // slot per output in slot order) - the routing grid's "Use the device's
    // order" button.
    Q_INVOKABLE void useDeviceOrder();

    Q_INVOKABLE void setIdentifyLevelDb(double db);
    // Starts the identify tone on `slot`, moving it there if another slot
    // was already sounding it. No-op for slot < 0.
    Q_INVOKABLE void startIdentify(int slot);
    Q_INVOKABLE void stopIdentify();

    [[nodiscard]] bool resumeQueue() const;
    void setResumeQueue(bool on);
    [[nodiscard]] QString onFailure() const;
    void setOnFailure(const QString& policy);
    [[nodiscard]] QString networkName() const;
    void setNetworkName(const QString& name);
    [[nodiscard]] bool networkDiscover() const;
    void setNetworkDiscover(bool on);
    [[nodiscard]] QVariantList pairingRecords() const;
    // Forgets the pairing record whose id is `id` (pairingRecords()' own
    // "id" field): the sink or player has to pair again, with a new code.
    // Silently does nothing for an id that is not a well-formed record key,
    // which covers a stale id from a row the list has already dropped.
    Q_INVOKABLE void forgetPairing(const QString& id);

    [[nodiscard]] QString theme() const;
    void setTheme(const QString& theme);
    [[nodiscard]] QString palette() const;
    void setPalette(const QString& palette);
    [[nodiscard]] QString textScale() const;
    void setTextScale(const QString& scale);

    // The diagnostics file: the report as text, composed from named facts
    // and never from a pairing key, a pairing code or a queued item's path
    // (diagnostics_report.hpp says how that is held); a suggested file: URL
    // in the Documents folder; and the export itself, which writes UTF-8
    // with LF line endings and reports through diagnosticsMessage - the same
    // three-invokable shape apps/crucible/ui/crucible_controller.hpp uses.
    [[nodiscard]] QString diagnosticsMessage() const { return diagnostics_message_; }
    Q_INVOKABLE QString diagnosticsReport() const;
    Q_INVOKABLE QString suggestedDiagnosticsFile() const;
    Q_INVOKABLE bool exportDiagnostics(const QString& fileUrl);

signals:
    void queueChanged();
    void stateChanged();
    void positionChanged();
    void decoderSettingsChanged();
    void speakerSetupChanged();
    void firstRunSeenChanged();
    void settingsChanged();
    void pairingChanged();
    void diagnosticsChanged();

private:
    void poll();
    // Saves the settings and, while resumeQueue is on, the queue and its
    // play position - connected to QCoreApplication::aboutToQuit, since a
    // play position changes on every pump and has nowhere sensible to save
    // from on every one of them. A hard kill loses whatever this would have
    // written, the same trade every setting here already makes by calling
    // sync() only on a change rather than continuously.
    void save_on_quit();

    std::unique_ptr<ac3::hearth::Engine> engine_;
    QTimer poll_timer_;

    // The process-wide note ring the engine and this controller share -
    // given to the engine in start() so a diagnostics export carries what it
    // did, not just what this controller did. Declared before the settings
    // members below: it does not depend on them, and the constructor's
    // initialiser list has to follow this declaration order regardless.
    ac3::hearth::DiagnosticLog& log_;
    // The four-argument constructor: the two-argument one always uses the
    // native store (the registry here) whatever QSettings::setDefaultFormat
    // says, which would let a QML test suite read and write the developer's
    // own settings - apps/crucible/ui/crucible_controller.cpp's own
    // constructor carries the identical comment for the identical reason.
    QSettings settings_;
    // Implements ac3::hearth::SettingsStore over settings_
    // (hearth_controller.cpp's QSettingsStore); held through the base class
    // so this header never needs settings_model.hpp's full definition.
    // Declared after settings_ and before pairing_: both depend on the one
    // before them, in this order.
    std::unique_ptr<ac3::hearth::SettingsStore> store_;
    std::unique_ptr<ac3::hearth::PairingStore> pairing_;
    QString diagnostics_message_;

    QVariantList queue_;
    int current_index_ = -1;
    QString state_ = QStringLiteral("stopped");
    bool gapless_ = true;
    QString output_reason_;
    QString note_;
    QString error_;

    qlonglong position_ms_ = 0;
    qlonglong duration_ms_ = 0;

    QVariantMap decoder_settings_;

    QVariantList trim_db_;
    QVariantList delay_ms_;
    double crossover_hz_ = 80.0;
    QVariantList routing_;
    int routing_outputs_ = 0;
    QString device_name_;
    QStringList speaker_labels_;
    QVariantList speaker_small_;
    // -20.0 here mirrors render::IdentifyTone::kDefaultLevelDb without this
    // header needing that include - see setDecoderSettings()'s own comment
    // on why ac3::render stays out of this file. Overwritten by the first
    // poll() regardless, the way speakerLabels' own comment explains.
    double identify_level_db_ = -20.0;
    int identify_slot_ = -1;
};

}  // namespace ac3::hearth::ui

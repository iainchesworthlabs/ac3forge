#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QtQmlIntegration>

#include <memory>

namespace ac3::hearth {
class Engine;
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
// `.ac3`/`.ec3` files (item_loader.hpp). The layout is a fixed "2.0" until
// the Speakers page grows a layout picker of its own (its speakerLabels
// property's own comment says why). The engine is given every render
// endpoint to decide between (EngineOutputs::endpoints,
// apps/hearth/engine/output_selector.hpp) rather than one fixed sink, so the
// output picker's "Play here" can name an endpoint - no passthrough sink is
// given yet, so every item still decodes to PCM, on the platform's default
// device until a picker row is chosen.

namespace ac3::hearth::ui {

class HearthController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

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

    // --- decoder settings (the Decoder page, AC-3 and E-AC-3) ------------
    // The whole of DecoderSettings, as one map QML reads field by field and
    // writes back through setDecoderSettings() - see that method's own
    // comment for the field names. Not every control the design shows has a
    // field here yet: rf_ceiling (OutputConfig's, not DecoderSettings')
    // and the JOC domain/fast-inverse-transform switches are library-level
    // settings this app does not carry a knob for yet, so the page shows
    // them inactive.
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
    // The open device's own endpoint id - what the output picker (A5's
    // output-picker dialog) compares each row's own id against to say which
    // one is "playing here", rather than matching on the name, which two
    // distinct endpoints can share.
    Q_PROPERTY(QString currentDeviceId READ currentDeviceId NOTIFY speakerSetupChanged)
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

    // --- output picker (Main.qml's header, OutputPicker.qml) -------------
    // This machine's own render endpoints, refreshed on request rather than
    // polled: refreshOutputDevices() is what the dialog calls when it opens,
    // since enumerating them can probe each one and is not free enough to
    // read on every poll() tick. Each entry: id, name, isDefault (bool),
    // channels (int, 0 for "not reported"), speakers (the mask's speaker
    // names, "" for "not reported"), sampleRates (a list of Hz, empty for
    // "not reported"), supportsAc3, supportsEac3 (bool, ac3::audio::
    // RenderDeviceInfo's own probe of IEC 61937 passthrough in exclusive
    // mode - read here, not acted on: no passthrough sink is wired into this
    // engine yet).
    Q_PROPERTY(QVariantList outputDevices READ outputDevices NOTIFY outputDevicesChanged)

public:
    explicit HearthController(QObject* parent = nullptr);
    ~HearthController() override;

    // Starts the engine thread. Called from Main.qml's Component.onCompleted
    // - not the constructor, so a singleton QML creates before the window is
    // on screen does not open a device with nothing yet shown for it.
    Q_INVOKABLE void start();

    [[nodiscard]] QVariantList queue() const { return queue_; }
    [[nodiscard]] int currentIndex() const { return current_index_; }
    [[nodiscard]] QString state() const { return state_; }
    [[nodiscard]] bool playing() const { return state_ == QStringLiteral("playing"); }
    [[nodiscard]] bool gapless() const { return gapless_; }
    void setGapless(bool on);
    [[nodiscard]] QString outputReason() const { return output_reason_; }
    [[nodiscard]] QString noteText() const { return note_; }
    [[nodiscard]] QString errorText() const { return error_; }

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void playItem(int index);
    Q_INVOKABLE void removeAt(int index);
    // Each path becomes one queue item, titled by its file name.
    Q_INVOKABLE void addFiles(const QStringList& paths);

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
    [[nodiscard]] QString currentDeviceId() const { return device_id_; }
    [[nodiscard]] QStringList speakerLabels() const { return speaker_labels_; }
    [[nodiscard]] QVariantList speakerSmall() const { return speaker_small_; }

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

    [[nodiscard]] QVariantList outputDevices() const { return output_devices_; }
    // Enumerates this machine's render endpoints again and replaces
    // outputDevices() with the result - the dialog's own onOpened. Runs on
    // the calling (GUI) thread and can probe each endpoint in turn
    // (ac3::audio::enumerate_render_devices's own comment), so it is not
    // bound to a poll tick; a dialog open is an occasional, deliberate ask,
    // not a per-frame one.
    Q_INVOKABLE void refreshOutputDevices();
    // Pins playback to `deviceId` (one of outputDevices()'s own "id"
    // fields) and decodes to it - the picker's "Play here" on a "this
    // computer" row. The item playing is decided again and moves there if
    // it is not already (Engine::set_output_preferences's own comment);
    // what changed, or why nothing did, shows up in noteText().
    Q_INVOKABLE void selectOutputDevice(const QString& deviceId);

signals:
    void queueChanged();
    void stateChanged();
    void decoderSettingsChanged();
    void speakerSetupChanged();
    void outputDevicesChanged();

private:
    void poll();

    std::unique_ptr<ac3::hearth::Engine> engine_;
    QTimer poll_timer_;

    QVariantList queue_;
    int current_index_ = -1;
    QString state_ = QStringLiteral("stopped");
    bool gapless_ = true;
    QString output_reason_;
    QString note_;
    QString error_;

    QVariantMap decoder_settings_;

    QVariantList trim_db_;
    QVariantList delay_ms_;
    double crossover_hz_ = 80.0;
    QVariantList routing_;
    int routing_outputs_ = 0;
    QString device_name_;
    QString device_id_;
    QStringList speaker_labels_;
    QVariantList speaker_small_;

    QVariantList output_devices_;
};

}  // namespace ac3::hearth::ui

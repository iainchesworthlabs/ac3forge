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
// `.ac3`/`.ec3` files (item_loader.hpp). The output picker and the decoder
// settings pages are not built yet, so the sink is the platform's default
// device - but the layout is no longer fixed: start() opens at "2.0" and
// the Speakers page's setLayoutText()/setHeights()/setSpeakerSmall() can
// change it from there for the engine's whole life (Player::set_layout()).

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
    // Each render layout slot's own speaker name ("L", "C", "LFE", ...), from
    // the layout currently in effect - recomputed whenever layoutText
    // changes, since setLayout()/setLayoutText() can change what a slot index
    // even means. NOTIFY, not CONSTANT: QML reads this property while
    // building the page tree, before start() has posted anything to the
    // engine thread, let alone before its first status has come back -
    // CONSTANT would tell the binding engine to cache that first, empty read
    // forever, and this can then go on changing for the engine's whole life.
    Q_PROPERTY(QStringList speakerLabels READ speakerLabels NOTIFY speakerSetupChanged)
    // Each slot's own render::Speaker::small - whether its bass is
    // redirected to the LFE feed rather than reproduced there. Read-write:
    // setSpeakerSmall() is the Size column's Large/Small control. Same
    // NOTIFY, same recompute-on-layout-change reason as speakerLabels.
    Q_PROPERTY(QVariantList speakerSmall READ speakerSmall NOTIFY speakerSetupChanged)
    // Each slot's own kind - true where render() never places anything but
    // the bed's LFE (render::Speaker::Kind::kLfe) - what the Size column
    // shows "-" for instead of a Large/Small control, and what setHeights()
    // and the size toggle both need to leave alone.
    Q_PROPERTY(QVariantList speakerIsLfe READ speakerIsLfe NOTIFY speakerSetupChanged)

    // --- speaker layout (the Speakers page's "01 Speaker layout" card) ---
    // The layout in effect, as OutputLayout::text() gives it back: a name
    // ("7.1.4") when it was chosen as one and nothing since has needed the
    // list form, or the list form (with any ':small'/realization suffixes)
    // once it has. The "As text" field reads and writes this directly;
    // the layout picker's own "selected" segment is computed in QML by
    // comparing this against its six preset names, falling back to "List".
    Q_PROPERTY(QString layoutText READ layoutText NOTIFY speakerSetupChanged)
    // Whether the current layout has any slot the Heights control can act on
    // (OutputLayout::is_realizable_height()) - what gates that control.
    Q_PROPERTY(bool layoutHasHeight READ layoutHasHeight NOTIFY speakerSetupChanged)
    // Whether the current layout has an LFE feed at all - what gates the
    // Size column's controls (render::Speaker::small needs one to redirect a
    // small speaker's bass to; see OutputLayout::with_small()).
    Q_PROPERTY(bool layoutHasLfe READ layoutHasLfe NOTIFY speakerSetupChanged)
    // "wall"/"ceiling"/"upfiring" when every re-tierable height slot agrees,
    // "" when they do not (or there is none) - the Heights SegmentedControl's
    // currentValue, matching the values setHeights() takes.
    Q_PROPERTY(QString heightsRealization READ heightsRealization NOTIFY speakerSetupChanged)

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
    [[nodiscard]] QStringList speakerLabels() const { return speaker_labels_; }
    [[nodiscard]] QVariantList speakerSmall() const { return speaker_small_; }
    [[nodiscard]] QVariantList speakerIsLfe() const { return speaker_is_lfe_; }
    [[nodiscard]] QString layoutText() const { return layout_text_; }
    [[nodiscard]] bool layoutHasHeight() const { return layout_has_height_; }
    [[nodiscard]] bool layoutHasLfe() const { return layout_has_lfe_; }
    [[nodiscard]] QString heightsRealization() const { return heights_realization_; }

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

    // A name ("7.1.4") or a list (ac3::render::OutputLayout::parse()'s own
    // grammar - the layout picker's presets and the "As text" field both call
    // this directly), parsed here so an unparseable edit is simply refused
    // with nothing posted to the engine, the same way an out-of-range trim or
    // delay is dropped by the double-parsing TextFields elsewhere on this
    // page - there is no engine round trip to fail against.
    Q_INVOKABLE void setLayoutText(const QString& text);
    // Every re-tierable height slot set to `realization` ("wall"/"ceiling"/
    // "upfiring" - see heightsRealization()), keeping everything else about
    // the current layout - built from a fresh engine_->status().layout()
    // rather than the (possibly one poll stale) layoutText property, the way
    // setDecoderSettings() already reads a fresh snapshot rather than a
    // cached one for the same reason.
    Q_INVOKABLE void setHeights(const QString& realization);
    // One slot's ':small' flipped, keeping everything else - same fresh-read
    // reasoning as setHeights(). A no-op when the engine refuses it (out of
    // range, or no LFE feed to redirect a newly-small speaker's bass to).
    Q_INVOKABLE void setSpeakerSmall(int slot, bool small);

signals:
    void queueChanged();
    void stateChanged();
    void decoderSettingsChanged();
    void speakerSetupChanged();

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
    QStringList speaker_labels_;
    QVariantList speaker_small_;
    QVariantList speaker_is_lfe_;
    QString layout_text_;
    bool layout_has_height_ = false;
    bool layout_has_lfe_ = false;
    QString heights_realization_;
};

}  // namespace ac3::hearth::ui

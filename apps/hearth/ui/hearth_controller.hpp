#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QtQmlIntegration>

#include <memory>

#include "diagnostic_log.hpp"

namespace ac3::hearth {
class Engine;
class MediaInspector;
struct MediaInfo;
class SettingsStore;
class PairingStore;
}

namespace ac3::hearth::ui {
struct TestOutputs;  // test_outputs.hpp
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
// `.ac3`/`.ec3` files (item_loader.hpp). The engine is given every render
// endpoint to decide between (EngineOutputs::endpoints,
// apps/hearth/engine/output_selector.hpp) rather than one fixed sink, so the
// output picker's "Play here" can name an endpoint - no passthrough sink is
// given yet, so every item still decodes to PCM, on the platform's default
// device until a picker row is chosen. The layout is no longer fixed either:
// start() opens at whatever settings_model.hpp's SavedSpeakerSetup last
// kept ("2.0" the first time, or when that no longer parses), and the
// Speakers page's setLayoutText()/setHeights()/setSpeakerSmall() can change
// it from there for the engine's whole life (Player::set_layout()) -
// save_on_quit() keeps whatever it last settled on, with trim/delay/
// crossover/routing, for the next start() to reopen at (issue #885). One
// setup today, not one per output device - SavedSpeakerSetup's own comment
// says why.

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
    // streamKind ("AC-3", "E-AC-3" or "" before the item has been probed),
    // codecBadge ("A3"/"E3", "" before probed or for an item a probe will
    // never reach - AC-4, today), bitrateKbps (absent until probed).
    Q_PROPERTY(QVariantList queue READ queue NOTIFY queueChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY queueChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
    Q_PROPERTY(bool gapless READ gapless WRITE setGapless NOTIFY stateChanged)
    // The transport bar's master volume, in dB (EngineStatus::volume_db) -
    // not writable directly: like trimDb/crossoverHz, a set can be refused
    // (out of [Player::kMinVolumeDb, Player::kMaxVolumeDb]), so it goes
    // through setVolumeDb() and comes back through poll() rather than an
    // optimistic local write.
    Q_PROPERTY(double volumeDb READ volumeDb NOTIFY stateChanged)
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

    // --- media information (the Media page; also the Decoder page's "This
    // stream"/"Programme" cards and the Play page's "Now playing" line) ----
    // What the playing item's own file says about itself (media_info.hpp),
    // read once off the GUI thread and kept for as long as it stays the item
    // asked about - never from status(), which only ever carries what a
    // probe read at queue-add time. Both maps follow media_info_json()'s own
    // document shape (see that function's comment), turned into nested
    // QVariantMaps/QVariantLists field by field rather than left as text, so
    // QML can bind to a field directly; `json` on each carries the document
    // whole, for Copy/Export JSON.
    //
    // currentMedia always follows the item playing now, empty when nothing
    // is. inspectedMedia follows whichever queue item inspectItem() last
    // asked for - the Media page's own "Showing" picker - and defaults to
    // mirroring currentMedia until a different item is asked for; an AC-4
    // item, never playing in this build, is reachable only through it.
    Q_PROPERTY(QVariantMap currentMedia READ currentMedia NOTIFY currentMediaChanged)
    Q_PROPERTY(QVariantMap inspectedMedia READ inspectedMedia NOTIFY inspectedMediaChanged)
    Q_PROPERTY(int inspectedIndex READ inspectedIndex NOTIFY inspectedMediaChanged)

    // --- decoder settings (the Decoder page, AC-3 and E-AC-3) ------------
    // The whole of DecoderSettings, as one map QML reads field by field and
    // writes back through setDecoderSettings() - see that method's own
    // comment for the field names. Every control the design shows now has a
    // field here.
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
    // Each device output's own name - "FL", "LFE", "SL" - for the routing
    // grid's column headers to show beside their 1-based output number
    // (Speakers.qml's own "1 FL" mockup, planning/hearth-design.md). From
    // EngineStatus::speaker_mask (ac3::audio::speakers.hpp's output_names()),
    // which falls back to the standard arrangement for routingOutputs' width
    // when the device does not report a mask; an entry is empty where even
    // that cannot name the output, and the grid shows the bare number alone.
    Q_PROPERTY(QStringList outputNames READ outputNames NOTIFY speakerSetupChanged)
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY speakerSetupChanged)
    // The open device's own endpoint id - what the output picker (A5's
    // output-picker dialog) compares each row's own id against to say which
    // one is "playing here", rather than matching on the name, which two
    // distinct endpoints can share.
    Q_PROPERTY(QString currentDeviceId READ currentDeviceId NOTIFY speakerSetupChanged)
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
    // Also the Signal path panel's own "onto N outputs" render summary.
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
    // The identify tone's IDENTIFY card: the pink-noise level every session
    // plays at (the design offers -30/-20/-12 dB; render::IdentifyTone's
    // own range is wider) and which speakerLabels slot is currently
    // sounding it, or -1 for none - the same index space as trimDb/
    // speakerLabels, not a device output.
    Q_PROPERTY(double identifyLevelDb READ identifyLevelDb NOTIFY speakerSetupChanged)
    Q_PROPERTY(int identifySlot READ identifySlot NOTIFY speakerSetupChanged)

    // --- play monitor (the Play page: levels, loudness, this frame's ------
    // detail, object placement and signal path) ---------------------------
    // Everything below comes from Engine::meters() and Engine::unit_report(),
    // polled the same tick as status() - planning/hearth-reference-player.md's
    // Monitor, which A5's first slice deferred until there was something real
    // to read (PlayPage.qml's own former header comment said so). Neither
    // call is new: both have existed on Engine since A3 slices 6 and 8: this
    // is the first thing in the tree to poll them.

    // One entry per render layout slot, in slot order (matching
    // speakerLabels): {peakDb, holdDb, rmsDb, clipped}, from
    // MeterSnapshot::levels (ac3::analysis::ChannelLevel). Empty while no
    // output is open or nothing has been metered yet.
    Q_PROPERTY(QVariantList levels READ levels NOTIFY monitorChanged)
    // momentary/shortTerm/integrated (LUFS), range (LU) and truePeak (dBTP).
    // A key is ABSENT, not zero, while that reading has no value yet
    // (MeterSnapshot's own optionals) - QML checks with `!== undefined`.
    Q_PROPERTY(QVariantMap loudness READ loudness NOTIFY monitorChanged)
    // The access unit the device is playing now: dialnorm, compr, dynrng
    // (its block range), shortBlocks, bitrateKbps and sequence. shortBlocks
    // is AC-3 only and absent for E-AC-3 (UnitReport::short_blocks' own
    // comment says why); bitrateKbps is likewise absent for the last unit of
    // a stream, released by finish() (UnitReport::bitrate_kbps' own comment
    // says why) - the panel says so rather than showing a wrong number.
    // sequence is a running count of units reported so far, not a position
    // in the file (UnitReport::sequence's own comment).
    Q_PROPERTY(QVariantMap thisFrame READ thisFrame NOTIFY monitorChanged)
    // This unit's objects, bed channels and dynamic alike, one entry per
    // oba::describe_objects() result: {x, y, z, gainDb, snap, active, label,
    // raised}. label is empty for a dynamic object; raised is position.z
    // above the bed plane, the same test objectsPlaced counts active,
    // unlabelled entries by.
    Q_PROPERTY(QVariantList objects READ objects NOTIFY monitorChanged)
    // Dynamic (unlabelled), active objects right now - the Objects panel's
    // own "N placed".
    Q_PROPERTY(int objectsPlaced READ objectsPlaced NOTIFY monitorChanged)
    // Whether the playing unit carries object metadata at all - a plain
    // AC-3/E-AC-3 stream has none, and the Objects panel says so rather than
    // drawing an empty room.
    Q_PROPERTY(bool hasObjectMetadata READ hasObjectMetadata NOTIFY monitorChanged)
    // sampleRate, channels and mode ("pcm"/"bitstream"/"bitstreamAsAc3"/
    // "networkGroup"/"none") - EngineStatus::output, named for the Signal
    // path panel's "you hear it on" stage. Polled with output_reason (both
    // change together, at an open or a reopen), so this shares stateChanged
    // rather than adding a signal nothing else would use.
    Q_PROPERTY(QVariantMap outputFormat READ outputFormat NOTIFY stateChanged)

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
    // The network group pinned as output (selectOutputGroup()'s own id),
    // or empty for none/a local device - what the output picker compares
    // NetworkController's own group rows' id against, the same way
    // currentDeviceId says which outputDevices() row is "playing here".
    // Empty does not mean nothing plays: it is also the state while a local
    // device is pinned instead (selectOutputDevice() clears it).
    Q_PROPERTY(QString outputGroupName READ outputGroupName NOTIFY stateChanged)

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
    [[nodiscard]] double volumeDb() const { return volume_db_; }
    [[nodiscard]] QString outputReason() const { return output_reason_; }
    [[nodiscard]] QString noteText() const { return note_; }
    [[nodiscard]] QString errorText() const { return error_; }
    [[nodiscard]] bool firstRunSeen() const;
    void setFirstRunSeen(bool seen);

    [[nodiscard]] qlonglong positionMs() const { return position_ms_; }
    [[nodiscard]] qlonglong durationMs() const { return duration_ms_; }

    [[nodiscard]] QVariantMap currentMedia() const { return current_media_; }
    [[nodiscard]] QVariantMap inspectedMedia() const { return inspected_media_; }
    [[nodiscard]] int inspectedIndex() const { return inspected_index_; }
    // Points the Media page at queue item `index`; -1 (the default) follows
    // whatever is playing now, the same item currentMedia describes. Out of
    // range for the current queue is ignored.
    Q_INVOKABLE void inspectItem(int index);
    // Writes inspectedMedia's own JSON export to `fileUrl` (the Media page's
    // "Export JSON..." dialog, a local file the user picked). False when
    // there is nothing to export yet or the file could not be written.
    Q_INVOKABLE bool exportInspectedMedia(const QUrl& fileUrl);

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
    // A FileDialog/FolderDialog result or a DropArea drop.urls entry has no
    // toLocalFile() of its own once QML hands it to JavaScript - only a real
    // C++ QUrl does, which is what marshalling it through this invokable's
    // own QUrl parameter produces. exportInspectedMedia() below already
    // relies on the same marshalling for its own fileUrl parameter.
    Q_INVOKABLE QString urlToLocalFile(const QUrl& url) const {
        return url.isLocalFile() ? url.toLocalFile() : url.toString();
    }
    // Each path becomes one queue item, titled by its file name.
    Q_INVOKABLE void addFiles(const QStringList& paths);
    // Every media file item_loader.hpp's list_folder_items() finds under
    // `path` (recursively), added the same way addFiles() adds a file
    // picked directly - including a container list_folder_items() lists but
    // make_file_item_loader() cannot yet open, which lands in the queue
    // unplayable with a reason, same as addFiles() already does for one.
    Q_INVOKABLE void addFolder(const QString& path);
    Q_INVOKABLE void setVolumeDb(double db);

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
    [[nodiscard]] QStringList outputNames() const { return output_names_; }
    [[nodiscard]] QString deviceName() const { return device_name_; }
    [[nodiscard]] QString currentDeviceId() const { return device_id_; }
    [[nodiscard]] QStringList speakerLabels() const { return speaker_labels_; }
    [[nodiscard]] QVariantList speakerSmall() const { return speaker_small_; }
    [[nodiscard]] QVariantList speakerIsLfe() const { return speaker_is_lfe_; }
    [[nodiscard]] QString layoutText() const { return layout_text_; }
    [[nodiscard]] bool layoutHasHeight() const { return layout_has_height_; }
    [[nodiscard]] bool layoutHasLfe() const { return layout_has_lfe_; }
    [[nodiscard]] QString heightsRealization() const { return heights_realization_; }
    [[nodiscard]] double identifyLevelDb() const { return identify_level_db_; }
    [[nodiscard]] int identifySlot() const { return identify_slot_; }

    [[nodiscard]] QVariantList levels() const { return levels_; }
    [[nodiscard]] QVariantMap loudness() const { return loudness_; }
    [[nodiscard]] QVariantMap thisFrame() const { return this_frame_; }
    [[nodiscard]] QVariantList objects() const { return objects_; }
    [[nodiscard]] int objectsPlaced() const { return objects_placed_; }
    [[nodiscard]] bool hasObjectMetadata() const { return has_object_metadata_; }
    [[nodiscard]] QVariantMap outputFormat() const { return output_format_; }

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

    [[nodiscard]] QString outputGroupName() const { return output_group_name_; }
    // Pins playback to the network group named `groupId` (one of
    // NetworkController's own group rows' "id" fields - a stable id, not the
    // group's editable display name: network_sinks.hpp's own group()
    // comment says why one resolves unambiguously and the other cannot) -
    // the picker's "Play here" on a group row, once it grows one. Readiness
    // is read fresh from NetworkOutputStatus at the moment of the call and
    // again every poll() tick after (network_output_status.hpp's own
    // comment on why this cannot be captured once), so a sink connecting or
    // dropping after this call still reaches Engine::set_output_preferences
    // without the person having to reselect the group. An empty `groupId`
    // clears the pin, the same as selectOutputDevice("") would if it
    // allowed one - selectOutputDevice() itself remains how playback moves
    // back to a local device.
    Q_INVOKABLE void selectOutputGroup(const QString& groupId);

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

    // For the Qt Quick suites alone (ui/tests/qml_test_main.cpp): the PCM
    // sink, endpoint list and device enumeration start() and
    // refreshOutputDevices() use instead of this machine's own
    // (test_outputs.hpp says why). Deliberately neither Q_INVOKABLE nor a
    // property, the same as CrucibleController::set_test_services(): nothing
    // in QML and nothing in the shipped window can reach it. Only read by
    // start() - set it before the first start(), which a suite does from its
    // own initTestCase() - and by every refreshOutputDevices() after.
    void set_test_outputs(std::shared_ptr<TestOutputs> outputs) { test_outputs_ = std::move(outputs); }

signals:
    void queueChanged();
    void stateChanged();
    void currentMediaChanged();
    void inspectedMediaChanged();
    void positionChanged();
    void decoderSettingsChanged();
    void speakerSetupChanged();
    void monitorChanged();
    void outputDevicesChanged();
    void firstRunSeenChanged();
    void settingsChanged();
    void pairingChanged();
    void diagnosticsChanged();

private:
    void poll();
    // Saves the settings; while resumeQueue is on, the queue and its play
    // position; and the speaker setup (trim/delay/crossover/routing/layout)
    // unconditionally - connected to QCoreApplication::aboutToQuit, since a
    // play position (and a trim or delay slider mid-drag) changes too often
    // to save from on every one of them. A hard kill loses whatever this
    // would have written, the same trade every setting here already makes by
    // calling sync() only on a change rather than continuously.
    void save_on_quit();

    std::unique_ptr<ac3::hearth::Engine> engine_;
    QTimer poll_timer_;
    // Set only by set_test_outputs(); null in the shipped window.
    std::shared_ptr<TestOutputs> test_outputs_;

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
    double volume_db_ = 0.0;
    QString output_reason_;
    QString note_;
    QString error_;

    // Each on its own thread (media_inspector.hpp), so the Media page's own
    // pick never has to wait for whatever currentMedia is mid-reading, and
    // vice versa.
    std::unique_ptr<ac3::hearth::MediaInspector> now_playing_inspector_;
    std::unique_ptr<ac3::hearth::MediaInspector> inspected_item_inspector_;
    QVariantMap current_media_;
    QVariantMap inspected_media_;
    QString now_playing_path_;
    QString inspected_path_;
    // -1 until inspectItem() is called with a real index: "follow
    // currentIndex", which also covers the queue being empty.
    int inspected_index_ = -1;

    qlonglong position_ms_ = 0;
    qlonglong duration_ms_ = 0;

    QVariantMap decoder_settings_;

    QVariantList trim_db_;
    QVariantList delay_ms_;
    double crossover_hz_ = 80.0;
    QVariantList routing_;
    int routing_outputs_ = 0;
    QStringList output_names_;
    QString device_name_;
    QString device_id_;
    QStringList speaker_labels_;
    QVariantList speaker_small_;

    QVariantList output_devices_;
    // The network group pinned as output, empty for none - selectOutputGroup()'s
    // own store, refreshed against NetworkOutputStatus every poll() tick
    // (outputGroupName()'s own comment says why). output_group_ready_ is
    // the readiness last posted for it, so poll() only posts again when
    // NetworkOutputStatus disagrees rather than every tick regardless.
    QString output_group_name_;
    bool output_group_ready_ = false;
    QVariantList speaker_is_lfe_;
    QString layout_text_;
    bool layout_has_height_ = false;
    bool layout_has_lfe_ = false;
    QString heights_realization_;
    // -20.0 here mirrors render::IdentifyTone::kDefaultLevelDb without this
    // header needing that include - see setDecoderSettings()'s own comment
    // on why ac3::render stays out of this file. Overwritten by the first
    // poll() regardless, the way speakerLabels' own comment explains.
    double identify_level_db_ = -20.0;
    int identify_slot_ = -1;

    QVariantList levels_;
    QVariantMap loudness_;
    QVariantMap this_frame_;
    QVariantList objects_;
    int objects_placed_ = 0;
    bool has_object_metadata_ = false;
    QVariantMap output_format_;
};

}  // namespace ac3::hearth::ui

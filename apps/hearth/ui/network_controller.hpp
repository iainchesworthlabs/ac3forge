#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration>

#include <memory>
#include <string>

// Forward-declared, not included: PairingStore and QSettingsStore both reach
// ac3::render transitively (settings_model.hpp includes engine_thread.hpp),
// and this header is a Qt one - hearth_controller.hpp keeps the same distance
// from ac3::hearth's own headers for the same reason
// (hearth-ui-qt-slots-macro-collides-with-render-layout). Unlike that class's
// single Engine pointer, this one needs two objects with no natural owner
// outside this class, so both are unique_ptr here too, constructed in
// network_controller.cpp where the real headers - and that file's own
// #undef slots - already are.
namespace ac3::hearth {
class NetworkSinks;
class PairingStore;
}

// The one object the Network page talks to (planning/hearth-reference-player.md,
// A6: discovery, connection, pairing and groups). Kept apart from
// HearthController rather than adding to it: what it wraps, NetworkSinks, is a
// standing Sendspin server with its own thread and its own lifetime, not a
// command queue in front of one Player the way Engine is - the two controllers
// poll two unrelated engine-side objects, on the same timer rhythm for the same
// reason CrucibleController's rate is reused throughout this application.
//
// Pairing records live in the process's one PairingStore
// (shared_pairing_store.hpp), which HearthController's Settings page lists and
// forgets from too - a record made here outlives the process. This computer's
// server identity, which every record is bound to (server_identity.hpp), and
// the network settings are read through this class's own QSettings, under the
// same "ac3forge"/"Hearth" identity HearthController's own settings file uses.

namespace ac3::hearth::ui {

class QSettingsStore;

class NetworkController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Every discovered sink, in NetworkSinkList.qml's own row shape: id,
    // name, icon, subtitle, badge, badgeText, notice, linkText, connected.
    Q_PROPERTY(QVariantList sinks READ sinks NOTIFY sinksChanged)
    Q_PROPERTY(QString selectedId READ selectedId NOTIFY sinksChanged)
    // The selected sink's own detail rows (NetworkSinkInfo.qml) plus its
    // badge, pairedOnText, linkText, connected, pageUrl, and what the page
    // may offer for it: pairing ("none" | "requested" | "code" | "active"),
    // canPair and canConnect (network_view.hpp's SinkDetail says what each
    // means); an empty map while nothing is selected or the selection is gone.
    Q_PROPERTY(QVariantMap selectedSink READ selectedSink NOTIFY sinksChanged)
    Q_PROPERTY(int discoveredCount READ discoveredCount NOTIFY sinksChanged)
    Q_PROPERTY(int groupCount READ groupCount NOTIFY sinksChanged)
    Q_PROPERTY(bool canCreateGroups READ canCreateGroups CONSTANT)
    Q_PROPERTY(int pairingDigitCount READ pairingDigitCount CONSTANT)
    Q_PROPERTY(QString pairingError READ pairingError NOTIFY sinksChanged)

    // --- a Hearth sink's own settings pages (A6's second slice) ----------
    // planning/hearth-reference-player.md#a6-network-outputs-in-the-application;
    // docs/hearth/design/screenshots/network-sink-{speakers,decoder}.png.
    // True for a paired, connected sink offering _ac3forge_player@v1 - what
    // Network.qml checks to show these pages instead of the plain "paired"
    // card a standard Sendspin player still gets.
    Q_PROPERTY(bool selectedSinkSettable READ selectedSinkSettable NOTIFY sinksChanged)
    // layoutText, labels[], small[], isLfe[], hasHeight, hasLfe,
    // heightsRealization, trimDb[], delayMs[], crossoverHz, routing[],
    // outputs (int) - Speakers.qml's own field names, so NetworkSinkSpeakers.qml
    // can bind the same way. management: {routing, trimMinDb, trimMaxDb,
    // maxDelayMs, crossoverMinHz, crossoverMaxHz, identify} - the sink's own
    // advertised ranges, absent fields disabling the matching control.
    // identifySlot: which labels[] index is sounding the tone, or -1.
    // Before this app has pushed anything this run, these describe a local
    // draft (see sinkSettingsBase()'s own comment), not the sink's actual
    // state - there is no read-back to show instead (SinkFacts::
    // intended_settings's own comment).
    Q_PROPERTY(QVariantMap sinkSpeakerSettings READ sinkSpeakerSettings NOTIFY sinksChanged)
    // mode, drcCut, drcBoost, heavyCompression, normaliseDialogue, downmix,
    // mixLfe, objects, concealment - DecoderEac3.qml's own field names for
    // the keys the extension's DecoderSettings actually carries (no
    // rfCeilingDb/ltrtPhaseShift/dualMono/jocDomain/fastInverseTransform -
    // the sink has no such settings). acceptedKeys: which of those this
    // sink's own support object lists (ac3forge::kDecoderSettingNames) -
    // what gates each control, since a sink can support a subset.
    Q_PROPERTY(QVariantMap sinkDecoderSettings READ sinkDecoderSettings NOTIFY sinksChanged)
    // What the sink itself has reported (ac3forge::State, "WHAT THE SINK
    // REPORTS" in the decoder mockup): settingsRevision, settingsAppliedText,
    // settingsErrorText, streamText, objectsText, dialogueText, playedText,
    // problemsText - each already the display string the page shows, the
    // same reason SinkDetail's own fields are (network_view.cpp).
    Q_PROPERTY(QVariantMap sinkReport READ sinkReport NOTIFY sinksChanged)
    // name, slotsText, network, firmware, url - the "ONLY ON THE SINK" panel
    // (the speakers mockup): identity the sink itself owns, which this page
    // only shows and links out to, never edits.
    Q_PROPERTY(QVariantMap sinkOnlyOnSink READ sinkOnlyOnSink NOTIFY sinksChanged)

    // Every group, in NetworkSinkList.qml's own row shape: id, name, icon,
    // subtitle, badge, badgeText, membersText, ready - shown above `sinks` in
    // the same list, and in the output picker's network section.
    Q_PROPERTY(QVariantList groups READ groups NOTIFY sinksChanged)
    Q_PROPERTY(QString selectedGroupId READ selectedGroupId NOTIFY sinksChanged)
    // The selected group's own editor rows (NetworkGroupEdit.qml): name,
    // members (each with id/name/getsText/volume/muted/volumeSupported/
    // muteSupported/connected), groupVolume, groupMuted,
    // membersConnectedText, leadTimeText. Empty map while nothing is
    // selected or the selection is gone.
    Q_PROPERTY(QVariantMap selectedGroup READ selectedGroup NOTIFY sinksChanged)

public:
    explicit NetworkController(QObject* parent = nullptr);
    // Explicit: stops the NetworkSinks (and with it every thread that calls
    // into the pairing store) before the members it uses go.
    ~NetworkController() override;

    // Whether start() asks Windows for a firewall exception for its mDNS
    // socket (sendspin::discovery::mdns::Options::request_firewall_exception):
    // true for the window. The Qt Quick suites (ui/tests/qml_test_main.cpp)
    // turn it off before any suite starts the network - ac3hearth_qmltests has
    // no main() of its own to finish the elevated relaunch the request makes,
    // so left on, every suite that opens Main.qml would ask for elevation on
    // every run.
    static void set_firewall_exception_requested(bool requested);

    // Starts the Sendspin server and mDNS browsing. Called from Main.qml's
    // Component.onCompleted, the same reason HearthController::start() is:
    // not the constructor, so a singleton QML creates before the window is
    // on screen does not open a socket with nothing yet shown for it.
    Q_INVOKABLE void start();

    [[nodiscard]] QVariantList sinks() const { return sinks_; }
    [[nodiscard]] QString selectedId() const { return selected_id_; }
    [[nodiscard]] QVariantMap selectedSink() const { return selected_sink_; }
    [[nodiscard]] int discoveredCount() const { return static_cast<int>(sinks_.size()); }
    [[nodiscard]] int groupCount() const { return static_cast<int>(groups_.size()); }
    [[nodiscard]] bool canCreateGroups() const { return true; }
    [[nodiscard]] int pairingDigitCount() const { return 6; }
    [[nodiscard]] QString pairingError() const { return pairing_error_; }
    [[nodiscard]] QVariantList groups() const { return groups_; }
    [[nodiscard]] QString selectedGroupId() const { return selected_group_id_; }
    [[nodiscard]] QVariantMap selectedGroup() const { return selected_group_; }

    Q_INVOKABLE void rescan();
    // Selection only: pairing is pairSink().
    Q_INVOKABLE void selectSink(const QString& id);
    // NetworkSinks::pair_sink(): the sink then shows six digits on its own
    // page and console, which submitPairingCode() takes.
    Q_INVOKABLE void pairSink(const QString& id);
    Q_INVOKABLE void submitPairingCode(const QString& id, const QString& code);
    Q_INVOKABLE void cancelPairing(const QString& id);
    // NetworkSinks::connect_sink(): takes a paired sink back from another
    // server, or connects to one that is not connected, now.
    Q_INVOKABLE void connectSink(const QString& id);
    // NetworkSinks::forget_pairing(): the sink has to be paired again.
    Q_INVOKABLE void forgetSink(const QString& id);

    [[nodiscard]] bool selectedSinkSettable() const { return selected_sink_settable_; }
    [[nodiscard]] QVariantMap sinkSpeakerSettings() const { return sink_speaker_settings_; }
    [[nodiscard]] QVariantMap sinkDecoderSettings() const { return sink_decoder_settings_; }
    [[nodiscard]] QVariantMap sinkReport() const { return sink_report_; }
    [[nodiscard]] QVariantMap sinkOnlyOnSink() const { return sink_only_on_sink_; }

    // Each reads the selected sink's own current draft (sinkSpeakerSettings()/
    // sinkDecoderSettings(), or sinkSettingsBase() fresh where those two
    // properties' own values would be a poll tick stale) as `base`, changes
    // just this one field, and pushes the WHOLE result - NetworkSinks::
    // push_sink_settings()'s own comment on why, the same "whole struct, apply
    // what changed" shape HearthController::setDecoderSettings() uses locally.
    // Silently does nothing for a selected sink that is not selectedSinkSettable().
    Q_INVOKABLE void setSinkLayoutText(const QString& text);
    Q_INVOKABLE void setSinkHeights(const QString& realization);
    Q_INVOKABLE void setSinkSpeakerSmall(int slot, bool small);
    // `output` is a SINK OUTPUT index (0 to sinkSpeakerSettings().outputs -
    // 1), not a render slot index: unlike HearthController's own trimDb/
    // delayMs (one entry per slot, applied before routing), the wire's
    // trim_db/delay_ms are sized and checked against support.outputs.count
    // (ac3forge_player.cpp's check_settings()) - the sink applies them after
    // its own routing, per physical output. sinkSpeakerSettings()'s own
    // levels-table row reads trimDb[routing[slot]]/delayMs[routing[slot]]
    // to show the right value beside each speaker's OUT column.
    Q_INVOKABLE void setSinkTrimDb(int output, double db);
    Q_INVOKABLE void setSinkDelayMs(int output, double ms);
    Q_INVOKABLE void setSinkCrossoverHz(double hz);
    // Same rule as HearthController::setRoutingAssignment(): refused, whole,
    // for an output another slot already has.
    Q_INVOKABLE void setSinkRoutingAssignment(int slot, int output);
    // Every key sinkDecoderSettings() reads back; unknown or missing keys
    // keep the sink's own last-known value for that field, the same rule
    // HearthController::setDecoderSettings()'s own comment states.
    Q_INVOKABLE void setSinkDecoderSettings(const QVariantMap& settings);

    // The identify tone on the sink itself (Command::kIdentify), not this
    // computer's own render::IdentifyTone - HearthController's identify
    // methods are a separate, unrelated pair. No-op for slot < 0 or a sink
    // whose management object does not offer identify.
    Q_INVOKABLE void startSinkIdentify(int slot);
    Q_INVOKABLE void stopSinkIdentify();

    Q_INVOKABLE QString createGroup(const QString& name);
    Q_INVOKABLE void renameGroup(const QString& groupId, const QString& name);
    Q_INVOKABLE void deleteGroup(const QString& groupId);
    Q_INVOKABLE void selectGroup(const QString& groupId);
    Q_INVOKABLE void addGroupMember(const QString& groupId, const QString& sinkId);
    Q_INVOKABLE void removeGroupMember(const QString& groupId, const QString& sinkId);
    Q_INVOKABLE void setGroupVolume(const QString& groupId, int volume);
    Q_INVOKABLE void setGroupMuted(const QString& groupId, bool muted);
    Q_INVOKABLE void setMemberVolume(const QString& groupId, const QString& sinkId, int volume);
    Q_INVOKABLE void setMemberMuted(const QString& groupId, const QString& sinkId, bool muted);

    // For the Qt Quick suites alone (ui/tests/qml_test_main.cpp), the same
    // kind of seam as HearthController::set_test_outputs(): the running
    // NetworkSinks, or null before start(), so a suite can hand it an
    // in-process test sink (apps/hearth/testsink) as a found service -
    // NetworkSinks::on_found() is public for exactly this use (its own
    // comment) - rather than depend on mDNS multicast, which a CI container
    // does not carry. Not Q_INVOKABLE: nothing in QML can reach it.
    [[nodiscard]] ac3::hearth::NetworkSinks* sinks_for_test() const { return sinks_engine_.get(); }

signals:
    void sinksChanged();

private:
    void poll();
    // Records whether a settings push to `sink_id` was actually sent
    // (NetworkSinks::push_sink_settings()'s result) and republishes, so a
    // refused one shows in that sink's report rather than vanishing.
    void note_push(bool sent, const std::string& sink_id);

    // The four-argument constructor, network_controller.cpp's own comment
    // says why - hearth_controller.hpp carries the identical comment for
    // the identical reason. Declared before settings_store_, which depends
    // on it.
    QSettings settings_;
    std::unique_ptr<ac3::hearth::ui::QSettingsStore> settings_store_;
    // The process's one pairing store (shared_pairing_store.hpp).
    std::shared_ptr<ac3::hearth::PairingStore> pairing_store_;
    // Declared after everything it uses, and reset first by the destructor
    // regardless: its threads call into pairing_store_ until it is gone.
    std::unique_ptr<ac3::hearth::NetworkSinks> sinks_engine_;
    QTimer poll_timer_;

    QVariantList sinks_;
    QString selected_id_;
    QVariantMap selected_sink_;
    QString pairing_error_;

    bool selected_sink_settable_ = false;
    QVariantMap sink_speaker_settings_;
    QVariantMap sink_decoder_settings_;
    QVariantMap sink_report_;
    QVariantMap sink_only_on_sink_;
    // The sink the last settings push was refused for, or empty - see
    // note_push().
    QString refused_push_sink_;

    QVariantList groups_;
    QString selected_group_id_;
    QVariantMap selected_group_;
};

}  // namespace ac3::hearth::ui

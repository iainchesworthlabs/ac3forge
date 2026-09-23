#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration>

#include <memory>

// Forward-declared, not included: PairingStore and MemorySettingsStore both
// reach ac3::render transitively (settings_model.hpp includes engine_thread.hpp),
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
class MemorySettingsStore;
}

// The one object the Network page talks to (planning/hearth-reference-player.md,
// A6's first slice: discovery and pairing). Kept apart from HearthController
// rather than adding to it: what it wraps, NetworkSinks, is a standing
// Sendspin server with its own thread and its own lifetime, not a command
// queue in front of one Player the way Engine is - the two controllers poll
// two unrelated engine-side objects, on the same timer rhythm for the same
// reason CrucibleController's rate is reused throughout this application.
//
// Pairing records live in a MemorySettingsStore for this slice, not the
// window's real settings file: nothing in apps/hearth/ui implements
// SettingsStore over QSettings yet (that is the Settings page's own slice),
// so a record made here does not outlive the process. Once that store
// exists, constructing PairingStore over it instead of MemorySettingsStore is
// the whole change - everything else here is unaffected, since it only ever
// sees PairingStore's own interface.

namespace ac3::hearth::ui {

class NetworkController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Every discovered sink, in NetworkSinkList.qml's own row shape: id,
    // name, icon, subtitle, badge, badgeText.
    Q_PROPERTY(QVariantList sinks READ sinks NOTIFY sinksChanged)
    Q_PROPERTY(QString selectedId READ selectedId NOTIFY sinksChanged)
    // The selected sink's own detail rows (NetworkSinkInfo.qml) plus its
    // badge and pairedOnText; an empty map while nothing is selected or the
    // selection is gone.
    Q_PROPERTY(QVariantMap selectedSink READ selectedSink NOTIFY sinksChanged)
    Q_PROPERTY(int discoveredCount READ discoveredCount NOTIFY sinksChanged)
    // Always 0 in this slice - see canCreateGroups.
    Q_PROPERTY(int groupCount READ groupCount NOTIFY sinksChanged)
    // False until groups exist to create (a later A6 slice): NetworkSinks
    // never starts a ServerHost::Group, so "+ New group..." has nothing to
    // do yet - see network_sinks.hpp's own comment on what A6 still needs.
    Q_PROPERTY(bool canCreateGroups READ canCreateGroups CONSTANT)
    Q_PROPERTY(int pairingDigitCount READ pairingDigitCount CONSTANT)
    Q_PROPERTY(QString pairingError READ pairingError NOTIFY sinksChanged)

public:
    explicit NetworkController(QObject* parent = nullptr);
    ~NetworkController() override;

    // Starts the Sendspin server and mDNS browsing. Called from Main.qml's
    // Component.onCompleted, the same reason HearthController::start() is:
    // not the constructor, so a singleton QML creates before the window is
    // on screen does not open a socket with nothing yet shown for it.
    Q_INVOKABLE void start();

    [[nodiscard]] QVariantList sinks() const { return sinks_; }
    [[nodiscard]] QString selectedId() const { return selected_id_; }
    [[nodiscard]] QVariantMap selectedSink() const { return selected_sink_; }
    [[nodiscard]] int discoveredCount() const { return static_cast<int>(sinks_.size()); }
    [[nodiscard]] int groupCount() const { return 0; }
    [[nodiscard]] bool canCreateGroups() const { return false; }
    [[nodiscard]] int pairingDigitCount() const { return 6; }
    [[nodiscard]] QString pairingError() const { return pairing_error_; }

    Q_INVOKABLE void rescan();
    Q_INVOKABLE void selectSink(const QString& id);
    Q_INVOKABLE void submitPairingCode(const QString& id, const QString& code);
    Q_INVOKABLE void cancelPairing(const QString& id);

signals:
    void sinksChanged();

private:
    void poll();

    std::unique_ptr<ac3::hearth::NetworkSinks> sinks_engine_;
    std::unique_ptr<ac3::hearth::MemorySettingsStore> settings_store_;
    std::unique_ptr<ac3::hearth::PairingStore> pairing_store_;
    QTimer poll_timer_;

    QVariantList sinks_;
    QString selected_id_;
    QVariantMap selected_sink_;
    QString pairing_error_;
};

}  // namespace ac3::hearth::ui

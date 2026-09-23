#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration>

#include <memory>

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
// A6's first slice: discovery and pairing). Kept apart from HearthController
// rather than adding to it: what it wraps, NetworkSinks, is a standing
// Sendspin server with its own thread and its own lifetime, not a command
// queue in front of one Player the way Engine is - the two controllers poll
// two unrelated engine-side objects, on the same timer rhythm for the same
// reason CrucibleController's rate is reused throughout this application.
//
// Pairing records live in a QSettingsStore (qsettings_store.hpp) over this
// class's own QSettings, under the same "ac3forge"/"Hearth" identity
// HearthController's own settings file uses - a record made here outlives
// the process. This class owns and constructs its QSettingsStore itself
// rather than sharing HearthController's C++ instance: the two controllers
// are independent QML singletons with no natural owner to hand a reference
// between them, and QSettings instances that share an org/app identity
// already read and write the same underlying store without one.

namespace ac3::hearth::ui {

class QSettingsStore;

class NetworkController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Every discovered sink, in NetworkSinkList.qml's own row shape: id,
    // name, icon, subtitle, badge, badgeText, notice.
    Q_PROPERTY(QVariantList sinks READ sinks NOTIFY sinksChanged)
    Q_PROPERTY(QString selectedId READ selectedId NOTIFY sinksChanged)
    // The selected sink's own detail rows (NetworkSinkInfo.qml) plus its
    // badge and pairedOnText; an empty map while nothing is selected or the
    // selection is gone.
    Q_PROPERTY(QVariantMap selectedSink READ selectedSink NOTIFY sinksChanged)
    Q_PROPERTY(int discoveredCount READ discoveredCount NOTIFY sinksChanged)
    Q_PROPERTY(int groupCount READ groupCount NOTIFY sinksChanged)
    Q_PROPERTY(bool canCreateGroups READ canCreateGroups CONSTANT)
    Q_PROPERTY(int pairingDigitCount READ pairingDigitCount CONSTANT)
    Q_PROPERTY(QString pairingError READ pairingError NOTIFY sinksChanged)

    // Every group, in NetworkSinkList.qml's own row shape: id, name, icon,
    // subtitle, badge, badgeText - shown above `sinks` in the same list.
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
    [[nodiscard]] int groupCount() const { return static_cast<int>(groups_.size()); }
    [[nodiscard]] bool canCreateGroups() const { return true; }
    [[nodiscard]] int pairingDigitCount() const { return 6; }
    [[nodiscard]] QString pairingError() const { return pairing_error_; }
    [[nodiscard]] QVariantList groups() const { return groups_; }
    [[nodiscard]] QString selectedGroupId() const { return selected_group_id_; }
    [[nodiscard]] QVariantMap selectedGroup() const { return selected_group_; }

    Q_INVOKABLE void rescan();
    Q_INVOKABLE void selectSink(const QString& id);
    Q_INVOKABLE void submitPairingCode(const QString& id, const QString& code);
    Q_INVOKABLE void cancelPairing(const QString& id);

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

signals:
    void sinksChanged();

private:
    void poll();

    std::unique_ptr<ac3::hearth::NetworkSinks> sinks_engine_;
    // The four-argument constructor, network_controller.cpp's own comment
    // says why - hearth_controller.hpp carries the identical comment for
    // the identical reason. Declared before settings_store_ and pairing_
    // store_, both of which depend on the one before them in this order.
    QSettings settings_;
    std::unique_ptr<ac3::hearth::ui::QSettingsStore> settings_store_;
    std::unique_ptr<ac3::hearth::PairingStore> pairing_store_;
    QTimer poll_timer_;

    QVariantList sinks_;
    QString selected_id_;
    QVariantMap selected_sink_;
    QString pairing_error_;
    QVariantList groups_;
    QString selected_group_id_;
    QVariantMap selected_group_;
};

}  // namespace ac3::hearth::ui

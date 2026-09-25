import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The Network tab (planning/hearth-reference-player.md, A6): discovery,
// connection and pairing. The list on the left (NetworkSinkList.qml) is
// shared with every state; which view fills the rest of the page follows the
// selected sink's own pair state, the same switch DecoderPage.qml makes on
// stream format.
//
// A sink's own speaker and decoder settings pages (NetworkSinkSettings.qml,
// issue #875) show once NetworkController.selectedSinkSettable is true (a
// paired Hearth sink), replacing the plain "paired" card below. A paired
// sink that is not connected - another server took it, or it is not
// answering - gets a banner above either, with the way to connect to it now.
// Groups (NetworkGroupEdit.qml, issue #874: create, add, remove, volume,
// mute, and playing to one) are backed by an actual ac3::sendspin::Group.
//
// Still the rest of A6, not built here: a warning BEFORE this computer takes
// a paired sink another server is playing to needs ac3::sendspin to grow a
// way to ask without taking (network_sinks.hpp's own comment says why it
// cannot today); only the after-the-fact notice is here.
Item {
    id: root

    RowLayout {
        anchors.fill: parent
        anchors.margins: Theme.pad
        spacing: Theme.gap * 2

        NetworkSinkList {
            // fillWidth: false alone is not enough to keep this at its
            // preferred 300px - a Layout item is still free to grow to its
            // own content's implicit width when there is room, and
            // NetworkSinkList.qml's elided/wrapped Text swallowed the whole
            // page's width that way before every one of them also got
            // Layout.minimumWidth: 0 (that file's own comment has the
            // story). maximumWidth is the actual, robust cap.
            Layout.preferredWidth: 300
            Layout.maximumWidth: 300
            Layout.fillWidth: false
            Layout.fillHeight: true
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.gap

            // A paired sink this computer is not connected to (network_view.hpp's
            // SinkDetail::can_connect). Read straight off the singleton, for the
            // Loader comment's reason below.
            Rectangle {
                objectName: "networkSinkConnectBanner"
                Layout.fillWidth: true
                visible: NetworkController.selectedGroup.id === undefined
                         && NetworkController.selectedSink.canConnect === true
                implicitHeight: bannerRow.implicitHeight + Theme.gap
                color: Theme.surface
                border.color: Theme.border
                border.width: 1

                RowLayout {
                    id: bannerRow
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.margins: Theme.gap
                    spacing: Theme.gap

                    Text {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: (NetworkController.selectedSink.notice ?? "").length > 0
                              ? qsTr("%1 Taking it back stops whatever that server plays to it.")
                                .arg(NetworkController.selectedSink.notice)
                              : qsTr("Not connected: %1.").arg(NetworkController.selectedSink.linkText ?? "")
                        color: Theme.text
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                    AppButton {
                        objectName: "networkSinkConnect"
                        text: (NetworkController.selectedSink.notice ?? "").length > 0 ? qsTr("Take it back")
                                                                                        : qsTr("Connect now")
                        onClicked: NetworkController.connectSink(NetworkController.selectedSink.id)
                    }
                }
            }

            Loader {
                Layout.fillWidth: true
                Layout.fillHeight: true
                // Reads the selected sink/group through the maps' members
                // directly off NetworkController, never through a local
                // (`const group = NetworkController.selectedGroup; if (group &&
                // group.id ...)`) - do not "simplify" it back. Under Qt 6.9.3,
                // qmlcachegen compiles a member read on such a local as a
                // value-type lookup on QVariant itself: the generated C++ passes
                // QMetaType::fromName("QVariant").metaObject() - null, QVariant
                // has no meta-object - to AOTCompiledContext::
                // initGetValueLookup(), which dereferences it, and ac3hearth
                // segfaulted on start (Network.qml is built with Main.qml's
                // StackLayout). A read straight off the singleton's QVariantMap
                // property is compiled as an ordinary lookup.
                // tst_network_page_aot.qml builds the page with the compiled
                // bindings to hold this.
                sourceComponent: {
                    if (NetworkController.selectedGroup.id !== undefined) {
                        return groupState;
                    }
                    if (NetworkController.selectedSink.id === undefined) {
                        return emptyState;
                    }
                    if (NetworkController.selectedSink.badge !== "paired") {
                        return pairingState;
                    }
                    return NetworkController.selectedSinkSettable ? settingsState : pairedState;
                }
            }
        }
    }

    Component {
        id: groupState
        NetworkGroupEdit { }
    }

    Component {
        id: emptyState
        Item {
            anchors.fill: parent
            Text {
                anchors.centerIn: parent
                width: 320
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: qsTr("Select a sink on the left to pair it, or to see what it is.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontNormal
            }
        }
    }

    Component {
        id: pairingState
        NetworkPairing { }
    }

    Component {
        id: settingsState
        NetworkSinkSettings { }
    }

    // A sink already paired and idle, but not offering _ac3forge_player@v1
    // (a standard Sendspin player, e.g. "Kitchen speaker" in the mockups) -
    // nothing to decide for it: it takes stereo only, and has no settings
    // command to speak of. Small enough, and specific enough to this state,
    // that it does not earn its own file the way NetworkPairing.qml does
    // (DecoderEac3.qml's and DecoderAc4.qml's own reason for existing).
    Component {
        id: pairedState
        RowLayout {
            anchors.fill: parent
            spacing: Theme.gap * 2

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 2
                spacing: Theme.gap

                Card {
                    ordinal: "02"
                    // SectionHeader uppercases the label itself, so the
                    // name goes in as it is rather than through
                    // toUpperCase(), which has no business running over a
                    // translated or non-Latin string.
                    title: NetworkController.selectedSink.name ?? ""

                    Text {
                        Layout.fillWidth: true
                        // See NetworkSinkList.qml's own comment: a wrapped
                        // Text's default minimum width is its full,
                        // unwrapped width.
                        Layout.minimumWidth: 0
                        text: qsTr("Paired with this computer on %1. It takes streams from "
                                  + "Hearth without a code until the pairing is forgotten.")
                                  .arg(NetworkController.selectedSink.pairedOnText ?? "")
                        color: Theme.text
                        wrapMode: Text.WordWrap
                    }
                    Text {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: qsTr("A standard Sendspin player: it takes stereo only, with no speaker "
                                  + "or decoder settings of its own to show here. Add it to a group "
                                  + "from the list on the left, or make a new one.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                    AppButton {
                        objectName: "networkSinkForget"
                        text: qsTr("Forget this pairing")
                        onClicked: NetworkController.forgetSink(NetworkController.selectedSink.id)
                    }
                }

                Item { Layout.fillHeight: true }
            }

            NetworkSinkInfo {
                // See NetworkPairing.qml's own comment: fillWidth is needed
                // for the 2:1 ratio against it to actually hold.
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                sink: NetworkController.selectedSink
            }
        }
    }
}

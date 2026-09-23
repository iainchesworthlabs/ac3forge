import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The Network tab (planning/hearth-reference-player.md, A6): discovery and
// pairing. The list on the left (NetworkSinkList.qml) is shared with every
// state; which view fills the rest of the page follows the selected sink's
// own pair state, the same switch DecoderPage.qml makes on stream format.
//
// Groups, a sink's own speaker/decoder settings pages, a sink already in use
// by another server, and a group's reported levels are the rest of A6 and are
// not built here - the plan splits A6 into slices, and this is the first: a
// sink can be found and paired. "In use elsewhere" needs ac3::sendspin to
// grow a way to learn that (network_sinks.hpp's own comment says why it
// cannot today), so it is not a UI gap this slice left behind - there is
// nothing yet for the page to show. NetworkSinkList's "+ New group..." names
// the grouping slice rather than hiding it.
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

        Loader {
            Layout.fillWidth: true
            Layout.fillHeight: true
            sourceComponent: {
                const sink = NetworkController.selectedSink;
                if (!sink || sink.id === undefined) {
                    return emptyState;
                }
                return sink.badge === "paired" ? pairedState : pairingState;
            }
        }
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

    // A sink already paired and idle: nothing to decide yet (grouping and the
    // sink's own settings pages are later slices), so this just confirms what
    // pairing means for it. Small enough, and specific enough to this state,
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

                Text {
                    Layout.fillWidth: true
                    // See NetworkSinkList.qml's own comment on elide vs.
                    // Layout.minimumWidth.
                    Layout.minimumWidth: 0
                    text: qsTr("02 %1").arg(NetworkController.selectedSink.name ?? "").toUpperCase()
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    font.bold: true
                    elide: Text.ElideRight
                }

                Card {
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
                        text: qsTr("Grouping this sink with others, and its own speaker and "
                                  + "decoder settings, are not built in this version yet.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
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

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The Network tab (planning/hearth-reference-player.md, A6): discovery and
// pairing. The list on the left (NetworkSinkList.qml) is shared with every
// state; which view fills the rest of the page follows the selected sink's
// own pair state, the same switch DecoderPage.qml makes on stream format.
//
// A sink's own speaker and decoder settings pages (NetworkSinkSettings.qml,
// issue #875) show once NetworkController.selectedSinkSettable is true (a
// paired, connected Hearth sink), replacing the plain "paired" card below.
// Groups (NetworkGroupEdit.qml, issue #874: create, add, remove, volume,
// mute) are real, backed by an actual ac3::sendspin::Group, but not yet a
// live programme - Player has no network-group output seam yet (#874's own
// follow-up), so the group editor's "State"/"Late chunks" rows say so
// honestly rather than showing numbers this slice cannot make true.
//
// Still the rest of A6, not built here: a sink already in use by another
// server needs ac3::sendspin to grow a way to learn that
// (network_sinks.hpp's own comment says why it cannot today); a group's
// reported levels needs that same network-group output seam #874's own
// follow-up would add. Neither is a UI gap this slice left behind - there is
// nothing yet for the page to show for either.
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
                const group = NetworkController.selectedGroup;
                if (group && group.id !== undefined) {
                    return groupState;
                }
                const sink = NetworkController.selectedSink;
                if (!sink || sink.id === undefined) {
                    return emptyState;
                }
                if (sink.badge !== "paired") {
                    return pairingState;
                }
                return NetworkController.selectedSinkSettable ? settingsState : pairedState;
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

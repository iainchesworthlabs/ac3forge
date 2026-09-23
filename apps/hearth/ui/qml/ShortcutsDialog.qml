import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// Keyboard shortcuts reference, opened from the header's "?" button and F1
// (issue #830). The same Dialog shape as Crucible's AboutDialog.qml, on
// Hearth's own plain-Button convention. The header "?"/F1 stays this
// dialog's alone (agreed between the #830 and #854 sessions rather than a
// second header control); About… chains to AboutDialog.qml (issue #854),
// which chains its own Licences… to LicencesDialog.qml one hop further -
// "? -> Shortcuts -> About -> Licences".
Dialog {
    id: root
    objectName: "shortcutsDialog"
    // About…: Main.qml opens AboutDialog over this one.
    signal showAbout()
    modal: true
    // A Popup honours CloseOnEscape only while it has active focus, so the
    // dialog takes it as it opens; and Close is where a keyboard user wants
    // to start, so it is what has it.
    focus: true
    onOpened: shortcutsCloseButton.forceActiveFocus()
    anchors.centerIn: parent
    width: Math.min(480, parent ? parent.width - 60 : 480)
    padding: Theme.space6
    title: ""
    background: Rectangle {
        color: Theme.bg
        border.color: Theme.text
        border.width: 2
    }

    component Kicker: Text {
        Layout.topMargin: Theme.space3
        font.pixelSize: Theme.fontMicro
        font.letterSpacing: 1.2
        font.capitalization: Font.AllUppercase
        color: Theme.textMuted
    }
    component Key: Text {
        color: Theme.text
        font.family: Theme.monoFamily
        font.pixelSize: Theme.fontSmall
    }
    component Action: Text {
        Layout.fillWidth: true
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.WordWrap
    }

    contentItem: ColumnLayout {
        spacing: Theme.space2
        // On the content, not the dialog: a Popup is not an Item.
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Keyboard shortcuts")

        Text {
            text: qsTr("Keyboard shortcuts")
            color: Theme.text
            font.family: Theme.headingFamily
            font.pixelSize: Theme.fontTitle
            font.weight: Font.Bold
        }

        Kicker { text: qsTr("PAGES") }
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: Theme.space4
            rowSpacing: Theme.space1
            Key { text: "Ctrl+1" }
            Action { text: qsTr("Play") }
            Key { text: "Ctrl+2" }
            Action { text: qsTr("Media information") }
            Key { text: "Ctrl+3" }
            Action { text: qsTr("Speakers") }
            Key { text: "Ctrl+4" }
            Action { text: qsTr("Decoder") }
            Key { text: "Ctrl+5" }
            Action { text: qsTr("Network") }
            Key { text: "Ctrl+6" }
            Action { text: qsTr("Settings") }
        }

        Kicker { text: qsTr("TRANSPORT AND QUEUE") }
        Action {
            text: qsTr("Previous, Play/Pause, Stop, Next and Gapless are in the transport bar at the bottom of every page; Add files… and the queue are on Play. Tab reaches each control in turn, and Space or Return activates whichever one has focus.")
        }

        Kicker { text: qsTr("THIS WINDOW") }
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: Theme.space4
            rowSpacing: Theme.space1
            Key { text: "F1" }
            Action { text: qsTr("Open this reference") }
        }

        RowLayout {
            Layout.topMargin: Theme.space3
            Item { Layout.fillWidth: true }
            Button { objectName: "shortcutsAboutButton"; text: qsTr("About…"); onClicked: root.showAbout() }
            Button {
                id: shortcutsCloseButton
                objectName: "shortcutsCloseButton"
                text: qsTr("Close")
                onClicked: root.close()
            }
        }
    }
}

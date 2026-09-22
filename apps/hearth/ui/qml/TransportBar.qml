import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// Pinned to the bottom of every page, the way every round-1 artboard shows
// it (planning/hearth-design.md). None of these buttons is inside a
// Repeater, so the native-Button-in-a-Repeater hang the family has hit
// elsewhere does not apply here.
Rectangle {
    color: Theme.surface
    border.color: Theme.border
    border.width: 1
    implicitHeight: 48

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.pad
        anchors.rightMargin: Theme.pad
        spacing: Theme.gap

        Button {
            objectName: "transportPrevious"
            text: qsTr("Previous")
            enabled: HearthController.currentIndex > 0
            onClicked: HearthController.previous()
        }
        Button {
            objectName: "transportPlayPause"
            text: HearthController.playing ? qsTr("Pause") : qsTr("Play")
            enabled: HearthController.queue.length > 0
            onClicked: HearthController.playing ? HearthController.pause() : HearthController.play()
        }
        Button {
            objectName: "transportStop"
            text: qsTr("Stop")
            enabled: HearthController.state !== "stopped"
            onClicked: HearthController.stop()
        }
        Button {
            objectName: "transportNext"
            text: qsTr("Next")
            enabled: HearthController.currentIndex >= 0 &&
                     HearthController.currentIndex + 1 < HearthController.queue.length
            onClicked: HearthController.next()
        }

        Text {
            objectName: "transportState"
            Layout.fillWidth: true
            text: HearthController.errorText.length > 0 ? HearthController.errorText
                  : HearthController.noteText.length > 0 ? HearthController.noteText
                  : HearthController.state
            color: HearthController.errorText.length > 0 ? Theme.bad : Theme.textMuted
            font.pixelSize: Theme.fontSmall
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }

        CheckBox {
            objectName: "transportGapless"
            text: qsTr("Gapless")
            checked: HearthController.gapless
            onToggled: HearthController.gapless = checked
        }
    }
}

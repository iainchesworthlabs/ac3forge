import QtQuick
import QtQuick.Controls as QQC

import Ac3Forge

// A text field drawn to the design system (components.png, "FIELDS AND
// LISTS"): 30 px tall, a neutral100 fill, a 1 px divider border, and an
// accent border while it has the keyboard.
//
// Basic's own TextField fills from palette.base, which this family pins to
// Theme.surface - the same fill as the Card it usually sits on, so the field
// is held apart from its background by its border alone. That is the
// fill-on-identical-fill trap Hearth's Main.qml already documents for
// buttons.
//
// `unit` writes a suffix INSIDE the box, right-aligned and muted, which is
// where the design puts "dB", "ms", "Hz" and "dBFS" rather than in a
// separate label after the field.
QQC.TextField {
    id: control

    property string unit: ""

    implicitHeight: Math.max(30, contentHeight + 10)
    color: Theme.text
    font.pixelSize: Theme.fontBody
    font.family: Theme.monoFamily
    leftPadding: Theme.space3
    rightPadding: unitLabel.visible ? unitLabel.implicitWidth + Theme.space3 * 2 : Theme.space3
    verticalAlignment: TextInput.AlignVCenter
    selectByMouse: true

    background: Rectangle {
        color: Theme.neutral100
        border.color: control.activeFocus ? Theme.focusRing : Theme.divider
        border.width: 1
        radius: Theme.radius
    }

    Text {
        id: unitLabel
        visible: control.unit.length > 0
        anchors.right: parent.right
        anchors.rightMargin: Theme.space3
        anchors.verticalCenter: parent.verticalCenter
        text: control.unit
        color: Theme.textMuted
        font.family: Theme.monoFamily
        font.pixelSize: Theme.fontSmall
    }
}

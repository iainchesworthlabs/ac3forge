import QtQuick
import QtQuick.Layouts

import Ac3Forge

// One boxed stat, the shape the design's five-column grids repeat
// (main-play.png, "04 LOUDNESS"/"05 THIS FRAME"): a muted micro-label over a
// bold mono value, in the same neutral100-on-border box PlaySignalPathCard's
// own decode/render/output steps use - a section itself no longer gets that
// box (Card.qml's own `flat`), but an individual stat inside one still does.
Rectangle {
    id: root

    property alias label: labelText.text
    property alias value: valueText.text

    Layout.fillWidth: true
    implicitHeight: column.implicitHeight + Theme.pad
    color: Theme.neutral100
    border.color: Theme.border
    border.width: 1

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: Theme.pad / 2
        spacing: 2

        Text {
            id: labelText
            color: Theme.textMuted
            font.pixelSize: Theme.fontMicro
            font.bold: true
        }

        Text {
            id: valueText
            color: Theme.text
            font.pixelSize: Theme.fontBody
            font.bold: true
            font.family: Theme.monoFamily
        }
    }
}

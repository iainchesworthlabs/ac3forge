import QtQuick

import Ac3ForgeDesk

// The GUI's flat bordered button, as the mockups draw it: 30 px tall,
// 1 px divider border, accent fill when primary.
Rectangle {
    id: root
    property string text: ""
    property bool primary: false
    property bool enabled: true
    signal clicked()

    implicitHeight: 30
    implicitWidth: label.implicitWidth + 24
    color: primary ? Theme.accent : "transparent"
    border.color: Theme.divider
    border.width: 1
    opacity: enabled ? 1.0 : 0.45
    Accessible.role: Accessible.Button
    Accessible.name: root.text
    Accessible.onPressAction: if (root.enabled) root.clicked()

    Text {
        id: label
        anchors.centerIn: parent
        // A button narrower than its label (a long translation in a fixed
        // rail) elides rather than spills.
        width: Math.min(implicitWidth, root.width - 16)
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
        text: root.text
        color: root.primary ? Theme.bg : Theme.text
        font.pixelSize: 13
    }
    MouseArea {
        anchors.fill: parent
        enabled: root.enabled
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}

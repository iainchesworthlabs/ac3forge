import QtQuick
import QtQuick.Layouts

import Ac3ForgeDesk

// An unplaced application in the bed tray. Draggable into a room view;
// a full-screen one shows a lock and cannot be dragged.
Rectangle {
    id: root
    required property var app
    signal clicked()

    implicitHeight: 34
    implicitWidth: row.implicitWidth + 14
    color: Theme.surface
    border.color: Theme.divider
    border.width: 1
    opacity: Drag.active ? 0.6 : 1.0

    Drag.active: dragArea.drag.active && !root.app.fullscreen
    Drag.dragType: Drag.Automatic
    Drag.supportedActions: Qt.MoveAction
    Drag.mimeData: { "app": String(root.app.app) }
    Drag.keys: ["app"]
    Drag.hotSpot.x: width / 2
    Drag.hotSpot.y: height / 2

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: Theme.space2
        Monogram { name: root.app.name; size: 24; fill: root.app.active ? Theme.neutral600 : Theme.neutral500 }
        Text { text: root.app.name; color: Theme.text; font.pixelSize: 13 }
        Canvas {
            visible: root.app.fullscreen
            width: 12; height: 12
            onPaint: {
                const c = getContext("2d");
                c.clearRect(0, 0, width, height);
                c.strokeStyle = Theme.textMuted;
                c.lineWidth = 1.3;
                c.strokeRect(2, 5.5, 8, 5);
                c.beginPath(); c.moveTo(4, 5.5); c.lineTo(4, 3.5); c.arc(6, 3.5, 2, Math.PI, 0); c.lineTo(8, 5.5); c.stroke();
            }
        }
    }
    MouseArea {
        id: dragArea
        anchors.fill: parent
        drag.target: root.app.fullscreen ? null : root
        cursorShape: root.app.fullscreen ? Qt.ArrowCursor : Qt.OpenHandCursor
        onClicked: root.clicked()
        onReleased: if (root.Drag.active) root.Drag.drop()
    }
}

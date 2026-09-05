import QtQuick
import QtQuick.Layouts

import Ac3ForgeCrucible

// One application in the bed tray: icon, name, and a lock when it is
// full-screen. Dragged into a room view to place it.
//
// The drag is Qt Quick's internal one, not the platform's: a platform
// (automatic) drag hands the mouse to Windows' drag loop, which fights the
// MouseArea that moves the chip and loses the press. Internal drags keep
// everything in the scene; the DropArea reads the source item.
Rectangle {
    id: root
    required property var app
    signal clicked()
    // The keyboard's drag: the page places it in the centre of the room.
    signal place()
    objectName: "chip-" + root.app.app
    implicitHeight: Math.max(34, row.implicitHeight + 10)
    implicitWidth: row.implicitWidth + 14
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("%1, in the bed").arg(root.app.name)
    Accessible.description: root.app.fullscreen
        ? qsTr("full-screen: stays in the bed")
        : qsTr("Enter places it in the centre of the room")
    Accessible.focusable: !root.app.fullscreen
    Accessible.onPressAction: root.place()

    // A full-screen application cannot leave the bed, so its chip is not a
    // tab stop: Tab walks what can be acted on.
    activeFocusOnTab: !root.app.fullscreen
    Keys.onPressed: function(event) {
        if (root.app.fullscreen) {
            return;
        }
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            root.clicked();
            root.place();
            event.accepted = true;
        }
    }
    color: Theme.surface
    border.color: Theme.divider
    border.width: 1
    opacity: Drag.active ? 0.6 : 1.0
    z: Drag.active ? 10 : 0
    Drag.active: dragArea.drag.active && !root.app.fullscreen
    Drag.dragType: Drag.Internal
    Drag.supportedActions: Qt.MoveAction
    Drag.keys: ["app"]
    Drag.hotSpot.x: width / 2
    Drag.hotSpot.y: height / 2
    Drag.source: root
    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: Theme.space2
        AppIcon { name: root.app.name; imagePath: root.app.imagePath; iconName: root.app.iconName; appId: root.app.appId; size: 24; fill: root.app.active ? Theme.neutral600 : Theme.neutral500; dimmed: root.app.silent }
        Text { text: root.app.name; color: root.app.silent ? Theme.textMuted : Theme.text; font.pixelSize: Theme.fontBody; elide: Text.ElideRight; Layout.maximumWidth: 160 }
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
    // Where the chip sat before a drag, so an unaccepted drop puts it back
    // rather than leaving it where the mouse let go.
    property real homeX: 0
    property real homeY: 0
    MouseArea {
        id: dragArea
        anchors.fill: parent
        drag.target: root.app.fullscreen ? null : root
        drag.threshold: 4
        cursorShape: root.app.fullscreen ? Qt.ArrowCursor : Qt.OpenHandCursor
        preventStealing: true
        onPressed: { root.homeX = root.x; root.homeY = root.y; }
        // A chip IS where a keyboard session continues from - Enter on it
        // places the application - so a click on one takes focus.
        onClicked: { root.forceActiveFocus(); root.clicked(); }
        onReleased: {
            if (root.Drag.active) {
                root.Drag.drop();
            }
            root.x = root.homeX;
            root.y = root.homeY;
        }
    }
    FocusRing {}
}

import QtQuick
import QtQuick.Layouts

import Ac3ForgeDesk

// One of the two room views. `elevation` false: the plan (x across, y
// down, front at the top). `elevation` true: the side view (y across, z
// up, ceiling at the top). Markers for placed applications are dragged
// here; a drop from the bed tray lands here too.
//
// Room coordinates are ac3::oba::Position's: x 0..1 left to right, y 0..1
// front to back, z -1..1 floor to ceiling.
Item {
    id: root
    property bool elevation: false
    property var apps: []
    property int selectedApp: -1
    property string caption: ""
    property string hint: ""
    signal select(int app)
    signal moved(int app, double x, double y, double z)
    signal returned(int app)
    signal dropped(int app, double x, double y)

    implicitWidth: 400
    implicitHeight: elevation ? 170 + heading.implicitHeight + Theme.space2 : 400 + heading.implicitHeight + Theme.space2

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space2
        RowLayout {
            id: heading
            Layout.fillWidth: true
            Text { text: root.caption; color: Theme.textMuted; font.pixelSize: 11; font.letterSpacing: 1; font.capitalization: Font.AllUppercase }
            Item { Layout.fillWidth: true }
            Text { text: root.hint; color: Theme.textMuted; font.pixelSize: 11 }
        }
        Rectangle {
            id: field
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.neutral100
            border.color: dropArea.containsDrag ? Theme.accent : Theme.divider
            border.width: 1

            // Crosshair and axis labels.
            Rectangle { visible: !root.elevation; x: parent.width / 2; y: 0; width: 1; height: parent.height; color: Theme.neutral300 }
            Rectangle { x: 0; y: parent.height / 2; width: parent.width; height: 1; color: Theme.neutral300 }
            Text { x: 8; y: 6; text: root.elevation ? qsTr("ceiling") : qsTr("front"); color: Theme.neutral500; font.pixelSize: 11 }
            Text { x: 8; y: parent.height - 18; text: root.elevation ? qsTr("floor") : qsTr("rear"); color: Theme.neutral500; font.pixelSize: 11 }
            Text { visible: root.elevation; x: 8; y: parent.height / 2 - 14; text: qsTr("ear level"); color: Theme.neutral500; font.pixelSize: 11 }
            Text { visible: root.elevation; x: 60; y: parent.height - 18; text: qsTr("front"); color: Theme.neutral500; font.pixelSize: 11 }
            Text { visible: root.elevation; x: parent.width - 34; y: parent.height - 18; text: qsTr("rear"); color: Theme.neutral500; font.pixelSize: 11 }

            // Speakers, plan only: the bed's five positions.
            Repeater {
                model: root.elevation ? [] : [
                    { label: qsTr("L"), px: 0.15, py: 0.10 }, { label: qsTr("R"), px: 0.85, py: 0.10 }, { label: qsTr("C"), px: 0.50, py: 0.08 },
                    { label: qsTr("Ls"), px: 0.15, py: 0.87 }, { label: qsTr("Rs"), px: 0.85, py: 0.87 }]
                delegate: Item {
                    required property var modelData
                    x: field.width * modelData.px
                    y: field.height * modelData.py
                    Rectangle { x: -5; y: -5; width: 10; height: 10; color: "transparent"; border.color: Theme.neutral500; border.width: 1 }
                    Text { anchors.horizontalCenter: parent.left; y: 8; text: modelData.label; color: Theme.neutral500; font.family: Theme.monoFamily; font.pixelSize: 10 }
                }
            }
            // The listener.
            Rectangle {
                visible: !root.elevation
                x: field.width / 2 - 5; y: field.height / 2 - 5
                width: 10; height: 10
                color: Theme.neutral600
                rotation: 45
            }

            // Markers.
            Repeater {
                model: root.apps
                delegate: Item {
                    id: marker
                    required property var modelData
                    readonly property bool placed: modelData.slot >= 0
                    readonly property bool selected: modelData.app === root.selectedApp
                    visible: placed
                    // Position from the engine unless a drag is in progress.
                    property bool dragging: false
                    x: dragging ? x : (root.elevation ? modelData.y : modelData.x) * field.width
                    y: dragging ? y : (root.elevation ? (1 - modelData.z) / 2 : modelData.y) * field.height
                    z: selected ? 2 : 1
                    Behavior on x { enabled: !marker.dragging; NumberAnimation { duration: 90 } }
                    Behavior on y { enabled: !marker.dragging; NumberAnimation { duration: 90 } }

                    // Elevation: a stem down to ear level.
                    Rectangle {
                        visible: root.elevation
                        x: 0
                        y: Math.min(0, field.height / 2 - marker.y)
                        width: 1
                        height: Math.abs(field.height / 2 - marker.y)
                        color: Theme.accent400
                    }
                    // A split application: the pair's two objects sit either
                    // side of the marker (the engine's spread, 0.15 of the
                    // room width, in the plan; the elevation shows one).
                    Repeater {
                        model: !root.elevation && marker.modelData.width === 2 ? [-1, 1] : []
                        delegate: Rectangle {
                            required property int modelData
                            x: modelData * 0.15 * field.width - 4
                            y: -4
                            width: 8; height: 8; radius: 4
                            color: marker.selected ? Theme.accent : Theme.neutral500
                            border.color: Theme.bg
                            border.width: 1
                        }
                    }
                    Row {
                        x: -13; y: -13
                        spacing: 6
                        Monogram { name: marker.modelData.name; size: 26; fill: marker.selected ? Theme.accent600 : Theme.neutral700 }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: marker.modelData.name + (root.elevation ? " · z " + (marker.modelData.z >= 0 ? "+" : "") + marker.modelData.z.toFixed(2) : "")
                            color: Theme.text
                            font.family: Theme.monoFamily
                            font.pixelSize: 11
                        }
                    }
                    Rectangle {
                        x: -14; y: -14; width: 28; height: 28
                        color: "transparent"
                        border.color: marker.selected ? Theme.accent : Theme.divider
                        border.width: marker.selected ? 2 : 1
                    }
                    MouseArea {
                        x: -14; y: -14; width: 28; height: 28
                        cursorShape: Qt.SizeAllCursor
                        onPressed: { root.select(marker.modelData.app); marker.dragging = true; }
                        onPositionChanged: function(mouse) {
                            if (!marker.dragging) return;
                            const p = mapToItem(field, mouse.x, mouse.y);
                            marker.x = Math.max(0, Math.min(field.width, p.x));
                            marker.y = Math.max(0, Math.min(field.height, p.y));
                            marker.emitMove();
                        }
                        onReleased: { marker.emitMove(); marker.dragging = false; }
                        onDoubleClicked: root.returned(marker.modelData.app)
                    }
                    function emitMove() {
                        const u = marker.x / field.width;
                        const v = marker.y / field.height;
                        if (root.elevation) {
                            root.moved(marker.modelData.app, marker.modelData.x, u, 1 - 2 * v);
                        } else {
                            root.moved(marker.modelData.app, u, v, marker.modelData.z);
                        }
                    }
                }
            }

            // A chip dragged in from the bed tray.
            DropArea {
                id: dropArea
                anchors.fill: parent
                keys: ["app"]
                onDropped: function(drop) {
                    root.dropped(parseInt(drop.getDataAsString("app")), drop.x / field.width, drop.y / field.height);
                    drop.accept();
                }
            }
        }
    }
}

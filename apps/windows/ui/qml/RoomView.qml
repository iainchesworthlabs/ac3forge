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
//
// Dragging: a marker follows the engine's position except while it is
// being dragged, when it follows the mouse and tells the engine where it
// is. The two are kept in separate properties (the engine's position
// arrives as a binding, the drag position is assigned) so a drag never
// breaks the binding, and the entries behind `apps` are live objects, so a
// position or level change never rebuilds the markers mid-drag.
Item {
    id: root
    property bool elevation: false
    property var apps: []
    property int selectedApp: -1
    property string caption: ""
    property string hint: ""
    signal select(int app)
    signal moved(int app, double x, double y, double z)
    signal movedSide(int app, int side, double x, double y, double z)
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
            Text { text: root.caption; color: Theme.textMuted; font.pixelSize: 11; font.letterSpacing: 1; font.capitalization: Font.AllUppercase; elide: Text.ElideRight; Layout.fillWidth: true }
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
                    readonly property var app: modelData
                    readonly property bool placed: app.slot >= 0
                    readonly property bool selected: app.app === root.selectedApp
                    visible: placed
                    // Where the engine has it, in field pixels.
                    readonly property real engineX: (root.elevation ? app.y : app.x) * field.width
                    readonly property real engineY: (root.elevation ? (1 - app.z) / 2 : app.y) * field.height
                    // Where the mouse has it, while dragging.
                    property bool dragging: false
                    property real dragX: 0
                    property real dragY: 0
                    // After a release the engine's position arrives through the
                    // poll a moment later; until it changes, the marker stays where
                    // it was dropped rather than snapping to the stale value.
                    property bool settling: false
                    onEngineXChanged: settling = false
                    onEngineYChanged: settling = false
                    // While one of a pair's objects is being dragged, the marker
                    // (the pair's centre) follows the midpoint of the dragged
                    // object and the other one locally, rather than waiting for
                    // the engine's answer through the poll, which arrived in
                    // 120 ms steps and restarted the glide each time.
                    property int sideDragging: -1
                    property real sideDragX: 0
                    property real sideDragY: 0
                    readonly property real otherX: (root.elevation ? (sideDragging === 0 ? app.ry : app.ly) : (sideDragging === 0 ? app.rx : app.lx)) * field.width
                    readonly property real otherY: (root.elevation ? (1 - (sideDragging === 0 ? app.rz : app.lz)) / 2 : (sideDragging === 0 ? app.ry : app.ly)) * field.height
                    x: dragging || settling ? dragX : (sideDragging >= 0 ? (sideDragX + otherX) / 2 : engineX)
                    y: dragging || settling ? dragY : (sideDragging >= 0 ? (sideDragY + otherY) / 2 : engineY)
                    z: selected ? 2 : 1
                    // The glide is for the engine's steps, which are small.
                    // A move across a good part of the field is the field
                    // itself being sized (the views sit in a hidden layout
                    // while the 3D room shows, at no size until they are
                    // first shown), so the marker jumps rather than sets
                    // off from the corner.
                    Behavior on x {
                        id: glideX
                        enabled: !marker.dragging && marker.sideDragging < 0
                        NumberAnimation { duration: Math.abs(glideX.targetValue - marker.x) > field.width * 0.35 ? 0 : 90 }
                    }
                    Behavior on y {
                        id: glideY
                        enabled: !marker.dragging && marker.sideDragging < 0
                        NumberAnimation { duration: Math.abs(glideY.targetValue - marker.y) > field.height * 0.35 ? 0 : 90 }
                    }

                    // Elevation: a stem down to ear level.
                    Rectangle {
                        visible: root.elevation
                        x: 0
                        y: Math.min(0, field.height / 2 - marker.y)
                        width: 1
                        height: Math.abs(field.height / 2 - marker.y)
                        color: Theme.accent400
                    }
                    // A split application: the pair's two objects, each a
                    // marker of its own that can be dragged where it should
                    // go (the engine keeps the pair's centre between them).
                    Repeater {
                        model: marker.app.width === 2 ? [0, 1] : []
                        delegate: Item {
                            id: satellite
                            required property int modelData
                            readonly property int side: modelData
                            readonly property real sx: side === 0 ? marker.app.lx : marker.app.rx
                            readonly property real sy: side === 0 ? marker.app.ly : marker.app.ry
                            readonly property real sz: side === 0 ? marker.app.lz : marker.app.rz
                            // Field position, relative to the marker (which
                            // sits at the pair's centre).
                            readonly property real fieldX: (root.elevation ? sy : sx) * field.width
                            readonly property real fieldY: (root.elevation ? (1 - sz) / 2 : sy) * field.height
                            property bool dragging: false
                            property real dragX: 0
                            property real dragY: 0
                            property bool settling: false
                            onFieldXChanged: settling = false
                            onFieldYChanged: settling = false
                            // In the elevation view a standard pair sits at the
                            // centre's depth and height, under its icon; nudge the
                            // two apart so both stay reachable.
                            readonly property real nudge: root.elevation && Math.abs(fieldX - marker.engineX) < 12 && Math.abs(fieldY - marker.engineY) < 12 ? (side === 0 ? -18 : 18) : 0
                            x: (dragging || settling ? dragX : fieldX + nudge) - marker.x
                            y: (dragging || settling ? dragY : fieldY) - marker.y
                            z: 3
                            Behavior on x {
                                id: sideGlideX
                                enabled: !satellite.dragging
                                NumberAnimation { duration: Math.abs(sideGlideX.targetValue - satellite.x) > field.width * 0.35 ? 0 : 90 }
                            }
                            Behavior on y {
                                id: sideGlideY
                                enabled: !satellite.dragging
                                NumberAnimation { duration: Math.abs(sideGlideY.targetValue - satellite.y) > field.height * 0.35 ? 0 : 90 }
                            }
                            Rectangle {
                                x: -5; y: -5
                                width: 10; height: 10; radius: 5
                                color: marker.selected ? Theme.accent : Theme.neutral500
                                border.color: Theme.bg
                                border.width: 1
                            }
                            Text {
                                x: 7; y: -7
                                text: satellite.side === 0 ? "L" : "R"
                                color: Theme.textMuted
                                font.family: Theme.monoFamily
                                font.pixelSize: 9
                            }
                            MouseArea {
                                x: -8; y: -8; width: 16; height: 16
                                cursorShape: Qt.SizeAllCursor
                                preventStealing: true
                                onPressed: {
                                    root.select(marker.app.app);
                                    satellite.dragX = satellite.fieldX;
                                    satellite.dragY = satellite.fieldY;
                                    satellite.dragging = true;
                                    marker.sideDragX = satellite.fieldX;
                                    marker.sideDragY = satellite.fieldY;
                                    marker.sideDragging = satellite.side;
                                }
                                onPositionChanged: function(mouse) {
                                    if (!satellite.dragging) return;
                                    const p = mapToItem(field, mouse.x, mouse.y);
                                    satellite.dragX = Math.max(0, Math.min(field.width, p.x));
                                    satellite.dragY = Math.max(0, Math.min(field.height, p.y));
                                    marker.sideDragX = satellite.dragX;
                                    marker.sideDragY = satellite.dragY;
                                    satellite.emitMove();
                                }
                                onReleased: { if (satellite.dragging) { satellite.emitMove(); satellite.settling = true; satellite.dragging = false; marker.sideDragging = -1; } }
                                onCanceled: { satellite.dragging = false; marker.sideDragging = -1; }
                            }
                            function emitMove() {
                                const u = satellite.dragX / field.width;
                                const v = satellite.dragY / field.height;
                                if (root.elevation) {
                                    root.movedSide(marker.app.app, satellite.side, satellite.sx, u, 1 - 2 * v);
                                } else {
                                    root.movedSide(marker.app.app, satellite.side, u, v, satellite.sz);
                                }
                            }
                        }
                    }
                    AppIcon { x: -13; y: -13; name: marker.app.name; imagePath: marker.app.imagePath; size: 26; fill: marker.selected ? Theme.accent600 : Theme.neutral700; dimmed: marker.app.silent }
                    Text {
                        id: markerLabel
                        // Right of the icon, or left of it when that would run
                        // off the field's right edge.
                        readonly property bool flip: marker.x + 19 + implicitWidth > field.width - 4
                        x: flip ? -19 - implicitWidth : 19
                        y: -implicitHeight / 2
                        text: marker.app.name
                        color: Theme.text
                        font.family: Theme.monoFamily
                        font.pixelSize: 11
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
                        preventStealing: true
                        onPressed: function(mouse) {
                            root.select(marker.app.app);
                            marker.dragX = marker.x;
                            marker.dragY = marker.y;
                            marker.dragging = true;
                        }
                        onPositionChanged: function(mouse) {
                            if (!marker.dragging) return;
                            const p = mapToItem(field, mouse.x, mouse.y);
                            marker.dragX = Math.max(0, Math.min(field.width, p.x));
                            marker.dragY = Math.max(0, Math.min(field.height, p.y));
                            marker.emitMove();
                        }
                        onReleased: { if (marker.dragging) { marker.emitMove(); marker.settling = true; marker.dragging = false; } }
                        onCanceled: marker.dragging = false
                        onDoubleClicked: root.returned(marker.app.app)
                    }
                    function emitMove() {
                        const u = marker.dragX / field.width;
                        const v = marker.dragY / field.height;
                        if (root.elevation) {
                            root.moved(marker.app.app, marker.app.x, u, 1 - 2 * v);
                        } else {
                            root.moved(marker.app.app, u, v, marker.app.z);
                        }
                    }
                }
            }

            // A chip dragged in from the bed tray (an internal drag: the
            // chip itself is the source).
            DropArea {
                id: dropArea
                anchors.fill: parent
                keys: ["app"]
                onDropped: function(drop) {
                    const source = drop.source;
                    if (source && source.app) {
                        root.dropped(source.app.app, drop.x / field.width, drop.y / field.height);
                        drop.accept(Qt.MoveAction);
                    }
                }
            }
        }
    }
}

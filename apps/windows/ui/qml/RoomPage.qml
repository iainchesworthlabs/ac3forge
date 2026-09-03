import QtQuick
import QtQuick.Layouts

import Ac3ForgeDesk

// The Room: the application rail, the plan and elevation views, the bed
// tray, and the output summary (docs/platforms/windows-demo.md, "UI").
//
// Sizing: the rails keep a preferred width and give way a little; the
// centre column takes the rest and the room views scale with it, side by
// side when there is room for two and stacked when there is not, so a
// narrow window shrinks the room rather than pushing the right rail off
// the edge, and a wide one gets a bigger room rather than a gap. The
// centre scrolls vertically when the window is short.
Item {
    id: page
    // The room view the user chose last time, or what the window asks for.
    property bool threeD: DeskController.roomView === "3d"
    property int selectedApp: -1

    function appById(id) {
        const apps = DeskController.apps;
        for (let i = 0; i < apps.length; ++i) if (apps[i].app === id) return apps[i];
        return null;
    }
    readonly property var selected: appById(selectedApp)

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // --- 01 applications --------------------------------------------------
        Rectangle {
            Layout.preferredWidth: 320
            Layout.minimumWidth: 240
            Layout.maximumWidth: 360
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "transparent"
            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.divider }
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.space4
                spacing: Theme.space3
                RailBlock {
                    ordinal: "01"
                    Layout.fillHeight: false
                    label: qsTr("APPLICATIONS")
                    Layout.fillWidth: true
                    Text {
                        text: DeskController.apps.length === 0 ? qsTr("nothing is playing") : DeskController.apps.length + qsTr(" with sound")
                        color: Theme.textMuted
                        font.family: Theme.monoFamily
                        font.pixelSize: 11
                    }
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 2
                    model: DeskController.apps
                    delegate: AppRow {
                        required property var modelData
                        width: ListView.view.width
                        app: modelData
                        selected: modelData.app === page.selectedApp
                        onClicked: page.selectedApp = modelData.app
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Full-screen applications stay in the bed; the lock lifts when they leave full-screen.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.WordWrap
                }
            }
        }

        // --- 02 room / 03 bed ---------------------------------------------------
        Flickable {
            id: centre
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 420
            clip: true
            contentWidth: width
            contentHeight: centreColumn.implicitHeight + Theme.space4 * 2
            // The side of one room view: two of them across the column when
            // that leaves each at least 240 px, else one at a time; never
            // above 560, which is where the markers stop reading as a room.
            readonly property real innerWidth: width - Theme.space6 * 2
            readonly property bool sideBySide: innerWidth >= 240 * 2 + Theme.space6
            readonly property real viewSide: sideBySide
                ? Math.max(240, Math.min(560, (innerWidth - Theme.space6) / 2))
                : Math.max(240, Math.min(560, innerWidth))
            ColumnLayout {
                id: centreColumn
                x: Theme.space6
                y: Theme.space4
                width: centre.innerWidth
                spacing: Theme.space4
                RailBlock {
                    ordinal: "02"
                    Layout.fillHeight: false
                    label: qsTr("ROOM")
                    Layout.fillWidth: true
                    Text { text: DeskController.placedCount + qsTr(" of 10 slots placed"); color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: 11 }
                }
                // Bed only: placement pans within 5.1, and height does nothing.
                // Said here, where the placing happens, rather than left for
                // the receiver to not show.
                Rectangle {
                    Layout.fillWidth: true
                    visible: !DeskController.objectsEnabled
                    implicitHeight: noticeText.implicitHeight + Theme.space3 * 2
                    color: Theme.neutral100
                    border.color: Theme.accent700
                    border.width: 1
                    Text {
                        id: noticeText
                        anchors.fill: parent
                        anchors.margins: Theme.space3
                        text: qsTr("No signing key, so objects are off: the stream is the 5.1 bed only. Placing an application pans it between the five bed speakers; height and size have no effect, and a receiver renders nothing you place. Load a key in Settings to send objects.")
                        color: Theme.text
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }
                // The 3D picture of the room, when the build has Qt Quick 3D;
                // placement stays in the plan and elevation.
                RowLayout {
                    Layout.fillWidth: true
                    visible: DeskController.has3D
                    Item { Layout.fillWidth: true }
                    SegmentedControl {
                        objectName: "roomViewChoice"
                        model: [{ label: qsTr("Plan + elevation"), value: "2d" }, { label: qsTr("3D"), value: "3d" }]
                        currentValue: page.threeD ? "3d" : "2d"
                        accessibleName: qsTr("Room view")
                        onSelected: function(value) { DeskController.roomView = value; page.threeD = value === "3d"; }
                    }
                }
                Loader {
                    id: room3d
                    objectName: "room3dLoader"
                    visible: page.threeD
                    active: page.threeD && DeskController.has3D
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: centre.sideBySide ? centre.viewSide * 2 + Theme.space6 : centre.viewSide
                    Layout.preferredHeight: centre.viewSide + 22
                    source: "Room3DView.qml"
                    onLoaded: {
                        item.caption = qsTr("Room (3D)");
                        item.apps = Qt.binding(function() { return DeskController.apps; });
                        item.selectedApp = Qt.binding(function() { return page.selectedApp; });
                        item.select.connect(function(app) { page.selectedApp = app; });
                        item.moved.connect(function(app, x, y, z) { DeskController.position(app, x, y, z); });
                    }
                }
                GridLayout {
                    visible: !page.threeD
                    Layout.alignment: Qt.AlignHCenter
                    columns: centre.sideBySide ? 2 : 1
                    columnSpacing: Theme.space6
                    rowSpacing: Theme.space4
                    RoomView {
                        id: plan
                        Layout.preferredWidth: centre.viewSide
                        Layout.preferredHeight: centre.viewSide + 22
                        Layout.alignment: Qt.AlignTop
                        caption: qsTr("Plan (top-down)")
                        hint: qsTr("drag to place")
                        apps: DeskController.apps
                        selectedApp: page.selectedApp
                        onSelect: function(app) { page.selectedApp = app; }
                        onMoved: function(app, x, y, z) { DeskController.position(app, x, y, z); }
                        onReturned: function(app) { DeskController.unposition(app); }
                        onDropped: function(app, x, y) { DeskController.position(app, x, y, 0); page.selectedApp = app; }
                    }
                    ColumnLayout {
                        Layout.preferredWidth: centre.viewSide
                        Layout.alignment: Qt.AlignTop
                        spacing: Theme.space2
                        RoomView {
                            elevation: true
                            Layout.preferredWidth: centre.viewSide
                            Layout.preferredHeight: Math.round(centre.viewSide * 0.425) + 22
                            caption: qsTr("Elevation (side-on)")
                            hint: DeskController.objectsEnabled ? qsTr("drag: depth + height") : qsTr("height: objects only")
                            opacity: DeskController.objectsEnabled ? 1.0 : 0.55
                            apps: DeskController.apps
                            selectedApp: page.selectedApp
                            onSelect: function(app) { page.selectedApp = app; }
                            onMoved: function(app, x, y, z) { DeskController.position(app, x, y, z); }
                            onReturned: function(app) { DeskController.unposition(app); }
                            onDropped: function(app, x, y) { DeskController.position(app, 0.5, x, 1 - 2 * y); page.selectedApp = app; }
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: !DeskController.objectsEnabled
                            text: qsTr("Bed only: depth still pans front to rear; height is carried in object metadata, which is off.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                    }
                }
                // The selected application.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: selectedColumn.implicitHeight + Theme.space6
                    color: Theme.surface
                    border.color: Theme.divider
                    border.width: 1
                    visible: page.selected !== null
                    ColumnLayout {
                        id: selectedColumn
                        anchors.fill: parent
                        anchors.margins: Theme.space3
                        spacing: Theme.space2
                        RowLayout {
                            spacing: Theme.space2
                            Text { text: page.selected ? page.selected.name : ""; color: Theme.text; font.family: Theme.headingFamily; font.pixelSize: 15; font.weight: Font.DemiBold; elide: Text.ElideRight; Layout.maximumWidth: 260 }
                            Text {
                                Layout.fillWidth: true
                                text: page.selected ? (page.selected.slot >= 0
                                    ? qsTr("slot ") + (page.selected.slot + 1) + " · x " + page.selected.x.toFixed(2) + " · y " + page.selected.y.toFixed(2) + " · z " + (page.selected.z >= 0 ? "+" : "") + page.selected.z.toFixed(2)
                                    : qsTr("in the bed")) : ""
                                color: Theme.textMuted
                                font.family: Theme.monoFamily
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: Theme.space2
                            DeskButton {
                                text: page.selected && page.selected.slot >= 0 ? qsTr("Send to bed") : qsTr("Place in the room")
                                enabled: page.selected !== null && !(page.selected.fullscreen)
                                onClicked: {
                                    if (page.selected.slot >= 0) DeskController.unposition(page.selected.app);
                                    else DeskController.position(page.selected.app, 0.5, 0.5, 0);
                                }
                            }
                            DeskButton {
                                text: qsTr("Centre")
                                enabled: page.selected !== null && page.selected.slot >= 0
                                onClicked: DeskController.position(page.selected.app, 0.5, 0.5, 0)
                            }
                            DeskButton {
                                objectName: "splitButton"
                                text: page.selected && page.selected.width === 2 ? qsTr("Mono") : qsTr("Split")
                                enabled: page.selected !== null
                                onClicked: DeskController.setSplit(page.selected.app, page.selected.width !== 2)
                            }
                        }
                        // Quick placements, for lining things up without a drag.
                        Flow {
                            Layout.fillWidth: true
                            spacing: Theme.space2
                            Text { height: 26; verticalAlignment: Text.AlignVCenter; text: qsTr("Put"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall }
                            Repeater {
                                model: [
                                    { label: qsTr("in front"), x: 0.5, y: 0.1, z: 0.0 },
                                    { label: qsTr("behind"), x: 0.5, y: 0.9, z: 0.0 },
                                    { label: qsTr("left"), x: 0.1, y: 0.5, z: 0.0 },
                                    { label: qsTr("right"), x: 0.9, y: 0.5, z: 0.0 },
                                    { label: qsTr("overhead"), x: 0.5, y: 0.5, z: 0.8 },
                                    { label: qsTr("front left"), x: 0.15, y: 0.15, z: 0.0 },
                                    { label: qsTr("front right"), x: 0.85, y: 0.15, z: 0.0 },
                                    { label: qsTr("rear left"), x: 0.15, y: 0.85, z: 0.0 },
                                    { label: qsTr("rear right"), x: 0.85, y: 0.85, z: 0.0 }]
                                delegate: DeskButton {
                                    required property var modelData
                                    text: modelData.label
                                    enabled: page.selected !== null && !(page.selected.fullscreen)
                                    onClicked: { DeskController.position(page.selected.app, modelData.x, modelData.y, modelData.z); }
                                }
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: page.selected ? (page.selected.fullscreen ? qsTr("full-screen: stays in the bed") : (page.selected.slot >= 0 ? page.describe(page.selected) : "")) : ""
                            visible: text.length > 0
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                        RowLayout {
                            spacing: Theme.space3
                            Text { text: qsTr("Size"); color: Theme.text; font.pixelSize: 13 }
                            Rectangle {
                                id: sizeTrack
                                objectName: "sizeSlider"
                                implicitWidth: 180
                                implicitHeight: 14
                                color: "transparent"
                                opacity: DeskController.objectsEnabled ? 1.0 : 0.55
                                readonly property real value: page.selected ? page.selected.size : 0
                                Rectangle { y: 6; width: parent.width; height: 2; color: Theme.divider }
                                Rectangle { y: 6; width: parent.width * sizeTrack.value; height: 2; color: Theme.accent }
                                Rectangle { x: parent.width * sizeTrack.value - 5; y: 2; width: 10; height: 10; radius: 5; color: Theme.accent }
                                MouseArea {
                                    anchors.fill: parent
                                    enabled: page.selected !== null
                                    cursorShape: Qt.PointingHandCursor
                                    function apply(mouse) { DeskController.setSize(page.selected.app, Math.max(0, Math.min(1, mouse.x / width))); }
                                    onPressed: function(mouse) { apply(mouse); }
                                    onPositionChanged: function(mouse) { if (mouse.buttons & Qt.LeftButton) apply(mouse); }
                                }
                                Accessible.role: Accessible.Slider
                                Accessible.name: qsTr("Object size")
                            }
                            Text { text: page.selected ? (page.selected.size === 0 ? qsTr("point") : Math.round(page.selected.size * 100) + "%") : ""; color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: 11 }
                            Text {
                                Layout.fillWidth: true
                                text: DeskController.objectsEnabled ? qsTr("extent the receiver's renderer spreads the object over; the bed hears a point") : qsTr("object metadata: no effect while the stream is bed only")
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSmall
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
                RailBlock {
                    ordinal: "03"
                    Layout.fillHeight: false
                    label: qsTr("BED")
                    Layout.fillWidth: true
                    Text { text: qsTr("unplaced applications, mixed to the 5.1 bed"); color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: 11 }
                }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: bedFlow.implicitHeight + Theme.space2 * 2
                    color: "transparent"
                    border.color: Theme.divider
                    border.width: 1
                    Flow {
                        id: bedFlow
                        anchors.fill: parent
                        anchors.margins: Theme.space2
                        spacing: Theme.space2
                        Repeater {
                            model: DeskController.apps
                            delegate: BedChip {
                                required property var modelData
                                visible: modelData.slot < 0
                                app: modelData
                                onClicked: page.selectedApp = modelData.app
                            }
                        }
                        Text {
                            height: 34
                            width: Math.min(implicitWidth, bedFlow.width - Theme.space2)
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: Theme.space2
                            text: DeskController.bedCount === 0 ? qsTr("every application is placed") : qsTr("drag one into the room to place it · drag a marker back here, or double-click it, to return it")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            elide: Text.ElideRight
                        }
                    }
                    DropArea {
                        anchors.fill: parent
                        keys: ["marker"]
                    }
                }
            }
        }

        // --- 04 output / 05 default / 06 signing ---------------------------------
        Rectangle {
            Layout.preferredWidth: 280
            Layout.minimumWidth: 220
            Layout.maximumWidth: 320
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "transparent"
            Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: Theme.divider }
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.space4
                spacing: Theme.space3
                RailBlock { ordinal: "04"; label: qsTr("OUTPUT"); Layout.fillWidth: true; Layout.fillHeight: false }
                Card {
                    Layout.fillHeight: false
                    ColumnLayout {
                        spacing: Theme.space2
                        Text { Layout.fillWidth: true; text: DeskController.modeName; color: Theme.text; font.family: Theme.headingFamily; font.pixelSize: 18; font.weight: Font.DemiBold; elide: Text.ElideRight }
                        Text { Layout.fillWidth: true; text: DeskController.endpointName; color: Theme.text; font.pixelSize: 13; elide: Text.ElideRight; visible: text.length > 0 }
                        Text { Layout.fillWidth: true; text: DeskController.outputReason; color: Theme.textMuted; font.pixelSize: Theme.fontSmall; wrapMode: Text.WordWrap }
                        RowLayout {
                            Text { Layout.fillWidth: true; text: qsTr("pin: ") + DeskController.pinned; color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: 11; elide: Text.ElideRight }
                            DeskButton { text: qsTr("Output…"); onClicked: page.parent.parent.page = "output" }
                        }
                    }
                }
                RailBlock { ordinal: "05"; label: qsTr("DEFAULT OUTPUT"); Layout.fillWidth: true; Layout.fillHeight: false }
                Card {
                    Layout.fillHeight: false
                    ColumnLayout {
                        spacing: Theme.space2
                        RowLayout {
                            spacing: Theme.space2
                            Rectangle { width: 8; height: 8; color: DeskController.defaultIsNullSink ? Theme.accent : Theme.neutral500 }
                            Text { Layout.fillWidth: true; text: DeskController.defaultOutputName.length ? DeskController.defaultOutputName : qsTr("no default output"); color: Theme.text; font.pixelSize: 13; elide: Text.ElideRight }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: DeskController.defaultIsNullSink
                                ? qsTr("Applications render here, silently.") + (DeskController.previousDefaultName.length ? " " + DeskController.previousDefaultName + qsTr(" is restored on quit.") : "")
                                : qsTr("Applications still render to a real device, so you hear them directly as well. Move the default to the silent device to fix that.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                        DeskButton {
                            Layout.fillWidth: true
                            text: DeskController.defaultIsNullSink
                                ? qsTr("Restore ") + (DeskController.previousDefaultName.length ? DeskController.previousDefaultName : qsTr("previous output"))
                                : qsTr("Move default to ") + DeskController.nullSinkName
                            enabled: DeskController.defaultIsNullSink ? DeskController.previousDefaultName.length > 0 : DeskController.nullSinkPresent
                            onClicked: DeskController.defaultIsNullSink ? DeskController.restoreDefault() : DeskController.moveDefaultToNullSink()
                        }
                        Text { Layout.fillWidth: true; visible: DeskController.defaultMessage.length > 0; text: DeskController.defaultMessage; color: Theme.accent; font.pixelSize: Theme.fontSmall; wrapMode: Text.WordWrap }
                    }
                }
                RailBlock { ordinal: "06"; label: qsTr("SIGNING"); Layout.fillWidth: true; Layout.fillHeight: false }
                Text { Layout.fillWidth: true; text: DeskController.objectsEnabled ? qsTr("key loaded · objects on") : qsTr("no key · 5.1 bed only"); color: Theme.text; font.pixelSize: 13; elide: Text.ElideRight }
                Text { Layout.fillWidth: true; text: DeskController.keyPath.length ? DeskController.keyPath : qsTr("load one in Settings"); color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: 11; elide: Text.ElideMiddle }
                Item { Layout.fillHeight: true }
            }
        }
    }

    // A plain-words reading of a position, for the selected card.
    function describe(app) {
        const lr = app.x < 0.35 ? qsTr("left") : app.x > 0.65 ? qsTr("right") : qsTr("centre");
        const fb = app.y < 0.35 ? qsTr("in front of you") : app.y > 0.65 ? qsTr("behind you") : qsTr("beside you");
        const ud = app.z > 0.3 ? qsTr("up") : app.z < -0.3 ? qsTr("low") : "";
        return [ud, fb, lr === "centre" ? "" : qsTr("to the ") + lr].filter(s => s.length).join(", ");
    }
}

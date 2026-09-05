import QtQuick
import QtQuick.Layouts

import Ac3ForgeCrucible

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
    property bool threeD: CrucibleController.roomView === "3d"
    property int selectedApp: -1
    // The Output card's button: the window switches pages.
    signal openOutput()

    function appById(id) {
        const apps = CrucibleController.apps;
        for (let i = 0; i < apps.length; ++i) if (apps[i].app === id) return apps[i];
        return null;
    }
    readonly property var selected: appById(selectedApp)

    function indexOfApp(id) {
        const apps = CrucibleController.apps;
        for (let i = 0; i < apps.length; ++i) if (apps[i].app === id) return i;
        return -1;
    }
    // The list's current row and the page's selection are two views of one
    // thing, and the selection is the one that decides. The row follows it
    // and never the other way about, because the row is not always moved by
    // a person: a ListView writes its own currentIndex back to 0 whenever
    // the VALUE of its model changes, and the controller replaces the
    // applications list on every poll where the membership or the
    // sound-first order moved - an application starting, exiting, or merely
    // going quiet. A row that led the selection would hand the room's arrow
    // keys to whatever had just floated to the top of the rail.
    //
    // So: the arrow keys and a click set the selection, and anything else
    // that moves the row is put back.
    function syncListRow() {
        const row = page.indexOfApp(page.selectedApp);
        if (appList.currentIndex !== row) appList.currentIndex = row;
    }
    // Up and Down in the list, moving the selection rather than a cursor of
    // the view's own that the selection would then have to chase.
    function stepSelection(delta) {
        const apps = CrucibleController.apps;
        if (apps.length === 0) return;
        const at = page.indexOfApp(page.selectedApp);
        const row = at < 0
            ? (delta > 0 ? 0 : apps.length - 1)
            : Math.max(0, Math.min(apps.length - 1, at + delta));
        page.selectedApp = apps[row].app;
        appList.positionViewAtIndex(row, ListView.Contain);
    }
    onSelectedAppChanged: page.syncListRow()

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
                        text: CrucibleController.apps.length === 0 ? qsTr("no applications") : CrucibleController.apps.length + qsTr(" applications · ") + CrucibleController.soundingCount + qsTr(" with sound")
                        color: Theme.textMuted
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fontMono
                    }
                }
                // The applications, as a list the keyboard can walk: Up and
                // Down choose one, Enter places it in the centre of the room
                // and hands the keys to the room, where the arrows move it.
                //
                // The list sits inside a plain Item so the focus ring has
                // somewhere to hang: a Flickable reparents its visual
                // children into the content it scrolls, and a ring in there
                // would scroll away with the rows.
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    ListView {
                        id: appList
                        objectName: "appList"
                        anchors.fill: parent
                        clip: true
                        spacing: 2
                        model: CrucibleController.apps
                        activeFocusOnTab: true
                        // The view's own cursor is off: page.stepSelection
                        // moves the selection and syncListRow brings the row
                        // to it, which is the only direction that survives
                        // the model being replaced under the view.
                        keyNavigationEnabled: false
                        // Nothing is current until something is chosen: the
                        // page opens with no selected application, as it did
                        // when only a click could select one.
                        currentIndex: -1
                        Accessible.role: Accessible.List
                        Accessible.name: qsTr("Applications")
                        Accessible.focusable: true
                        onCurrentIndexChanged: page.syncListRow()
                        Keys.onPressed: function(event) {
                            if (event.key === Qt.Key_Up) {
                                page.stepSelection(-1);
                                event.accepted = true;
                                return;
                            }
                            if (event.key === Qt.Key_Down) {
                                page.stepSelection(1);
                                event.accepted = true;
                                return;
                            }
                            if (event.key !== Qt.Key_Return && event.key !== Qt.Key_Enter && event.key !== Qt.Key_Space) {
                                return;
                            }
                            if (page.selected && page.selected.slot < 0 && !page.selected.fullscreen) {
                                CrucibleController.position(page.selected.app, 0.5, 0.5, 0);
                                A11y.announce(qsTr("%1 placed in the centre of the room").arg(page.selected.name));
                            }
                            roomKeys.forceActiveFocus();
                            event.accepted = true;
                        }
                        delegate: AppRow {
                            required property var modelData
                            width: ListView.view.width
                            app: modelData
                            selected: modelData.app === page.selectedApp
                            onClicked: { page.selectedApp = modelData.app; appList.forceActiveFocus(); }
                        }
                    }
                    FocusRing { active: appList.activeFocus }
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Every running application with a window is listed; one with nothing to tap is greyed until it plays. A browser's windows and tabs share one entry. A placed application stays placed while it runs.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.WordWrap
                }
                // The full-screen rule, or the platform's reason it is off
                // here; the reason is the controller's, so no platform is
                // named in this file.
                Text {
                    objectName: "fullscreenRuleNote"
                    Layout.fillWidth: true
                    text: CrucibleController.fullscreenRuleReason.length > 0
                        ? qsTr("The full-screen rule is off here: %1.").arg(CrucibleController.fullscreenRuleReason)
                        : qsTr("Full-screen applications stay in the bed; the lock lifts when they leave full-screen.")
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
                    Text { text: CrucibleController.placedCount + qsTr(" of 10 slots placed"); color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: Theme.fontMono }
                }
                // Bed only: placement pans within 5.1, and height does nothing.
                // Said here, where the placing happens, rather than left for
                // the receiver to not show.
                Rectangle {
                    Layout.fillWidth: true
                    visible: !CrucibleController.objectsEnabled
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
                    visible: CrucibleController.has3D
                    Item { Layout.fillWidth: true }
                    SegmentedControl {
                        objectName: "roomViewChoice"
                        model: [{ label: qsTr("Plan + elevation"), value: "2d" }, { label: qsTr("3D"), value: "3d" }]
                        currentValue: page.threeD ? "3d" : "2d"
                        accessibleName: qsTr("Room view")
                        onSelected: function(value) { CrucibleController.roomView = value; page.threeD = value === "3d"; }
                    }
                }
                // The room, and the keys that move what is in it. Every view
                // inside answers to the same arrows, so which picture is on
                // screen changes nothing about the keyboard.
                RoomKeys {
                    id: roomKeys
                    objectName: "roomKeys"
                    Layout.fillWidth: true
                    app: page.selected
                    objectsEnabled: CrucibleController.objectsEnabled
                    onMoved: function(app, x, y, z) { CrucibleController.position(app, x, y, z); }
                    onPlaced: function(app) { CrucibleController.position(app, 0.5, 0.5, 0); }
                    onReturned: function(app) { CrucibleController.unposition(app); }
                    onResized: function(app, size) { CrucibleController.setSize(app, size); }
                    onAnnounced: function(text) { A11y.announce(text); }

                    Loader {
                        id: room3d
                        objectName: "room3dLoader"
                        visible: page.threeD
                        // Once shown, kept loaded while hidden so the camera a person set
                        // survives a visit to the plan views. Set for the initial
                        // visibility too: the change handler does not fire for it, and
                        // a window that opened in 3D lost its view on the first switch.
                        property bool shown: false
                        onVisibleChanged: if (visible) shown = true
                        Component.onCompleted: if (visible) shown = true
                        active: (page.threeD || shown) && CrucibleController.has3D
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: centre.sideBySide ? centre.viewSide * 2 + Theme.space6 : centre.viewSide
                        Layout.preferredHeight: centre.viewSide + 22
                        source: "Room3DView.qml"
                        onLoaded: {
                            item.caption = qsTr("Room (3D)");
                            item.apps = Qt.binding(function() { return CrucibleController.apps; });
                            item.selectedApp = Qt.binding(function() { return page.selectedApp; });
                            item.select.connect(function(app) { page.selectedApp = app; roomKeys.forceActiveFocus(); });
                            item.moved.connect(function(app, x, y, z) { CrucibleController.position(app, x, y, z); });
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
                            apps: CrucibleController.apps
                            selectedApp: page.selectedApp
                            onSelect: function(app) { page.selectedApp = app; }
                            onFocusRequested: roomKeys.forceActiveFocus()
                            onMoved: function(app, x, y, z) { CrucibleController.position(app, x, y, z); }
                            onMovedSide: function(app, side, x, y, z) { CrucibleController.positionSide(app, side, x, y, z); }
                            onReturned: function(app) { CrucibleController.unposition(app); }
                            onDropped: function(app, x, y) { CrucibleController.position(app, x, y, 0); page.selectedApp = app; }
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
                                hint: CrucibleController.objectsEnabled ? qsTr("drag: depth + height") : qsTr("height: objects only")
                                opacity: CrucibleController.objectsEnabled ? 1.0 : 0.55
                                apps: CrucibleController.apps
                                selectedApp: page.selectedApp
                                onSelect: function(app) { page.selectedApp = app; }
                                onFocusRequested: roomKeys.forceActiveFocus()
                                onMoved: function(app, x, y, z) { CrucibleController.position(app, x, y, z); }
                                onMovedSide: function(app, side, x, y, z) { CrucibleController.positionSide(app, side, x, y, z); }
                                onReturned: function(app) { CrucibleController.unposition(app); }
                                onDropped: function(app, x, y) { CrucibleController.position(app, 0.5, x, 1 - 2 * y); page.selectedApp = app; }
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: !CrucibleController.objectsEnabled
                                text: qsTr("Bed only: depth still pans front to rear; height is carried in object metadata, which is off.")
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSmall
                                wrapMode: Text.WordWrap
                            }
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
                            Text { text: page.selected ? page.selected.name : ""; color: Theme.text; font.family: Theme.headingFamily; font.pixelSize: Theme.fontHeading; font.weight: Font.DemiBold; elide: Text.ElideRight; Layout.maximumWidth: 260 }
                            Text {
                                Layout.fillWidth: true
                                text: !page.selected ? ""
                                    : page.selected.slot >= 0
                                        ? RoomWords.placement(page.selected) + " · " + RoomWords.coords(page.selected.x, page.selected.y, page.selected.z)
                                        : RoomWords.placement(page.selected)
                                color: Theme.textMuted
                                font.family: Theme.monoFamily
                                font.pixelSize: Theme.fontMono
                                elide: Text.ElideRight
                            }
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: Theme.space2
                            // Every button on this card acts on the selected
                            // application, which the card's heading says and
                            // a reader would otherwise have to remember: the
                            // description carries the name to each of them.
                            CrucibleButton {
                                objectName: "placeButton"
                                text: page.selected && page.selected.slot >= 0 ? qsTr("Send to bed") : qsTr("Place in the room")
                                enabled: page.selected !== null && !(page.selected.fullscreen)
                                Accessible.description: page.selected ? qsTr("for %1").arg(page.selected.name) : ""
                                onClicked: {
                                    if (page.selected.slot >= 0) CrucibleController.unposition(page.selected.app);
                                    else CrucibleController.position(page.selected.app, 0.5, 0.5, 0);
                                }
                            }
                            CrucibleButton {
                                objectName: "centreButton"
                                text: qsTr("Centre")
                                enabled: page.selected !== null && page.selected.slot >= 0
                                Accessible.description: page.selected ? qsTr("for %1").arg(page.selected.name) : ""
                                onClicked: CrucibleController.position(page.selected.app, 0.5, 0.5, 0)
                            }
                            CrucibleButton {
                                objectName: "splitButton"
                                text: page.selected && page.selected.width === 2 ? qsTr("Mono") : qsTr("Split")
                                enabled: page.selected !== null
                                Accessible.description: page.selected ? qsTr("for %1").arg(page.selected.name) : ""
                                onClicked: CrucibleController.setSplit(page.selected.app, page.selected.width !== 2)
                            }
                            CrucibleButton {
                                objectName: "standardStereoButton"
                                text: qsTr("Standard stereo")
                                visible: page.selected !== null && page.selected.width === 2 && page.selected.pairCustom
                                Accessible.description: page.selected ? qsTr("for %1").arg(page.selected.name) : ""
                                onClicked: CrucibleController.resetPair(page.selected.app)
                            }
                        }
                        // Quick placements, for lining things up without a drag.
                        Flow {
                            Layout.fillWidth: true
                            spacing: Theme.space2
                            Text { height: Math.max(26, implicitHeight); verticalAlignment: Text.AlignVCenter; text: qsTr("Put"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall }
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
                                delegate: CrucibleButton {
                                    required property var modelData
                                    objectName: "put-" + modelData.x + "-" + modelData.y + "-" + modelData.z
                                    text: modelData.label
                                    enabled: page.selected !== null && !(page.selected.fullscreen)
                                    Accessible.description: page.selected ? qsTr("put %1 %2").arg(page.selected.name).arg(modelData.label) : ""
                                    onClicked: { CrucibleController.position(page.selected.app, modelData.x, modelData.y, modelData.z); }
                                }
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: page.selected ? (page.selected.fullscreen ? qsTr("full-screen: stays in the bed") : (page.selected.slot >= 0 ? RoomWords.describe(page.selected.x, page.selected.y, page.selected.z) : "")) : ""
                            visible: text.length > 0
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                        RowLayout {
                            spacing: Theme.space3
                            Text { text: qsTr("Size"); color: Theme.text; font.pixelSize: Theme.fontBody }
                            Rectangle {
                                id: sizeTrack
                                objectName: "sizeSlider"
                                implicitWidth: 180
                                implicitHeight: 14
                                color: "transparent"
                                opacity: CrucibleController.objectsEnabled ? 1.0 : 0.55
                                readonly property real value: page.selected ? page.selected.size : 0
                                // Read by name, not by these being anything
                                // in particular: QAccessibleQuickItem's value
                                // interface looks for value/minimumValue/
                                // maximumValue/stepSize on the item, which is
                                // how a hand-drawn track reports a range.
                                readonly property real minimumValue: 0
                                readonly property real maximumValue: 1
                                readonly property real stepSize: 0.05
                                Rectangle { y: 6; width: parent.width; height: 2; color: Theme.divider }
                                Rectangle { y: 6; width: parent.width * sizeTrack.value; height: 2; color: Theme.accent }
                                Rectangle { x: parent.width * sizeTrack.value - 5; y: 2; width: 10; height: 10; radius: 5; color: Theme.accent }
                                MouseArea {
                                    anchors.fill: parent
                                    enabled: page.selected !== null
                                    cursorShape: Qt.PointingHandCursor
                                    function apply(mouse) { CrucibleController.setSize(page.selected.app, Math.max(0, Math.min(1, mouse.x / width))); }
                                    onPressed: function(mouse) { sizeTrack.forceActiveFocus(); apply(mouse); }
                                    onPositionChanged: function(mouse) { if (mouse.buttons & Qt.LeftButton) apply(mouse); }
                                }
                                Accessible.role: Accessible.Slider
                                Accessible.name: qsTr("Object size")
                                Accessible.focusable: true
                                Accessible.description: page.selected
                                    ? (page.selected.size === 0 ? qsTr("point") : qsTr("%1%").arg(Math.round(page.selected.size * 100)))
                                    : ""
                                activeFocusOnTab: page.selected !== null
                                function step(by) {
                                    if (!page.selected) return;
                                    CrucibleController.setSize(page.selected.app, Math.max(0, Math.min(1, sizeTrack.value + by)));
                                }
                                Keys.onPressed: function(event) {
                                    const by = (event.modifiers & Qt.ShiftModifier) ? 0.01 : sizeTrack.stepSize;
                                    if (event.key === Qt.Key_Left || event.key === Qt.Key_Down) sizeTrack.step(-by);
                                    else if (event.key === Qt.Key_Right || event.key === Qt.Key_Up) sizeTrack.step(by);
                                    else if (event.key === Qt.Key_Home) { if (page.selected) CrucibleController.setSize(page.selected.app, 0); }
                                    else if (event.key === Qt.Key_End) { if (page.selected) CrucibleController.setSize(page.selected.app, 1); }
                                    else return;
                                    event.accepted = true;
                                }
                                FocusRing {}
                            }
                            Text { text: page.selected ? (page.selected.size === 0 ? qsTr("point") : Math.round(page.selected.size * 100) + "%") : ""; color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: Theme.fontMono }
                            Text {
                                Layout.fillWidth: true
                                text: CrucibleController.objectsEnabled ? qsTr("extent the receiver's renderer spreads the object over; the bed hears a point") : qsTr("object metadata: no effect while the stream is bed only")
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
                    Text { text: qsTr("unplaced applications, mixed to the 5.1 bed"); color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: Theme.fontMono }
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
                            model: CrucibleController.apps
                            delegate: BedChip {
                                required property var modelData
                                visible: modelData.slot < 0
                                app: modelData
                                onClicked: page.selectedApp = modelData.app
                                // Enter on a chip is the keyboard's drag: it
                                // lands in the centre of the room and the
                                // keys follow it there.
                                onPlace: {
                                    CrucibleController.position(modelData.app, 0.5, 0.5, 0);
                                    page.selectedApp = modelData.app;
                                    roomKeys.forceActiveFocus();
                                    A11y.announce(qsTr("%1 placed in the centre of the room").arg(modelData.name));
                                }
                            }
                        }
                        Text {
                            height: Math.max(34, implicitHeight)
                            width: Math.min(implicitWidth, bedFlow.width - Theme.space2)
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: Theme.space2
                            text: CrucibleController.bedCount === 0 ? qsTr("every application is placed") : qsTr("drag one into the room to place it · drag a marker back here, or double-click it, to return it")
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

        // --- 04 signal path / 05 signing -----------------------------------------
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
                RailBlock {
                    ordinal: "04"
                    label: qsTr("SIGNAL PATH")
                    Layout.fillWidth: true
                    Layout.fillHeight: false
                    Text { Layout.fillWidth: true; text: qsTr("applications → this app → what you hear"); color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: Theme.fontMono; elide: Text.ElideRight }
                }
                SignalPath {
                    Layout.fillWidth: true
                    wide: false
                    onOpenOutput: page.openOutput()
                }
                RailBlock { ordinal: "05"; label: qsTr("SIGNING"); Layout.fillWidth: true; Layout.fillHeight: false }
                Text { Layout.fillWidth: true; text: CrucibleController.objectsEnabled ? qsTr("key loaded · objects on") : qsTr("no key · 5.1 bed only"); color: Theme.text; font.pixelSize: Theme.fontBody; elide: Text.ElideRight }
                Text { Layout.fillWidth: true; text: CrucibleController.keyPath.length ? CrucibleController.keyPath : qsTr("load one in Settings"); color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: Theme.fontMono; elide: Text.ElideMiddle }
                Item { Layout.fillHeight: true }
            }
        }
    }

    // A position in words used to be spelled out here, and again in the
    // rows, and a third time for a reader. It is RoomWords.describe now, and
    // every one of them says the same thing.
}

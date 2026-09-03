import QtQuick
import QtQuick.Layouts

import Ac3ForgeDesk

// The Room: the application rail, the plan and elevation views, the bed
// tray, and the output summary (docs/platforms/windows-demo.md, "UI").
Item {
    id: page
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

        // --- 03 room / 04 bed ---------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Theme.space4
            Layout.leftMargin: Theme.space6
            Layout.rightMargin: Theme.space6
            spacing: Theme.space4
            RailBlock {
                ordinal: "02"
                Layout.fillHeight: false
                label: qsTr("ROOM")
                Layout.fillWidth: true
                Text { text: DeskController.placedCount + qsTr(" of 10 slots placed"); color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: 11 }
            }
            RowLayout {
                spacing: Theme.space6
                RoomView {
                    id: plan
                    Layout.preferredWidth: 400
                    Layout.preferredHeight: 400 + 22
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
                    Layout.preferredWidth: 400
                    Layout.alignment: Qt.AlignTop
                    spacing: Theme.space4
                    RoomView {
                        elevation: true
                        Layout.preferredWidth: 400
                        Layout.preferredHeight: 170 + 22
                        caption: qsTr("Elevation (side-on)")
                        hint: qsTr("drag: depth + height")
                        apps: DeskController.apps
                        selectedApp: page.selectedApp
                        onSelect: function(app) { page.selectedApp = app; }
                        onMoved: function(app, x, y, z) { DeskController.position(app, x, y, z); }
                        onReturned: function(app) { DeskController.unposition(app); }
                        onDropped: function(app, x, y) { DeskController.position(app, 0.5, x, 1 - 2 * y); page.selectedApp = app; }
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
                                Text { text: page.selected ? page.selected.name : ""; color: Theme.text; font.family: Theme.headingFamily; font.pixelSize: 15; font.weight: Font.DemiBold }
                                Text {
                                    text: page.selected ? (page.selected.slot >= 0
                                        ? qsTr("slot ") + (page.selected.slot + 1) + " · x " + page.selected.x.toFixed(2) + " · y " + page.selected.y.toFixed(2) + " · z " + (page.selected.z >= 0 ? "+" : "") + page.selected.z.toFixed(2)
                                        : qsTr("in the bed")) : ""
                                    color: Theme.textMuted
                                    font.family: Theme.monoFamily
                                    font.pixelSize: 11
                                }
                            }
                            RowLayout {
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
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: page.selected ? (page.selected.fullscreen ? qsTr("full-screen: stays in the bed") : (page.selected.slot >= 0 ? page.describe(page.selected) : "")) : ""
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fontSmall
                                }
                            }
                        }
                    }
                }
                Item { Layout.fillWidth: true }
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
                implicitHeight: 50
                color: "transparent"
                border.color: Theme.divider
                border.width: 1
                Flow {
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
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: Theme.space2
                        text: DeskController.bedCount === 0 ? qsTr("every application is placed") : qsTr("drag one into the room to place it · drag a marker back here, or double-click it, to return it")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }
                }
                DropArea {
                    anchors.fill: parent
                    keys: ["marker"]
                }
            }
            Item { Layout.fillHeight: true }
        }

        // --- 05 output / 06 default / 07 signing ---------------------------------
        Rectangle {
            Layout.preferredWidth: 280
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
                        Text { text: DeskController.modeName; color: Theme.text; font.family: Theme.headingFamily; font.pixelSize: 18; font.weight: Font.DemiBold }
                        Text { text: DeskController.endpointName; color: Theme.text; font.pixelSize: 13; visible: text.length > 0 }
                        Text { Layout.fillWidth: true; text: DeskController.outputReason; color: Theme.textMuted; font.pixelSize: Theme.fontSmall; wrapMode: Text.WordWrap }
                        RowLayout {
                            Text { text: qsTr("pin: ") + DeskController.pinned; color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: 11 }
                            Item { Layout.fillWidth: true }
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
                Text { text: DeskController.objectsEnabled ? qsTr("key loaded · objects on") : qsTr("no key · 5.1 bed only"); color: Theme.text; font.pixelSize: 13 }
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

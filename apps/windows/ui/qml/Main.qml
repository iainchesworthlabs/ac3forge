import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.platform as Platform

import Ac3ForgeDesk

// The window: header with the mode pill and the page switch, one of three
// pages, the status strip, and the tray icon that keeps the engine alive
// when the window is closed (docs/platforms/windows-demo.md, "UI").
ApplicationWindow {
    id: window
    width: 1480
    height: 820
    // The pages adapt down to this; below it the rails would have to
    // collapse, which they do not.
    minimumWidth: 960
    minimumHeight: 620
    visible: true
    title: qsTr("Desktop Atmos")
    color: Theme.bg

    property string page: "room"
    property bool roomThreeD: false

    Component.onCompleted: {
        Theme.preference = DeskController.theme;
        Theme.paletteChoice = DeskController.palette;
        DeskController.start();
        if (DeskController.moveDefaultOnLaunch && !DeskController.defaultIsNullSink) {
            DeskController.moveDefaultToNullSink();
        }
    }
    Connections {
        target: DeskController
        function onSettingsChanged() {
            Theme.preference = DeskController.theme;
            Theme.paletteChoice = DeskController.palette;
        }
    }

    onClosing: function(close) {
        if (DeskController.keepRunningWhenClosed) {
            close.accepted = false;
            window.hide();
        } else {
            DeskController.stop();
            Qt.quit();
        }
    }

    // --- header ---------------------------------------------------------------
    header: Rectangle {
        height: 64
        color: Theme.surface
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.divider }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.space6
            anchors.rightMargin: Theme.space6
            spacing: Theme.space4
            Text {
                text: qsTr("Desktop Atmos")
                color: Theme.text
                font.family: Theme.headingFamily
                font.pixelSize: Theme.fontTitle
                font.weight: Font.Bold
            }
            Text {
                text: qsTr("your applications, placed in the room")
                color: Theme.textMuted
                font.pixelSize: 13
            }
            Item { Layout.fillWidth: true }
            // The status pill: what is going out, where. Opens Output.
            Rectangle {
                implicitHeight: 28
                implicitWidth: pillRow.implicitWidth + Theme.space6
                color: Theme.neutral100
                border.color: Theme.divider
                border.width: 1
                RowLayout {
                    id: pillRow
                    anchors.centerIn: parent
                    spacing: Theme.space2
                    Rectangle { width: 8; height: 8; color: DeskController.modeKey === "none" ? Theme.neutral500 : Theme.accent }
                    // The path in one line: where applications play (a warning
                    // when that is a real device), then what is heard, where.
                    Text {
                        text: ((DeskController.defaultIsNullSink ? qsTr("apps → ") : qsTr("⚠ apps heard direct → "))
                               + DeskController.modeName + (DeskController.endpointName.length ? " · " + DeskController.endpointName : "")
                               + (DeskController.objectsEnabled ? qsTr(" · objects signed") : qsTr(" · 5.1 bed only"))).toUpperCase()
                        color: Theme.text
                        font.family: Theme.monoFamily
                        font.pixelSize: 11
                        font.letterSpacing: 0.6
                    }
                    Text { text: "›"; color: Theme.textMuted; font.pixelSize: 13 }
                }
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: window.page = "output" }
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Open the Signal path page")
            }
            SegmentedControl {
                model: [{ label: qsTr("Room"), value: "room" }, { label: qsTr("Signal path"), value: "output" }, { label: qsTr("Settings"), value: "settings" }]
                currentValue: window.page
                accessibleName: qsTr("Page")
                onSelected: function(value) { window.page = value; }
            }
            DeskButton {
                text: "?"
                implicitWidth: 30
                onClicked: about.open()
                Accessible.name: qsTr("About Desktop Atmos")
            }
        }
    }
    AboutDialog { id: about }
    function openAbout() { about.open(); }

    // --- pages ----------------------------------------------------------------
    StackLayout {
        anchors.fill: parent
        currentIndex: window.page === "room" ? 0 : window.page === "output" ? 1 : 2
        RoomPage { threeD: window.roomThreeD || DeskController.roomView === "3d"; onOpenOutput: window.page = "output" }
        OutputPage {}
        SettingsPage {}
    }

    // --- status strip -----------------------------------------------------------
    footer: Rectangle {
        height: 32
        color: Theme.surface
        Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.divider }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.space6
            anchors.rightMargin: Theme.space6
            spacing: Theme.space6
            Repeater {
                model: [
                    qsTr("encode %1 ms").arg(DeskController.encodeMs.toFixed(2)),
                    qsTr("cadence %1 ms").arg(DeskController.lastFrameMs.toFixed(1)),
                    qsTr("worst %1 ms").arg(DeskController.worstFrameMs.toFixed(1)),
                    qsTr("%1 underruns").arg(DeskController.underruns),
                    qsTr("%1 starved reads").arg(DeskController.starvedReads),
                    (DeskController.lowLatency ? qsTr("1-block frames") : qsTr("6-block frames")) + " · E-AC-3" + (DeskController.objectsEnabled ? " JOC" : " 5.1"),
                    qsTr("15 objects · %1 placed · %2 in the bed · 5 bed slots · %3-channel taps").arg(DeskController.placedCount).arg(DeskController.bedCount).arg(DeskController.tapChannels)
                ]
                delegate: Text {
                    required property string modelData
                    text: modelData
                    color: Theme.textMuted
                    font.family: Theme.monoFamily
                    font.pixelSize: 11
                }
            }
            Item { Layout.fillWidth: true }
            Text {
                text: DeskController.lastError.length ? DeskController.lastError : (DeskController.running ? qsTr("engine running") : qsTr("engine stopped"))
                color: DeskController.lastError.length ? Theme.accent : Theme.textMuted
                font.family: Theme.monoFamily
                font.pixelSize: 11
                elide: Text.ElideLeft
                Layout.maximumWidth: 360
            }
            DeskButton {
                text: DeskController.running ? qsTr("Stop") : qsTr("Start")
                implicitHeight: 22
                onClicked: DeskController.running ? DeskController.stop() : DeskController.start()
                Accessible.name: DeskController.running ? qsTr("Stop the engine") : qsTr("Start the engine")
            }
        }
    }

    // --- tray -----------------------------------------------------------------
    Platform.SystemTrayIcon {
        id: tray
        visible: true
        icon.source: "qrc:/qt/qml/Ac3ForgeDesk/tray.svg"
        tooltip: qsTr("Desktop Atmos") + " · " + DeskController.modeName + (DeskController.endpointName.length ? " · " + DeskController.endpointName : "")
        onActivated: function(reason) {
            if (reason === Platform.SystemTrayIcon.Trigger || reason === Platform.SystemTrayIcon.DoubleClick) {
                window.show();
                window.raise();
                window.requestActivate();
            }
        }
        menu: Platform.Menu {
            Platform.MenuItem { text: qsTr("Open the room"); onTriggered: { window.page = "room"; window.show(); window.raise(); window.requestActivate(); } }
            Platform.Menu {
                title: qsTr("Signal path") + " · " + (DeskController.pinned === "auto" ? qsTr("auto") : DeskController.pinned)
                Platform.MenuItemGroup { id: pinGroup }
                Platform.MenuItem { text: qsTr("Automatic"); checkable: true; checked: DeskController.pinned === "auto"; group: pinGroup; onTriggered: DeskController.pinned = "auto" }
                Platform.MenuItem { text: qsTr("Atmos"); checkable: true; checked: DeskController.pinned === "atmos"; group: pinGroup; onTriggered: DeskController.pinned = "atmos" }
                Platform.MenuItem { text: qsTr("Dolby Digital Plus 5.1"); checkable: true; checked: DeskController.pinned === "ddplus"; group: pinGroup; onTriggered: DeskController.pinned = "ddplus" }
                Platform.MenuItem { text: qsTr("Dolby Digital 5.1"); checkable: true; checked: DeskController.pinned === "dd"; group: pinGroup; onTriggered: DeskController.pinned = "dd" }
                Platform.MenuItem { text: qsTr("PCM surround"); checkable: true; checked: DeskController.pinned === "pcm"; group: pinGroup; onTriggered: DeskController.pinned = "pcm" }
                Platform.MenuItem { text: qsTr("Headphones"); checkable: true; checked: DeskController.pinned === "headphones"; group: pinGroup; onTriggered: DeskController.pinned = "headphones" }
                Platform.MenuItem { text: qsTr("Stereo"); checkable: true; checked: DeskController.pinned === "stereo"; group: pinGroup; onTriggered: DeskController.pinned = "stereo" }
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                text: DeskController.defaultIsNullSink ? qsTr("Default output: ") + DeskController.defaultOutputName : qsTr("Move default output to ") + DeskController.nullSinkName
                enabled: !DeskController.defaultIsNullSink && DeskController.nullSinkPresent
                onTriggered: DeskController.moveDefaultToNullSink()
            }
            Platform.MenuItem {
                text: qsTr("Restore ") + (DeskController.previousDefaultName.length ? DeskController.previousDefaultName : qsTr("previous default output"))
                enabled: DeskController.defaultIsNullSink && DeskController.previousDefaultName.length > 0
                onTriggered: DeskController.restoreDefault()
            }
            Platform.MenuSeparator {}
            Platform.MenuItem { text: DeskController.objectsEnabled ? qsTr("Objects on · key loaded") : qsTr("Objects off · no key"); enabled: false }
            Platform.MenuItem { text: qsTr("Settings…"); onTriggered: { window.page = "settings"; window.show(); window.raise(); window.requestActivate(); } }
            Platform.MenuItem { text: qsTr("About…"); onTriggered: { window.show(); window.raise(); window.requestActivate(); about.open(); } }
            Platform.MenuSeparator {}
            Platform.MenuItem { text: qsTr("Quit"); onTriggered: { DeskController.stop(); Qt.quit(); } }
        }
    }
}

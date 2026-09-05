import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.platform as Platform

import Ac3ForgeCrucible

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
    title: qsTr("Crucible")
    color: Theme.bg

    property string page: "room"
    property bool roomThreeD: false
    // A capture run (main.cpp, --shot) sets this before the first event-loop
    // turn, so the first-run dialog never lands in a screenshot that did
    // not ask for it.
    property bool suppressFirstRun: false
    property alias firstRunDialogRef: firstRun

    Component.onCompleted: {
        Theme.preference = CrucibleController.theme;
        Theme.paletteChoice = CrucibleController.palette;
        // start() changes no sound setting: the output policy never opens
        // the endpoint that is the default, and its probe is what fills the
        // facts the first-run dialog sits over.
        CrucibleController.start();
        // One turn later, so main.cpp's setProperty calls (which run after
        // this handler and before the loop) can suppress the dialog. The
        // launch-time move waits behind the dialog on the one launch it has
        // not been seen: Send performs it, Not now leaves the setting for
        // the next launch, which is what was asked for, now explained.
        Qt.callLater(function() {
            if (!CrucibleController.firstRunAcknowledged && !window.suppressFirstRun) {
                firstRun.open();
            } else if (CrucibleController.moveDefaultOnLaunch && !CrucibleController.defaultIsNullSink) {
                CrucibleController.moveDefaultToNullSink();
            }
        });
    }
    Connections {
        target: CrucibleController
        function onSettingsChanged() {
            Theme.preference = CrucibleController.theme;
            Theme.paletteChoice = CrucibleController.palette;
        }
    }

    onClosing: function(close) {
        if (CrucibleController.keepRunningWhenClosed) {
            close.accepted = false;
            window.hide();
        } else {
            CrucibleController.quit();
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
                text: qsTr("Crucible")
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
                    Rectangle { width: 8; height: 8; color: CrucibleController.modeKey === "none" ? Theme.neutral500 : Theme.accent }
                    // The path in one line: where applications play (a warning
                    // when that is a real device), then what is heard, where.
                    Text {
                        text: ((CrucibleController.defaultIsNullSink ? qsTr("apps → ") : qsTr("⚠ apps heard direct → "))
                               + CrucibleController.modeName + (CrucibleController.endpointName.length ? " · " + CrucibleController.endpointName : "")
                               + (CrucibleController.objectsEnabled ? qsTr(" · objects signed") : qsTr(" · 5.1 bed only"))).toUpperCase()
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
            CrucibleButton {
                text: "?"
                implicitWidth: 30
                onClicked: about.open()
                Accessible.name: qsTr("About Crucible")
            }
        }
    }
    AboutDialog { id: about; onShowLicences: licences.open() }
    function openAbout() { about.open(); }
    FirstRunDialog { id: firstRun; onOpenSettings: window.page = "settings" }
    function openFirstRun() { firstRun.open(); }
    // The notices this build ships, over About (the two stack) or on their
    // own from `--page licences`; the aliases are for the tests.
    LicencesDialog { id: licences }
    function openLicences() { licences.open(); }
    property alias aboutDialog: about
    property alias licencesDialog: licences

    // --- pages ----------------------------------------------------------------
    StackLayout {
        anchors.fill: parent
        currentIndex: window.page === "room" ? 0 : window.page === "output" ? 1 : 2
        RoomPage { threeD: window.roomThreeD || CrucibleController.roomView === "3d"; onOpenOutput: window.page = "output" }
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
                    qsTr("encode %1 ms").arg(CrucibleController.encodeMs.toFixed(2)),
                    qsTr("cadence %1 ms").arg(CrucibleController.lastFrameMs.toFixed(1)),
                    qsTr("worst %1 ms").arg(CrucibleController.worstFrameMs.toFixed(1)),
                    qsTr("%1 underruns").arg(CrucibleController.underruns),
                    qsTr("%1 starved reads").arg(CrucibleController.starvedReads),
                    (CrucibleController.lowLatency ? qsTr("1-block frames") : qsTr("6-block frames")) + " · E-AC-3" + (CrucibleController.objectsEnabled ? " JOC" : " 5.1"),
                    qsTr("15 objects · %1 placed · %2 in the bed · 5 bed slots · %3-channel taps").arg(CrucibleController.placedCount).arg(CrucibleController.bedCount).arg(CrucibleController.tapChannels)
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
                text: CrucibleController.lastError.length ? CrucibleController.lastError : (CrucibleController.running ? qsTr("engine running") : qsTr("engine stopped"))
                color: CrucibleController.lastError.length ? Theme.accent : Theme.textMuted
                font.family: Theme.monoFamily
                font.pixelSize: 11
                elide: Text.ElideLeft
                Layout.maximumWidth: 360
            }
            CrucibleButton {
                text: CrucibleController.running ? qsTr("Stop") : qsTr("Start")
                implicitHeight: 22
                onClicked: CrucibleController.running ? CrucibleController.stop() : CrucibleController.start()
                Accessible.name: CrucibleController.running ? qsTr("Stop the engine") : qsTr("Start the engine")
            }
        }
    }

    // --- tray -----------------------------------------------------------------
    Platform.SystemTrayIcon {
        id: tray
        visible: true
        icon.source: "qrc:/qt/qml/Ac3ForgeCrucible/tray.svg"
        tooltip: qsTr("Crucible") + " · " + CrucibleController.modeName + (CrucibleController.endpointName.length ? " · " + CrucibleController.endpointName : "")
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
                title: qsTr("Signal path") + " · " + (CrucibleController.pinned === "auto" ? qsTr("auto") : CrucibleController.pinned)
                Platform.MenuItemGroup { id: pinGroup }
                Platform.MenuItem { text: qsTr("Automatic"); checkable: true; checked: CrucibleController.pinned === "auto"; group: pinGroup; onTriggered: CrucibleController.pinned = "auto" }
                Platform.MenuItem { text: qsTr("Atmos"); checkable: true; checked: CrucibleController.pinned === "atmos"; group: pinGroup; onTriggered: CrucibleController.pinned = "atmos" }
                Platform.MenuItem { text: qsTr("Dolby Digital Plus 5.1"); checkable: true; checked: CrucibleController.pinned === "ddplus"; group: pinGroup; onTriggered: CrucibleController.pinned = "ddplus" }
                Platform.MenuItem { text: qsTr("Dolby Digital 5.1"); checkable: true; checked: CrucibleController.pinned === "dd"; group: pinGroup; onTriggered: CrucibleController.pinned = "dd" }
                Platform.MenuItem { text: qsTr("PCM surround"); checkable: true; checked: CrucibleController.pinned === "pcm"; group: pinGroup; onTriggered: CrucibleController.pinned = "pcm" }
                Platform.MenuItem { text: qsTr("Headphones"); checkable: true; checked: CrucibleController.pinned === "headphones"; group: pinGroup; onTriggered: CrucibleController.pinned = "headphones" }
                Platform.MenuItem { text: qsTr("Stereo"); checkable: true; checked: CrucibleController.pinned === "stereo"; group: pinGroup; onTriggered: CrucibleController.pinned = "stereo" }
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                text: CrucibleController.defaultIsNullSink ? qsTr("Default output: ") + CrucibleController.defaultOutputName : qsTr("Move default output to ") + CrucibleController.nullSinkName
                enabled: !CrucibleController.defaultIsNullSink && (CrucibleController.nullSinkPresent || CrucibleController.silentDeviceCanCreate)
                onTriggered: CrucibleController.moveDefaultToNullSink()
            }
            Platform.MenuItem {
                text: qsTr("Restore ") + (CrucibleController.previousDefaultName.length ? CrucibleController.previousDefaultName : qsTr("previous default output"))
                enabled: CrucibleController.defaultIsNullSink && CrucibleController.previousDefaultName.length > 0
                onTriggered: CrucibleController.restoreDefault()
            }
            Platform.MenuSeparator {}
            Platform.MenuItem { text: CrucibleController.objectsEnabled ? qsTr("Objects on · key loaded") : qsTr("Objects off · no key"); enabled: false }
            Platform.MenuItem { text: qsTr("Settings…"); onTriggered: { window.page = "settings"; window.show(); window.raise(); window.requestActivate(); } }
            Platform.MenuItem { text: qsTr("About…"); onTriggered: { window.show(); window.raise(); window.requestActivate(); about.open(); } }
            Platform.MenuSeparator {}
            Platform.MenuItem { text: qsTr("Quit"); onTriggered: CrucibleController.quit() }
        }
    }
}

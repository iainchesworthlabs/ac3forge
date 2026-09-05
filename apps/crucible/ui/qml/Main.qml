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

    // The text size the person chose. Every size in the window is a multiple
    // of this (Theme.fontScale), so the Text size setting moves all of them
    // together and the controls grow rather than clip.
    //
    // 100% is the default and is what the window is drawn at. "System" reads
    // the point size the platform's theme reports and counts 9 pt as 100%,
    // which is the base size on Windows and what its Text size setting
    // scales; several Linux desktops report 10 or 11 pt with nothing about
    // text size touched, so "System" starts the window larger there and is
    // an explicit choice rather than the default. A platform that reports a
    // pixel size instead has no point size to read and stays at 1.0.
    function applyTextScale() {
        const choice = CrucibleController.textScale;
        if (choice === "system") {
            const points = Application.font.pointSize;
            Theme.fontScale = points > 0 ? Math.max(1.0, Math.min(2.0, points / 9)) : 1.0;
        } else {
            Theme.fontScale = Number(choice) / 100;
        }
    }

    Component.onCompleted: {
        Theme.preference = CrucibleController.theme;
        Theme.paletteChoice = CrucibleController.palette;
        window.applyTextScale();
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
            window.applyTextScale();
        }
    }

    // --- keyboard -------------------------------------------------------------
    // The three pages by number, and help where every desktop puts it. The
    // rest of the keyboard belongs to the controls themselves; the key map is
    // docs/crucible/accessibility.md.
    Shortcut { sequence: "Ctrl+1"; onActivated: window.page = "room" }
    Shortcut { sequence: "Ctrl+2"; onActivated: window.page = "output" }
    Shortcut { sequence: "Ctrl+3"; onActivated: window.page = "settings" }
    Shortcut { sequence: StandardKey.HelpContents; onActivated: about.open() }

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
        // The mockup's 64, and taller than that when the text is larger, so
        // the pill and the page switch keep their room instead of clipping.
        height: Math.max(64, headerRow.implicitHeight + Theme.space4)
        color: Theme.surface
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.divider }
        RowLayout {
            id: headerRow
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
                font.pixelSize: Theme.fontBody
            }
            Item { Layout.fillWidth: true }
            // The status pill: what is going out, where. Opens Output.
            Rectangle {
                id: signalPathPill
                objectName: "signalPathPill"
                implicitHeight: Math.max(28, pillRow.implicitHeight + 6)
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
                        font.pixelSize: Theme.fontMono
                        font.letterSpacing: 0.6
                    }
                    Text { text: "›"; color: Theme.textMuted; font.pixelSize: Theme.fontBody }
                }
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: window.page = "output" }
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Open the Signal path page")
                Accessible.focusable: true
                Accessible.onPressAction: window.page = "output"
                activeFocusOnTab: true
                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        window.page = "output";
                        event.accepted = true;
                    }
                }
                FocusRing {}
            }
            SegmentedControl {
                objectName: "pageChoice"
                model: [{ label: qsTr("Room"), value: "room" }, { label: qsTr("Signal path"), value: "output" }, { label: qsTr("Settings"), value: "settings" }]
                currentValue: window.page
                accessibleName: qsTr("Page")
                onSelected: function(value) { window.page = value; }
            }
            CrucibleButton {
                objectName: "aboutButton"
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

    // --- what the window says out loud -----------------------------------------
    // One transition hook. Every change worth telling a screen reader about
    // becomes a sentence HERE, and each sentence goes two ways: to
    // A11y.announce, which this item relays to the platform's reader, and to
    // the diagnostics ring, so a bug report carries what the window said as
    // well as what the engine did. Two paths, one set of call sites.
    //
    // Every fact is compared with the sentence it last produced, because the
    // controller republishes all of them on every 60 ms poll and a reader
    // must not hear the same line sixteen times a second. A move in the room
    // is not announced from here at all: the keys announced the intent
    // already, and the engine's echo would say each nudge twice.
    Item {
        id: announcer
        objectName: "announcer"
        width: 1
        height: 1

        // The last thing said, as this item's accessible name, so it is
        // readable after the fact rather than gone.
        Accessible.role: Accessible.StaticText
        Accessible.name: A11y.lastMessage

        // Set once the window is up: the state a launch starts in is not a
        // change, and announcing it would talk over the reader's own
        // description of the new window.
        property bool primed: false
        property string saidEngine: ""
        property string saidHearing: ""
        property string saidSigning: ""
        property string saidDefault: ""
        property string saidDriver: ""
        property double saidAt: 0

        function say(text, urgent) {
            if (!text || text.length === 0) {
                return;
            }
            // Two facts can change on one poll and produce the same
            // sentence twice; half a second apart is a repeat, not news.
            const now = Date.now();
            if (text === A11y.lastMessage && now - announcer.saidAt < 500) {
                return;
            }
            announcer.saidAt = now;
            A11y.announce(text, urgent);
            CrucibleController.note(text);
        }

        function review() {
            const failed = CrucibleController.lastError.length > 0;
            const engine = failed
                ? qsTr("Engine error: %1").arg(CrucibleController.lastError)
                : (CrucibleController.running ? qsTr("Engine running") : qsTr("Engine stopped"));
            if (engine !== announcer.saidEngine) {
                announcer.saidEngine = engine;
                if (announcer.primed) {
                    announcer.say(engine, failed);
                }
            }
            const hearing = CrucibleController.modeName
                + (CrucibleController.endpointName.length ? " · " + CrucibleController.endpointName : "");
            if (hearing !== announcer.saidHearing) {
                announcer.saidHearing = hearing;
                if (announcer.primed) {
                    announcer.say(qsTr("You hear it on %1").arg(hearing), false);
                }
            }
            if (CrucibleController.signingStatus !== announcer.saidSigning) {
                announcer.saidSigning = CrucibleController.signingStatus;
                if (announcer.primed) {
                    announcer.say(announcer.saidSigning, false);
                }
            }
        }

        function reviewDefault() {
            const where = CrucibleController.defaultMessage.length > 0
                ? CrucibleController.defaultMessage
                : qsTr("Applications play to %1").arg(CrucibleController.defaultOutputName);
            if (where !== announcer.saidDefault) {
                announcer.saidDefault = where;
                if (announcer.primed) {
                    announcer.say(where, false);
                }
            }
        }

        function reviewDriver() {
            if (CrucibleController.driverMessage !== announcer.saidDriver) {
                announcer.saidDriver = CrucibleController.driverMessage;
                if (announcer.primed && announcer.saidDriver.length > 0) {
                    announcer.say(announcer.saidDriver, false);
                }
            }
        }

        Connections {
            target: CrucibleController
            function onStateChanged() { announcer.review(); }
            function onDefaultChanged() { announcer.reviewDefault(); }
            function onDriverChanged() { announcer.reviewDriver(); }
        }
        Connections {
            target: A11y
            function onAnnounced(text, assertive) {
                // Polite by default; assertive interrupts, which is for an
                // error and nothing else (A11y.qml on the two values).
                if (assertive) {
                    announcer.Accessible.announce(text, A11y.assertive);
                } else {
                    announcer.Accessible.announce(text);
                }
            }
        }
        Component.onCompleted: {
            announcer.review();
            announcer.reviewDefault();
            announcer.reviewDriver();
            // A turn later, so the start() the window's own onCompleted runs
            // is part of the launch rather than the first announcement.
            Qt.callLater(function() {
                announcer.review();
                announcer.reviewDefault();
                announcer.reviewDriver();
                announcer.primed = true;
            });
        }
    }

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
        height: Math.max(32, footerRow.implicitHeight + Theme.space1)
        color: Theme.surface
        Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.divider }
        RowLayout {
            id: footerRow
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
                    font.pixelSize: Theme.fontMono
                }
            }
            Item { Layout.fillWidth: true }
            Text {
                id: engineStatus
                objectName: "engineStatus"
                text: CrucibleController.lastError.length ? CrucibleController.lastError : (CrucibleController.running ? qsTr("engine running") : qsTr("engine stopped"))
                color: CrucibleController.lastError.length ? Theme.accentInk : Theme.textMuted
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontMono
                elide: Text.ElideLeft
                Layout.maximumWidth: 360
                Accessible.role: Accessible.StaticText
                Accessible.name: engineStatus.text
            }
            CrucibleButton {
                objectName: "engineButton"
                text: CrucibleController.running ? qsTr("Stop") : qsTr("Start")
                // Shorter than a button on a page, because the strip is; it
                // follows the text size like everything else.
                implicitHeight: Math.round(22 * Theme.fontScale)
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

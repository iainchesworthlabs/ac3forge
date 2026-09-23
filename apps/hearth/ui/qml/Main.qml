import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The window (planning/hearth-reference-player.md, A5): a header with the
// six-page switch, one page at a time in the body, and the transport bar
// pinned to the bottom on every page, the way planning/hearth-design.md's
// artboards show it. Play, Speakers and Decoder are built to the design;
// Media information, Network and Settings are placeholders their own
// slices replace.
ApplicationWindow {
    id: window
    width: 1280
    height: 800
    // The design's own minimum (planning/hearth-design.md, "Play ·
    // minimum size, 960 x 620").
    minimumWidth: 960
    minimumHeight: 620
    visible: true
    title: qsTr("Hearth")
    color: Theme.bg

    // apps/gui/qml/Main.qml carries the same root; see its own comment for
    // what padding still has to do on its own.
    LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true

    readonly property var pageOrder: ["play", "media", "speakers", "decoder", "network", "settings"]
    property string page: "play"
    // A capture run (main.cpp, --shot) sets this before the first event-loop
    // turn, so the first-run dialog never lands in a screenshot that did not
    // ask for it.
    property bool suppressFirstRun: false

    Component.onCompleted: {
        HearthController.start();
        // One turn later, so main.cpp's setProperty("suppressFirstRun", ...)
        // - which runs after this handler and before the event loop starts -
        // has already landed (Crucible's own Main.qml carries the identical
        // comment for the identical reason).
        Qt.callLater(function() {
            if (!HearthController.firstRunSeen && !window.suppressFirstRun) {
                firstRun.open();
            }
        });
    }

    // The one place that opens the output picker: the header's own summary
    // below, main.cpp's `--open-output-picker` debug flag (there is no
    // other way to drive a mouse click headlessly for a screenshot), and -
    // once their own issues build them - the Play page's signal-path
    // "Choose..." button and the Speakers page's "SETUP FOR" device summary.
    function openOutputPicker() { outputPicker.open(); }

    Shortcut { sequence: "Ctrl+1"; onActivated: window.page = "play" }
    Shortcut { sequence: "Ctrl+2"; onActivated: window.page = "media" }
    Shortcut { sequence: "Ctrl+3"; onActivated: window.page = "speakers" }
    Shortcut { sequence: "Ctrl+4"; onActivated: window.page = "decoder" }
    Shortcut { sequence: "Ctrl+5"; onActivated: window.page = "network" }
    Shortcut { sequence: "Ctrl+6"; onActivated: window.page = "settings" }
    // The Speakers page's IDENTIFY card says this stops it. A harmless no-op
    // when nothing is sounding the tone, so this needs no guard on which
    // page is showing.
    Shortcut { sequence: "Escape"; onActivated: HearthController.stopIdentify() }
    Shortcut { sequence: StandardKey.HelpContents; onActivated: shortcuts.open() }

    header: Rectangle {
        color: Theme.surface
        border.color: Theme.border
        border.width: 1
        implicitHeight: 56

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.pad
            anchors.rightMargin: Theme.pad
            spacing: Theme.gap

            Text {
                text: qsTr("Hearth")
                color: Theme.text
                font.pixelSize: Theme.fontTitle
                font.bold: true
            }

            // The output summary, opening the output picker (#828): where
            // this used to be plain text, it is now a bordered, focusable
            // control - a Rectangle + MouseArea rather than a native Button,
            // for the same reason PlayPage.qml's queue rows and Speakers.qml's
            // routing cells are (qml-native-button-repeater-offscreen-hang;
            // this one is not inside a Repeater, but the family keeps the
            // idiom for every custom-shaped clickable control regardless).
            // The Play page's own signal-path "Choose..." button and the
            // Speakers page's "SETUP FOR" summary are meant to open the same
            // dialog too, once their own issues build them - this is the
            // first of the three.
            Rectangle {
                id: outputSummary
                objectName: "outputSummaryButton"
                Layout.fillWidth: true
                Layout.preferredHeight: summaryRow.implicitHeight + Theme.gap
                color: outputSummaryArea.containsMouse ? Theme.neutral200 : Theme.bg
                border.color: Theme.border
                border.width: 1
                radius: Theme.radius

                Accessible.role: Accessible.Button
                Accessible.name: summaryLabel.text
                Accessible.description: qsTr("Opens where Hearth plays")
                Accessible.onPressAction: window.openOutputPicker()

                activeFocusOnTab: true
                Keys.onSpacePressed: window.openOutputPicker()
                Keys.onReturnPressed: window.openOutputPicker()

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -Theme.focusRingOffset
                    visible: outputSummary.activeFocus
                    color: "transparent"
                    border.color: Theme.focusRing
                    border.width: Theme.focusRingWidth
                    z: 100
                }

                RowLayout {
                    id: summaryRow
                    anchors.fill: parent
                    anchors.leftMargin: Theme.gap
                    anchors.rightMargin: Theme.gap
                    spacing: Theme.gap / 2

                    Text {
                        id: summaryLabel
                        Layout.fillWidth: true
                        text: HearthController.outputReason.length > 0
                              ? HearthController.outputReason : qsTr("no output chosen yet")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignHCenter
                    }
                    Text {
                        text: "›"
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }
                }

                MouseArea {
                    id: outputSummaryArea
                    objectName: "outputSummaryArea"
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: window.openOutputPicker()
                }
            }

            SegmentedControl {
                id: pageSwitch
                accessibleName: qsTr("Page")
                currentValue: window.page
                model: [
                    { value: "play", label: qsTr("Play") },
                    { value: "media", label: qsTr("Media") },
                    { value: "speakers", label: qsTr("Speakers") },
                    { value: "decoder", label: qsTr("Decoder") },
                    { value: "network", label: qsTr("Network") },
                    { value: "settings", label: qsTr("Settings") }
                ]
                onSelected: function(value) { window.page = value; }
            }

            Button {
                objectName: "helpButton"
                text: "?"
                implicitWidth: 30
                onClicked: shortcuts.open()
                Accessible.name: qsTr("Keyboard shortcuts")
            }
        }
    }

    // Reached from the header's "?" button and F1; ShortcutsDialog's own
    // About… chains to AboutDialog, whose own Licences… chains to
    // LicencesDialog one hop further ("? -> Shortcuts -> About ->
    // Licences") - agreed between the #830 and #854 sessions rather than a
    // second header control. `--page shortcuts`/`about`/`licences`
    // (main.cpp) open any of the three directly, for a capture.
    ShortcutsDialog { id: shortcuts; onShowAbout: about.open() }
    function openShortcuts() { shortcuts.open(); }
    property alias shortcutsDialog: shortcuts

    AboutDialog { id: about; onShowLicences: licences.open() }
    function openAbout() { about.open(); }
    LicencesDialog { id: licences }
    function openLicences() { licences.open(); }

    StackLayout {
        anchors.fill: parent
        currentIndex: window.pageOrder.indexOf(window.page)

        PlayPage { }
        PlaceholderPage { pageName: qsTr("Media information") }
        Speakers { }
        DecoderPage { }
        PlaceholderPage { pageName: qsTr("Network") }
        PlaceholderPage { pageName: qsTr("Settings") }
    }

    footer: TransportBar { }

    OutputPicker {
        id: outputPicker
        objectName: "outputPicker"
        onNetworkPageRequested: window.page = "network"
    }

    FirstRunDialog { id: firstRun; onOpenSpeakers: window.page = "speakers" }
    function openFirstRun() { firstRun.open(); }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import Ac3ForgeCrucible
import Ac3ForgeCrucibleLanguage

// Settings, in the order they matter on a new machine: the virtual output
// device (what makes the whole thing silent to the rest of Windows), the
// signing key (what turns objects on), then latency, codec, appearance and
// behaviour. Two balanced columns when the window is wide enough for them,
// one column otherwise; every block is bounded by its column and wraps.
Flickable {
    id: page
    contentHeight: grid.implicitHeight + Theme.space6 * 2
    clip: true
    readonly property real innerWidth: width - Theme.space6 * 2
    readonly property bool twoUp: innerWidth >= 1000

    FileDialog {
        id: keyDialog
        title: qsTr("Choose the signing key")
        onAccepted: CrucibleController.loadKey(selectedFile.toString())
    }
    // Where the diagnostics file goes. selectedFile is set before open(), so
    // the suggested name and folder appear in the dialog.
    FileDialog {
        id: diagnosticsDialog
        title: qsTr("Save diagnostics")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "txt"
        nameFilters: [qsTr("Text files (*.txt)"), qsTr("All files (*)")]
        onAccepted: CrucibleController.exportDiagnostics(selectedFile.toString())
    }

    // A labelled row: a fixed label, a field, and a note that wraps.
    component SettingRow: RowLayout {
        property alias label: labelText.text
        default property alias content: slot.data
        Layout.fillWidth: true
        spacing: Theme.space3
        Text { id: labelText; Layout.preferredWidth: 110; Layout.alignment: Qt.AlignTop; topPadding: 7; color: Theme.text; font.pixelSize: Theme.fontBody }
        RowLayout { id: slot; Layout.fillWidth: true; spacing: Theme.space3 }
    }
    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.WordWrap
    }
    // One line of a status: a tick when it is as the path needs, a warning
    // sign when it is not, so what still needs doing reads as such.
    component StatusRow: RowLayout {
        property bool ok: false
        property alias text: body.text
        Layout.fillWidth: true
        spacing: Theme.space2
        Text { text: parent.ok ? "✓" : "⚠"; color: parent.ok ? Theme.textMuted : Theme.accentInk; font.pixelSize: Theme.fontBody; Layout.alignment: Qt.AlignTop; Accessible.ignored: true }
        Text { id: body; Layout.fillWidth: true; color: parent.ok ? Theme.textMuted : Theme.text; font.pixelSize: Theme.fontBody; wrapMode: Text.WordWrap }
    }
    component Field: Rectangle {
        property alias input: input
        property alias text: input.text
        property string objectId: ""
        // What the row's label says, passed in rather than typed again, so
        // the name a reader hears is the label a sighted person reads.
        property string accessibleName: ""
        implicitWidth: 240
        implicitHeight: Math.max(30, input.implicitHeight + 10)
        Layout.fillWidth: true
        Layout.maximumWidth: 320
        color: Theme.neutral100
        border.color: input.activeFocus ? Theme.focusRing : Theme.divider
        border.width: 1
        TextInput {
            id: input
            objectName: parent.objectId
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            verticalAlignment: TextInput.AlignVCenter
            color: Theme.text
            font.pixelSize: Theme.fontBody
            selectByMouse: true
            clip: true
            // A plain TextInput is not in the tab chain by default; the
            // Controls' TextField is, and a field a person must type in
            // has to be reachable without a mouse.
            activeFocusOnTab: true
            Accessible.role: Accessible.EditableText
            Accessible.name: parent.accessibleName
        }
    }

    GridLayout {
        id: grid
        x: Theme.space6
        y: Theme.space6
        width: page.innerWidth
        columns: page.twoUp ? 2 : 1
        columnSpacing: Theme.space6
        rowSpacing: Theme.space6

        // --- left column: what makes it work -----------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space6

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space3
                RailBlock { ordinal: "01"; label: qsTr("SILENT DEVICE · WHERE APPLICATIONS PLAY"); Layout.fillWidth: true; Layout.fillHeight: false }
                Note { objectName: "silentDeviceNote"; text: qsTr("Sound takes two stages here. Applications play into the system default output; this app taps them there and sends the result to the endpoint you hear. For the first stage to be silent, the default must be a device that discards what it is given: \"%1\". Until it is there, any silent endpoint whose name matches the filter under Advanced stands in.").arg(CrucibleController.nullSinkName) }
                // What a person needs to know, in the order it matters: is the
                // device there; do applications play to it; and, only while it
                // is not there, what stands between this machine and one. On
                // Windows that is a test-signed driver, so a packaged install
                // carries the signed driver and installs it with the
                // application (Phase 6), and the source-build tools (the
                // folder, install, remove) live under Advanced rather than in
                // the way. On Linux the application makes the device itself,
                // and the page says so instead of showing a folder that
                // means nothing there: silentDeviceFromPackage decides.
                Card {
                    visible: CrucibleController.silentDeviceNeeded
                    ColumnLayout {
                        spacing: Theme.space2
                        StatusRow {
                            ok: CrucibleController.nullSinkPresent
                            text: CrucibleController.nullSinkPresent ? qsTr("The silent device is installed: an endpoint named like \"%1\".").arg(CrucibleController.nullSinkName) : qsTr("No silent device: nothing named like \"%1\" exists, so applications can only play to a real device and are heard directly.").arg(CrucibleController.nullSinkName)
                        }
                        StatusRow {
                            ok: CrucibleController.defaultIsNullSink
                            text: CrucibleController.defaultIsNullSink ? qsTr("Applications play to it: it is the system default output.") : qsTr("Applications do not play to it yet: the system default output is %1. Send them there from the Room or Signal path page.").arg(CrucibleController.defaultOutputName.length ? CrucibleController.defaultOutputName : qsTr("not set"))
                        }
                        // What stands in the way, in the platform's own words.
                        // The sentence is composed where the facts are - test
                        // signing and memory integrity on Windows, a module
                        // load elsewhere - so this view does not have to know
                        // which platform it is running on.
                        StatusRow {
                            visible: !CrucibleController.nullSinkPresent
                            ok: CrucibleController.silentDeviceBlocker.length === 0
                            text: CrucibleController.silentDeviceBlocker.length > 0
                                ? CrucibleController.silentDeviceBlocker
                                : qsTr("This machine can load the silent device.")
                        }
                        Note {
                            visible: !CrucibleController.nullSinkPresent
                            text: !CrucibleController.silentDeviceFromPackage
                                ? qsTr("Nothing to install: this application makes the silent device itself. Create it now, or it is created when you send applications to it.")
                                : CrucibleController.driverPackageFound
                                    ? qsTr("A built driver package is in the driver folder, ready to install.")
                                    : qsTr("An installed copy of this application brings the driver with it. This is a build from source: build the driver, then point Advanced at its folder, or put a built package there.")
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: Theme.space2
                            CrucibleButton { objectName: "installDriverButton"; visible: !CrucibleController.nullSinkPresent; text: CrucibleController.silentDeviceFromPackage ? qsTr("Install driver") : qsTr("Create device"); primary: true; enabled: CrucibleController.driverPackageFound && !CrucibleController.driverBusy; onClicked: CrucibleController.installDriver() }
                            CrucibleButton { text: qsTr("Check again"); enabled: !CrucibleController.driverBusy; onClicked: CrucibleController.refreshDriver() }
                        }
                        Note { visible: CrucibleController.driverMessage.length > 0; text: CrucibleController.driverMessage }
                    }
                }
                // Advanced: the source-build tools, and what counts as silent.
                ColumnLayout {
                    id: advanced
                    objectName: "advancedSection"
                    Layout.fillWidth: true
                    spacing: Theme.space2
                    property bool open: false
                    Item {
                        id: advancedToggle
                        objectName: "advancedToggle"
                        Layout.fillWidth: true
                        implicitHeight: advancedRow.implicitHeight
                        // A disclosure is a button, and its state is what a
                        // reader needs before deciding to press it.
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Advanced")
                        Accessible.description: advanced.open ? qsTr("expanded") : qsTr("collapsed")
                        Accessible.focusable: true
                        Accessible.onPressAction: advanced.open = !advanced.open
                        activeFocusOnTab: true
                        Keys.onPressed: function(event) {
                            if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                                advanced.open = !advanced.open;
                                event.accepted = true;
                            }
                        }
                        Row {
                            id: advancedRow
                            spacing: Theme.space2
                            Text { text: advanced.open ? "▾" : "▸"; color: Theme.textMuted; font.pixelSize: Theme.fontSmall; Accessible.ignored: true }
                            Text { text: qsTr("Advanced"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall; font.letterSpacing: 0.5; Accessible.ignored: true }
                        }
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: advanced.open = !advanced.open }
                        FocusRing {}
                    }
                    // The folder and its package only exist where the device
                    // is a driver; elsewhere "remove" undoes the application's
                    // own device and there is nothing to point at.
                    SettingRow {
                        objectName: "driverFolderRow"
                        visible: advanced.open && CrucibleController.silentDeviceFromPackage
                        label: qsTr("Driver folder")
                        Field {
                            objectId: "driverFolderInput"
                            accessibleName: qsTr("Driver folder")
                            text: CrucibleController.driverDir
                            input.onEditingFinished: CrucibleController.driverDir = input.text
                        }
                    }
                    Note { visible: advanced.open && CrucibleController.silentDeviceFromPackage; text: qsTr("Where install.ps1, remove.ps1 and the built package live: beside this app by default, or apps/windows/driver in a source tree.") + " " + (CrucibleController.driverPackageFound ? qsTr("A built package is there.") : qsTr("No built package is there.")) }
                    Flow {
                        visible: advanced.open
                        Layout.fillWidth: true
                        spacing: Theme.space2
                        CrucibleButton { objectName: "removeDriverButton"; text: CrucibleController.silentDeviceFromPackage ? qsTr("Remove driver") : qsTr("Remove device"); enabled: CrucibleController.driverPackageFound && !CrucibleController.driverBusy; onClicked: CrucibleController.removeDriver() }
                    }
                    Note { visible: advanced.open; text: CrucibleController.silentDeviceFromPackage ? qsTr("Removes the device and the driver package this folder's remove.ps1 knows about; an installed copy of the application removes its own on uninstall.") : qsTr("Removes this application's own silent device; it also goes when the application does.") }
                    SettingRow {
                        visible: advanced.open
                        label: qsTr("Silent device")
                        Field {
                            objectId: "silentDeviceInput"
                            accessibleName: qsTr("Silent device")
                            text: CrucibleController.nullSinkName
                            input.onEditingFinished: CrucibleController.nullSinkName = input.text
                        }
                    }
                    Note { visible: advanced.open; text: qsTr("Any endpoint whose name contains this is treated as the silent device and is never chosen as an output.") }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space3
                RailBlock { ordinal: "02"; label: qsTr("SIGNING KEY"); Layout.fillWidth: true; Layout.fillHeight: false }
                Note { text: qsTr("Objects need a signing key; without one the stream is the 5.1 bed only and placement pans within it. Only the path is remembered; the key stays in its file.") }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space2
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: Math.max(30, keyPathText.implicitHeight + 10)
                        color: Theme.neutral100
                        border.color: Theme.divider
                        border.width: 1
                        Text {
                            id: keyPathText
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            verticalAlignment: Text.AlignVCenter
                            text: CrucibleController.keyPath.length ? CrucibleController.keyPath : qsTr("no key file chosen")
                            color: CrucibleController.keyPath.length ? Theme.text : Theme.textMuted
                            font.family: Theme.monoFamily
                            font.pixelSize: Theme.fontSmall
                            elide: Text.ElideMiddle
                        }
                    }
                    CrucibleButton { text: qsTr("Browse…"); onClicked: keyDialog.open() }
                    CrucibleButton { text: qsTr("Clear"); enabled: CrucibleController.keyPath.length > 0; onClicked: CrucibleController.clearKey() }
                }
                RowLayout {
                    spacing: Theme.space2
                    Rectangle { width: 8; height: 8; color: CrucibleController.objectsEnabled ? Theme.accent : Theme.neutral500 }
                    Text { Layout.fillWidth: true; text: CrucibleController.signingStatus; color: Theme.text; font.pixelSize: Theme.fontBody; wrapMode: Text.WordWrap }
                }
                Note { text: qsTr("An unsigned object container would be refused outright by a validating decoder, so without a key no objects are sent.") }
                Note { text: qsTr("With no file chosen here, the environment is honoured: AC3FORGE_SIGNING_KEY_FILE names a key file and AC3FORGE_SIGNING_KEY carries the key itself.") }
            }
        }

        // --- right column: how it sounds and looks --------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space6

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space3
                RailBlock { ordinal: "03"; label: qsTr("LATENCY"); Layout.fillWidth: true; Layout.fillHeight: false }
                SegmentedControl {
                    model: [{ label: qsTr("Normal · 32 ms frames"), value: "normal" }, { label: qsTr("Low · 5.3 ms frames"), value: "low" }]
                    currentValue: CrucibleController.lowLatency ? "low" : "normal"
                    accessibleName: qsTr("Latency")
                    onSelected: function(value) { CrucibleController.lowLatency = value === "low"; }
                }
                Note { text: qsTr("Low latency shortens the E-AC-3 frame to one block and raises the bitrate to about 1.5 Mb/s so fifteen objects' metadata still fits. The receiver's own decode delay does not change. Changing this restarts the stream.") }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space3
                RailBlock { ordinal: "04"; label: qsTr("CODEC"); Layout.fillWidth: true; Layout.fillHeight: false }
                SettingRow {
                    label: qsTr("Bitrate")
                    ComboBox {
                        id: bitrateBox
                        objectName: "bitrateBox"
                        // The row's label is a separate Text beside it, which
                        // a reader has no way to tie to this control.
                        Accessible.name: qsTr("Bitrate")
                        implicitWidth: 150
                        implicitHeight: Math.max(30, bitrateText.implicitHeight + 10)
                        model: [
                            { label: qsTr("automatic"), value: 0 }, { label: qsTr("256 kb/s"), value: 256 }, { label: qsTr("384 kb/s"), value: 384 },
                            { label: qsTr("448 kb/s"), value: 448 }, { label: qsTr("640 kb/s"), value: 640 }, { label: qsTr("1024 kb/s"), value: 1024 },
                            { label: qsTr("1536 kb/s"), value: 1536 }, { label: qsTr("2048 kb/s"), value: 2048 }]
                        textRole: "label"
                        valueRole: "value"
                        function sync() { const i = indexOfValue(CrucibleController.bitrate); currentIndex = i < 0 ? 0 : i; }
                        Component.onCompleted: sync()
                        onModelChanged: sync()
                        onActivated: CrucibleController.bitrate = currentValue
                        Connections { target: CrucibleController; function onSettingsChanged() { bitrateBox.sync(); } }
                        font.pixelSize: Theme.fontSmall
                        background: Rectangle { color: Theme.neutral100; border.color: bitrateBox.activeFocus ? Theme.focusRing : Theme.divider; border.width: 1 }
                        contentItem: Text { id: bitrateText; leftPadding: 10; text: bitrateBox.displayText; color: Theme.text; font.family: Theme.monoFamily; font.pixelSize: Theme.fontSmall; verticalAlignment: Text.AlignVCenter }
                        indicator: Text { x: bitrateBox.width - 22; anchors.verticalCenter: parent.verticalCenter; text: "⌄"; color: Theme.textMuted; font.pixelSize: Theme.fontNormal }
                        popup.background: Rectangle { color: Theme.surface; border.color: Theme.divider; border.width: 1 }
                        delegate: ItemDelegate {
                            required property var modelData
                            required property int index
                            width: bitrateBox.width
                            contentItem: Text { text: modelData.label; color: Theme.text; font.family: Theme.monoFamily; font.pixelSize: Theme.fontSmall }
                            background: Rectangle { color: highlighted ? Theme.neutral200 : "transparent" }
                            highlighted: bitrateBox.highlightedIndex === index
                        }
                    }
                    Note { text: qsTr("E-AC-3 · 5.1 bed + up to 15 objects · automatic is 448 kb/s, or 1536 kb/s in low latency") }
                }
                CrucibleCheck { objectName: "splitCheck"; Layout.fillWidth: true; text: qsTr("Split stereo applications into two objects"); note: qsTr("Costs two slots per application; the pair sits either side of the position you place, and an application that cannot get two free slots waits in the bed. Applies to applications the engine meets from now on; each application's row can override it."); checked: CrucibleController.splitStereo; onToggled: function(on) { CrucibleController.splitStereo = on; } }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space3
                RailBlock { ordinal: "05"; label: qsTr("APPEARANCE"); Layout.fillWidth: true; Layout.fillHeight: false }
                SettingRow {
                    label: qsTr("Theme")
                    SegmentedControl {
                        model: [{ label: qsTr("System"), value: "system" }, { label: qsTr("Light"), value: "light" }, { label: qsTr("Dark"), value: "dark" }]
                        currentValue: CrucibleController.theme
                        accessibleName: qsTr("Theme")
                        onSelected: function(value) { CrucibleController.theme = value; }
                    }
                }
                SettingRow {
                    label: qsTr("Palette")
                    SegmentedControl {
                        model: [{ label: qsTr("System"), value: "system" }, { label: qsTr("Signal"), value: "signal" }, { label: qsTr("Ink"), value: "ink" }, { label: qsTr("Console"), value: "console" }]
                        currentValue: CrucibleController.palette
                        accessibleName: qsTr("Palette")
                        onSelected: function(value) { CrucibleController.palette = value; }
                    }
                }
                SettingRow {
                    label: qsTr("Language")
                    ComboBox {
                        id: languageBox
                        objectName: "languageBox"
                        Accessible.name: qsTr("Language")
                        implicitWidth: 240
                        implicitHeight: Math.max(30, languageText.implicitHeight + 10)
                        // "System" first, then every language the app ships.
                        model: [{ code: "", name: qsTr("System") }].concat(LanguageManager.availableLanguages())
                        textRole: "name"
                        valueRole: "code"
                        // Re-selected from the manager's own state whenever the
                        // model is rebuilt (every retranslate rebuilds it) or the
                        // language changes, so a chosen language stays chosen.
                        function sync() { currentIndex = LanguageManager.hasOverride() ? Math.max(0, indexOfValue(LanguageManager.currentLanguage)) : 0; }
                        Component.onCompleted: sync()
                        onModelChanged: sync()
                        Connections { target: LanguageManager; function onCurrentLanguageChanged() { languageBox.sync(); } }
                        onActivated: currentValue === "" ? LanguageManager.useSystemLanguage() : LanguageManager.setLanguage(currentValue)
                        font.pixelSize: Theme.fontBody
                        background: Rectangle { color: Theme.neutral100; border.color: languageBox.activeFocus ? Theme.focusRing : Theme.divider; border.width: 1 }
                        contentItem: Text { id: languageText; leftPadding: 10; text: languageBox.displayText; color: Theme.text; font.pixelSize: Theme.fontBody; verticalAlignment: Text.AlignVCenter }
                        indicator: Text { x: languageBox.width - 22; anchors.verticalCenter: parent.verticalCenter; text: "⌄"; color: Theme.textMuted; font.pixelSize: Theme.fontNormal }
                        popup.background: Rectangle { color: Theme.surface; border.color: Theme.divider; border.width: 1 }
                        delegate: ItemDelegate {
                            required property var modelData
                            required property int index
                            width: languageBox.width
                            contentItem: Text { text: modelData.name; color: Theme.text; font.pixelSize: Theme.fontBody }
                            background: Rectangle { color: highlighted ? Theme.neutral200 : "transparent" }
                            highlighted: languageBox.highlightedIndex === index
                        }
                    }
                    Note { text: qsTr("System follows Windows; the translations are mechanical for now") }
                }
                SettingRow {
                    label: qsTr("Text size")
                    SegmentedControl {
                        objectName: "textSizeChoice"
                        model: [{ label: qsTr("System"), value: "system" }, { label: "100%", value: "100" },
                                { label: "125%", value: "125" }, { label: "150%", value: "150" }, { label: "175%", value: "175" }]
                        currentValue: CrucibleController.textScale
                        accessibleName: qsTr("Text size")
                        onSelected: function(value) { CrucibleController.textScale = value; }
                    }
                    Note { text: qsTr("Every size in the window follows this. System takes the size the desktop's own text setting reports, which is how a larger-text setting outside this application reaches it.") }
                }
                SettingRow {
                    label: qsTr("3D layout")
                    SegmentedControl {
                        model: [{ label: qsTr("Auto"), value: "auto" }, { label: "5.1", value: "5.1" }, { label: "7.1", value: "7.1" }, { label: "7.1.4", value: "7.1.4" }]
                        currentValue: CrucibleController.roomLayout
                        accessibleName: qsTr("3D reference layout")
                        onSelected: function(value) { CrucibleController.roomLayout = value; }
                    }
                    Note { text: qsTr("The speakers the 3D room draws for reference. Auto shows 5.1 while the stream is bed only and 7.1.4 once objects are on.") }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space3
                RailBlock { ordinal: "06"; label: qsTr("BEHAVIOUR"); Layout.fillWidth: true; Layout.fillHeight: false }
                CrucibleCheck {
                    Layout.fillWidth: true
                    text: qsTr("Move the default output to the silent device on launch")
                    note: qsTr("Restored to the previous device on quit.")
                    checked: CrucibleController.moveDefaultOnLaunch
                    onToggled: function(on) { CrucibleController.moveDefaultOnLaunch = on; }
                }
                CrucibleCheck {
                    Layout.fillWidth: true
                    text: qsTr("Keep running in the tray when the window is closed")
                    checked: CrucibleController.keepRunningWhenClosed
                    onToggled: function(on) { CrucibleController.keepRunningWhenClosed = on; }
                }
                CrucibleCheck {
                    Layout.fillWidth: true
                    text: qsTr("Show applications with no audio")
                    note: qsTr("Running applications with a window but no audio session, greyed until they play. Off hides them unless they are placed.")
                    checked: CrucibleController.showSilentApps
                    onToggled: function(on) { CrucibleController.showSilentApps = on; }
                }
                CrucibleCheck {
                    Layout.fillWidth: true
                    text: qsTr("Show background processes in the room")
                    note: qsTr("Processes with sound but no window of their own (a virtual machine's backend, the text-input host). They stay in the bed either way.")
                    checked: CrucibleController.showBackgroundApps
                    onToggled: function(on) { CrucibleController.showBackgroundApps = on; }
                }
            }

            // A text file for a bug report. What it holds and what it
            // withholds is the controller's business (diagnostics.hpp); the
            // note says both so the person reads it before attaching it.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space3
                RailBlock { ordinal: "07"; label: qsTr("DIAGNOSTICS"); Layout.fillWidth: true; Layout.fillHeight: false }
                Note { text: qsTr("A text file for a bug report: the version and platform, the engine's counters, the endpoints the probe found, the two devices of the signal path, this app's settings and its recent messages. It does not carry the signing key, the path to it, or any environment variable's value; it does name your audio devices and running applications, so read it before you attach it.") }
                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.space2
                    CrucibleButton { objectName: "exportDiagnosticsButton"; text: qsTr("Save diagnostics…"); onClicked: { diagnosticsDialog.selectedFile = CrucibleController.suggestedDiagnosticsFile(); diagnosticsDialog.open(); } }
                }
                Note { objectName: "diagnosticsMessage"; visible: CrucibleController.diagnosticsMessage.length > 0; text: CrucibleController.diagnosticsMessage }
            }
        }
    }
}

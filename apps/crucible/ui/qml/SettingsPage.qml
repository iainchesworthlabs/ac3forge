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

    // A labelled row: a fixed label, a field, and a note that wraps.
    component SettingRow: RowLayout {
        property alias label: labelText.text
        default property alias content: slot.data
        Layout.fillWidth: true
        spacing: Theme.space3
        Text { id: labelText; Layout.preferredWidth: 110; Layout.alignment: Qt.AlignTop; topPadding: 7; color: Theme.text; font.pixelSize: 13 }
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
        Text { text: parent.ok ? "✓" : "⚠"; color: parent.ok ? Theme.textMuted : Theme.accent; font.pixelSize: 13; Layout.alignment: Qt.AlignTop }
        Text { id: body; Layout.fillWidth: true; color: parent.ok ? Theme.textMuted : Theme.text; font.pixelSize: 13; wrapMode: Text.WordWrap }
    }
    component Field: Rectangle {
        property alias input: input
        property alias text: input.text
        property string objectId: ""
        implicitWidth: 240
        implicitHeight: 30
        Layout.fillWidth: true
        Layout.maximumWidth: 320
        color: Theme.neutral100
        border.color: Theme.divider
        border.width: 1
        TextInput {
            id: input
            objectName: parent.objectId
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            verticalAlignment: TextInput.AlignVCenter
            color: Theme.text
            font.pixelSize: 13
            selectByMouse: true
            clip: true
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
                Note { text: qsTr("Sound takes two stages here. Applications play into the Windows default output; this app taps them there and sends the result to the endpoint you hear. For the first stage to be silent, the default must be a device that discards what it is given: the Desktop Atmos driver. Until it is installed, any silent endpoint whose name matches the filter under Advanced stands in.") }
                // What a person needs to know, in the order it matters: is the
                // device there; do applications play to it; and, only while it
                // is not there, what stands between this machine and a
                // test-signed driver. A packaged install carries the signed
                // driver and installs it with the application (Phase 6), so
                // the source-build tools (the folder, install, remove) live
                // under Advanced rather than in the way.
                Card {
                    ColumnLayout {
                        spacing: Theme.space2
                        StatusRow {
                            ok: CrucibleController.nullSinkPresent
                            text: CrucibleController.nullSinkPresent ? qsTr("The silent device is installed: an endpoint named like \"%1\".").arg(CrucibleController.nullSinkName) : qsTr("No silent device: nothing named like \"%1\" exists, so applications can only play to a real device and are heard directly.").arg(CrucibleController.nullSinkName)
                        }
                        StatusRow {
                            ok: CrucibleController.defaultIsNullSink
                            text: CrucibleController.defaultIsNullSink ? qsTr("Applications play to it: it is the Windows default output.") : qsTr("Applications do not play to it yet: the Windows default output is %1. Send them there from the Room or Signal path page.").arg(CrucibleController.defaultOutputName.length ? CrucibleController.defaultOutputName : qsTr("not set"))
                        }
                        StatusRow {
                            visible: !CrucibleController.nullSinkPresent
                            ok: CrucibleController.codeIntegrityKnown && CrucibleController.testSigningOn && !CrucibleController.memoryIntegrityOn
                            text: !CrucibleController.codeIntegrityKnown ? qsTr("Could not read the kernel's code-integrity state.")
                                : (CrucibleController.testSigningOn && !CrucibleController.memoryIntegrityOn
                                    ? qsTr("This machine can load the test-signed driver: test signing is on and memory integrity is off.")
                                    : qsTr("This machine cannot load the test-signed driver yet: ")
                                      + (CrucibleController.testSigningOn ? "" : qsTr("turn test signing on (bcdedit /set testsigning on, then restart)"))
                                      + (!CrucibleController.testSigningOn && CrucibleController.memoryIntegrityOn ? qsTr(" and ") : "")
                                      + (CrucibleController.memoryIntegrityOn ? qsTr("turn memory integrity off (Windows Security, Core isolation, then restart)") : "")
                                      + ".")
                        }
                        Note {
                            visible: !CrucibleController.nullSinkPresent
                            text: CrucibleController.driverPackageFound
                                ? qsTr("A built driver package is in the driver folder, ready to install.")
                                : qsTr("An installed copy of this application brings the driver with it. This is a build from source: build the driver, then point Advanced at its folder, or put a built package there.")
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: Theme.space2
                            CrucibleButton { objectName: "installDriverButton"; visible: !CrucibleController.nullSinkPresent; text: qsTr("Install driver"); primary: true; enabled: CrucibleController.driverPackageFound && !CrucibleController.driverBusy; onClicked: CrucibleController.installDriver() }
                            CrucibleButton { text: qsTr("Check again"); enabled: !CrucibleController.driverBusy; onClicked: CrucibleController.refreshDriver() }
                        }
                        Note { visible: CrucibleController.driverMessage.length > 0; text: CrucibleController.driverMessage }
                    }
                }
                // Advanced: the source-build tools, and what counts as silent.
                ColumnLayout {
                    id: advanced
                    Layout.fillWidth: true
                    spacing: Theme.space2
                    property bool open: false
                    Item {
                        Layout.fillWidth: true
                        implicitHeight: advancedRow.implicitHeight
                        Row {
                            id: advancedRow
                            spacing: Theme.space2
                            Text { text: advanced.open ? "▾" : "▸"; color: Theme.textMuted; font.pixelSize: 12 }
                            Text { text: qsTr("Advanced"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall; font.letterSpacing: 0.5 }
                        }
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: advanced.open = !advanced.open }
                    }
                    SettingRow {
                        visible: advanced.open
                        label: qsTr("Driver folder")
                        Field {
                            objectId: "driverFolderInput"
                            text: CrucibleController.driverDir
                            input.onEditingFinished: CrucibleController.driverDir = input.text
                        }
                    }
                    Note { visible: advanced.open; text: qsTr("Where install.ps1, remove.ps1 and the built package live: beside this app by default, or apps/windows/driver in a source tree.") + " " + (CrucibleController.driverPackageFound ? qsTr("A built package is there.") : qsTr("No built package is there.")) }
                    Flow {
                        visible: advanced.open
                        Layout.fillWidth: true
                        spacing: Theme.space2
                        CrucibleButton { text: qsTr("Remove driver"); enabled: CrucibleController.driverPackageFound && !CrucibleController.driverBusy; onClicked: CrucibleController.removeDriver() }
                    }
                    Note { visible: advanced.open; text: qsTr("Removes the device and the driver package this folder's remove.ps1 knows about; an installed copy of the application removes its own on uninstall.") }
                    SettingRow {
                        visible: advanced.open
                        label: qsTr("Silent device")
                        Field {
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
                        implicitHeight: 30
                        color: Theme.neutral100
                        border.color: Theme.divider
                        border.width: 1
                        Text {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            verticalAlignment: Text.AlignVCenter
                            text: CrucibleController.keyPath.length ? CrucibleController.keyPath : qsTr("no key file chosen")
                            color: CrucibleController.keyPath.length ? Theme.text : Theme.textMuted
                            font.family: Theme.monoFamily
                            font.pixelSize: 12
                            elide: Text.ElideMiddle
                        }
                    }
                    CrucibleButton { text: qsTr("Browse…"); onClicked: keyDialog.open() }
                    CrucibleButton { text: qsTr("Clear"); enabled: CrucibleController.keyPath.length > 0; onClicked: CrucibleController.clearKey() }
                }
                RowLayout {
                    spacing: Theme.space2
                    Rectangle { width: 8; height: 8; color: CrucibleController.objectsEnabled ? Theme.accent : Theme.neutral500 }
                    Text { Layout.fillWidth: true; text: CrucibleController.signingStatus; color: Theme.text; font.pixelSize: 13; wrapMode: Text.WordWrap }
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
                        implicitWidth: 150
                        implicitHeight: 30
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
                        font.pixelSize: 12
                        background: Rectangle { color: Theme.neutral100; border.color: Theme.divider; border.width: 1 }
                        contentItem: Text { leftPadding: 10; text: bitrateBox.displayText; color: Theme.text; font.family: Theme.monoFamily; font.pixelSize: 12; verticalAlignment: Text.AlignVCenter }
                        indicator: Text { x: bitrateBox.width - 22; anchors.verticalCenter: parent.verticalCenter; text: "⌄"; color: Theme.textMuted; font.pixelSize: 14 }
                        popup.background: Rectangle { color: Theme.surface; border.color: Theme.divider; border.width: 1 }
                        delegate: ItemDelegate {
                            required property var modelData
                            required property int index
                            width: bitrateBox.width
                            contentItem: Text { text: modelData.label; color: Theme.text; font.family: Theme.monoFamily; font.pixelSize: 12 }
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
                        implicitWidth: 240
                        implicitHeight: 30
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
                        font.pixelSize: 13
                        background: Rectangle { color: Theme.neutral100; border.color: Theme.divider; border.width: 1 }
                        contentItem: Text { leftPadding: 10; text: languageBox.displayText; color: Theme.text; font.pixelSize: 13; verticalAlignment: Text.AlignVCenter }
                        indicator: Text { x: languageBox.width - 22; anchors.verticalCenter: parent.verticalCenter; text: "⌄"; color: Theme.textMuted; font.pixelSize: 14 }
                        popup.background: Rectangle { color: Theme.surface; border.color: Theme.divider; border.width: 1 }
                        delegate: ItemDelegate {
                            required property var modelData
                            required property int index
                            width: languageBox.width
                            contentItem: Text { text: modelData.name; color: Theme.text; font.pixelSize: 13 }
                            background: Rectangle { color: highlighted ? Theme.neutral200 : "transparent" }
                            highlighted: languageBox.highlightedIndex === index
                        }
                    }
                    Note { text: qsTr("System follows Windows; the translations are mechanical for now") }
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
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import Ac3ForgeDesk
import Ac3ForgeDeskLanguage

// Settings: latency, codec, the signing key, the virtual device,
// appearance, behaviour.
Flickable {
    id: page
    contentHeight: grid.implicitHeight + Theme.space6 * 2
    clip: true

    FileDialog {
        id: keyDialog
        title: qsTr("Choose the signing key")
        onAccepted: DeskController.loadKey(selectedFile.toString())
    }

    GridLayout {
        id: grid
        x: Theme.space6
        y: Theme.space6
        width: page.width - Theme.space6 * 2
        columns: 2
        columnSpacing: Theme.space6
        rowSpacing: Theme.space6

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space6

            ColumnLayout {
                spacing: Theme.space3
                RailBlock { ordinal: "01"; label: qsTr("LATENCY"); Layout.fillWidth: true }
                SegmentedControl {
                    model: [{ label: qsTr("Normal · 32 ms frames"), value: "normal" }, { label: qsTr("Low · 5.3 ms frames"), value: "low" }]
                    currentValue: DeskController.lowLatency ? "low" : "normal"
                    accessibleName: qsTr("Latency")
                    onSelected: function(value) { DeskController.lowLatency = value === "low"; }
                }
                Text { Layout.fillWidth: true; text: qsTr("Low latency shortens the E-AC-3 frame to one block and raises the bitrate to about 1.5 Mb/s so fifteen objects' metadata still fits. The receiver's own decode delay does not change. Changing this restarts the stream."); color: Theme.textMuted; font.pixelSize: Theme.fontSmall; wrapMode: Text.WordWrap }
            }

            ColumnLayout {
                spacing: Theme.space3
                RailBlock { ordinal: "02"; label: qsTr("CODEC"); Layout.fillWidth: true }
                RowLayout {
                    spacing: Theme.space3
                    Text { Layout.preferredWidth: 120; text: qsTr("Bitrate"); color: Theme.text; font.pixelSize: 13 }
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
                        currentIndex: indexOfValue(DeskController.bitrate)
                        onActivated: DeskController.bitrate = currentValue
                        Connections { target: DeskController; function onSettingsChanged() { bitrateBox.currentIndex = bitrateBox.indexOfValue(DeskController.bitrate); } }
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
                    Text { text: qsTr("E-AC-3 · 5.1 bed + up to 15 objects · automatic is 448 kb/s, or 1536 kb/s in low latency"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall }
                }
                DeskCheck { text: qsTr("Split stereo applications into two objects"); note: qsTr("Costs two slots per application; the pair sits either side of the position you place. Not built yet."); checked: false; enabled: false }
            }

            ColumnLayout {
                spacing: Theme.space3
                RailBlock { ordinal: "03"; label: qsTr("SIGNING KEY"); Layout.fillWidth: true }
                RowLayout {
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
                            text: DeskController.keyPath.length ? DeskController.keyPath : qsTr("no key file chosen · AC3FORGE_SIGNING_KEY_FILE and AC3FORGE_SIGNING_KEY are honoured")
                            color: DeskController.keyPath.length ? Theme.text : Theme.textMuted
                            font.family: Theme.monoFamily
                            font.pixelSize: 12
                            elide: Text.ElideMiddle
                        }
                    }
                    DeskButton { text: qsTr("Browse…"); onClicked: keyDialog.open() }
                    DeskButton { text: qsTr("Clear"); enabled: DeskController.keyPath.length > 0; onClicked: DeskController.clearKey() }
                }
                RowLayout {
                    spacing: Theme.space2
                    Rectangle { width: 8; height: 8; color: DeskController.objectsEnabled ? Theme.accent : Theme.neutral500 }
                    Text { Layout.fillWidth: true; text: DeskController.signingStatus; color: Theme.text; font.pixelSize: 13; wrapMode: Text.WordWrap }
                }
                Text { Layout.fillWidth: true; text: qsTr("Only the path is remembered; the key stays in its file. Without a key the app streams the 5.1 bed only: an unsigned object container would be refused outright by a validating decoder."); color: Theme.textMuted; font.pixelSize: Theme.fontSmall; wrapMode: Text.WordWrap }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space6

            ColumnLayout {
                spacing: Theme.space3
                RailBlock { ordinal: "04"; label: qsTr("VIRTUAL OUTPUT DEVICE"); Layout.fillWidth: true }
                Card {
                    ColumnLayout {
                        spacing: Theme.space2
                        RowLayout {
                            spacing: Theme.space2
                            Rectangle { width: 8; height: 8; color: DeskController.nullSinkPresent ? Theme.accent : Theme.neutral500 }
                            Text { Layout.fillWidth: true; text: DeskController.nullSinkPresent ? qsTr("an endpoint named like \"%1\" is present").arg(DeskController.nullSinkName) : qsTr("no endpoint named like \"%1\"").arg(DeskController.nullSinkName); color: Theme.text; font.pixelSize: 13; wrapMode: Text.WordWrap }
                        }
                        Text { Layout.fillWidth: true; text: qsTr("A null-sink driver that discards what it is given. Applications render into it; surround-capable games render 7.1 into it and reach the bed intact. The driver itself is Phase 4; until then any silent endpoint stands in."); color: Theme.textMuted; font.pixelSize: Theme.fontSmall; wrapMode: Text.WordWrap }
                    }
                }
                RowLayout {
                    spacing: Theme.space3
                    Text { Layout.preferredWidth: 120; text: qsTr("Silent device"); color: Theme.text; font.pixelSize: 13 }
                    Rectangle {
                        implicitWidth: 240
                        implicitHeight: 30
                        color: Theme.neutral100
                        border.color: Theme.divider
                        border.width: 1
                        TextInput {
                            id: nullSinkInput
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            verticalAlignment: TextInput.AlignVCenter
                            text: DeskController.nullSinkName
                            color: Theme.text
                            font.pixelSize: 13
                            selectByMouse: true
                            onEditingFinished: DeskController.nullSinkName = text
                        }
                    }
                    Text { Layout.fillWidth: true; text: qsTr("any endpoint whose name contains this is never an output"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall; wrapMode: Text.WordWrap }
                }
            }

            ColumnLayout {
                spacing: Theme.space3
                RailBlock { ordinal: "05"; label: qsTr("APPEARANCE"); Layout.fillWidth: true }
                RowLayout {
                    spacing: Theme.space3
                    Text { Layout.preferredWidth: 120; text: qsTr("Theme"); color: Theme.text; font.pixelSize: 13 }
                    SegmentedControl {
                        model: [{ label: qsTr("System"), value: "system" }, { label: qsTr("Light"), value: "light" }, { label: qsTr("Dark"), value: "dark" }]
                        currentValue: DeskController.theme
                        accessibleName: qsTr("Theme")
                        onSelected: function(value) { DeskController.theme = value; }
                    }
                }
                RowLayout {
                    spacing: Theme.space3
                    Text { Layout.preferredWidth: 120; text: qsTr("Language"); color: Theme.text; font.pixelSize: 13 }
                    ComboBox {
                        id: languageBox
                        implicitWidth: 240
                        implicitHeight: 30
                        // "System" first, then every language the app ships.
                        model: [{ code: "", name: qsTr("System") }].concat(LanguageManager.availableLanguages())
                        textRole: "name"
                        valueRole: "code"
                        currentIndex: LanguageManager.hasOverride() ? indexOfValue(LanguageManager.currentLanguage) : 0
                        onActivated: currentValue === "" ? LanguageManager.useSystemLanguage() : LanguageManager.setLanguage(currentValue)
                        font.pixelSize: 13
                        background: Rectangle { color: Theme.neutral100; border.color: Theme.divider; border.width: 1 }
                        contentItem: Text { leftPadding: 10; text: languageBox.displayText; color: Theme.text; font.pixelSize: 13; verticalAlignment: Text.AlignVCenter }
                        indicator: Text { x: languageBox.width - 22; anchors.verticalCenter: parent.verticalCenter; text: "\u2304"; color: Theme.textMuted; font.pixelSize: 14 }
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
                    Text { Layout.fillWidth: true; text: qsTr("System follows Windows; the translations are mechanical for now"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall; wrapMode: Text.WordWrap }
                }
                RowLayout {
                    spacing: Theme.space3
                    Text { Layout.preferredWidth: 120; text: qsTr("Palette"); color: Theme.text; font.pixelSize: 13 }
                    SegmentedControl {
                        model: [{ label: qsTr("Signal"), value: "signal" }, { label: qsTr("Ink"), value: "ink" }, { label: qsTr("Console"), value: "console" }, { label: qsTr("System"), value: "system" }]
                        currentValue: DeskController.palette
                        accessibleName: qsTr("Palette")
                        onSelected: function(value) { DeskController.palette = value; }
                    }
                }
            }

            ColumnLayout {
                spacing: Theme.space3
                RailBlock { ordinal: "06"; label: qsTr("BEHAVIOUR"); Layout.fillWidth: true }
                DeskCheck {
                    text: qsTr("Move the default output to the silent device on launch")
                    note: qsTr("Restored to the previous device on quit.")
                    checked: DeskController.moveDefaultOnLaunch
                    onToggled: function(on) { DeskController.moveDefaultOnLaunch = on; }
                }
                DeskCheck {
                    text: qsTr("Keep running in the tray when the window is closed")
                    checked: DeskController.keepRunningWhenClosed
                    onToggled: function(on) { DeskController.keepRunningWhenClosed = on; }
                }
            }
        }
    }
}

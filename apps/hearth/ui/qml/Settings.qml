import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import Ac3ForgeHearth

// The Settings page (planning/hearth-design.md; issue #853): playback,
// network (with the pairing records A6's Sendspin server will start filling
// in), appearance, language and a diagnostics export - the same five cards
// the mockup shows, in the same order. Playback and network are real engine
// settings, kept through HearthController's QSettingsStore
// (apps/hearth/ui/hearth_controller.cpp) the way
// apps/hearth/engine/settings_model.hpp says the window has to. Appearance
// writes straight to Theme, the way apps/crucible/ui/qml/SettingsPage.qml's
// own theme/palette/textScale trio does - Main.qml binds Theme to it the
// same way.
ScrollView {
    id: root
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    // The label gutter settings.png measures: every control in both columns
    // starts 122 px after the card's content edge, which is this plus
    // Theme.gap. In pixels scaled by the text-size setting, since a label
    // that grows has to keep its column.
    readonly property int labelWidth: Math.round(110 * Theme.fontScale)

    // Where the diagnostics file goes. selectedFile is set before open(), so
    // the suggested name and folder appear in the dialog - the same shape as
    // apps/gui/qml/PreferencesDialog.qml's own diagnosticsDialog.
    FileDialog {
        id: diagnosticsDialog
        title: qsTr("Save diagnostics")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "txt"
        nameFilters: [qsTr("Text files (*.txt)"), qsTr("All files (*)")]
        onAccepted: HearthController.exportDiagnostics(selectedFile.toString())
    }

    ColumnLayout {
        width: root.availableWidth
        implicitWidth: root.availableWidth
        spacing: Theme.gap * 2

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.pad
            spacing: Theme.gap * 2

            // --- left column: playback and network -----------------------
            ColumnLayout {
                Layout.preferredWidth: 1
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: Theme.gap * 2

                Card {
                    ordinal: "01"
                    title: qsTr("Playback")
                    framed: true

                    AppCheckBox {
                        objectName: "settingsGapless"
                        Layout.fillWidth: true
                        text: qsTr("Gapless between items")
                        note: qsTr("Keeps the output open from one item to the next when both have the "
                                  + "same sample rate and speaker layout. When either changes, the "
                                  + "output reopens and the queue says so.")
                        checked: HearthController.gapless
                        onToggled: function(on) { HearthController.gapless = on; }
                    }

                    AppCheckBox {
                        objectName: "settingsResumeQueue"
                        Layout.fillWidth: true
                        text: qsTr("Pick up the queue where it was left")
                        note: qsTr("On the next start, at the item and position playing when Hearth closed.")
                        checked: HearthController.resumeQueue
                        onToggled: function(on) { HearthController.resumeQueue = on; }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text {
                            text: qsTr("An item fails")
                            color: Theme.text
                            font.pixelSize: Theme.fontNormal
                            elide: Text.ElideRight
                            Layout.preferredWidth: root.labelWidth
                        }
                        SegmentedControl {
                            accessibleName: qsTr("When an item fails")
                            currentValue: HearthController.onFailure
                            model: [
                                { value: "skip", label: qsTr("Skip to the next") },
                                { value: "stop", label: qsTr("Stop") }
                            ]
                            onSelected: function(value) { HearthController.onFailure = value; }
                        }
                    }
                }

                Card {
                    ordinal: "02"
                    title: qsTr("Network")
                    framed: true

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text {
                            text: qsTr("Name")
                            color: Theme.text
                            font.pixelSize: Theme.fontNormal
                            elide: Text.ElideRight
                            Layout.preferredWidth: root.labelWidth
                        }
                        AppTextField {
                            id: networkNameField
                            objectName: "settingsNetworkName"
                            Layout.preferredWidth: Math.round(320 * Theme.fontScale)
                            text: HearthController.networkName
                            Accessible.name: qsTr("Name")
                            onEditingFinished: HearthController.networkName = text
                            // Typing writes `text` directly, which destroys
                            // the declarative binding for good - so the field
                            // is re-read from the controller whenever it is
                            // not the one being edited. Without this it shows
                            // whatever was typed even when the engine kept
                            // something else.
                            Binding on text {
                                value: HearthController.networkName
                                when: !networkNameField.activeFocus
                                restoreMode: Binding.RestoreBindingOrValue
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Item { Layout.preferredWidth: root.labelWidth }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("How sinks and players show this computer.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                    }

                    AppCheckBox {
                        objectName: "settingsNetworkDiscover"
                        Layout.fillWidth: true
                        text: qsTr("Look for Sendspin players on this network")
                        note: qsTr("Over mDNS. Off, the Network page lists only players already paired.")
                        checked: HearthController.networkDiscover
                        onToggled: function(on) { HearthController.networkDiscover = on; }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: Theme.gap
                        spacing: Theme.gap / 2

                        Text {
                            text: qsTr("PAIRING RECORDS")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontMicro
                            font.bold: true
                            font.letterSpacing: Theme.trackingWide
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: HearthController.pairingRecords.length === 0
                            text: qsTr("No sink or player has paired with this computer yet.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }

                        // One bordered table with a rule under the header and
                        // between the rows, not a stack of loose rows - the
                        // shape settings.png draws (and the same one the
                        // output picker's device list wants).
                        Rectangle {
                            Layout.fillWidth: true
                            visible: HearthController.pairingRecords.length > 0
                            implicitHeight: pairingTable.implicitHeight
                            color: Theme.bg
                            border.color: Theme.divider
                            border.width: 1
                            radius: Theme.radius

                            ColumnLayout {
                                id: pairingTable
                                anchors.fill: parent
                                spacing: 0

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.margins: Theme.space2
                                    spacing: Theme.gap
                                    Text {
                                        Layout.fillWidth: true
                                        text: qsTr("SINK OR PLAYER")
                                        color: Theme.textMuted
                                        font.pixelSize: Theme.fontMicro
                                        font.bold: true
                                        font.letterSpacing: Theme.trackingWide
                                    }
                                    Text {
                                        Layout.preferredWidth: Math.round(132 * Theme.fontScale)
                                        text: qsTr("PAIRED")
                                        color: Theme.textMuted
                                        font.pixelSize: Theme.fontMicro
                                        font.bold: true
                                        font.letterSpacing: Theme.trackingWide
                                    }
                                    Item { Layout.preferredWidth: Math.round(64 * Theme.fontScale) }
                                }
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 1
                                    color: Theme.divider
                                }

                                Repeater {
                                    model: HearthController.pairingRecords

                                    delegate: ColumnLayout {
                                        id: pairingRow
                                        required property var modelData
                                        required property int index
                                        Layout.fillWidth: true
                                        spacing: 0

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.margins: Theme.space2
                                            spacing: Theme.gap

                                            Text {
                                                Layout.fillWidth: true
                                                text: pairingRow.modelData.name.length > 0
                                                      ? pairingRow.modelData.name
                                                      : pairingRow.modelData.id.substring(0, 12)
                                                color: Theme.text
                                                font.pixelSize: Theme.fontNormal
                                                elide: Text.ElideRight
                                            }
                                            Text {
                                                Layout.preferredWidth: Math.round(132 * Theme.fontScale)
                                                text: pairingRow.modelData.pairedOn.length > 0
                                                      ? qsTr("paired %1").arg(pairingRow.modelData.pairedOn)
                                                      : ""
                                                color: Theme.textMuted
                                                font.family: Theme.monoFamily
                                                font.pixelSize: Theme.fontSmall
                                                elide: Text.ElideRight
                                            }
                                            AppButton {
                                                objectName: "pairingForget-" + pairingRow.index
                                                Layout.preferredWidth: Math.round(64 * Theme.fontScale)
                                                text: qsTr("Forget")
                                                Accessible.description: qsTr("Forgets this pairing; it has to pair again with a new code.")
                                                onClicked: HearthController.forgetPairing(pairingRow.modelData.id)
                                            }
                                        }
                                        Rectangle {
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 1
                                            visible: pairingRow.index < HearthController.pairingRecords.length - 1
                                            color: Theme.divider
                                        }
                                    }
                                }
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Kept in this computer's settings folder, readable by your account only. "
                                      + "Forgetting one means pairing again with a new code.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            // --- right column: appearance, language, diagnostics ---------
            ColumnLayout {
                Layout.preferredWidth: 1
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: Theme.gap * 2

                Card {
                    ordinal: "03"
                    title: qsTr("Appearance")
                    framed: true

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text {
                            text: qsTr("Theme")
                            color: Theme.text
                            font.pixelSize: Theme.fontNormal
                            elide: Text.ElideRight
                            Layout.preferredWidth: root.labelWidth
                        }
                        SegmentedControl {
                            accessibleName: qsTr("Theme")
                            currentValue: HearthController.theme
                            model: [
                                { value: "system", label: qsTr("System") },
                                { value: "light", label: qsTr("Light") },
                                { value: "dark", label: qsTr("Dark") }
                            ]
                            onSelected: function(value) { HearthController.theme = value; }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text {
                            text: qsTr("Palette")
                            color: Theme.text
                            font.pixelSize: Theme.fontNormal
                            elide: Text.ElideRight
                            Layout.preferredWidth: root.labelWidth
                        }
                        SegmentedControl {
                            accessibleName: qsTr("Palette")
                            currentValue: HearthController.palette
                            model: [
                                //: Palette name. A product name: leave it as it is unless the language has an established rendering of its own.
                                { value: "signal", label: qsTr("Signal") },
                                //: Palette name, as "Signal" above.
                                { value: "ink", label: qsTr("Ink") },
                                //: Palette name, as "Signal" above.
                                { value: "console", label: qsTr("Console") },
                                { value: "system", label: qsTr("System") }
                            ]
                            onSelected: function(value) { HearthController.palette = value; }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text {
                            text: qsTr("Text size")
                            color: Theme.text
                            font.pixelSize: Theme.fontNormal
                            elide: Text.ElideRight
                            Layout.preferredWidth: root.labelWidth
                        }
                        SegmentedControl {
                            objectName: "settingsTextSize"
                            accessibleName: qsTr("Text size")
                            currentValue: HearthController.textScale
                            model: [
                                { value: "100", label: "100%" },
                                { value: "125", label: "125%" },
                                { value: "150", label: "150%" },
                                { value: "175", label: "175%" },
                                { value: "system", label: qsTr("System") }
                            ]
                            onSelected: function(value) { HearthController.textScale = value; }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Item { Layout.preferredWidth: root.labelWidth }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Every size in the window follows this; 100% is the size it is drawn at. "
                                      + "System takes the text size the desktop reports and counts 9 pt as 100%.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                Card {
                    ordinal: "04"
                    title: qsTr("Language")
                    framed: true

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text {
                            text: qsTr("Language")
                            color: Theme.text
                            font.pixelSize: Theme.fontNormal
                            elide: Text.ElideRight
                            Layout.preferredWidth: root.labelWidth
                        }
                        AppComboBox {
                            enabled: false
                            Layout.preferredWidth: Math.round(320 * Theme.fontScale)
                            model: [qsTr("English · the system language")]
                            Accessible.name: qsTr("Language")
                        }
                        Item { Layout.fillWidth: true }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Item { Layout.preferredWidth: root.labelWidth }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Not adjustable from this build yet: Hearth has no translation catalogues "
                                      + "wired in, so every page reads in English regardless of the desktop's own "
                                      + "language. A later slice adds the six languages ac3gui and Crucible already "
                                      + "offer.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                Card {
                    ordinal: "05"
                    title: qsTr("Diagnostics")
                    framed: true

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("A text file of what Hearth has done: outputs opened, streams played, sinks "
                                  + "found and paired, and every error. Pairing keys, codes and file paths are "
                                  + "left out.")
                        color: Theme.text
                        font.pixelSize: Theme.fontNormal
                        wrapMode: Text.WordWrap
                    }
                    AppButton {
                        objectName: "settingsDiagnosticsButton"
                        text: qsTr("Save diagnostics…")
                        Accessible.description: qsTr("Writes a plain-text support file where you choose. Nothing is sent anywhere.")
                        onClicked: {
                            diagnosticsDialog.selectedFile = HearthController.suggestedDiagnosticsFile();
                            diagnosticsDialog.open();
                        }
                    }
                    Text {
                        objectName: "settingsDiagnosticsMessage"
                        Layout.fillWidth: true
                        visible: HearthController.diagnosticsMessage.length > 0
                        text: HearthController.diagnosticsMessage
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }
}

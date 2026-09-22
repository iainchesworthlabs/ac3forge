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
            spacing: Theme.gap * 2

            // --- left column: playback and network -----------------------
            ColumnLayout {
                Layout.preferredWidth: 1
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: Theme.gap * 2

                Card {
                    title: qsTr("01 Playback")

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        CheckBox {
                            objectName: "settingsGapless"
                            text: qsTr("Gapless between items")
                            checked: HearthController.gapless
                            onToggled: HearthController.gapless = checked
                        }
                        Text {
                            Layout.fillWidth: true
                            Layout.leftMargin: 32
                            text: qsTr("Keeps the output open from one item to the next when both have the "
                                      + "same sample rate and speaker layout. When either changes, the "
                                      + "output reopens and the queue says so.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        CheckBox {
                            objectName: "settingsResumeQueue"
                            text: qsTr("Pick up the queue where it was left")
                            checked: HearthController.resumeQueue
                            onToggled: HearthController.resumeQueue = checked
                        }
                        Text {
                            Layout.fillWidth: true
                            Layout.leftMargin: 32
                            text: qsTr("On the next start, at the item and position playing when Hearth closed.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("An item fails"); color: Theme.textMuted; Layout.preferredWidth: 90 }
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
                    title: qsTr("02 Network")

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Name"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                        TextField {
                            id: networkNameField
                            objectName: "settingsNetworkName"
                            Layout.fillWidth: true
                            text: HearthController.networkName
                            Accessible.name: qsTr("Name")
                            onEditingFinished: HearthController.networkName = text
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("How sinks and players show this computer.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        CheckBox {
                            objectName: "settingsNetworkDiscover"
                            text: qsTr("Look for Sendspin players on this network")
                            checked: HearthController.networkDiscover
                            onToggled: HearthController.networkDiscover = checked
                        }
                        Text {
                            Layout.fillWidth: true
                            Layout.leftMargin: 32
                            text: qsTr("Over mDNS. Off, the Network page lists only players already paired.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
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
                            font.letterSpacing: 1
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: HearthController.pairingRecords.length === 0
                            text: qsTr("No sink or player has paired with this computer yet.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: HearthController.pairingRecords.length > 0
                            spacing: Theme.gap / 2

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.gap
                                Text { Layout.fillWidth: true; text: qsTr("SINK OR PLAYER"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro }
                                Text { Layout.preferredWidth: 96; text: qsTr("PAIRED"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro }
                                Item { Layout.preferredWidth: 64 }
                            }

                            Repeater {
                                model: HearthController.pairingRecords

                                delegate: RowLayout {
                                    id: pairingRow
                                    required property var modelData
                                    required property int index
                                    Layout.fillWidth: true
                                    spacing: Theme.gap

                                    Text {
                                        Layout.fillWidth: true
                                        text: pairingRow.modelData.name.length > 0
                                              ? pairingRow.modelData.name
                                              : pairingRow.modelData.id.substring(0, 12)
                                        color: Theme.text
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.preferredWidth: 96
                                        text: pairingRow.modelData.pairedOn
                                        color: Theme.textMuted
                                        font.family: Theme.monoFamily
                                        font.pixelSize: Theme.fontSmall
                                    }
                                    Button {
                                        objectName: "pairingForget-" + pairingRow.index
                                        Layout.preferredWidth: 64
                                        text: qsTr("Forget")
                                        Accessible.description: qsTr("Forgets this pairing; it has to pair again with a new code.")
                                        onClicked: HearthController.forgetPairing(pairingRow.modelData.id)
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
                    title: qsTr("03 Appearance")

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Theme"); color: Theme.textMuted; Layout.preferredWidth: 90 }
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
                        Text { text: qsTr("Palette"); color: Theme.textMuted; Layout.preferredWidth: 90 }
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
                        Text { text: qsTr("Text size"); color: Theme.textMuted; Layout.preferredWidth: 90 }
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
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Every size in the window follows this; 100% is the size it is drawn at. "
                                  + "System takes the text size the desktop reports and counts 9 pt as 100%.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }

                Card {
                    title: qsTr("04 Language")

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Language"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                        ComboBox {
                            enabled: false
                            Layout.fillWidth: true
                            model: [qsTr("English · the system language")]
                            Accessible.name: qsTr("Language")
                        }
                    }
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

                Card {
                    title: qsTr("05 Diagnostics")

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("A text file of what Hearth has done: outputs opened, streams played, sinks "
                                  + "found and paired, and every error. Pairing keys, codes and file paths are "
                                  + "left out.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                    Button {
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

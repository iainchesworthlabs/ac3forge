import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeDesk

// Output: what is going out and why, every endpoint with what the probe
// found, the pin, the default-output switch, and the codec-path switch.
Flickable {
    id: page
    contentHeight: column.implicitHeight + Theme.space6 * 2
    clip: true

    ColumnLayout {
        id: column
        x: Theme.space6
        y: Theme.space6
        width: page.width - Theme.space6 * 2
        spacing: Theme.space6

        RailBlock { ordinal: "01"; label: qsTr("NOW"); Layout.fillWidth: true }
        Card {
            RowLayout {
                spacing: Theme.space6
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space2
                    Text { text: DeskController.modeName + (DeskController.modeKey === "atmos" ? qsTr(" · E-AC-3 JOC over HDMI") : ""); color: Theme.text; font.family: Theme.headingFamily; font.pixelSize: Theme.fontTitle; font.weight: Font.Bold }
                    Text { text: DeskController.endpointName; color: Theme.text; font.pixelSize: Theme.fontNormal; visible: text.length > 0 }
                    Text { Layout.fillWidth: true; text: DeskController.outputReason + qsTr(" The output follows the hardware: pull HDMI and it moves to the next best endpoint below."); color: Theme.textMuted; font.pixelSize: 13; wrapMode: Text.WordWrap }
                }
                ColumnLayout {
                    Layout.preferredWidth: 420
                    Layout.alignment: Qt.AlignTop
                    spacing: Theme.space2
                    Text { text: qsTr("PIN"); color: Theme.textMuted; font.pixelSize: 11; font.letterSpacing: 1 }
                    ComboBox {
                        id: pinBox
                        Layout.fillWidth: true
                        implicitHeight: 30
                        model: [
                            { label: qsTr("Automatic · best the hardware can carry"), value: "auto" },
                            { label: qsTr("Atmos"), value: "atmos" },
                            { label: qsTr("Dolby Digital Plus 5.1"), value: "ddplus" },
                            { label: qsTr("Dolby Digital 5.1"), value: "dd" },
                            { label: qsTr("PCM surround"), value: "pcm" },
                            { label: qsTr("Headphones (Windows Spatial Sound)"), value: "headphones" },
                            { label: qsTr("Stereo"), value: "stereo" }]
                        textRole: "label"
                        valueRole: "value"
                        currentIndex: indexOfValue(DeskController.pinned)
                        onActivated: DeskController.pinned = currentValue
                        Connections { target: DeskController; function onSettingsChanged() { pinBox.currentIndex = pinBox.indexOfValue(DeskController.pinned); } }
                        font.pixelSize: 13
                        background: Rectangle { color: Theme.neutral100; border.color: Theme.divider; border.width: 1 }
                        contentItem: Text { leftPadding: 10; text: pinBox.displayText; color: Theme.text; font.pixelSize: 13; verticalAlignment: Text.AlignVCenter }
                        indicator: Text { x: pinBox.width - 22; anchors.verticalCenter: parent.verticalCenter; text: "⌄"; color: Theme.textMuted; font.pixelSize: 14 }
                        popup.background: Rectangle { color: Theme.surface; border.color: Theme.divider; border.width: 1 }
                        delegate: ItemDelegate {
                            required property var modelData
                            required property int index
                            width: pinBox.width
                            contentItem: Text { text: modelData.label; color: Theme.text; font.pixelSize: 13 }
                            background: Rectangle { color: highlighted ? Theme.neutral200 : "transparent" }
                            highlighted: pinBox.highlightedIndex === index
                        }
                    }
                    Text { Layout.fillWidth: true; text: qsTr("A pin holds as long as some endpoint can carry it, then falls back and says why."); color: Theme.textMuted; font.pixelSize: Theme.fontSmall; wrapMode: Text.WordWrap }
                }
            }
        }

        RailBlock {
            ordinal: "02"
            label: qsTr("ENDPOINTS")
            Layout.fillWidth: true
            Text { text: qsTr("re-probed on every device change"); color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: 11 }
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: table.implicitHeight
            color: Theme.surface
            border.color: Theme.divider
            border.width: 1
            ColumnLayout {
                id: table
                width: parent.width
                spacing: 0
                RowLayout {
                    Layout.fillWidth: true
                    Layout.margins: Theme.space2
                    Layout.leftMargin: Theme.space3
                    Layout.rightMargin: Theme.space3
                    spacing: Theme.space2
                    Text { Layout.preferredWidth: 300; text: qsTr("ENDPOINT"); color: Theme.textMuted; font.pixelSize: 11; font.letterSpacing: 1 }
                    Text { Layout.preferredWidth: 70; text: qsTr("E-AC-3"); color: Theme.textMuted; font.pixelSize: 11; font.letterSpacing: 1; horizontalAlignment: Text.AlignHCenter }
                    Text { Layout.preferredWidth: 70; text: qsTr("AC-3"); color: Theme.textMuted; font.pixelSize: 11; font.letterSpacing: 1; horizontalAlignment: Text.AlignHCenter }
                    Text { Layout.preferredWidth: 70; text: qsTr("PCM CH"); color: Theme.textMuted; font.pixelSize: 11; font.letterSpacing: 1; horizontalAlignment: Text.AlignHCenter }
                    Text { Layout.preferredWidth: 80; text: qsTr("SPATIAL"); color: Theme.textMuted; font.pixelSize: 11; font.letterSpacing: 1; horizontalAlignment: Text.AlignHCenter }
                    Text { Layout.fillWidth: true; text: qsTr("NOTE"); color: Theme.textMuted; font.pixelSize: 11; font.letterSpacing: 1 }
                }
                Repeater {
                    model: DeskController.endpoints
                    delegate: ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 0
                        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.margins: 10
                            Layout.leftMargin: Theme.space3
                            Layout.rightMargin: Theme.space3
                            spacing: Theme.space2
                            Text { Layout.preferredWidth: 300; text: modelData.name; color: Theme.text; font.pixelSize: 13; elide: Text.ElideRight }
                            Text { Layout.preferredWidth: 70; text: modelData.eac3 ? "✓" : "—"; color: modelData.eac3 ? Theme.text : Theme.neutral500; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 13 }
                            Text { Layout.preferredWidth: 70; text: modelData.ac3 ? "✓" : "—"; color: modelData.ac3 ? Theme.text : Theme.neutral500; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 13 }
                            Text { Layout.preferredWidth: 70; text: modelData.pcmChannels > 0 ? modelData.pcmChannels : "—"; color: modelData.pcmChannels > 0 ? Theme.text : Theme.neutral500; horizontalAlignment: Text.AlignHCenter; font.family: Theme.monoFamily; font.pixelSize: 12 }
                            Text { Layout.preferredWidth: 80; text: modelData.spatial ? "✓" : "—"; color: modelData.spatial ? Theme.text : Theme.neutral500; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 13 }
                            Text {
                                Layout.fillWidth: true
                                text: modelData.chosen ? qsTr("chosen") + (DeskController.modeKey === "atmos" || DeskController.modeKey === "ddplus" || DeskController.modeKey === "dd" ? qsTr(" · exclusive mode") : "")
                                    : modelData.isNullSink ? qsTr("null sink · ") + (modelData.isDefault ? qsTr("the system default · ") : "") + qsTr("never an output")
                                    : modelData.isDefault ? qsTr("the system default · applications render here")
                                    : modelData.spatial ? qsTr("spatial sound on · headphones fallback")
                                    : modelData.pcmChannels >= 6 ? qsTr("surround PCM fallback") : qsTr("stereo fallback")
                                color: modelData.chosen ? Theme.accent : Theme.textMuted
                                font.pixelSize: Theme.fontSmall
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
                Text {
                    visible: DeskController.endpoints.length === 0
                    Layout.margins: Theme.space3
                    text: qsTr("no render endpoints probed yet")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space6
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space3
                RailBlock { ordinal: "03"; label: qsTr("DEFAULT OUTPUT"); Layout.fillWidth: true }
                Card {
                    ColumnLayout {
                        spacing: Theme.space2
                        Text {
                            Layout.fillWidth: true
                            textFormat: Text.StyledText
                            text: DeskController.defaultIsNullSink
                                ? qsTr("Windows default output is <b>") + DeskController.defaultOutputName + qsTr("</b>. Every application renders into it silently; this app taps each one and sends the result to the endpoint above.")
                                : qsTr("Windows default output is <b>") + (DeskController.defaultOutputName.length ? DeskController.defaultOutputName : qsTr("not set")) + qsTr("</b>, a real device. You hear each application directly as well as through this app, and a receiver on that device cannot be opened exclusively while applications render to it.")
                            color: Theme.text
                            font.pixelSize: 13
                            wrapMode: Text.WordWrap
                        }
                        RowLayout {
                            spacing: Theme.space2
                            DeskButton {
                                text: DeskController.defaultIsNullSink ? qsTr("Restore ") + (DeskController.previousDefaultName.length ? DeskController.previousDefaultName : qsTr("previous output")) : qsTr("Move default to ") + DeskController.nullSinkName
                                enabled: DeskController.defaultIsNullSink ? DeskController.previousDefaultName.length > 0 : DeskController.nullSinkPresent
                                onClicked: DeskController.defaultIsNullSink ? DeskController.restoreDefault() : DeskController.moveDefaultToNullSink()
                            }
                            DeskButton { text: qsTr("Open Sound settings"); onClicked: DeskController.openSoundSettings() }
                            DeskButton { text: qsTr("Re-probe"); onClicked: DeskController.reprobe() }
                        }
                        Text { Layout.fillWidth: true; visible: DeskController.defaultMessage.length > 0; text: DeskController.defaultMessage; color: Theme.accent; font.pixelSize: Theme.fontSmall; wrapMode: Text.WordWrap }
                    }
                }
            }
            ColumnLayout {
                Layout.preferredWidth: 380
                Layout.alignment: Qt.AlignTop
                spacing: Theme.space3
                RailBlock { ordinal: "04"; label: qsTr("CODEC PATH"); Layout.fillWidth: true }
                Card {
                    DeskCheck {
                        text: qsTr("Bypass the codec on headphones and PCM")
                        note: qsTr("Off: headphones and PCM play a decode of the E-AC-3 stream, so what you hear went through the codec. On: taps go straight to the renderer, lower latency, codec out of the loop. Not wired to the engine yet.")
                        checked: false
                        enabled: false
                    }
                }
            }
        }
    }
}

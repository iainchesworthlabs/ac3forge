import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeCrucible

// The signal path: the three stations sound passes through, side by side,
// then each stage's detail: the pin (what you hear it as), every endpoint
// with what the probe found, where applications play, the codec path.
//
// Sizing: one column that scrolls; the two-up rows become one-up below
// about 900 px, the endpoint table drops its note column below about 760,
// and every button row wraps rather than overflows.
Flickable {
    id: page
    contentHeight: column.implicitHeight + Theme.space6 * 2
    clip: true
    readonly property real innerWidth: width - Theme.space6 * 2
    readonly property bool twoUp: innerWidth >= 900
    readonly property bool wideTable: innerWidth >= 760

    ColumnLayout {
        id: column
        x: Theme.space6
        y: Theme.space6
        width: page.innerWidth
        spacing: Theme.space6

        RailBlock {
            ordinal: "01"
            label: qsTr("SIGNAL PATH")
            Layout.fillWidth: true
            Layout.fillHeight: false
            Text { text: qsTr("two devices, two stages: applications play into one, you hear the result on the other"); color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: 11; elide: Text.ElideRight; Layout.fillWidth: true }
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("Every application plays into the Windows default output. With the silent Desktop Atmos device as that default, nothing is heard from it; this app taps each application there, places it in the room, encodes the scene, and sends the result to the endpoint the pin and the hardware choose. That endpoint is the only thing you hear.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }
        SignalPath {
            Layout.fillWidth: true
            wide: page.twoUp
            showChoose: false
        }

        RailBlock { ordinal: "02"; label: qsTr("WHAT YOU HEAR IT AS"); Layout.fillWidth: true; Layout.fillHeight: false }
        Card {
            GridLayout {
                Layout.fillWidth: true
                columns: page.twoUp ? 2 : 1
                columnSpacing: Theme.space6
                rowSpacing: Theme.space4
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    spacing: Theme.space2
                    Text { Layout.fillWidth: true; text: CrucibleController.modeName + (CrucibleController.modeKey === "atmos" ? qsTr(" · E-AC-3 JOC over HDMI") : ""); color: Theme.text; font.family: Theme.headingFamily; font.pixelSize: Theme.fontTitle; font.weight: Font.Bold; wrapMode: Text.WordWrap }
                    Text { Layout.fillWidth: true; text: CrucibleController.outputReason + qsTr(" The endpoint follows the hardware: pull HDMI and it moves to the next best one below."); color: Theme.textMuted; font.pixelSize: 13; wrapMode: Text.WordWrap }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.maximumWidth: page.twoUp ? 420 : -1
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
                        // Never blank: an unknown or empty pin reads as automatic.
                        function sync() { const i = indexOfValue(CrucibleController.pinned); currentIndex = i < 0 ? 0 : i; }
                        Component.onCompleted: sync()
                        onModelChanged: sync()
                        onActivated: CrucibleController.pinned = currentValue
                        Connections { target: CrucibleController; function onSettingsChanged() { pinBox.sync(); } }
                        font.pixelSize: 13
                        background: Rectangle { color: Theme.neutral100; border.color: Theme.divider; border.width: 1 }
                        contentItem: Text { leftPadding: 10; rightPadding: 26; text: pinBox.displayText; color: Theme.text; font.pixelSize: 13; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
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
            ordinal: "03"
            label: qsTr("ENDPOINTS · WHERE YOU CAN HEAR IT")
            Layout.fillWidth: true
            Text { text: qsTr("re-probed on every device change"); color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: 11 }
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("What the probe found on each render endpoint. \"Hear it here\" chooses one: it gets the best mode it can carry, the pin when it can, and \"Automatic\" hands the choice back (the best endpoint for the best mode, a receiver first). \"Send applications here\" is the other stage, the Windows default: on a real device you would hear every application directly, so the silent device is the one to send them to.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
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
                    Text { Layout.fillWidth: true; Layout.minimumWidth: 140; text: qsTr("ENDPOINT"); color: Theme.textMuted; font.pixelSize: 11; font.letterSpacing: 1 }
                    Text { Layout.preferredWidth: 56; text: qsTr("E-AC-3"); color: Theme.textMuted; font.pixelSize: 11; font.letterSpacing: 1; horizontalAlignment: Text.AlignHCenter }
                    Text { Layout.preferredWidth: 48; text: qsTr("AC-3"); color: Theme.textMuted; font.pixelSize: 11; font.letterSpacing: 1; horizontalAlignment: Text.AlignHCenter }
                    Text { Layout.preferredWidth: 56; text: qsTr("PCM CH"); color: Theme.textMuted; font.pixelSize: 11; font.letterSpacing: 1; horizontalAlignment: Text.AlignHCenter }
                    Text { Layout.preferredWidth: 60; text: qsTr("SPATIAL"); color: Theme.textMuted; font.pixelSize: 11; font.letterSpacing: 1; horizontalAlignment: Text.AlignHCenter }
                    Text { visible: page.wideTable; Layout.preferredWidth: 220; text: qsTr("NOTE"); color: Theme.textMuted; font.pixelSize: 11; font.letterSpacing: 1 }
                    Item { Layout.preferredWidth: 150 + 100 + Theme.space2 }
                }
                Repeater {
                    model: CrucibleController.endpoints
                    delegate: ColumnLayout {
                        id: endpointRow
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 0
                        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.margins: 8
                            Layout.leftMargin: Theme.space3
                            Layout.rightMargin: Theme.space3
                            spacing: Theme.space2
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 140
                                spacing: 1
                                Text { Layout.fillWidth: true; text: endpointRow.modelData.name; color: Theme.text; font.pixelSize: 13; elide: Text.ElideRight }
                                Text { visible: !page.wideTable; Layout.fillWidth: true; text: endpointRow.note; color: endpointRow.modelData.chosen ? Theme.accent : Theme.textMuted; font.pixelSize: Theme.fontSmall; elide: Text.ElideRight }
                            }
                            Text { Layout.preferredWidth: 56; text: endpointRow.modelData.eac3 ? "✓" : "—"; color: endpointRow.modelData.eac3 ? Theme.text : Theme.neutral500; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 13 }
                            Text { Layout.preferredWidth: 48; text: endpointRow.modelData.ac3 ? "✓" : "—"; color: endpointRow.modelData.ac3 ? Theme.text : Theme.neutral500; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 13 }
                            Text { Layout.preferredWidth: 56; text: endpointRow.modelData.pcmChannels > 0 ? endpointRow.modelData.pcmChannels : "—"; color: endpointRow.modelData.pcmChannels > 0 ? Theme.text : Theme.neutral500; horizontalAlignment: Text.AlignHCenter; font.family: Theme.monoFamily; font.pixelSize: 12 }
                            Text { Layout.preferredWidth: 60; text: endpointRow.modelData.spatial ? "✓" : "—"; color: endpointRow.modelData.spatial ? Theme.text : Theme.neutral500; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 13 }
                            Text {
                                visible: page.wideTable
                                Layout.preferredWidth: 220
                                text: endpointRow.note
                                color: endpointRow.modelData.chosen ? Theme.accent : Theme.textMuted
                                font.pixelSize: Theme.fontSmall
                                elide: Text.ElideRight
                            }
                            CrucibleButton {
                                Layout.preferredWidth: 100
                                text: endpointRow.modelData.preferred ? qsTr("Automatic") : qsTr("Hear it here")
                                enabled: !endpointRow.modelData.isNullSink
                                primary: endpointRow.modelData.preferred
                                onClicked: CrucibleController.preferredEndpoint = endpointRow.modelData.preferred ? "" : endpointRow.modelData.id
                            }
                            CrucibleButton {
                                Layout.preferredWidth: 150
                                text: endpointRow.modelData.isDefault ? qsTr("Applications play here") : qsTr("Send applications here")
                                enabled: !endpointRow.modelData.isDefault
                                onClicked: CrucibleController.setDefaultOutput(endpointRow.modelData.id)
                            }
                        }
                        readonly property string note: modelData.chosen ? qsTr("you hear it here") + (modelData.preferred ? qsTr(" · your choice") : qsTr(" · automatic")) + (CrucibleController.modeKey === "atmos" || CrucibleController.modeKey === "ddplus" || CrucibleController.modeKey === "dd" ? qsTr(" · exclusive mode") : "")
                            : modelData.preferred ? qsTr("your choice, but it cannot be used: see the reason above")
                            : modelData.isNullSink ? qsTr("the silent device · ") + (modelData.isDefault ? qsTr("applications play here · ") : "") + qsTr("never heard")
                            : modelData.isDefault ? qsTr("applications play here · a real device, so heard directly")
                            : modelData.spatial ? qsTr("spatial sound on · headphones fallback")
                            : modelData.pcmChannels >= 6 ? qsTr("surround PCM fallback") : qsTr("stereo fallback")
                    }
                }
                Text {
                    visible: CrucibleController.endpoints.length === 0
                    Layout.margins: Theme.space3
                    text: qsTr("no render endpoints probed yet")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: page.twoUp ? 2 : 1
            columnSpacing: Theme.space6
            rowSpacing: Theme.space6
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                spacing: Theme.space3
                RailBlock { ordinal: "04"; label: qsTr("WHERE APPLICATIONS PLAY"); Layout.fillWidth: true; Layout.fillHeight: false }
                Card {
                    ColumnLayout {
                        spacing: Theme.space2
                        Text {
                            Layout.fillWidth: true
                            textFormat: Text.StyledText
                            text: CrucibleController.defaultIsNullSink
                                ? qsTr("Applications play to <b>%1</b>, the Windows default output and the silent device: nothing is heard from it, and this app taps each application there.").arg(CrucibleController.defaultOutputName)
                                : qsTr("Applications play to <b>%1</b>, the Windows default output, which is a real device: you hear each application directly as well as through this app, and a receiver on it cannot be opened exclusively while they do.").arg(CrucibleController.defaultOutputName.length ? CrucibleController.defaultOutputName : qsTr("nothing"))
                            color: Theme.text
                            font.pixelSize: 13
                            wrapMode: Text.WordWrap
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: Theme.space2
                            CrucibleButton {
                                text: CrucibleController.defaultIsNullSink ? qsTr("Restore %1").arg(CrucibleController.previousDefaultName.length ? CrucibleController.previousDefaultName : qsTr("the previous output")) : qsTr("Send applications to %1").arg(CrucibleController.nullSinkName)
                                enabled: CrucibleController.defaultIsNullSink ? CrucibleController.previousDefaultName.length > 0 : CrucibleController.nullSinkPresent
                                onClicked: CrucibleController.defaultIsNullSink ? CrucibleController.restoreDefault() : CrucibleController.moveDefaultToNullSink()
                            }
                            CrucibleButton { text: qsTr("Open Sound settings"); onClicked: CrucibleController.openSoundSettings() }
                            CrucibleButton { text: qsTr("Re-probe"); onClicked: CrucibleController.reprobe() }
                        }
                        Text { Layout.fillWidth: true; visible: CrucibleController.defaultMessage.length > 0; text: CrucibleController.defaultMessage; color: Theme.accent; font.pixelSize: Theme.fontSmall; wrapMode: Text.WordWrap }
                    }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                spacing: Theme.space3
                RailBlock { ordinal: "05"; label: qsTr("CODEC PATH"); Layout.fillWidth: true; Layout.fillHeight: false }
                Card {
                    CrucibleCheck {
                        objectName: "bypassCheck"
                        Layout.fillWidth: true
                        text: qsTr("Bypass the codec on headphones and PCM")
                        note: qsTr("Off: headphones, PCM and stereo play a decode of the E-AC-3 stream, so what you hear went through the codec. On: headphones render the engine's own objects and PCM and stereo take its 5.1 bed, codec out of the loop.")
                        checked: CrucibleController.bypassCodec
                        onToggled: function(on) { CrucibleController.bypassCodec = on; }
                    }
                }
            }
        }
    }
}

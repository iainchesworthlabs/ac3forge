import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3Forge

// The AC-4 tab: the options of `ac3cli ac4-encode` the page can express, each
// read from and written to EncoderController's AC-4 block. What it leaves to
// the command line (substreams, presentations, dialogue stems and the rest)
// is named at the foot of the tab and in ac4_encode_settings.hpp.
ColumnLayout {
    id: panel
    Layout.fillWidth: true
    spacing: Theme.gap

    readonly property bool idle: !EncoderController.busy

    Text {
        Layout.fillWidth: true
        Layout.leftMargin: 24
        Layout.rightMargin: 24
        Layout.topMargin: Theme.space4
        text: qsTr("AC-4 encodes the source in its own layout (mono, stereo, 5.0 or 5.1) to a raw stream or an MP4 file, as “ac3cli ac4-encode” does.")
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.WordWrap
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignTop
        Layout.leftMargin: 24
        Layout.rightMargin: 24
        spacing: 40

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: Theme.gap

            Card {
                title: qsTr("Frames and rate")

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: Theme.gap
                    rowSpacing: Theme.gap

                    Text {
                        id: frameRateLabel
                        text: qsTr("Frame rate")
                        color: Theme.text
                        font.pixelSize: Theme.fontNormal
                    }
                    ComboBox {
                        objectName: "ac4FrameRate"
                        Accessible.name: frameRateLabel.text
                        Layout.fillWidth: true
                        enabled: panel.idle
                        model: EncoderController.ac4FrameRateNames
                        currentIndex: EncoderController.ac4FrameRateIndex
                        onActivated: EncoderController.ac4FrameRateIndex = currentIndex
                    }

                    Text {
                        id: rateModeLabel
                        text: qsTr("Rate mode")
                        color: Theme.text
                        font.pixelSize: Theme.fontNormal
                    }
                    ComboBox {
                        objectName: "ac4RateMode"
                        Accessible.name: rateModeLabel.text
                        Layout.fillWidth: true
                        enabled: panel.idle
                        model: EncoderController.ac4RateModeNames
                        currentIndex: EncoderController.ac4RateModeIndex
                        onActivated: EncoderController.ac4RateModeIndex = currentIndex
                    }

                    Text {
                        id: codecModeLabel
                        text: qsTr("Codec mode")
                        color: Theme.text
                        font.pixelSize: Theme.fontNormal
                    }
                    ComboBox {
                        objectName: "ac4CodecMode"
                        Accessible.name: codecModeLabel.text
                        Layout.fillWidth: true
                        enabled: panel.idle
                        model: EncoderController.ac4CodecModeNames
                        currentIndex: EncoderController.ac4CodecModeIndex
                        onActivated: EncoderController.ac4CodecModeIndex = currentIndex
                    }

                    Text {
                        id: iframeLabel
                        text: qsTr("I-frame interval")
                        color: Theme.text
                        font.pixelSize: Theme.fontNormal
                    }
                    SpinBox {
                        objectName: "ac4IframeInterval"
                        Accessible.name: iframeLabel.text
                        from: 1
                        to: 1000
                        editable: true
                        enabled: panel.idle
                        value: EncoderController.ac4IframeInterval
                        onValueModified: EncoderController.ac4IframeInterval = value
                    }

                    CheckBox {
                        objectName: "ac4Crc"
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        text: qsTr("CRC on each raw sync frame")
                        // An MP4 sample is the frame alone, with no sync word
                        // and no CRC (TS 103 190-2 Annex E).
                        enabled: panel.idle && EncoderController.containerIndex !== 3
                        checked: EncoderController.ac4Crc
                        onToggled: EncoderController.ac4Crc = checked
                    }
                }
            }

            Card {
                title: qsTr("Loudness and DRC")

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: Theme.gap
                    rowSpacing: Theme.gap

                    Text {
                        id: dialnormLabel
                        text: qsTr("dialnorm (dB below full scale)")
                        color: Theme.text
                        font.pixelSize: Theme.fontNormal
                    }
                    // Quarter-dB steps, 0 to 31.75 (TS 103 190-1 4.3.12.2.1):
                    // the SpinBox counts quarters and shows decibels.
                    SpinBox {
                        objectName: "ac4Dialnorm"
                        Accessible.name: dialnormLabel.text
                        from: 0
                        to: 127
                        editable: true
                        enabled: panel.idle && !EncoderController.ac4MeasureDialnorm
                        value: Math.round(EncoderController.ac4Dialnorm * 4)
                        textFromValue: function(value) { return String(value / 4); }
                        valueFromText: function(text) { return Math.round(Number(text) * 4); }
                        onValueModified: EncoderController.ac4Dialnorm = value / 4
                    }

                    CheckBox {
                        objectName: "ac4MeasureDialnorm"
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        text: qsTr("Measure dialnorm from the programme")
                        enabled: panel.idle
                        checked: EncoderController.ac4MeasureDialnorm
                        onToggled: EncoderController.ac4MeasureDialnorm = checked
                    }

                    Text {
                        id: loudnessLabel
                        text: qsTr("Loudness values")
                        color: Theme.text
                        font.pixelSize: Theme.fontNormal
                    }
                    ComboBox {
                        objectName: "ac4Loudness"
                        Accessible.name: loudnessLabel.text
                        Layout.fillWidth: true
                        enabled: panel.idle
                        model: EncoderController.ac4LoudnessNames
                        currentIndex: EncoderController.ac4LoudnessIndex
                        onActivated: EncoderController.ac4LoudnessIndex = currentIndex
                    }

                    Text {
                        id: drcLabel
                        text: qsTr("DRC profile")
                        color: Theme.text
                        font.pixelSize: Theme.fontNormal
                    }
                    ComboBox {
                        objectName: "ac4Drc"
                        Accessible.name: drcLabel.text
                        Layout.fillWidth: true
                        enabled: panel.idle
                        model: EncoderController.ac4DrcNames
                        currentIndex: EncoderController.ac4DrcIndex
                        onActivated: EncoderController.ac4DrcIndex = currentIndex
                    }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: Theme.gap

            Card {
                title: qsTr("Stereo downmix")

                // A 5.0 or 5.1 source alone has a stereo downmix to describe.
                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: Theme.gap
                    rowSpacing: Theme.gap
                    enabled: EncoderController.ac4DownmixAvailable

                    Text {
                        id: centreLabel
                        text: qsTr("Centre (Lo/Ro)")
                        color: Theme.text
                        font.pixelSize: Theme.fontNormal
                    }
                    ComboBox {
                        objectName: "ac4Centre"
                        Accessible.name: centreLabel.text
                        Layout.fillWidth: true
                        enabled: panel.idle
                        model: EncoderController.ac4CentreNames
                        currentIndex: EncoderController.ac4CentreIndex
                        onActivated: EncoderController.ac4CentreIndex = currentIndex
                    }

                    Text {
                        id: surroundLabel
                        text: qsTr("Surround (Lo/Ro)")
                        color: Theme.text
                        font.pixelSize: Theme.fontNormal
                    }
                    ComboBox {
                        objectName: "ac4Surround"
                        Accessible.name: surroundLabel.text
                        Layout.fillWidth: true
                        enabled: panel.idle
                        model: EncoderController.ac4SurroundNames
                        currentIndex: EncoderController.ac4SurroundIndex
                        onActivated: EncoderController.ac4SurroundIndex = currentIndex
                    }

                    Text {
                        id: preferredLabel
                        text: qsTr("Preferred downmix")
                        color: Theme.text
                        font.pixelSize: Theme.fontNormal
                    }
                    ComboBox {
                        objectName: "ac4PreferredDownmix"
                        Accessible.name: preferredLabel.text
                        Layout.fillWidth: true
                        enabled: panel.idle
                        model: EncoderController.ac4PreferredDownmixNames
                        currentIndex: EncoderController.ac4PreferredDownmixIndex
                        onActivated: EncoderController.ac4PreferredDownmixIndex = currentIndex
                    }
                }
            }

            Card {
                title: qsTr("Dialogue enhancement")

                // The channels that carry dialogue alone, which a decoder may
                // then raise.
                RowLayout {
                    spacing: Theme.gap

                    CheckBox {
                        objectName: "ac4DialogueLeft"
                        text: qsTr("Dialogue in %1").arg("L")
                        enabled: panel.idle
                        checked: EncoderController.ac4DialogueLeft
                        onToggled: EncoderController.ac4DialogueLeft = checked
                    }
                    CheckBox {
                        objectName: "ac4DialogueRight"
                        text: qsTr("Dialogue in %1").arg("R")
                        enabled: panel.idle
                        checked: EncoderController.ac4DialogueRight
                        onToggled: EncoderController.ac4DialogueRight = checked
                    }
                    CheckBox {
                        objectName: "ac4DialogueCentre"
                        text: qsTr("Dialogue in %1").arg("C")
                        enabled: panel.idle
                        checked: EncoderController.ac4DialogueCentre
                        onToggled: EncoderController.ac4DialogueCentre = checked
                    }
                }

                CheckBox {
                    objectName: "ac4DialogueMid"
                    Layout.fillWidth: true
                    text: qsTr("Raise the Mid of L and R")
                    enabled: panel.idle
                    checked: EncoderController.ac4DialogueMid
                    onToggled: EncoderController.ac4DialogueMid = checked
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: Theme.gap

                    Text {
                        id: maxGainLabel
                        text: qsTr("Largest dialogue boost")
                        color: Theme.text
                        font.pixelSize: Theme.fontNormal
                    }
                    ComboBox {
                        objectName: "ac4DialogueMaxGain"
                        Accessible.name: maxGainLabel.text
                        Layout.fillWidth: true
                        enabled: panel.idle
                        model: EncoderController.ac4DialogueMaxGainNames
                        currentIndex: EncoderController.ac4DialogueMaxGainIndex
                        onActivated: EncoderController.ac4DialogueMaxGainIndex = currentIndex
                    }
                }
            }
        }
    }

    Text {
        Layout.fillWidth: true
        Layout.leftMargin: 24
        Layout.rightMargin: 24
        text: qsTr("Left to “ac3cli ac4-encode”: several substreams and presentations, dialogue stems, a DRC profile per decoder mode, the LFE mix and the other layouts.")
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.WordWrap
    }

    Item { Layout.fillHeight: true }
}

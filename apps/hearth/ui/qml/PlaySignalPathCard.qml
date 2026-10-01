import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

import Hearth

// "07 Signal path" (main-play.png / play-minimum-size.png): decode -> render
// -> output. Pulled out of PlayPage.qml for the same reason and in the same
// way as PlayObjectsCard.qml - see that file's own header comment.
Card {
    id: root
    ordinal: "07"
    title: qsTr("Signal path")
    flat: true

    // A sink paired on the Network page but never chosen as (part of) the
    // output: Play still works, it just falls back to this computer's own
    // device with nothing else said - the exact gap behind the report that
    // Hearth "wouldn't play a song" after connecting to a network sink
    // (hearth-followups-group-ux-and-live-diagnostics-2026-09-26). True
    // whether no group exists yet or one does but was never picked -
    // outputGroupName is empty in both, and the hint's own wording covers
    // both remedies rather than branching on which applies.
    readonly property bool hasUnusedPairedSink: NetworkController.sinks.some((s) => s.badge === "paired")
                                                 && HearthController.outputGroupName.length === 0

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: decodeCol.implicitHeight + Theme.pad * 2
        color: Theme.neutral100
        border.color: Theme.border
        border.width: 1

        ColumnLayout {
            id: decodeCol
            anchors.fill: parent
            anchors.margins: Theme.pad
            spacing: 2

            Text {
                text: qsTr("1 · DECODE")
                color: Theme.textMuted; font.pixelSize: Theme.fontMicro; font.bold: true
            }
            Text {
                Layout.fillWidth: true
                text: {
                    if (HearthController.currentIndex < 0) return qsTr("Nothing playing");
                    var kind = HearthController.queue[HearthController.currentIndex].streamKind;
                    var modeName = HearthController.decoderSettings.mode;
                    var mode = modeName === "rf" ? qsTr("RF mode")
                             : modeName === "custom" ? qsTr("custom mode") : qsTr("line mode");
                    return (kind.length > 0 ? kind : qsTr("stream")) + " · " + mode;
                }
                color: Theme.text
                font.bold: true
                font.pixelSize: Theme.fontBody
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                visible: HearthController.thisFrame.dialnorm !== undefined
                text: qsTr("dialnorm −%1: %2 dB down%3")
                          .arg(HearthController.thisFrame.dialnorm)
                          .arg((31 - HearthController.thisFrame.dialnorm).toFixed(1))
                          .arg(HearthController.hasObjectMetadata
                               ? qsTr(" · %1 objects").arg(HearthController.objectsPlaced) : "")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
        }
    }

    Text {
        Layout.alignment: Qt.AlignHCenter
        text: "↓"
        color: Theme.textMuted
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: renderCol.implicitHeight + Theme.pad * 2
        color: Theme.neutral100
        border.color: Theme.border
        border.width: 1

        ColumnLayout {
            id: renderCol
            anchors.fill: parent
            anchors.margins: Theme.pad
            spacing: 2

            Text {
                text: qsTr("2 · RENDER")
                color: Theme.textMuted; font.pixelSize: Theme.fontMicro; font.bold: true
            }
            Text {
                Layout.fillWidth: true
                text: HearthController.layoutText.length > 0
                      ? qsTr("%1 onto %2 outputs").arg(HearthController.layoutText)
                                                   .arg(HearthController.routingOutputs)
                      : qsTr("Not rendering")
                color: Theme.text
                font.bold: true
                font.pixelSize: Theme.fontBody
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                text: {
                    var small = [];
                    for (var i = 0; i < HearthController.speakerSmall.length; i++) {
                        if (HearthController.speakerSmall[i]) {
                            small.push(HearthController.speakerLabels[i]);
                        }
                    }
                    var lede = small.length > 0
                        ? qsTr("%1 small at %2 Hz").arg(small.join(qsTr(" and ")))
                                                    .arg(HearthController.crossoverHz)
                        : qsTr("no small speakers");
                    return lede + qsTr(" · trims and delays");
                }
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
        }
    }

    Text {
        Layout.alignment: Qt.AlignHCenter
        text: "↓"
        color: Theme.textMuted
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: hearCol.implicitHeight + Theme.pad * 2
        color: Theme.neutral100
        border.color: Theme.border
        border.width: 1

        ColumnLayout {
            id: hearCol
            anchors.fill: parent
            anchors.margins: Theme.pad
            spacing: 2

            Text {
                text: qsTr("3 · YOU HEAR IT ON")
                color: Theme.textMuted; font.pixelSize: Theme.fontMicro; font.bold: true
            }
            Text {
                Layout.fillWidth: true
                text: HearthController.deviceName.length > 0
                      ? HearthController.deviceName : qsTr("No output chosen")
                color: Theme.text
                font.bold: true
                font.pixelSize: Theme.fontBody
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                visible: HearthController.outputFormat.channels > 0
                text: qsTr("%1 ch %2 at %3 kHz")
                          .arg(HearthController.outputFormat.channels)
                          .arg(HearthController.outputFormat.mode === "pcm"
                               ? qsTr("PCM") : qsTr("bitstream"))
                          .arg(Number(HearthController.outputFormat.sampleRate / 1000).toFixed(1))
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.gap / 2
                Text {
                    Layout.fillWidth: true
                    text: qsTr("output: your choice")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                }
                AppButton {
                    objectName: "playChooseOutput"
                    text: qsTr("Choose…")
                    // The enclosing ApplicationWindow, reached through the
                    // Window attached property - root.parent does not
                    // reliably chain up to it (GuidedWizard.qml's own
                    // requestWindow() comment explains why) - opened the
                    // same way the header's own output pill does
                    // (Main.qml's openOutputPicker()).
                    onClicked: root.Window.window.openOutputPicker()
                }
            }

            Text {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                objectName: "playUnusedPairedSinkHint"
                visible: root.hasUnusedPairedSink
                text: qsTr("A paired network sink isn't playing: no group is chosen as the output. "
                          + "Choose one above, or make one on the Network page.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
        }
    }
}

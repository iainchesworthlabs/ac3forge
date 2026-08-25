import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import Ac3Forge

// "Open stream" — the GUI twin of `ac3cli monitor` (play an already-encoded
// file's decoded bed through an ordinary output) fused with `ac3cli decode`'s
// export (a WAV of the bed, and for an Atmos stream one WAV per
// JOC-reconstructed object). Open → decode → real transport, the same
// "distinct surface, reachable from the header" shape QcDialog.qml/
// ObjectInspectorDialog.qml already use, for the identical reason: this
// reads a stream that already exists, with no plan, no source and no
// encoder anywhere in the path.
//
// StreamPlayerController has no scrub-through-metadata concept the way
// ObjectDecodeController's frameIndex does - positionSeconds() is real
// playback position, driven by an actual MonitorSink worker rather than a
// QML Timer walking a model index. See its own header comment.
Dialog {
    id: root
    objectName: "streamPlayerDialog"

    modal: true
    anchors.centerIn: parent
    width: Math.min(820, parent ? parent.width - 60 : 820)
    height: Math.min(720, parent ? parent.height - 60 : 720)
    padding: Theme.space6
    title: ""

    background: Rectangle {
        color: Theme.bg
        border.color: Theme.text
        border.width: 2
    }

    onVisibleChanged: if (!visible) { StreamPlayerController.pause(); }

    FileDialog {
        id: spFileDialog
        title: qsTr("Choose an AC-3 / E-AC-3 stream")
        nameFilters: [qsTr("AC-3 / E-AC-3 (*.ac3 *.ec3)"), qsTr("All files (*)")]
        onAccepted: StreamPlayerController.openFile(selectedFile)
    }

    FileDialog {
        id: spExportWavDialog
        title: qsTr("Export decoded WAV")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("WAV audio (*.wav)"), qsTr("All files (*)")]
        defaultSuffix: "wav"
        onAccepted: StreamPlayerController.exportDecodedWav(selectedFile)
    }

    // A folder picker, not a file one: exportObjects() writes one
    // object_NN.wav per JOC-reconstructed object into the folder chosen,
    // the same objects_dir shape `ac3cli decode` takes - see
    // Main.qml's saveFolderDialog for the identical "no filename field"
    // reasoning.
    FolderDialog {
        id: spExportObjectsDialog
        title: qsTr("Choose a folder for the exported objects")
        onAccepted: StreamPlayerController.exportObjects(selectedFolder)
    }

    contentItem: ColumnLayout {
        spacing: Theme.space4

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: qsTr("Open stream")
                font.pixelSize: 18
                font.weight: Font.ExtraBold
                font.family: Theme.headingFamily
                color: Theme.text
            }
            Button {
                objectName: "spCloseButton"
                text: qsTr("Close")
                onClicked: root.close()
            }
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Decodes an already-encoded AC-3/E-AC-3 file and plays its decoded bed through an ordinary output — like every other decode in this window, an Atmos stream plays its 5.1 bed here, not unmixed objects (see Inspect objects for those). Export writes the decode to a WAV, and for an Atmos stream one WAV per object.")
            font.pixelSize: 12
            color: Theme.neutral700
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space3

            Button {
                objectName: "spChooseFileButton"
                text: qsTr("Choose file…")
                enabled: !StreamPlayerController.busy
                onClicked: spFileDialog.open()
            }
            Text {
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                text: StreamPlayerController.filePath.length > 0
                      ? StreamPlayerController.filePath : qsTr("No file chosen yet")
                color: Theme.neutral700
                font.pixelSize: 12
                font.family: Theme.monoFamily
            }
            BusyIndicator {
                objectName: "spBusyIndicator"
                visible: StreamPlayerController.busy
                running: StreamPlayerController.busy
                implicitWidth: 24
                implicitHeight: 24
            }
        }

        Text {
            objectName: "spErrorText"
            Layout.fillWidth: true
            visible: StreamPlayerController.error.length > 0
            wrapMode: Text.WordWrap
            text: StreamPlayerController.error
            color: Theme.bad
            font.pixelSize: 12
        }

        Text {
            visible: !StreamPlayerController.hasResult && StreamPlayerController.error.length === 0
                     && !StreamPlayerController.busy
            Layout.fillWidth: true
            Layout.fillHeight: true
            text: qsTr("Choose an AC-3/E-AC-3 file above to play it.")
            color: Theme.textMuted
            font.pixelSize: 12
            verticalAlignment: Text.AlignTop
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: StreamPlayerController.hasResult
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: parent ? parent.width : 0
                spacing: Theme.space4

                Text {
                    objectName: "spSummaryText"
                    Layout.fillWidth: true
                    text: StreamPlayerController.summaryLine
                    font.pixelSize: 12
                    font.family: Theme.monoFamily
                    font.weight: Font.DemiBold
                    color: Theme.text
                }

                // ---- transport -------------------------------------------
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space3

                    Button {
                        objectName: "spPlayButton"
                        text: StreamPlayerController.playing ? qsTr("Pause") : qsTr("Play")
                        enabled: StreamPlayerController.hasResult
                        onClicked: StreamPlayerController.playing
                                   ? StreamPlayerController.pause()
                                   : StreamPlayerController.play()
                    }
                    Slider {
                        id: scrub
                        objectName: "spScrubSlider"
                        Layout.fillWidth: true
                        from: 0
                        to: Math.max(0.001, StreamPlayerController.durationSeconds)
                        // Same shape ObjectInspectorDialog.qml's own scrub
                        // slider uses: `value` is a plain binding re-synced
                        // explicitly from the Connections below rather than
                        // trusted to survive a drag (Slider's own drag
                        // handling writes `value` directly, which breaks a
                        // declarative binding on it for good - see that
                        // file's identical comment). Pausing the moment a
                        // drag starts is what stops that resync from
                        // fighting the user: positionSeconds stops moving on
                        // its own the instant playback does.
                        value: StreamPlayerController.positionSeconds
                        onMoved: {
                            StreamPlayerController.pause();
                            StreamPlayerController.seek(value);
                        }
                    }
                    Connections {
                        target: StreamPlayerController
                        function onPositionChanged() {
                            scrub.value = StreamPlayerController.positionSeconds;
                        }
                    }
                    Text {
                        objectName: "spPositionLabel"
                        Layout.preferredWidth: 110
                        horizontalAlignment: Text.AlignRight
                        text: qsTr("%1 / %2 s")
                                  .arg(StreamPlayerController.positionSeconds.toFixed(1))
                                  .arg(StreamPlayerController.durationSeconds.toFixed(1))
                        font.pixelSize: 10
                        font.family: Theme.monoFamily
                        color: Theme.textMuted
                    }
                }

                // ---- export -------------------------------------------------
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space3

                    Button {
                        objectName: "spExportWavButton"
                        text: qsTr("Export decoded WAV…")
                        enabled: StreamPlayerController.hasResult && !StreamPlayerController.exporting
                        onClicked: spExportWavDialog.open()
                    }
                    Button {
                        objectName: "spExportObjectsButton"
                        text: qsTr("Export objects…")
                        visible: StreamPlayerController.hasObjects
                        enabled: !StreamPlayerController.exporting
                        onClicked: spExportObjectsDialog.open()
                    }
                    BusyIndicator {
                        objectName: "spExportBusyIndicator"
                        visible: StreamPlayerController.exporting
                        running: StreamPlayerController.exporting
                        implicitWidth: 20
                        implicitHeight: 20
                    }
                    Item { Layout.fillWidth: true }
                }
                Text {
                    objectName: "spExportError"
                    Layout.fillWidth: true
                    visible: StreamPlayerController.exportError.length > 0
                    wrapMode: Text.WordWrap
                    text: StreamPlayerController.exportError
                    color: Theme.bad
                    font.pixelSize: 12
                }

                // ---- levels ---------------------------------------------------
                Text {
                    text: qsTr("LEVELS")
                    color: Theme.neutral600
                    font.pixelSize: 10
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Repeater {
                        objectName: "spMeterRows"
                        model: StreamPlayerController.channelMeta

                        delegate: ChannelMeter {
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            controller: StreamPlayerController
                            channelName: modelData.name
                            channelIndex: index
                            fed: modelData.fed !== false
                            level: index < StreamPlayerController.channelLevels.length
                                   ? StreamPlayerController.channelLevels[index] : ({})
                        }
                    }
                }

                // ---- soundfield -------------------------------------------
                Text {
                    Layout.topMargin: Theme.space2
                    text: qsTr("SOUNDFIELD")
                    color: Theme.neutral600
                    font.pixelSize: 10
                }
                SoundfieldView {
                    objectName: "spSoundfield"
                    Layout.fillWidth: true
                    controller: StreamPlayerController
                    atmosCaption: qsTr("Solid dots are bed positions this stream carries. Objects are not here — this plays the 5.1 bed only; open Inspect objects for per-object playback and position.")
                }
            }
        }
    }
}

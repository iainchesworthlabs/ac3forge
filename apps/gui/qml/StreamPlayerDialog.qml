import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import ForgeGui

// "Open stream" — the GUI twin of `forge monitor` (play an already-encoded
// file's decoded bed through an ordinary output) fused with `forge decode`'s
// export (a WAV of the bed, and for an Atmos or an AC-4 object stream one WAV
// per decoded object - planning/ac4.md, I5). Open → decode → real transport, the same
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
        objectName: "spFileDialog"
        title: qsTr("Choose an AC-3 / E-AC-3 stream")
        nameFilters: [qsTr("AC-3 / E-AC-3 (*.ac3 *.ec3)"), qsTr("AC-4 (*.ac4)"),
                      qsTr("All files (*)")]
        onAccepted: StreamPlayerController.openFile(selectedFile)
    }

    FileDialog {
        id: spExportWavDialog
        objectName: "spExportWavDialog"
        title: qsTr("Export decoded WAV")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("WAV audio (*.wav)"), qsTr("All files (*)")]
        defaultSuffix: "wav"
        onAccepted: StreamPlayerController.exportDecodedWav(selectedFile)
    }

    // A folder picker, not a file one: exportObjects() writes one
    // object_NN.wav per decoded object (JOC-reconstructed for E-AC-3, D10's
    // own for AC-4) into the folder chosen, the same objects_dir shape
    // `forge decode` takes - see
    // Main.qml's saveFolderDialog for the identical "no filename field"
    // reasoning.
    FolderDialog {
        id: spExportObjectsDialog
        objectName: "spExportObjectsDialog"
        title: qsTr("Choose a folder for the exported objects")
        onAccepted: StreamPlayerController.exportObjects(selectedFolder)
    }

    contentItem: ColumnLayout {
        spacing: Theme.space4

        // A Popup/Dialog is not itself an Item ("Accessible must be
        // attached to an Item or an Action" at runtime otherwise) - its
        // contentItem is. title is "" (a styled Text below draws the
        // visible "Open stream" heading instead), so Dialog's own
        // title-derived accessible name has nothing to read without this.
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Open stream")

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: qsTr("Open stream")
                font.pixelSize: Theme.fontArrow
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
            font.pixelSize: Theme.fontSmall
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
                font.pixelSize: Theme.fontSmall
                font.family: Theme.monoFamily
            }
            BusyIndicator {
                objectName: "spBusyIndicator"
                visible: StreamPlayerController.busy
                running: StreamPlayerController.busy
                implicitWidth: 24
                implicitHeight: 24
                Accessible.role: Accessible.Indicator
                Accessible.name: qsTr("Decoding…")
            }
        }

        // AC-4: which presentation plays, as `forge play presentation=<n>`
        // chooses it; the first entry is the one the decoder takes with no
        // preference.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space3
            visible: StreamPlayerController.isAc4
            Text {
                id: spPresentationLabel
                text: qsTr("PRESENTATION")
                font.pixelSize: Theme.fontMicro
                font.letterSpacing: 1.2
                color: Theme.textMuted
            }
            ComboBox {
                objectName: "spPresentation"
                Accessible.name: spPresentationLabel.text
                Layout.fillWidth: true
                enabled: !StreamPlayerController.busy
                model: [qsTr("The decoder's choice")].concat(StreamPlayerController.presentationNames)
                currentIndex: StreamPlayerController.presentationIndex + 1
                // Each decode hands the model over anew, which resets the
                // shown entry; a pick has already replaced the binding.
                onModelChanged: currentIndex = StreamPlayerController.presentationIndex + 1
                onActivated: StreamPlayerController.presentationIndex = currentIndex - 1
            }
        }

        Text {
            objectName: "spErrorText"
            Layout.fillWidth: true
            visible: StreamPlayerController.error.length > 0
            wrapMode: Text.WordWrap
            text: StreamPlayerController.error
            color: Theme.bad
            font.pixelSize: Theme.fontSmall
        }

        Text {
            visible: !StreamPlayerController.hasResult && StreamPlayerController.error.length === 0
                     && !StreamPlayerController.busy
            Layout.fillWidth: true
            Layout.fillHeight: true
            text: qsTr("Choose an AC-3/E-AC-3 file above to play it.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
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
                    font.pixelSize: Theme.fontSmall
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
                        Accessible.name: qsTr("Position")
                        Accessible.description: qsTr("%1 / %2 s")
                            .arg(StreamPlayerController.positionSeconds.toFixed(1))
                            .arg(StreamPlayerController.durationSeconds.toFixed(1))
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
                        font.pixelSize: Theme.fontMicro
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
                        Accessible.role: Accessible.Indicator
                        Accessible.name: qsTr("Exporting…")
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
                    font.pixelSize: Theme.fontSmall
                }

                // ---- levels ---------------------------------------------------
                Text {
                    text: qsTr("LEVELS")
                    color: Theme.neutral600
                    font.pixelSize: Theme.fontMicro
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
                    font.pixelSize: Theme.fontMicro
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

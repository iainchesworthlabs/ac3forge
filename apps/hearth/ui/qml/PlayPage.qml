import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import Ac3ForgeHearth

// The Play page (planning/hearth-design.md, "The main window, playing"): the
// queue and now playing, built over the real engine, plus the monitor beside
// it - levels, loudness, this frame's detail, object placement and signal
// path - all read from HearthController.levels/loudness/thisFrame/objects/
// outputFormat, which poll Engine::meters() and Engine::unit_report() the
// same tick as status() (hearth_controller.cpp's own poll()).
Item {
    id: root

    // The meter's own axis (-60,-48,-36,-24,-12,-6,0 dB): six EQUAL-WIDTH
    // segments over unequal dB spans, giving more resolution near 0 dB -
    // where clipping risk actually lives - the same idea a broadcast PPM's
    // non-linear scale uses. A bar's width and a tick's x both come from
    // this one mapping, so they stay comparable to each other and to the
    // axis printed under them.
    function levelFraction(db) {
        var breaks = [-60, -48, -36, -24, -12, -6, 0];
        var clamped = Math.max(breaks[0], Math.min(breaks[6], db));
        for (var i = 0; i < 6; i++) {
            if (clamped <= breaks[i + 1] || i === 5) {
                var span = breaks[i + 1] - breaks[i];
                var t = span > 0 ? (clamped - breaks[i]) / span : 0;
                return (i + t) / 6;
            }
        }
        return 1.0;
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: Theme.pad
        spacing: Theme.gap

        // --- queue -----------------------------------------------------
        ColumnLayout {
            // A ColumnLayout child's Layout.fillWidth defaults to true
            // (unlike a plain Item's), which without this override claimed
            // leftover width alongside the Card below and left it a sliver -
            // caught by actually running the built window, not by any
            // static check.
            Layout.fillWidth: false
            Layout.preferredWidth: 340
            Layout.fillHeight: true
            spacing: Theme.gap

            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Queue · %1 items").arg(HearthController.queue.length)
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    font.bold: true
                    font.capitalization: Font.AllUppercase
                }
            }

            Button {
                objectName: "addFiles"
                text: qsTr("Add files…")
                Layout.fillWidth: true
                onClicked: addFilesDialog.open()
            }

            ListView {
                id: queueList
                objectName: "queueList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: HearthController.queue
                spacing: Theme.gap / 2

                // A plain Rectangle + Text + MouseArea, not a native Button:
                // this Repeater-backed delegate carries real, non-empty
                // queue data, and a native QQC2 Button in that position has
                // hung the offscreen Qt Quick Test binary on Windows
                // elsewhere in this family (qml-native-button-repeater-
                // offscreen-hang). SegmentedControl's own delegate uses the
                // same shape for the same reason.
                delegate: Rectangle {
                    id: row
                    required property var modelData
                    required property int index
                    width: queueList.width
                    height: label.implicitHeight + note.implicitHeight + Theme.pad
                    color: modelData.current ? Theme.accent100 : Theme.surface
                    border.color: modelData.current ? Theme.accent : Theme.border
                    border.width: modelData.current ? 2 : 1
                    radius: Theme.radius

                    Accessible.role: Accessible.ListItem
                    Accessible.name: modelData.title
                    Accessible.selected: modelData.current

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.pad / 2
                        spacing: 2

                        Text {
                            id: label
                            Layout.fillWidth: true
                            text: (modelData.current && HearthController.playing ? "▶ " : "") + modelData.title
                            color: modelData.playable ? Theme.text : Theme.textMuted
                            font.pixelSize: Theme.fontBody
                            elide: Text.ElideRight
                        }
                        Text {
                            id: note
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: modelData.playable ? modelData.streamKind : modelData.note
                            color: modelData.playable ? Theme.textMuted : Theme.bad
                            font.pixelSize: Theme.fontSmall
                            elide: Text.ElideRight
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        enabled: modelData.playable
                        onClicked: HearthController.playItem(row.index)
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible: queueList.count === 0
                    text: qsTr("Nothing in the queue.\nAdd files or drop them here.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontBody
                    horizontalAlignment: Text.AlignHCenter
                }
            }
        }

        // --- now playing, levels, loudness, this frame -------------------
        ScrollView {
            id: monitorScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: monitorScroll.availableWidth
                spacing: Theme.gap * 2

                Card {
                    title: qsTr("02 Now playing")

                    Text {
                        Layout.fillWidth: true
                        text: HearthController.currentIndex >= 0
                              ? HearthController.queue[HearthController.currentIndex].title
                              : qsTr("Nothing playing")
                        color: Theme.text
                        font.pixelSize: Theme.fontHeading
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: HearthController.currentIndex >= 0
                        text: HearthController.currentIndex >= 0
                              ? HearthController.queue[HearthController.currentIndex].streamKind : ""
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }
                }

                Card {
                    title: qsTr("03 Levels · at play time")

                    Text {
                        Layout.fillWidth: true
                        text: HearthController.deviceName.length > 0
                              ? qsTr("%1 · %2 outputs").arg(HearthController.deviceName)
                                                        .arg(HearthController.routingOutputs)
                              : qsTr("No output device chosen yet")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: HearthController.levels.length === 0
                        text: qsTr("Nothing playing.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: HearthController.levels.length > 0
                        spacing: Theme.gap / 2

                        Repeater {
                            model: HearthController.levels

                            // The bar fills to rms_db (what was actually
                            // sustained), while the two ticks mark peak_db
                            // and hold_db past it - a common meter idiom
                            // (a solid RMS body with a peak reference beyond
                            // it) that puts all three of ChannelLevel's
                            // numbers on screen without three separate bars.
                            // The dB readout is peak_db: the more actionable
                            // "how close to clipping right now" figure.
                            delegate: RowLayout {
                                id: levelRow
                                required property var modelData
                                required property int index
                                Layout.fillWidth: true
                                spacing: Theme.gap / 2
                                readonly property int routedTo: HearthController.routing[levelRow.index] ?? -1

                                Text {
                                    Layout.preferredWidth: 28
                                    text: HearthController.speakerLabels[levelRow.index] ?? ""
                                    color: Theme.text
                                    font.bold: true
                                    font.pixelSize: Theme.fontSmall
                                }

                                Item {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 16

                                    Rectangle { anchors.fill: parent; color: Theme.neutral200 }
                                    Rectangle {
                                        anchors.left: parent.left
                                        anchors.top: parent.top
                                        anchors.bottom: parent.bottom
                                        width: parent.width * root.levelFraction(levelRow.modelData.rmsDb)
                                        color: levelRow.modelData.clipped ? Theme.bad : Theme.neutral800
                                    }
                                    Rectangle {
                                        x: Math.max(0, Math.min(parent.width - 2,
                                               parent.width * root.levelFraction(levelRow.modelData.peakDb)))
                                        width: 2
                                        height: parent.height
                                        color: Theme.textMuted
                                    }
                                    Rectangle {
                                        x: Math.max(0, Math.min(parent.width - 2,
                                               parent.width * root.levelFraction(levelRow.modelData.holdDb)))
                                        width: 2
                                        height: parent.height
                                        color: Theme.bad
                                    }
                                }

                                Text {
                                    Layout.preferredWidth: 44
                                    horizontalAlignment: Text.AlignRight
                                    text: Number(levelRow.modelData.peakDb).toFixed(1)
                                    color: Theme.text
                                    font.family: Theme.monoFamily
                                    font.pixelSize: Theme.fontSmall
                                }

                                Rectangle {
                                    Layout.preferredWidth: 40
                                    Layout.preferredHeight: 18
                                    color: levelRow.modelData.clipped ? Theme.bad : "transparent"
                                    border.color: levelRow.modelData.clipped ? Theme.bad : Theme.border
                                    border.width: 1

                                    Text {
                                        anchors.centerIn: parent
                                        text: qsTr("CLIP")
                                        font.pixelSize: Theme.fontMicro
                                        font.bold: true
                                        color: levelRow.modelData.clipped ? Theme.accentText : Theme.textMuted
                                    }
                                }

                                Text {
                                    Layout.preferredWidth: 68
                                    text: levelRow.routedTo >= 0
                                          ? qsTr("→ out %1").arg(levelRow.routedTo + 1) : qsTr("unrouted")
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fontMicro
                                }
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            Layout.topMargin: 2
                            text: qsTr("−60 · −48 · −36 · −24 · −12 · −6 · 0 dB")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontFine
                        }
                    }
                }

                Card {
                    title: qsTr("04 Loudness · BS.1770-4")

                    Text {
                        Layout.fillWidth: true
                        text: Object.keys(HearthController.loudness).length > 0
                              ? qsTr("Integrated since this item started")
                              : qsTr("Nothing playing.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        visible: Object.keys(HearthController.loudness).length > 0
                        columns: 5
                        columnSpacing: Theme.gap
                        rowSpacing: Theme.gap / 2

                        Repeater {
                            model: [
                                { key: "momentary", label: qsTr("MOMENTARY"), unit: qsTr("LUFS") },
                                { key: "shortTerm", label: qsTr("SHORT-TERM"), unit: qsTr("LUFS") },
                                { key: "integrated", label: qsTr("INTEGRATED"), unit: qsTr("LUFS") },
                                { key: "range", label: qsTr("RANGE"), unit: qsTr("LU") },
                                { key: "truePeak", label: qsTr("TRUE PEAK"), unit: qsTr("dBTP") }
                            ]

                            delegate: ColumnLayout {
                                id: loudnessTile
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 2

                                Text {
                                    text: loudnessTile.modelData.label
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fontMicro
                                    font.bold: true
                                }
                                Text {
                                    text: HearthController.loudness[loudnessTile.modelData.key] !== undefined
                                          ? Number(HearthController.loudness[loudnessTile.modelData.key]).toFixed(1)
                                            + " " + loudnessTile.modelData.unit
                                          : qsTr("—")
                                    color: Theme.text
                                    font.pixelSize: Theme.fontBody
                                    font.bold: true
                                    font.family: Theme.monoFamily
                                }
                            }
                        }
                    }
                }

                Card {
                    title: qsTr("05 This frame")

                    Text {
                        Layout.fillWidth: true
                        text: Object.keys(HearthController.thisFrame).length > 0
                              ? qsTr("At play time")
                              : qsTr("Nothing playing.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        visible: Object.keys(HearthController.thisFrame).length > 0
                        columns: 4
                        columnSpacing: Theme.gap
                        rowSpacing: Theme.gap / 2

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                text: qsTr("DIALNORM")
                                color: Theme.textMuted; font.pixelSize: Theme.fontMicro; font.bold: true
                            }
                            Text {
                                text: HearthController.thisFrame.dialnorm !== undefined
                                      ? qsTr("−%1 dB").arg(HearthController.thisFrame.dialnorm) : qsTr("—")
                                color: Theme.text; font.pixelSize: Theme.fontBody; font.bold: true
                                font.family: Theme.monoFamily
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                text: qsTr("COMPR")
                                color: Theme.textMuted; font.pixelSize: Theme.fontMicro; font.bold: true
                            }
                            Text {
                                text: HearthController.thisFrame.comprDb !== undefined
                                      ? qsTr("%1 dB").arg(Number(HearthController.thisFrame.comprDb).toFixed(1))
                                      : qsTr("—")
                                color: Theme.text; font.pixelSize: Theme.fontBody; font.bold: true
                                font.family: Theme.monoFamily
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                text: qsTr("DYNRNG")
                                color: Theme.textMuted; font.pixelSize: Theme.fontMicro; font.bold: true
                            }
                            Text {
                                text: HearthController.thisFrame.dynrngMinDb !== undefined
                                      ? qsTr("%1…%2 dB")
                                            .arg(Number(HearthController.thisFrame.dynrngMinDb).toFixed(1))
                                            .arg(Number(HearthController.thisFrame.dynrngMaxDb).toFixed(1))
                                      : qsTr("—")
                                color: Theme.text; font.pixelSize: Theme.fontBody; font.bold: true
                                font.family: Theme.monoFamily
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                text: qsTr("SHORT BLOCKS")
                                color: Theme.textMuted; font.pixelSize: Theme.fontMicro; font.bold: true
                            }
                            Text {
                                // AC-3 only - UnitReport::short_blocks' own
                                // comment says why E-AC-3 leaves this unset.
                                text: HearthController.thisFrame.shortBlocks !== undefined
                                      ? qsTr("%1 of %2").arg(HearthController.thisFrame.shortBlocks)
                                                         .arg(HearthController.thisFrame.blocks)
                                      : qsTr("n/a")
                                color: Theme.text; font.pixelSize: Theme.fontBody; font.bold: true
                                font.family: Theme.monoFamily
                            }
                        }
                    }
                }
            }
        }

        // --- objects and signal path --------------------------------------
        ScrollView {
            id: sidebarScroll
            Layout.preferredWidth: 300
            Layout.fillHeight: true
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: sidebarScroll.availableWidth
                spacing: Theme.gap * 2

                Card {
                    title: qsTr("06 Objects")

                    Text {
                        Layout.fillWidth: true
                        text: HearthController.hasObjectMetadata
                              ? qsTr("%1 placed").arg(HearthController.objectsPlaced)
                              : (HearthController.playing ? qsTr("No object metadata in this stream")
                                                           : qsTr("Nothing playing"))
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }

                    Item {
                        id: room
                        Layout.fillWidth: true
                        Layout.preferredHeight: width
                        visible: HearthController.hasObjectMetadata

                        Rectangle {
                            anchors.fill: parent
                            color: Theme.neutral100
                            border.color: Theme.border
                            border.width: 1
                        }
                        // A stand-in for the design's dashed room circle - a
                        // plain ring, since a Rectangle border has no dash
                        // pattern without pulling in QtQuick.Shapes for one
                        // decorative line.
                        Rectangle {
                            anchors.centerIn: parent
                            width: Math.min(room.width, room.height) * 0.85
                            height: width
                            radius: width / 2
                            color: "transparent"
                            border.color: Theme.neutral400
                            border.width: 1
                        }

                        Repeater {
                            model: HearthController.objects

                            // oba::Position's own room cuboid (x: 0 left
                            // wall to 1 right wall, y: 0 front to 1 back) -
                            // the same frame apps/gui's ObjectInspectorDialog
                            // plan view places its markers in
                            // (object_decode_controller.cpp), placed here
                            // directly with no reprojection. label
                            // distinguishes a bed/speaker entry (named) from
                            // a dynamic object (unnamed); raised is
                            // position.z above the bed plane.
                            delegate: Rectangle {
                                id: marker
                                required property var modelData
                                readonly property bool isSpeaker: marker.modelData.label.length > 0
                                readonly property color markerColor:
                                    marker.isSpeaker ? Theme.neutral800 : Theme.bad
                                width: 10
                                height: 10
                                color: marker.modelData.raised ? "transparent" : marker.markerColor
                                border.width: marker.modelData.raised ? 2 : 0
                                border.color: marker.markerColor
                                x: marker.modelData.x * room.width - width / 2
                                y: marker.modelData.y * room.height - height / 2

                                Text {
                                    visible: marker.isSpeaker
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.bottom: parent.top
                                    text: marker.modelData.label
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fontFine
                                }
                            }
                        }
                    }

                    Flow {
                        Layout.fillWidth: true
                        visible: HearthController.hasObjectMetadata
                        spacing: Theme.gap

                        Row {
                            spacing: 4
                            Rectangle { width: 8; height: 8; anchors.verticalCenter: parent.verticalCenter; color: Theme.bad }
                            Text { text: qsTr("object"); color: Theme.textMuted; font.pixelSize: Theme.fontFine }
                        }
                        Row {
                            spacing: 4
                            Rectangle {
                                width: 8; height: 8; anchors.verticalCenter: parent.verticalCenter
                                color: "transparent"; border.color: Theme.bad; border.width: 1
                            }
                            Text { text: qsTr("raised"); color: Theme.textMuted; font.pixelSize: Theme.fontFine }
                        }
                        Row {
                            spacing: 4
                            Rectangle {
                                width: 8; height: 8; anchors.verticalCenter: parent.verticalCenter
                                color: Theme.neutral800
                            }
                            Text { text: qsTr("speaker"); color: Theme.textMuted; font.pixelSize: Theme.fontFine }
                        }
                        Row {
                            spacing: 4
                            Rectangle {
                                width: 8; height: 8; anchors.verticalCenter: parent.verticalCenter
                                color: "transparent"; border.color: Theme.neutral800; border.width: 1
                            }
                            Text { text: qsTr("height"); color: Theme.textMuted; font.pixelSize: Theme.fontFine }
                        }
                    }
                }

                Card {
                    title: qsTr("07 Signal path")

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
                                Button {
                                    objectName: "playChooseOutput"
                                    text: qsTr("Choose…")
                                    enabled: false
                                    // The output picker dialog is its own,
                                    // not-yet-built piece (a separate issue);
                                    // this stays a visible, disabled
                                    // placeholder rather than a fake action -
                                    // the same convention the Speakers page's
                                    // Identify card uses for render::IdentifyTone.
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    DropArea {
        anchors.fill: parent
        onDropped: function(drop) {
            if (drop.hasUrls) {
                HearthController.addFiles(drop.urls.map(function(u) { return u.toLocalFile ? u.toLocalFile() : u; }));
            }
        }
    }

    FileDialog {
        id: addFilesDialog
        title: qsTr("Add files")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("AC-3 / E-AC-3 (*.ac3 *.ec3)"), qsTr("All files (*)")]
        onAccepted: HearthController.addFiles(selectedFiles.map(function(u) { return u.toLocalFile(); }))
    }
}

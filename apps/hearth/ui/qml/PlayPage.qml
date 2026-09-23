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
// same tick as status() (hearth_controller.cpp's own poll()). Objects and
// signal path hold their own sidebar column when the window is wide enough,
// and fold into the monitor column's own scroll when it is not (root.narrow).
Item {
    id: root

    // Below this, the fixed queue (340) and Objects/Signal-path sidebar
    // (300) columns leave too little room for the monitor column's grids
    // (issue #888) - PlayObjectsCard/PlaySignalPathCard fold into the
    // monitor column's own scroll instead of holding a dedicated sidebar,
    // matching play-minimum-size.png's collapse at the enforced 960x620
    // minimum (Main.qml's minimumWidth/minimumHeight).
    readonly property bool narrow: root.width < 1100

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

    // "0:02", or "" for an item nothing has probed yet.
    function formatDuration(ms) {
        if (!ms) {
            return "";
        }
        var total = Math.round(ms / 1000);
        var mm = Math.floor(total / 60);
        var ss = total % 60;
        return mm + ":" + (ss < 10 ? "0" : "") + ss;
    }

    // The queue row's own metadata line (main-play.png, "01 QUEUE"): whatever
    // a probe has found so far, joined with " · " and skipping anything not
    // known yet - HearthController.queue's own fields fill in progressively,
    // the first time an item is about to play or is read ahead
    // (apps/hearth/engine/player.cpp's start_session()/prepare_next()), not
    // at addFiles() time.
    function queueMetadataLine(item) {
        var parts = [];
        if (item.streamKind.length > 0) {
            parts.push(item.streamKind);
        }
        if (item.channels > 0) {
            parts.push(qsTr("%1 ch").arg(item.channels) + (item.hasObjects ? qsTr(" + objects") : ""));
        }
        if (item.sampleRate > 0) {
            parts.push(qsTr("%1 kHz").arg(item.sampleRate / 1000));
        }
        if (item.bitrateKbps !== undefined) {
            parts.push(qsTr("%1 kbit/s").arg(Math.round(item.bitrateKbps)));
        }
        var duration = root.formatDuration(item.durationMs);
        if (duration.length > 0) {
            parts.push(duration);
        }
        return parts.join(" · ");
    }

    // "02 Now playing"'s own metadata line (main-play.png): the real facts
    // HearthController.currentMedia reads off the file itself, once its own
    // MediaInspector has read it - richer than the queue row's own line
    // (queueMetadataLine above), which only ever has what a probe at
    // queue-add time found.
    function nowPlayingMetadataLine() {
        var media = HearthController.currentMedia;
        if (Object.keys(media).length === 0) {
            return "";
        }
        var parts = [];
        var codec = { "ac3": qsTr("AC-3"), "eac3": qsTr("E-AC-3"), "ac3+eac3": qsTr("AC-3 core + E-AC-3"),
                      "ac4": qsTr("AC-4") }[media.codec];
        var objectCount = media.probe?.objectCount;
        if (codec !== undefined) {
            // stream_kind_name() (hearth_controller.cpp) makes this same
            // call for the queue row's own codec string; kept in step by
            // hand here since this line reads media.codec (MediaInfo's own,
            // richer probe) rather than the queue's ItemFacts.
            parts.push(media.codec === "eac3" && objectCount !== undefined ? codec + qsTr(" JOC") : codec);
        }
        var programme = (media.programmes ?? [])[0];
        if (programme !== undefined) {
            parts.push(programme.layoutLabel + (objectCount !== undefined
                        ? qsTr(" bed + %1 objects").arg(objectCount) : ""));
        }
        if (media.sampleRate) {
            parts.push(qsTr("%1 kHz").arg(media.sampleRate / 1000));
        }
        if (media.probe?.measuredBitrateKbps !== undefined) {
            parts.push(qsTr("%1 kbit/s").arg(Math.round(media.probe.measuredBitrateKbps)));
        }
        parts.push(media.container?.format ?? qsTr("elementary stream"));
        const next = root.nextItemHint();
        if (next.length > 0) {
            parts.push(next);
        }
        return parts.join(" · ");
    }

    // The next PLAYABLE item after the one playing now, skipping any the
    // queue itself marks unplayable - positionally, not wrapping (whether
    // the queue repeats is Queue/Transport's own setting, not one this page
    // reads yet). "" once nothing playable is left.
    function nextItemHint() {
        var queue = HearthController.queue;
        var current = HearthController.currentIndex;
        if (current < 0) {
            return "";
        }
        for (var i = current + 1; i < queue.length; i++) {
            if (queue[i].playable) {
                return qsTr("next: %1%2").arg(queue[i].title)
                                          .arg(HearthController.gapless ? qsTr(", gapless") : "");
            }
        }
        return "";
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

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap

                Button {
                    objectName: "addFiles"
                    text: qsTr("Add files…")
                    Layout.fillWidth: true
                    onClicked: addFilesDialog.open()
                }
                Button {
                    objectName: "addFolder"
                    text: qsTr("Add folder…")
                    Layout.fillWidth: true
                    onClicked: addFolderDialog.open()
                }
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
                    height: content.implicitHeight + Theme.pad
                    color: modelData.current ? Theme.accent100 : Theme.surface
                    border.color: modelData.current ? Theme.accent : Theme.border
                    border.width: modelData.current ? 2 : 1
                    radius: Theme.radius

                    // codecBadge answers "" for an item nothing has probed
                    // yet, and always for AC-4: io::scan() refuses those
                    // bytes at Session::open(), so ItemFacts.stream never
                    // gets set (apps/hearth/ui/item_loader.cpp's own
                    // comment). The extension is a presentation fallback
                    // only, for this one row - the Media page's "Showing"
                    // picker reads the real table of contents instead.
                    readonly property string badge: modelData.codecBadge.length > 0 ? modelData.codecBadge
                                                     : (/\.ac4$/i.test(modelData.path) ? "A4" : "")

                    Accessible.role: Accessible.ListItem
                    Accessible.name: modelData.title
                    Accessible.selected: modelData.current

                    ColumnLayout {
                        id: content
                        anchors.fill: parent
                        anchors.margins: Theme.pad / 2
                        spacing: 2

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gap / 2

                            Rectangle {
                                visible: row.badge.length > 0
                                Layout.preferredWidth: badgeLabel.implicitWidth + 8
                                Layout.preferredHeight: badgeLabel.implicitHeight + 2
                                color: modelData.playable ? Theme.accent : Theme.neutral400
                                radius: 2
                                Text {
                                    id: badgeLabel
                                    anchors.centerIn: parent
                                    text: row.badge
                                    color: Theme.accentText
                                    font.bold: true
                                    font.pixelSize: Theme.fontMicro
                                }
                            }
                            Text {
                                id: label
                                Layout.fillWidth: true
                                text: (modelData.current && HearthController.playing ? "▶ " : "") + modelData.title
                                color: modelData.playable ? Theme.text : Theme.textMuted
                                font.pixelSize: Theme.fontBody
                                elide: Text.ElideRight
                            }
                            Rectangle {
                                // "playing" only while it actually is - a
                                // merely-current-but-paused/stopped item
                                // shows no pill, matching the design's own
                                // one-pill-means-one-state reading.
                                visible: (modelData.current && HearthController.playing) || !modelData.playable
                                Layout.preferredWidth: pillLabel.implicitWidth + 10
                                Layout.preferredHeight: pillLabel.implicitHeight + 2
                                color: "transparent"
                                border.color: modelData.playable ? Theme.border : Theme.bad
                                border.width: 1
                                radius: Theme.radius
                                Text {
                                    id: pillLabel
                                    anchors.centerIn: parent
                                    text: modelData.playable ? qsTr("playing") : qsTr("not playable")
                                    color: modelData.playable ? Theme.textMuted : Theme.bad
                                    font.pixelSize: Theme.fontFine
                                }
                            }
                        }
                        Text {
                            id: note
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: modelData.playable ? root.queueMetadataLine(modelData) : modelData.note
                            color: modelData.playable ? Theme.textMuted : Theme.bad
                            font.pixelSize: Theme.fontSmall
                            elide: Text.ElideRight
                        }
                        // The currently-playing item's own progress, under
                        // its row (main-play.png, "01 QUEUE") - invisible and
                        // sizeless for every other row, HearthController.
                        // positionMs/durationMs being about the item playing
                        // now, not each row's own item.
                        Item {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 3
                            visible: modelData.current

                            Rectangle { anchors.fill: parent; color: Theme.neutral200 }
                            Rectangle {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: HearthController.durationMs > 0
                                       ? parent.width * Math.min(1, HearthController.positionMs / HearthController.durationMs)
                                       : 0
                                color: Theme.accent
                            }
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
                    // Names what item_loader.cpp actually reads today -
                    // AC-3/E-AC-3 files, and now a folder of them. Not MP4/
                    // MKV/TS: list_folder_items() finds those too, but
                    // make_file_item_loader() still can't open them, and
                    // naming a format that always lands "not playable"
                    // would overpromise (docs/hearth/design/screenshots/
                    // first-run.png's own text names them, for the slice
                    // that makes it true).
                    text: qsTr("Nothing in the queue.\nDrop AC-3 or E-AC-3 files or a folder here.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontBody
                    horizontalAlignment: Text.AlignHCenter
                }
            }

            // A visible target for the page-wide DropArea below, which
            // already accepts a drop anywhere on the page - this box only
            // shows where, matching the design's own "01 QUEUE" box under
            // the list (main-play.png), distinct from the list's own
            // empty-state text above.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: dropHint.implicitHeight + Theme.pad * 2
                color: Theme.surface
                border.color: Theme.border
                border.width: 1
                radius: Theme.radius

                Text {
                    id: dropHint
                    anchors.centerIn: parent
                    width: parent.width - Theme.pad * 2
                    text: qsTr("Drop files or folders here to add them to the queue")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
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
                        visible: text.length > 0
                        // The richer line once currentMedia has read the
                        // file; the bare stream kind straight away, since a
                        // MediaInspector read takes "a noticeable part of a
                        // second" (media_info.hpp) and the queue's own probe
                        // already knows this much immediately.
                        text: root.nowPlayingMetadataLine() || (HearthController.currentIndex >= 0
                              ? HearthController.queue[HearthController.currentIndex].streamKind : "")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }

                Card {
                    title: qsTr("03 Levels · at play time")
                    summary: HearthController.deviceName.length > 0
                             ? qsTr("%1 · %2 outputs").arg(HearthController.deviceName)
                                                       .arg(HearthController.routingOutputs)
                             : qsTr("No output device chosen yet")

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
                    summary: Object.keys(HearthController.loudness).length > 0
                             ? qsTr("Integrated since this item started")
                             : qsTr("Nothing playing.")

                    // Objects and signal path are further down this same
                    // scroll, not off in their own column, once the window
                    // is this narrow (play-minimum-size.png).
                    Text {
                        Layout.fillWidth: true
                        visible: root.narrow
                        text: qsTr("Scroll for this frame, objects and signal path.")
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
                    summary: Object.keys(HearthController.thisFrame).length > 0
                             ? qsTr("At play time · access unit %1")
                                   .arg(Number(HearthController.thisFrame.sequence).toLocaleString())
                             : qsTr("Nothing playing.")

                    GridLayout {
                        Layout.fillWidth: true
                        visible: Object.keys(HearthController.thisFrame).length > 0
                        columns: 5
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
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                text: qsTr("BITRATE")
                                color: Theme.textMuted; font.pixelSize: Theme.fontMicro; font.bold: true
                            }
                            Text {
                                // Absent for the stream's last unit, released
                                // by finish() - UnitReport::bitrate_kbps' own
                                // comment says why.
                                text: HearthController.thisFrame.bitrateKbps !== undefined
                                      ? qsTr("%1 kbit/s").arg(Math.round(HearthController.thisFrame.bitrateKbps))
                                      : qsTr("—")
                                color: Theme.text; font.pixelSize: Theme.fontBody; font.bold: true
                                font.family: Theme.monoFamily
                            }
                        }
                    }
                }

                // Objects and signal path fold in here, right after This
                // frame, once the window is too narrow for their own sidebar
                // column below (root.narrow) - play-minimum-size.png's own
                // collapse. Same two cards sidebarScroll holds, just
                // relocated; each is only ever shown in one place at a time.
                PlayObjectsCard { visible: root.narrow }
                PlaySignalPathCard { visible: root.narrow }
            }
        }

        // --- objects and signal path --------------------------------------
        // Their own column when there is room for one; folded into
        // monitorScroll's own scroll above instead when there is not
        // (root.narrow).
        ScrollView {
            id: sidebarScroll
            visible: !root.narrow
            Layout.preferredWidth: 300
            Layout.fillHeight: true
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: sidebarScroll.availableWidth
                spacing: Theme.gap * 2

                PlayObjectsCard { }
                PlaySignalPathCard { }
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

    FolderDialog {
        id: addFolderDialog
        title: qsTr("Add folder")
        onAccepted: HearthController.addFolder(selectedFolder.toLocalFile())
    }
}

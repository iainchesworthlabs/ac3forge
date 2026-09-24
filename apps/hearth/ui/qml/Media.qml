import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import Ac3ForgeHearth

// The Media information page (planning/hearth-reference-player.md, Media
// information; docs/hearth/design/screenshots/media-ac3.png, media-eac3-
// joc.png, media-ac4.png): what the picked queue item's own file says about
// itself, read once for the whole file (HearthController.inspectedMedia,
// backed by apps/hearth/engine/media_info.hpp's MediaInfo off a thread of
// its own). Defaults to the item playing now; the "Showing" picker can ask
// about any other queue item instead, including an AC-4 one, which never
// plays in this build but still has a table of contents to read.
Item {
    id: root

    readonly property var media: HearthController.inspectedMedia
    readonly property var probe: root.media.probe ?? ({})
    readonly property var bitstream: root.media.bitstream ?? ({})
    readonly property var container: root.media.container ?? ({})
    readonly property var ac4: root.media.ac4
    readonly property int effectiveIndex: HearthController.inspectedIndex >= 0
                                          ? HearthController.inspectedIndex : HearthController.currentIndex
    readonly property var showingLabels: HearthController.queue.map(function(item) { return item.title; })

    // media_info_to_map() always sets several keys (path, container,
    // programmes, json, ...) once a probe reply has landed for the item
    // inspectedMedia currently names - before that, poll() has cleared it to
    // {}. Reading this distinguishes "not read yet" from "read, and this
    // particular field came back empty/absent", which a `?? {}`/`undefined`
    // default alone cannot - see the Container row below for why that
    // distinction matters.
    readonly property bool mediaLoaded: Object.keys(root.media).length > 0
    readonly property bool hasContainer: Object.keys(root.container).length > 0
    readonly property bool hasOamdTable: root.probe.oamd === true
    readonly property bool hasMultipleProgrammes: (root.media.programmes ?? []).length > 1

    // Card numbering runs as one sequence over whichever of the optional
    // cards below actually renders (Container, Programmes, Objects · OAMD),
    // so a hidden one never leaves a skipped number behind it - the same
    // "numbering fossil" the AC-4 cards had before this file's own renumber.
    readonly property int nContainer: 1
    readonly property int nStream: root.hasContainer ? 2 : 1
    readonly property int nBitstream: root.nStream + 1
    readonly property int nProgrammes: root.nBitstream + 1
    readonly property int nOamd: (root.hasMultipleProgrammes ? root.nProgrammes : root.nBitstream) + 1
    readonly property int nProbe: (root.hasOamdTable ? root.nOamd
                                   : (root.hasMultipleProgrammes ? root.nProgrammes : root.nBitstream)) + 1

    // The ordinal is its own run in the design - accent ink, fixed-width -
    // so it is handed to SectionHeader separately rather than folded into
    // the title (and, incidentally, kept out of the translated string).
    function cardOrdinal(n) {
        return n < 10 ? "0" + n : "" + n;
    }

    // ac3::hearth::codec_token()'s lower-case wire tokens, spelled the way
    // the rest of this page's prose does ("AC-3", not "AC3").
    function codecLabel(token) {
        switch (token) {
            case "ac3": return qsTr("AC-3");
            case "eac3": return qsTr("E-AC-3");
            case "ac3+eac3": return qsTr("AC-3 core + E-AC-3");
            case "ac4": return qsTr("AC-4");
            default: return qsTr("unknown");
        }
    }
    // apps::container_token()'s lower-case wire tokens.
    function containerLabel(token) {
        switch (token) {
            case "mp4": return qsTr("MP4");
            case "matroska": return qsTr("Matroska");
            case "mpegts": return qsTr("MPEG-TS");
            default: return token ?? "";
        }
    }
    function formatDb(value, digits) {
        if (value === undefined || value === null) {
            return qsTr("not carried");
        }
        const n = digits === undefined ? 1 : digits;
        return (value >= 0 ? "+" : "") + value.toFixed(n) + " dB";
    }
    function formatDuration(seconds) {
        if (!seconds && seconds !== 0) {
            return "";
        }
        const total = Math.round(seconds);
        const mm = Math.floor(total / 60);
        const ss = total % 60;
        return mm + ":" + (ss < 10 ? "0" : "") + ss;
    }
    // Shared between the Stream card's "Objects" row and the Objects · OAMD
    // card's own summary line, so the two never drift apart.
    function objectsSummary() {
        if (!root.mediaLoaded) {
            return qsTr("reading…");
        }
        if (root.probe.objectCount === undefined) {
            return qsTr("none");
        }
        if (root.probe.joc && root.probe.objectCount === 0) {
            // JOC reconstructs its objects from the bed at decode time
            // (§oba/joc), so the parse tier's own dynamic_objects count -
            // what a literal OAMD payload carries - is genuinely 0 for this
            // shape of stream.
            return qsTr("reconstructed by JOC from the %1").arg(root.probe.bedLabel ?? "");
        }
        return qsTr("%1 · %2").arg(root.probe.objectCount).arg(root.probe.bedLabel ?? "");
    }

    // A hidden TextEdit is the portable way to reach the system clipboard
    // from pure QML: QtQuick.Dialogs (already used for file pickers here)
    // has no clipboard type of its own, and Qt.labs.platform's Clipboard
    // would be a new module dependency for one button.
    TextEdit {
        id: clipboardBuffer
        visible: false
        text: root.media.json ?? ""
    }
    function copyJson() {
        clipboardBuffer.selectAll();
        clipboardBuffer.copy();
    }

    FileDialog {
        id: exportDialog
        title: qsTr("Export JSON")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("JSON (*.json)"), qsTr("All files (*)")]
        onAccepted: HearthController.exportInspectedMedia(selectedFile)
    }

    ScrollView {
        id: scrollView
        anchors.fill: parent
        anchors.margins: Theme.pad
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: scrollView.availableWidth
            implicitWidth: scrollView.availableWidth
            spacing: Theme.gap * 2

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap

                Text {
                    text: qsTr("Showing")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMicro
                    font.letterSpacing: Theme.trackingWide
                    font.capitalization: Font.AllUppercase
                }
                AppComboBox {
                    id: showingCombo
                    objectName: "showingCombo"
                    Layout.preferredWidth: Math.round(420 * Theme.fontScale)
                    model: root.showingLabels
                    currentIndex: root.effectiveIndex
                    enabled: count > 0
                    Accessible.name: qsTr("Showing")
                    onActivated: function(index) { HearthController.inspectItem(index); }
                    // Activating a ComboBox writes currentIndex directly and
                    // the binding above is gone, so it stops following
                    // playback after the first manual pick; the model is also
                    // rebuilt on every queue change, which resets it to 0.
                    Binding on currentIndex {
                        value: root.effectiveIndex
                        when: !showingCombo.activeFocus && !showingCombo.popup.visible
                        restoreMode: Binding.RestoreBindingOrValue
                    }
                }
                Item { Layout.fillWidth: true }
                AppButton {
                    text: qsTr("Copy")
                    enabled: (root.media.json ?? "").length > 0
                    onClicked: root.copyJson()
                }
                AppButton {
                    text: qsTr("Export JSON…")
                    enabled: (root.media.json ?? "").length > 0
                    onClicked: exportDialog.open()
                }
            }

            Text {
                Layout.fillWidth: true
                visible: root.showingLabels.length === 0
                text: qsTr("Nothing in the queue yet. Add files on the Play page.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontBody
            }

            Rectangle {
                Layout.fillWidth: true
                // Every AC-4 item, whether or not its table of contents
                // parsed cleanly: this build has no AC-4 decoder regardless,
                // the same fact apps/hearth/ui/item_loader.cpp's own
                // unplayable reason states for the queue row.
                visible: root.showingLabels.length > 0 && root.ac4 !== undefined
                color: Theme.neutral100
                border.color: Theme.accentInk
                border.width: 1
                radius: Theme.radius
                implicitHeight: notPlayableBanner.implicitHeight + Theme.pad * 2

                ColumnLayout {
                    id: notPlayableBanner
                    anchors.fill: parent
                    anchors.margins: Theme.pad
                    spacing: Theme.gap / 2
                    Text {
                        text: qsTr("NOT PLAYABLE IN THIS BUILD")
                        color: Theme.accentInk
                        font.pixelSize: Theme.fontSmall
                        font.bold: true
                        font.letterSpacing: Theme.trackingWide
                        font.capitalization: Font.AllUppercase
                    }
                    Text {
                        text: qsTr("This build has no AC-4 decoder")
                        color: Theme.text
                        font.bold: true
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("What follows is read from the stream's table of contents. The item "
                                  + "stays in the queue and is skipped when it comes up.")
                        color: Theme.textMuted
                        wrapMode: Text.WordWrap
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                // A genuine read failure - nothing below has real data for
                // this item - as opposed to the AC-4 banner above, which is
                // about playability rather than about whether the read
                // itself succeeded.
                visible: root.showingLabels.length > 0 && (root.media.error ?? "").length > 0
                color: Theme.neutral100
                border.color: Theme.accentInk
                border.width: 1
                radius: Theme.radius
                implicitHeight: errorBanner.implicitHeight + Theme.pad * 2

                Text {
                    id: errorBanner
                    anchors.fill: parent
                    anchors.margins: Theme.pad
                    text: root.media.error ?? ""
                    color: Theme.text
                    font.bold: true
                    wrapMode: Text.WordWrap
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: root.showingLabels.length > 0
                spacing: Theme.gap * 2

                // --- left column -----------------------------------------
                ColumnLayout {
                    Layout.preferredWidth: 1
                    Layout.fillWidth: true
                    spacing: Theme.gap * 2

                    Card {
                        ordinal: root.cardOrdinal(root.nContainer)
                        title: qsTr("Container")
                        framed: true
                        visible: root.ac4 === undefined && root.hasContainer

                        GridLayout {
                            columns: 2
                            columnSpacing: Theme.gap
                            rowSpacing: 4
                            Layout.fillWidth: true

                            Text { text: qsTr("Format"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: root.containerLabel(root.container.format)
                                      + (root.container.codecId ? qsTr(" · %1").arg(root.container.codecId) : "")
                                color: Theme.text
                            }
                            Text { text: qsTr("Track"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: root.container.track !== undefined
                                      ? qsTr("%1").arg(root.container.track)
                                        + (root.container.language && root.container.language !== "und"
                                           ? qsTr(" · %1").arg(root.container.language) : "")
                                      : ""
                                color: Theme.text
                            }
                            Text {
                                text: qsTr("Edit list"); color: Theme.textMuted
                                visible: root.container.edits !== undefined
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: root.container.edits !== undefined
                                text: root.container.edits > 0
                                      ? qsTr("%1 entr%2").arg(root.container.edits)
                                                         .arg(root.container.edits === 1 ? "y" : "ies")
                                      : qsTr("none")
                                color: Theme.text
                            }
                            Text {
                                text: qsTr("dec3"); color: Theme.textMuted
                                visible: root.container.codecBox !== undefined
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: root.container.codecBox !== undefined
                                text: root.container.codecBox
                                      ? qsTr("data rate %1 kbit/s · %2 independent substream%3, %4 dependent "
                                            + "· bsid %5 · %6%7")
                                            .arg(root.container.codecBox.dataRateKbps)
                                            .arg(root.container.codecBox.independentSubstreams)
                                            .arg(root.container.codecBox.independentSubstreams === 1 ? "" : "s")
                                            .arg(root.container.codecBox.numDepSub)
                                            .arg(root.container.codecBox.bsid)
                                            .arg(root.container.codecBox.bsmodLabel)
                                            .arg(root.container.codecBox.lfeon ? qsTr(" · LFE on") : "")
                                      : ""
                                color: Theme.text
                                wrapMode: Text.WordWrap
                            }
                            Text {
                                text: qsTr("Atmos extension"); color: Theme.textMuted
                                visible: root.container.codecBox?.complexityIndex !== undefined
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: root.container.codecBox?.complexityIndex !== undefined
                                text: qsTr("complexity index %1").arg(root.container.codecBox?.complexityIndex)
                                color: Theme.text
                            }
                            Text {
                                text: qsTr("MPEG-TS"); color: Theme.textMuted
                                visible: root.container.mpegts !== undefined
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: root.container.mpegts !== undefined
                                text: root.container.mpegts
                                      ? qsTr("program %1 · PMT PID %2 · stream type %3")
                                            .arg(root.container.mpegts.programNumber)
                                            .arg(root.container.mpegts.pmtPid)
                                            .arg(root.container.mpegts.streamType)
                                      : ""
                                color: Theme.text
                            }
                        }
                    }
                    Card {
                        ordinal: root.cardOrdinal(root.nStream)
                        title: qsTr("Stream")
                        framed: true
                        // Pre-existing gap, fixed alongside this card's own
                        // renumbering: nothing gated this out for an AC-4
                        // item, which has its own "01 Stream" card in the
                        // right column instead - this one has nothing of its
                        // own to say for it (every field below reads
                        // "unknown"/"none").
                        visible: root.ac4 === undefined

                        GridLayout {
                            columns: 2
                            columnSpacing: Theme.gap
                            rowSpacing: 4
                            Layout.fillWidth: true

                            Text { text: qsTr("Codec"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: root.codecLabel(root.media.codec)
                                color: Theme.text
                            }
                            Text { text: qsTr("Sample rate"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: root.media.sampleRate ? qsTr("%1 kHz").arg(root.media.sampleRate / 1000)
                                                             : qsTr("unknown")
                                color: Theme.text
                            }
                            Text { text: qsTr("Bitrate"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: root.probe.measuredBitrateKbps !== undefined
                                      ? qsTr("%1 kbit/s measured").arg(root.probe.measuredBitrateKbps.toFixed(1))
                                      : qsTr("unknown")
                                color: Theme.text
                            }
                            Text { text: qsTr("Duration"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: root.formatDuration(root.media.durationSeconds)
                                color: Theme.text
                            }
                            Text { text: qsTr("Objects"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: root.objectsSummary()
                                color: Theme.text
                            }
                            // Only while there is no dedicated Container card
                            // to show this instead (below) - a real container
                            // moves this row there, matching how the design's
                            // own raw-stream mockup shows no Container card
                            // at all and its wrapped-stream mockup shows no
                            // Container row here.
                            Text {
                                text: qsTr("Container"); color: Theme.textMuted
                                visible: !root.hasContainer
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: !root.hasContainer
                                text: !root.mediaLoaded ? qsTr("reading…") : qsTr("none: an elementary stream")
                                color: Theme.text
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    Card {
                        ordinal: root.cardOrdinal(root.nBitstream)
                        title: qsTr("Bitstream information")
                        framed: true
                        visible: root.ac4 === undefined && Object.keys(root.bitstream).length > 0

                        GridLayout {
                            columns: 2
                            columnSpacing: Theme.gap
                            rowSpacing: 4
                            Layout.fillWidth: true

                            Text { text: qsTr("Service"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: root.bitstream.bsmodLabel ?? ""
                                color: Theme.text
                            }
                            Text { text: qsTr("Dialogue level"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: root.probe.dialnormDb !== undefined
                                      ? qsTr("dialnorm %1, %2 dB down").arg(root.formatDb(root.probe.dialnormDb, 0))
                                                                      .arg(31 + root.probe.dialnormDb)
                                      : qsTr("not carried")
                                color: Theme.text
                            }
                            Text { text: qsTr("Surround"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: root.bitstream.dsurmodLabel ?? ""
                                color: Theme.text
                            }
                            Text { text: qsTr("Lo/Ro mix levels"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("centre %1 · surround %2%3")
                                      .arg(root.formatDb(root.bitstream.mixLevels?.centreDb))
                                      .arg(root.formatDb(root.bitstream.mixLevels?.surroundDb))
                                      .arg(root.bitstream.mixLevels?.lfeDb !== undefined
                                           ? qsTr(" · LFE %1").arg(root.formatDb(root.bitstream.mixLevels.lfeDb))
                                           : "")
                                color: Theme.text
                                wrapMode: Text.WordWrap
                            }
                            Text { text: qsTr("Lt/Rt mix levels"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("centre %1 · surround %2")
                                      .arg(root.formatDb(root.bitstream.mixLevels?.ltrtCentreDb))
                                      .arg(root.formatDb(root.bitstream.mixLevels?.ltrtSurroundDb))
                                color: Theme.text
                                wrapMode: Text.WordWrap
                            }
                            Text { text: qsTr("Copyright"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: (root.bitstream.copyright
                                       ? qsTr("yes") : qsTr("no")) + " · "
                                      + (root.bitstream.original
                                         ? qsTr("original bitstream: yes") : qsTr("original bitstream: no"))
                                color: Theme.text
                            }
                        }
                    }
                    Card {
                        ordinal: root.cardOrdinal(1)
                        title: qsTr("Stream")
                        framed: true
                        visible: root.ac4 !== undefined

                        GridLayout {
                            columns: 2
                            columnSpacing: Theme.gap
                            rowSpacing: 4
                            Layout.fillWidth: true

                            Text { text: qsTr("Bitstream version"); color: Theme.textMuted }
                            Text { text: root.ac4 ? root.ac4.bitstreamVersion : ""; color: Theme.text }
                            Text { text: qsTr("Sync frames"); color: Theme.textMuted }
                            Text {
                                text: root.ac4
                                      ? qsTr("%1 · CRC %2 failed").arg(root.ac4.syncFrames).arg(root.ac4.crcFailures)
                                      : ""
                                color: Theme.text
                            }
                            Text { text: qsTr("Presentations"); color: Theme.textMuted }
                            Text { text: root.ac4 ? root.ac4.presentationCount : ""; color: Theme.text }
                            Text { text: qsTr("Substreams"); color: Theme.textMuted }
                            Text { text: root.ac4 ? root.ac4.substreamCount : ""; color: Theme.text }
                        }
                    }
                    Card {
                        ordinal: root.cardOrdinal(2)
                        title: qsTr("Presentations")
                        framed: true
                        visible: root.ac4 !== undefined && (root.ac4?.presentations ?? []).length > 0

                        Repeater {
                            model: root.ac4 ? root.ac4.presentations : []
                            delegate: ColumnLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 0
                                Text {
                                    Layout.fillWidth: true
                                    text: qsTr("%1%2").arg(modelData.index + 1)
                                          .arg(modelData.id !== undefined ? qsTr(" · id %1").arg(modelData.id) : "")
                                    color: Theme.text
                                    font.bold: true
                                }
                                Repeater {
                                    model: modelData.substreams ?? modelData.groupRefs ?? []
                                    delegate: Text {
                                        required property var modelData
                                        Layout.leftMargin: 16
                                        text: modelData.channelMode !== undefined
                                              ? qsTr("%1 · %2").arg(modelData.role).arg(modelData.channelMode)
                                              : qsTr("group %1").arg(modelData)
                                        color: Theme.textMuted
                                        font.pixelSize: Theme.fontSmall
                                    }
                                }
                            }
                        }
                    }
                }

                // --- right column ----------------------------------------
                ColumnLayout {
                    Layout.preferredWidth: 1
                    Layout.fillWidth: true
                    spacing: Theme.gap * 2

                    Card {
                        ordinal: root.cardOrdinal(root.nProgrammes)
                        title: qsTr("Programmes")
                        framed: true
                        visible: root.ac4 === undefined && root.hasMultipleProgrammes

                        Repeater {
                            model: root.media.programmes ?? []
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: Theme.gap
                                Text {
                                    text: qsTr("%1").arg(modelData.substreamId)
                                    color: Theme.textMuted
                                    Layout.preferredWidth: 20
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: qsTr("%1 · %2 channels · %3")
                                          .arg(modelData.layoutLabel).arg(modelData.channels)
                                          .arg(modelData.bsmodLabel)
                                    color: Theme.text
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }



                    Card {
                        ordinal: root.cardOrdinal(3)
                        title: qsTr("Substream groups")
                        framed: true
                        visible: root.ac4 !== undefined && (root.ac4?.substreamGroups ?? []).length > 0

                        Repeater {
                            model: root.ac4 ? root.ac4.substreamGroups : []
                            delegate: ColumnLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 0
                                Text {
                                    text: qsTr("Group %1 · %2").arg(modelData.index + 1)
                                          .arg(modelData.channelCoded ? qsTr("channels") : qsTr("objects"))
                                    color: Theme.text
                                    font.bold: true
                                }
                                Repeater {
                                    model: modelData.substreams ?? []
                                    delegate: Text {
                                        required property var modelData
                                        Layout.leftMargin: 16
                                        Layout.fillWidth: true
                                        text: modelData
                                        color: Theme.textMuted
                                        font.pixelSize: Theme.fontSmall
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }
                        }
                    }

                    Card {
                        ordinal: root.cardOrdinal(4)
                        title: qsTr("Immersive")
                        framed: true
                        visible: root.ac4 !== undefined && root.ac4?.hasAjoc === true

                        GridLayout {
                            columns: 2
                            columnSpacing: Theme.gap
                            rowSpacing: 4
                            Layout.fillWidth: true

                            Text { text: qsTr("A-JOC"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("present")
                                color: Theme.text
                            }
                            Text { text: qsTr("Object metadata"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("present · not read by this build's inspector")
                                color: Theme.text
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    Card {
                        ordinal: root.cardOrdinal(root.nOamd)
                        title: qsTr("Objects · OAMD")
                        framed: true
                        visible: root.ac4 === undefined && root.hasOamdTable

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gap

                            GridLayout {
                                columns: 2
                                columnSpacing: Theme.gap
                                rowSpacing: 4
                                Layout.fillWidth: true

                                Text { text: qsTr("Objects"); color: Theme.textMuted }
                                Text {
                                    Layout.fillWidth: true
                                    text: root.objectsSummary()
                                    color: Theme.text
                                }
                                Text { text: qsTr("Complexity index"); color: Theme.textMuted }
                                Text {
                                    Layout.fillWidth: true
                                    text: root.probe.complexityIndex !== undefined
                                          ? qsTr("%1").arg(root.probe.complexityIndex) : qsTr("not carried")
                                    color: Theme.text
                                }
                                Text { text: qsTr("Authenticity tag"); color: Theme.textMuted }
                                Text {
                                    Layout.fillWidth: true
                                    text: (root.probe.authenticityTaggedFrames ?? 0) > 0
                                          ? qsTr("%1 of %2 access units")
                                                .arg(root.probe.authenticityTaggedFrames).arg(root.probe.accessUnits)
                                          : qsTr("none in this stream")
                                    color: Theme.text
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4
                                visible: (root.media.objects ?? []).length > 0

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.gap
                                    Text { text: qsTr("#"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall
                                           Layout.preferredWidth: 22 }
                                    Text { text: qsTr("X"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall
                                           Layout.preferredWidth: 46 }
                                    Text { text: qsTr("Y"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall
                                           Layout.preferredWidth: 46 }
                                    Text { text: qsTr("Z"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall
                                           Layout.preferredWidth: 46 }
                                    Text { text: qsTr("GAIN"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall
                                           Layout.preferredWidth: 64 }
                                    Text { text: qsTr("ACTIVE"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall
                                           Layout.fillWidth: true }
                                }
                                Repeater {
                                    model: root.media.objects ?? []
                                    delegate: RowLayout {
                                        required property var modelData
                                        required property int index
                                        Layout.fillWidth: true
                                        spacing: Theme.gap
                                        Text { text: qsTr("%1").arg(index + 1); color: Theme.text
                                               Layout.preferredWidth: 22 }
                                        Text { text: Number(modelData.x).toFixed(2); color: Theme.text
                                               Layout.preferredWidth: 46 }
                                        Text { text: Number(modelData.y).toFixed(2); color: Theme.text
                                               Layout.preferredWidth: 46 }
                                        Text { text: Number(modelData.z).toFixed(2); color: Theme.text
                                               Layout.preferredWidth: 46 }
                                        Text { text: root.formatDb(modelData.gainDb); color: Theme.text
                                               Layout.preferredWidth: 64 }
                                        Text { text: modelData.active ? qsTr("yes") : qsTr("no"); color: Theme.text
                                               Layout.fillWidth: true }
                                    }
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: (root.media.objects ?? []).length === 0
                                text: qsTr("The object layer could not be read as per-object detail for this stream.")
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSmall
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    Card {
                        ordinal: root.cardOrdinal(root.nProbe)
                        title: qsTr("Probe summary")
                        framed: true
                        visible: root.ac4 === undefined && Object.keys(root.probe).length > 0

                        GridLayout {
                            columns: 2
                            columnSpacing: Theme.gap
                            rowSpacing: 4
                            Layout.fillWidth: true

                            Text { text: qsTr("Dynamic range"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: root.probe.dynrngSeen
                                      ? qsTr("%1 to %2")
                                            .arg(root.formatDb(root.probe.dynrngMinDb))
                                            .arg(root.formatDb(root.probe.dynrngMaxDb))
                                      : qsTr("not carried")
                                color: Theme.text
                            }
                            Text { text: qsTr("Heavy compression"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: root.probe.comprSeen
                                      ? qsTr("%1 to %2")
                                            .arg(root.formatDb(root.probe.comprMinDb))
                                            .arg(root.formatDb(root.probe.comprMaxDb))
                                      : qsTr("not carried")
                                color: Theme.text
                            }
                            Text { text: qsTr("Block switching"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("in %1 of %2 blocks read")
                                      .arg(root.probe.blockSwitchBlocks).arg(root.probe.blocksParsed)
                                color: Theme.text
                            }
                            Text { text: qsTr("CRC"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("%1 access units · %2 failed")
                                      .arg(root.probe.accessUnits).arg(root.probe.crcFailures)
                                color: Theme.text
                            }
                            Text { text: qsTr("EMDF payloads"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: (root.probe.emdfPayloadLabels ?? []).length > 0
                                      ? root.probe.emdfPayloadLabels.join(", ") : qsTr("none")
                                color: Theme.text
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                }
            }
        }
    }
}

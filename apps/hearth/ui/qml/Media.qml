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

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        text: qsTr("SHOWING")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        font.bold: true
                        font.capitalization: Font.AllUppercase
                    }
                    ComboBox {
                        objectName: "showingCombo"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 360
                        model: root.showingLabels
                        currentIndex: root.effectiveIndex
                        enabled: count > 0
                        onActivated: function(index) { HearthController.inspectItem(index); }
                    }
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: qsTr("Copy")
                    enabled: (root.media.json ?? "").length > 0
                    onClicked: root.copyJson()
                }
                Button {
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
                color: Theme.accent100
                border.color: Theme.bad
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
                        color: Theme.bad
                        font.pixelSize: Theme.fontSmall
                        font.bold: true
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
                color: Theme.accent100
                border.color: Theme.bad
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
                        title: qsTr("01 Stream")

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
                                text: root.probe.objectCount === undefined
                                      ? qsTr("none")
                                      : (root.probe.joc && root.probe.objectCount === 0
                                         // JOC reconstructs its objects from the bed at decode
                                         // time (§oba/joc), so the parse tier's own
                                         // dynamic_objects count - what a literal OAMD payload
                                         // carries - is genuinely 0 for this shape of stream.
                                         ? qsTr("reconstructed by JOC from the %1").arg(root.probe.bedLabel ?? "")
                                         : qsTr("%1 · %2").arg(root.probe.objectCount)
                                                          .arg(root.probe.bedLabel ?? ""))
                                color: Theme.text
                            }
                            Text { text: qsTr("Container"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: root.container.format ?? qsTr("none: an elementary stream")
                                color: Theme.text
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    Card {
                        title: qsTr("02 Bitstream information")
                        visible: Object.keys(root.bitstream).length > 0

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
                            Text { text: qsTr("Mix levels"); color: Theme.textMuted }
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
                }

                // --- right column ----------------------------------------
                ColumnLayout {
                    Layout.preferredWidth: 1
                    Layout.fillWidth: true
                    spacing: Theme.gap * 2

                    Card {
                        title: qsTr("03 Programmes")
                        visible: root.ac4 === undefined

                        Text {
                            Layout.fillWidth: true
                            visible: (root.media.programmes ?? []).length === 0
                            text: qsTr("Read once the item has been probed.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                        }
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
                        title: qsTr("03 Stream")
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
                        title: qsTr("04 Presentations")
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

                    Card {
                        title: qsTr("05 Substream groups")
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
                        title: qsTr("06 Probe summary")
                        visible: root.ac4 === undefined && Object.keys(root.probe).length > 0

                        GridLayout {
                            columns: 2
                            columnSpacing: Theme.gap
                            rowSpacing: 4
                            Layout.fillWidth: true

                            Text { text: qsTr("Dynamic range"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: root.probe.dynrngSeen ? qsTr("carried") : qsTr("not carried")
                                color: Theme.text
                            }
                            Text { text: qsTr("Heavy compression"); color: Theme.textMuted }
                            Text {
                                Layout.fillWidth: true
                                text: root.probe.comprSeen ? qsTr("carried") : qsTr("not carried")
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
                                text: (root.probe.emdfPayloadIds ?? []).length > 0
                                      ? root.probe.emdfPayloadIds.join(", ") : qsTr("none")
                                color: Theme.text
                            }
                        }
                    }
                }
            }
        }
    }
}

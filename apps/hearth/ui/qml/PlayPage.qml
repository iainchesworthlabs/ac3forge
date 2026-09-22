import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import Ac3ForgeHearth

// The Play page (planning/hearth-design.md, "The main window, playing"): the
// queue and now playing, built over the real engine. The meters, loudness,
// this-frame detail, object placement and signal path panels the design
// shows beside the queue follow in a later slice, once the monitor
// (A5) has something real to read from - Engine::meters() and
// Engine::unit_report(), which this controller does not poll yet.
Item {
    id: root

    RowLayout {
        anchors.fill: parent
        anchors.margins: Theme.pad
        spacing: Theme.gap

        // --- queue -----------------------------------------------------
        ColumnLayout {
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

        // --- now playing -------------------------------------------------
        Card {
            Layout.fillWidth: true
            Layout.fillHeight: true
            title: qsTr("Now playing")

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
            Text {
                Layout.fillWidth: true
                Layout.topMargin: Theme.gap
                text: qsTr("Levels, loudness, this frame's detail, the object placement view and the "
                          + "signal path follow in the next slice.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
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

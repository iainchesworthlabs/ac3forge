import QtQuick
import QtQuick.Layouts

import Ac3ForgeHearth

// The right-hand "WHAT THE SINK REPORTS" panel (network-sink-decoder.png):
// NetworkController.sinkReport, already the display strings the page shows
// (network_controller.cpp's own sink_report_to_map(), the same "no
// formatting in QML" rule SinkDetail's own fields follow) - from the sink's
// own client/state messages, sent whenever something changes, not from
// what this app has asked for (NetworkSinkOnlyOnSink.qml's sibling panel
// shows THAT instead, beside Speakers).
ColumnLayout {
    id: root
    spacing: Theme.gap

    readonly property var report: NetworkController.sinkReport

    readonly property var rows: [
        { label: qsTr("Settings"), value: root.report.settingsText ?? "" },
        { label: qsTr("Stream"), value: root.report.streamText ?? "" },
        { label: qsTr("Objects"), value: root.report.objectsText ?? "" },
        { label: qsTr("Dialogue"), value: root.report.dialogueText ?? "" },
        { label: qsTr("Played"), value: root.report.playedText ?? "" },
        { label: qsTr("Problems"), value: root.report.problemsText ?? "" }
    ]

    Card {
        Layout.fillWidth: true
        title: qsTr("03 WHAT THE SINK REPORTS")

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: Theme.gap
            rowSpacing: Theme.gap / 2

            Repeater {
                model: root.rows

                delegate: Item {
                    required property var modelData
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    implicitHeight: Math.max(label.implicitHeight, value.implicitHeight)

                    Text {
                        id: label
                        anchors.left: parent.left
                        anchors.top: parent.top
                        width: 84
                        text: modelData.label
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }
                    Text {
                        id: value
                        anchors.left: parent.left
                        anchors.leftMargin: 88
                        anchors.right: parent.right
                        anchors.top: parent.top
                        text: modelData.value.length > 0 ? modelData.value : qsTr("—")
                        color: Theme.text
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.topMargin: Theme.gap / 2
            text: qsTr("From the sink's own state messages, which it sends whenever something changes.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }
    }

    Item { Layout.fillHeight: true }
}

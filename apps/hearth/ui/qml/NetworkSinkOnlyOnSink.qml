import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The right-hand "ONLY ON THE SINK" panel (network-sink-speakers.png):
// identity the sink itself owns - name, slot width, network and firmware -
// which this page only shows and links out to, never edits. Structured
// like NetworkSinkInfo.qml's own label/value rows, for a different map
// (NetworkController.sinkOnlyOnSink, network_controller.cpp's own
// sink_only_on_sink_to_map()).
ColumnLayout {
    id: root
    spacing: Theme.gap

    readonly property var sink: NetworkController.sinkOnlyOnSink

    readonly property var rows: [
        { label: qsTr("Name"), value: root.sink.name ?? "" },
        { label: qsTr("Slot width"), value: root.sink.slotsText ?? "" },
        { label: qsTr("Network"), value: root.sink.network ?? "" },
        { label: qsTr("Firmware"), value: root.sink.firmware ?? "" }
    ]

    // Flat with its ordinal as its own run, like the other two columns: the
    // mockup's right column is a heading and a list on the page background,
    // not a box. The header uppercases the words itself, so the string does
    // not have to - and the bare "03" stays out of it.
    Card {
        Layout.fillWidth: true
        flat: true
        ordinal: "03"
        title: qsTr("Only on the sink")

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
            text: qsTr("Set on the sink's page, together with its wiring and pairing.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }

        Button {
            objectName: "networkSinkOpenPage"
            Layout.topMargin: Theme.gap / 2
            visible: (root.sink.url ?? "").length > 0
            text: qsTr("🔗 Open %1").arg(root.sink.url ?? "")
            onClicked: Qt.openUrlExternally(root.sink.url)
        }
    }

    Item { Layout.fillHeight: true }
}

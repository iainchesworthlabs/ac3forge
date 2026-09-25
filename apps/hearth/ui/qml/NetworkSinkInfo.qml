import QtQuick
import QtQuick.Layouts

import Ac3ForgeHearth

// The right-hand "THIS SINK" panel (planning/hearth-design.md's
// network-pairing artboard): what NetworkController knows about the selected
// sink, as label/value rows - kind, address, roles, takes, outputs, latency
// and clock. NetworkController.selectedSink already carries these as display
// strings (network_view.cpp does the formatting), so this component only
// lays them out.
ColumnLayout {
    id: root
    spacing: Theme.gap

    property var sink: ({})

    readonly property var rows: {
        if (!root.sink || root.sink.id === undefined) {
            return [];
        }
        const rows = [
            { label: qsTr("Connection"), value: root.sink.linkText ?? "" },
            { label: qsTr("Kind"), value: root.sink.kindText ?? "" },
            { label: qsTr("Address"), value: root.sink.address ?? "" },
            { label: qsTr("Roles"), value: root.sink.rolesText ?? "" },
            { label: qsTr("Takes"), value: root.sink.takesText ?? "" },
            { label: qsTr("Outputs"), value: root.sink.outputsText ?? "" },
            { label: qsTr("Latency"), value: root.sink.latencyText ?? "" },
            { label: qsTr("Clock"), value: root.sink.clockText ?? "" }
        ];
        if ((root.sink.notice ?? "").length > 0) {
            rows.unshift({ label: qsTr("Status"), value: root.sink.notice });
        }
        return rows;
    }

    Card {
        Layout.fillWidth: true
        ordinal: "03"
        title: qsTr("This sink")

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
    }

    Item { Layout.fillHeight: true }
}

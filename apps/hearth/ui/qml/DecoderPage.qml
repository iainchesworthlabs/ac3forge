import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The Decoder tab (planning/hearth-design.md, "Speakers and decoder"): a
// sub-switch between the AC-3/E-AC-3 decoder, which this build actually
// runs, and the AC-4 decoder, shown inactive until chip D exists. Defaults
// to AC-3/E-AC-3, the common case and the one with live controls; nothing
// here reads the queue's own stream kind yet to choose for itself - the
// same gap DecoderEac3.qml's own comment notes for "this stream".
Item {
    id: root

    property string format: "eac3"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.pad
        spacing: Theme.gap

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gap

            SegmentedControl {
                accessibleName: qsTr("Decoder")
                currentValue: root.format
                model: [
                    { value: "eac3", label: qsTr("AC-3 / E-AC-3") },
                    { value: "ac4", label: qsTr("AC-4") }
                ]
                onSelected: function(value) { root.format = value; }
            }
            Text {
                text: qsTr("Changes reach the playing item at its next access unit.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
            }
            Item { Layout.fillWidth: true }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.format === "ac4" ? 1 : 0

            DecoderEac3 { }
            DecoderAc4 { }
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The Decoder tab (planning/hearth-design.md, "Speakers and decoder"): a
// sub-switch between the AC-3/E-AC-3 decoder and the AC-4 decoder
// (planning/ac4.md, I2). It defaults to AC-3/E-AC-3, and turns to whichever
// the playing item needs when that item changes, so the page open is the one
// whose settings are being heard; the switch still shows either at any time.
Item {
    id: root

    property string format: "eac3"

    // The playing item's format, once its media information has been read:
    // "ac4", or "eac3" for AC-3 and E-AC-3; "" while nothing is known.
    readonly property string playingFormat: {
        const codec = HearthController.currentMedia.codec;
        return codec === undefined ? "" : (codec === "ac4" ? "ac4" : "eac3");
    }
    onPlayingFormatChanged: {
        if (root.playingFormat.length > 0) {
            root.format = root.playingFormat;
        }
    }

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
                    { value: "eac3", label: qsTr("AC-3 and E-AC-3") },
                    { value: "ac4", label: qsTr("AC-4") }
                ]
                onSelected: function(value) { root.format = value; }
            }
            Text {
                text: root.format === "ac4"
                      ? qsTr("Changes reach the playing item at its next frame.")
                      : qsTr("Changes reach the playing item at its next access unit.")
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

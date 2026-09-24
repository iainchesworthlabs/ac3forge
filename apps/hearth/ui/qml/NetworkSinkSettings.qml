import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// A Hearth sink's own speaker and decoder settings pages (planning/hearth-
// reference-player.md#a6-network-outputs-in-the-application, A6's second
// slice; docs/hearth/design/screenshots/network-sink-{speakers,decoder}.png).
// Network.qml shows this instead of the plain "paired" card once
// NetworkController.selectedSinkSettable is true - a sink offering
// _ac3forge_player@v1, paired and connected.
//
// The Speakers/Decoder tab switch here (card "02") mirrors the top-level
// Speakers/Decoder pages' own controls one for one, reading and writing
// NetworkController.sinkSpeakerSettings/sinkDecoderSettings instead of
// HearthController.trimDb/decoderSettings/etc - a SEPARATE settings object
// on the SELECTED SINK, over the network, not this computer's own engine.
// The right column ("03") switches with the tab too: the sink's own
// identity (NetworkSinkOnlyOnSink, below) beside Speakers, what it reports
// back (NetworkSinkReport.qml) beside Decoder - matching the two mockups.
RowLayout {
    id: root
    anchors.fill: parent
    spacing: Theme.gap * 2

    property string activeTab: "speakers"

    readonly property var sink: NetworkController.selectedSink
    readonly property var speakers: NetworkController.sinkSpeakerSettings
    readonly property var decoderReport: NetworkController.sinkReport

    ColumnLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.preferredWidth: 2
        spacing: Theme.gap

        // Flat: the mockup boxes the tables further down the page, never this
        // heading, the tab pair or the note beside it. The ordinal is its own
        // accent-ink run rather than part of the title, which also keeps the
        // bare "02" out of a translated string - the sink's name is still
        // uppercased, by the header's own font, not by the string.
        Card {
            flat: true
            ordinal: "02"
            title: qsTr("%1 · settings").arg(root.sink.name ?? "")

            // The note sits BESIDE the tabs, not under them, and takes the
            // rest of the row.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                SegmentedControl {
                    objectName: "networkSinkSettingsTab"
                    accessibleName: qsTr("Speakers or decoder")
                    currentValue: root.activeTab
                    model: [
                        { value: "speakers", label: qsTr("Speakers") },
                        { value: "decoder", label: qsTr("Decoder") }
                    ]
                    onSelected: function(value) { root.activeTab = value; }
                }
                Text {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.alignment: Qt.AlignVCenter
                    text: root.activeTab === "speakers"
                          ? qsTr("Changes reach the sink and take effect at its next burst.")
                          : qsTr("The decoder settings this sink accepts, as it lists them. Changes take "
                                + "effect at its next burst.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.WordWrap
                }
            }
        }

        Loader {
            Layout.fillWidth: true
            Layout.fillHeight: true
            sourceComponent: root.activeTab === "speakers" ? speakersTab : decoderTab
        }
    }

    Loader {
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        Layout.fillHeight: true
        sourceComponent: root.activeTab === "speakers" ? onlyOnSinkPanel : reportPanel
    }

    Component {
        id: speakersTab
        NetworkSinkSpeakers { }
    }
    Component {
        id: decoderTab
        NetworkSinkDecoder { }
    }

    Component {
        id: onlyOnSinkPanel
        NetworkSinkOnlyOnSink { }
    }
    Component {
        id: reportPanel
        NetworkSinkReport { }
    }
}

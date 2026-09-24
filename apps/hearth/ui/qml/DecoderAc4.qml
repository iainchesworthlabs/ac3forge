import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The AC-4 decoder page (planning/hearth-design.md), inactive: AC-4 needs a
// decoder this build does not have (chip D). Every control the format will
// use is drawn, held inactive with the "not in this build" banner the design
// gives it, and the two controls AC-4 shares with AC-3/E-AC-3 (dynamic range
// mode, stereo/mono downmix) mirror what the Decoder tab's own AC-3/E-AC-3
// page holds, live, so the two pages cannot disagree once chip D lands and
// this one turns on for real. One simplification: "01 Presentation" is a
// single static ComboBox row, not the design's real #/language/channels/
// content/groups table - there is no stream to read a real table's worth of
// presentations from yet, so building it now would mean inventing rows
// rather than reading them.
ScrollView {
    id: root

    // Same control column the AC-3/E-AC-3 tab uses.
    readonly property int labelWidth: Math.round(108 * Theme.fontScale)
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    readonly property var settings: HearthController.decoderSettings

    ColumnLayout {
        width: root.availableWidth
        spacing: Theme.gap * 2

        Rectangle {
            Layout.fillWidth: true
            color: Theme.neutral100
            border.color: Theme.accentInk
            border.width: 1
            radius: Theme.radius
            implicitHeight: banner.implicitHeight + Theme.pad * 2

            ColumnLayout {
                id: banner
                anchors.fill: parent
                anchors.margins: Theme.pad
                spacing: Theme.gap / 2

                Text {
                    // Mixed case in the string, uppercased by the font
                    // property: a locale whose casing rules differ should
                    // not have "NOT IN THIS BUILD" baked into its catalogue.
                    text: qsTr("Not in this build")
                    color: Theme.accentInk
                    font.pixelSize: Theme.fontSmall
                    font.bold: true
                    font.letterSpacing: Theme.trackingWide
                    font.capitalization: Font.AllUppercase
                }
                Text {
                    text: qsTr("AC-4 needs a decoder this build does not have")
                    color: Theme.text
                    font.pixelSize: Theme.fontHeading
                    font.bold: true
                }
                Text {
                    Layout.fillWidth: true
                    font.pixelSize: Theme.fontBody
                    text: qsTr("These are the AC-4 settings Hearth will use, shown so the page is complete. "
                              + "They stay inactive until an AC-4 decoder is added; AC-4 items in the queue "
                              + "show their media information and are skipped when they come up. The two "
                              + "settings AC-4 shares with AC-3 and E-AC-3 are live here and on that tab.")
                    color: Theme.textMuted
                    wrapMode: Text.WordWrap
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gap * 2

            ColumnLayout {
                Layout.preferredWidth: 1
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: Theme.gap * 2

                Card {
                    ordinal: "01"
                    title: qsTr("Presentation")
                    framed: true
                    enabled: false

                    AppComboBox {
                        Layout.preferredWidth: Math.round(319 * Theme.fontScale)
                        model: [qsTr("1 · English · 5.1 · main")]
                        currentIndex: 0
                        Accessible.name: qsTr("Presentation")
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("From the stream's table of contents. With no choice made, the first "
                                  + "presentation in your language plays.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }

                Card {
                    ordinal: "02"
                    title: qsTr("Dialogue")
                    framed: true
                    enabled: false

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Enhancement"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                        AppSlider { id: enhancementSlider; Layout.preferredWidth: Math.round(200 * Theme.fontScale)
                                   from: 0; to: 12; value: 6 }
                        Text { text: qsTr("%1 dB").arg(enhancementSlider.value.toFixed(0)); color: Theme.textMuted
                               font.family: Theme.monoFamily; font.pixelSize: Theme.fontNormal }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Raises dialogue against the rest of the mix where the stream carries "
                                  + "dialogue enhancement data. 0 to 12 dB.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Item { Layout.preferredWidth: root.labelWidth }
                        AppCheckBox {
                            Layout.fillWidth: true
                            text: qsTr("Mix in audio description")
                            note: qsTr("When the presentation carries an associated programme.")
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Its level"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                        AppSlider { id: levelSlider; Layout.preferredWidth: Math.round(200 * Theme.fontScale)
                                   from: -12; to: 0; value: -6 }
                        Text { text: qsTr("%1 dB").arg(levelSlider.value.toFixed(0)).replace("-", "−")
                               color: Theme.textMuted
                               font.family: Theme.monoFamily; font.pixelSize: Theme.fontNormal }
                    }
                }
            }

            ColumnLayout {
                Layout.preferredWidth: 1
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: Theme.gap * 2

                Card {
                    ordinal: "03"
                    title: qsTr("Dynamic range")
                    framed: true

                    Text {
                        text: qsTr("SHARED WITH AC-3 AND E-AC-3 · LIVE")
                        color: Theme.accentInk
                        font.pixelSize: Theme.fontMicro
                        font.bold: true
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Mode"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                        SegmentedControl {
                            accessibleName: qsTr("Mode")
                            currentValue: root.settings.mode ?? "line"
                            model: [
                                { value: "line", label: qsTr("Line") },
                                { value: "rf", label: qsTr("RF") },
                                { value: "custom", label: qsTr("Custom") }
                            ]
                            onSelected: function(value) {
                                const next = Object.assign({}, root.settings);
                                next.mode = value;
                                HearthController.setDecoderSettings(next);
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Device"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                        AppComboBox {
                            Layout.preferredWidth: Math.round(259 * Theme.fontScale)
                            enabled: false
                            model: [qsTr("Home theatre")]
                            Accessible.name: qsTr("Device")
                        }
                        Item { Layout.fillWidth: true }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("AC-4 carries a compression curve for each kind of device; this picks "
                                  + "which one applies. AC-4 only.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }

                Card {
                    ordinal: "04"
                    title: qsTr("Stereo and mono")
                    framed: true

                    Text {
                        text: qsTr("SHARED WITH AC-3 AND E-AC-3 · LIVE")
                        color: Theme.accentInk
                        font.pixelSize: Theme.fontMicro
                        font.bold: true
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Downmix"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                        SegmentedControl {
                            accessibleName: qsTr("Downmix")
                            currentValue: root.settings.stereoFold ?? "loro"
                            model: [
                                { value: "loro", label: qsTr("Lo/Ro") },
                                { value: "ltrt", label: qsTr("Lt/Rt") }
                            ]
                            onSelected: function(value) {
                                const next = Object.assign({}, root.settings);
                                next.stereoFold = value;
                                HearthController.setDecoderSettings(next);
                            }
                        }
                        SegmentedControl {
                            enabled: false
                            accessibleName: qsTr("Pro Logic II")
                            currentValue: ""
                            model: [{ value: "plii", label: qsTr("Pro Logic II") }]
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Pro Logic II is an AC-4 downmix only, inactive until the decoder exists.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }
}

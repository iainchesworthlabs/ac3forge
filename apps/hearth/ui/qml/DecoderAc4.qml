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
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    readonly property var settings: HearthController.decoderSettings

    ColumnLayout {
        width: root.availableWidth
        spacing: Theme.gap * 2

        Rectangle {
            Layout.fillWidth: true
            color: Theme.accent100
            border.color: Theme.bad
            border.width: 1
            radius: Theme.radius
            implicitHeight: banner.implicitHeight + Theme.pad * 2

            ColumnLayout {
                id: banner
                anchors.fill: parent
                anchors.margins: Theme.pad
                spacing: Theme.gap / 2

                Text {
                    text: qsTr("NOT IN THIS BUILD")
                    color: Theme.bad
                    font.pixelSize: Theme.fontSmall
                    font.bold: true
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
                spacing: Theme.gap * 2

                Card {
                    title: qsTr("01 Presentation")
                    enabled: false

                    ComboBox {
                        Layout.fillWidth: true
                        model: [qsTr("1 · English · 5.1 · main")]
                        currentIndex: 0
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
                    title: qsTr("02 Dialogue")
                    enabled: false

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Enhancement"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                        Slider { Layout.fillWidth: true; from: 0; to: 12; value: 6 }
                        Text { text: qsTr("6 dB"); color: Theme.textMuted }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Raises dialogue against the rest of the mix where the stream carries "
                                  + "dialogue enhancement data. 0 to 12 dB.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        CheckBox { text: qsTr("Mix in audio description") }
                        Text {
                            Layout.fillWidth: true
                            Layout.leftMargin: 32
                            text: qsTr("When the presentation carries an associated programme.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Its level"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                        Slider { Layout.fillWidth: true; from: -12; to: 0; value: -6 }
                        Text { text: qsTr("-6 dB"); color: Theme.textMuted }
                    }
                }
            }

            ColumnLayout {
                Layout.preferredWidth: 1
                Layout.fillWidth: true
                spacing: Theme.gap * 2

                Card {
                    title: qsTr("03 Dynamic range")

                    Text {
                        text: qsTr("SHARED WITH AC-3 AND E-AC-3 · LIVE")
                        color: Theme.accentInk
                        font.pixelSize: Theme.fontMicro
                        font.bold: true
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Mode"); color: Theme.textMuted; Layout.preferredWidth: 90 }
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
                        Text { text: qsTr("Device"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                        ComboBox {
                            Layout.fillWidth: true
                            enabled: false
                            model: [qsTr("Home theatre")]
                        }
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
                    title: qsTr("04 Stereo and mono")

                    Text {
                        text: qsTr("SHARED WITH AC-3 AND E-AC-3 · LIVE")
                        color: Theme.accentInk
                        font.pixelSize: Theme.fontMicro
                        font.bold: true
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Downmix"); color: Theme.textMuted; Layout.preferredWidth: 90 }
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

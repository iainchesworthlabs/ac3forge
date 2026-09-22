import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The AC-3 and E-AC-3 decoder page (planning/hearth-design.md): every
// control DecoderSettings holds, read from and written straight back to
// HearthController.decoderSettings - a real, working decoder, not a
// settings-only form.
//
// Two controls the design shows have no field to bind yet, and are shown
// inactive with a short reason rather than silently dropped: RF ceiling
// (OutputConfig's, not DecoderSettings') and the JOC domain switch (a
// library-level choice this app does not carry a setting for). "This
// stream" and "Programme" need the current item's own media information,
// which this controller does not read yet - left for the Media page's own
// slice.
ScrollView {
    id: root
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    readonly property var settings: HearthController.decoderSettings
    function set(key, value) {
        const next = Object.assign({}, settings);
        next[key] = value;
        HearthController.setDecoderSettings(next);
    }

    ColumnLayout {
        width: root.availableWidth
        implicitWidth: root.availableWidth
        spacing: Theme.gap * 2

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gap * 2

            // --- left column ---------------------------------------------
            ColumnLayout {
                Layout.preferredWidth: 1
                Layout.fillWidth: true
                spacing: Theme.gap * 2

                Card {
                    title: qsTr("01 Dynamic range")

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
                            onSelected: function(value) { root.set("mode", value); }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Line: dialogue normalisation and the stream's dynamic range control in "
                                  + "full (§7.7.1). RF: heavy compression and overload protection as "
                                  + "well (§7.7.2). Custom: the settings below.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        enabled: root.settings.mode === "custom"
                        Text { text: qsTr("Cut"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                        Slider {
                            Layout.fillWidth: true
                            from: 0; to: 1
                            value: root.settings.drcCut ?? 1.0
                            onMoved: root.set("drcCut", value)
                        }
                        Text {
                            text: qsTr("%1%").arg(Math.round((root.settings.drcCut ?? 1.0) * 100))
                            color: Theme.textMuted
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        enabled: root.settings.mode === "custom"
                        Text { text: qsTr("Boost"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                        Slider {
                            Layout.fillWidth: true
                            from: 0; to: 1
                            value: root.settings.drcBoost ?? 1.0
                            onMoved: root.set("drcBoost", value)
                        }
                        Text {
                            text: qsTr("%1%").arg(Math.round((root.settings.drcBoost ?? 1.0) * 100))
                            color: Theme.textMuted
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        enabled: root.settings.mode === "custom"
                        CheckBox {
                            text: qsTr("Heavy compression")
                            checked: root.settings.heavyCompression ?? false
                            onToggled: root.set("heavyCompression", checked)
                        }
                        Text {
                            Layout.fillWidth: true
                            Layout.leftMargin: 32
                            text: qsTr("Uses the compr words where the stream carries them.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        enabled: root.settings.mode === "custom"
                        CheckBox {
                            text: qsTr("Dialogue normalisation")
                            checked: root.settings.normaliseDialogue ?? true
                            onToggled: root.set("normaliseDialogue", checked)
                        }
                        Text {
                            Layout.fillWidth: true
                            Layout.leftMargin: 32
                            text: qsTr("Brings dialogue to −31 dBFS and never raises it. Line and RF turn it on.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("RF ceiling"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                        TextField {
                            Layout.preferredWidth: 90
                            enabled: false
                            text: qsTr("0.0 dBFS")
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Not adjustable from this build yet; RF mode holds the fold under full scale regardless.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                Card {
                    title: qsTr("02 Stereo and mono")

                    Text {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: HearthController.speakerLabels.length === 2
                              ? qsTr("Used when the speaker layout is 2.0.")
                              : (HearthController.speakerLabels.length === 1
                                 ? qsTr("Used when the speaker layout is 1.0.")
                                 : qsTr("Not used: the current layout is rendered instead of folded."))
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
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
                            onSelected: function(value) { root.set("stereoFold", value); }
                        }
                    }
                    CheckBox {
                        text: qsTr("Phase-shift the surround sum")
                        checked: root.settings.ltrtPhaseShift ?? true
                        onToggled: root.set("ltrtPhaseShift", checked)
                    }
                    CheckBox {
                        text: qsTr("Mix the LFE in")
                        checked: root.settings.mixLfe ?? false
                        onToggled: root.set("mixLfe", checked)
                    }
                }
            }

            // --- right column --------------------------------------------
            ColumnLayout {
                Layout.preferredWidth: 1
                Layout.fillWidth: true
                spacing: Theme.gap * 2

                Card {
                    title: qsTr("03 Dual mono")

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("For a 1+1 stream: which of its two unrelated programmes plays, or both, one to each side.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                    SegmentedControl {
                        accessibleName: qsTr("Dual mono")
                        currentValue: root.settings.dualMono ?? "both"
                        model: [
                            { value: "first", label: qsTr("Channel 1") },
                            { value: "second", label: qsTr("Channel 2") },
                            { value: "both", label: qsTr("Both") }
                        ]
                        onSelected: function(value) { root.set("dualMono", value); }
                    }
                }

                Card {
                    title: qsTr("04 Objects")

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Reconstruct"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                        SegmentedControl {
                            accessibleName: qsTr("Reconstruct")
                            currentValue: root.settings.objects ?? "auto"
                            model: [
                                { value: "auto", label: qsTr("Auto") },
                                { value: "always", label: qsTr("Always") },
                                { value: "never", label: qsTr("Never") }
                            ]
                            onSelected: function(value) { root.set("objects", value); }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Auto reconstructs the object layer when the speaker layout has heights.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Domain"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                        SegmentedControl {
                            enabled: false
                            accessibleName: qsTr("Domain")
                            currentValue: "qmf"
                            model: [
                                { value: "qmf", label: qsTr("QMF") },
                                { value: "mdct", label: qsTr("MDCT band") }
                            ]
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Not adjustable from this build yet; objects reconstruct in the QMF domain, TS 103 420's own default.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }

                Card {
                    title: qsTr("05 Errors and transform")

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Bad frame"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                        SegmentedControl {
                            accessibleName: qsTr("Bad frame")
                            currentValue: root.settings.concealment ?? "repeatFade"
                            model: [
                                { value: "stop", label: qsTr("Stop") },
                                { value: "repeatFade", label: qsTr("Repeat and fade") },
                                { value: "mute", label: qsTr("Mute") }
                            ]
                            onSelected: function(value) { root.set("concealment", value); }
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        CheckBox {
                            text: qsTr("Fast inverse transform")
                            checked: root.settings.fastInverseTransform ?? true
                            onToggled: root.set("fastInverseTransform", checked)
                        }
                        Text {
                            Layout.fillWidth: true
                            Layout.leftMargin: 32
                            text: qsTr("The FFT form. Off uses the reference form, to compare the two.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
        }
    }
}

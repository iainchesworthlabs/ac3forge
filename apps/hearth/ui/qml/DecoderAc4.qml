import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The AC-4 decoder page (planning/hearth-design.md; planning/ac4.md, I2):
// every control DecoderSettings::ac4 holds, and the ones AC-4 shares with
// AC-3 and E-AC-3, read from and written straight back to
// HearthController.decoderSettings - the decoder the engine plays AC-4
// through takes each from its next frame.
//
// Dynamic range and the output level are AC-4's own, apart from the AC-3 and
// E-AC-3 page's operating mode (planning/ac4.md, decision 12). The stereo
// fold, the LFE in it and what a bad frame does are one control for both
// formats ("One control for both formats"), so they are the same settings
// here as on that page.
//
// The presentation table and "This stream"'s lines read the playing item's
// media information (HearthController.currentMedia.ac4), which the decoder
// reads off the file (media_info.hpp).
ScrollView {
    id: root

    // Same control column the AC-3/E-AC-3 tab uses.
    readonly property int labelWidth: Math.round(108 * Theme.fontScale)
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    readonly property var settings: HearthController.decoderSettings
    function set(key, value) {
        const next = Object.assign({}, settings);
        next[key] = value;
        HearthController.setDecoderSettings(next);
    }

    readonly property var ac4: HearthController.currentMedia.ac4 ?? ({})
    readonly property var presentations: root.ac4.presentations ?? []
    readonly property var metadata: root.ac4.metadata ?? ({})
    // The presentation the decoder is playing now, by its place, from the
    // last frame heard.
    readonly property int playingIndex: HearthController.thisFrame.ac4Presentation ?? -1

    // What each member of a presentation is (hearth_controller.cpp's
    // ac4_member_token()).
    function contentLabel(tokens) {
        const names = {
            "main": qsTr("main"),
            "musicAndEffects": qsTr("music and effects"),
            "dialogue": qsTr("dialogue"),
            "audioDescription": qsTr("audio description"),
            "hearingImpaired": qsTr("hearing impaired"),
            "commentary": qsTr("commentary"),
            "emergency": qsTr("emergency"),
            "voiceOver": qsTr("voice over"),
            "associated": qsTr("associated audio")
        };
        return (tokens ?? []).map(function(token) { return names[token] ?? token; }).join(" + ");
    }
    function presentationLabel(p) {
        let label = qsTr("%1 · %2 · %3 · %4").arg(p.index + 1)
                        .arg(p.name.length > 0 ? p.name : (p.language.length > 0 ? p.language : qsTr("no language")))
                        .arg(p.channels).arg(root.contentLabel(p.contents));
        if (!p.decodable) {
            label += qsTr(" (not decoded in this build)");
        } else if (!p.enabled) {
            label += qsTr(" (disabled)");
        }
        return label;
    }
    // The combo box's row for the choice in force: 0 for Automatic, else the
    // presentation's row, or the last row where the choice names none of this
    // stream's.
    function chosenRow() {
        const id = root.settings.ac4PresentationId ?? -1;
        const index = root.settings.ac4PresentationIndex ?? -1;
        if (id < 0 && index < 0) {
            return 0;
        }
        for (let i = 0; i < root.presentations.length; ++i) {
            const p = root.presentations[i];
            if ((id >= 0 && p.id === id) || (id < 0 && p.index === index)) {
                return i + 1;
            }
        }
        return root.presentations.length + 1;
    }
    function choose(row) {
        const next = Object.assign({}, root.settings);
        if (row <= 0 || row > root.presentations.length) {
            next.ac4PresentationId = -1;
            next.ac4PresentationIndex = -1;
        } else {
            const p = root.presentations[row - 1];
            next.ac4PresentationId = p.id !== undefined ? p.id : -1;
            next.ac4PresentationIndex = p.id !== undefined ? -1 : p.index;
        }
        HearthController.setDecoderSettings(next);
    }
    function signedDb(value) {
        const n = Number(value ?? 0);
        return (n > 0 ? "+" : (n < 0 ? "−" : "")) + Math.abs(n).toFixed(0);
    }
    function drcModeName(id) {
        switch (id) {
            case 0: return qsTr("home theatre");
            case 1: return qsTr("flat panel TV");
            case 2: return qsTr("portable speakers");
            case 3: return qsTr("portable headphones");
            default: return qsTr("mode %1").arg(id);
        }
    }

    readonly property var drcValues: ["auto", "homeTheatre", "flatPanelTv", "portableSpeakers",
                                      "portableHeadphones", "off"]

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
                Layout.alignment: Qt.AlignTop
                spacing: Theme.gap * 2

                Card {
                    ordinal: "01"
                    title: qsTr("Presentation")
                    framed: true

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Presentation"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                        AppComboBox {
                            id: presentationCombo
                            objectName: "ac4PresentationCombo"
                            Layout.fillWidth: true
                            Layout.maximumWidth: Math.round(319 * Theme.fontScale)
                            model: {
                                const rows = [qsTr("Automatic")];
                                for (let i = 0; i < root.presentations.length; ++i) {
                                    rows.push(root.presentationLabel(root.presentations[i]));
                                }
                                if (root.chosenRow() > root.presentations.length) {
                                    rows.push((root.settings.ac4PresentationId ?? -1) >= 0
                                              ? qsTr("presentation_id %1, not in this stream")
                                                    .arg(root.settings.ac4PresentationId)
                                              : qsTr("presentation %1, not in this stream")
                                                    .arg((root.settings.ac4PresentationIndex ?? 0) + 1));
                                }
                                return rows;
                            }
                            currentIndex: root.chosenRow()
                            Accessible.name: qsTr("Presentation")
                            onActivated: function(index) { root.choose(index); }
                            // Activating writes currentIndex directly, and the
                            // model is rebuilt with each settings change, so the
                            // choice is re-read whenever the list is closed.
                            Binding on currentIndex {
                                value: root.chosenRow()
                                when: !presentationCombo.popup.visible
                                restoreMode: Binding.RestoreBindingOrValue
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("From the stream's table of contents. With no choice made, the first "
                                  + "presentation in your language plays.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: root.presentations.length === 0
                        text: qsTr("Nothing AC-4 is playing.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }

                    // The design's table: one row a presentation, the one
                    // playing in bold.
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.presentations.length > 0
                        spacing: 2

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gap
                            Text { text: qsTr("#"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro
                                   font.capitalization: Font.AllUppercase; Layout.preferredWidth: 24 }
                            Text { text: qsTr("Language"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro
                                   font.capitalization: Font.AllUppercase; Layout.preferredWidth: 72 }
                            Text { text: qsTr("Channels"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro
                                   font.capitalization: Font.AllUppercase; Layout.preferredWidth: 64 }
                            Text { text: qsTr("Content"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro
                                   font.capitalization: Font.AllUppercase; Layout.fillWidth: true }
                            Text { text: qsTr("Groups"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro
                                   font.capitalization: Font.AllUppercase; Layout.preferredWidth: 56 }
                        }
                        Repeater {
                            model: root.presentations
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: Theme.gap
                                readonly property bool playing: modelData.index === root.playingIndex
                                Text { text: modelData.index + 1; color: Theme.text; font.bold: parent.playing
                                       Layout.preferredWidth: 24 }
                                Text { text: modelData.language.length > 0 ? modelData.language : "—"
                                       color: Theme.text; font.bold: parent.playing; font.family: Theme.monoFamily
                                       Layout.preferredWidth: 72; elide: Text.ElideRight }
                                Text { text: modelData.channels; color: Theme.text; font.bold: parent.playing
                                       Layout.preferredWidth: 64 }
                                Text { text: root.contentLabel(modelData.contents)
                                             + (modelData.name.length > 0 ? " · " + modelData.name : "")
                                       color: modelData.decodable ? Theme.text : Theme.textMuted
                                       font.bold: parent.playing; Layout.fillWidth: true; elide: Text.ElideRight }
                                Text { text: (modelData.groups ?? []).join(", ")
                                       color: Theme.text; font.bold: parent.playing; Layout.preferredWidth: 56 }
                            }
                        }
                    }
                }

                Card {
                    ordinal: "02"
                    title: qsTr("Dialogue")
                    framed: true

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Enhancement"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                        AppSlider {
                            id: enhancementSlider
                            Layout.preferredWidth: Math.round(200 * Theme.fontScale)
                            from: 0; to: 12; stepSize: 1; snapMode: Slider.SnapAlways
                            value: root.settings.ac4DialogueEnhancementDb ?? 0
                            Accessible.name: qsTr("Dialogue enhancement, dB")
                            onMoved: root.set("ac4DialogueEnhancementDb", value)
                            // A drag writes `value` directly and the binding
                            // above is gone, so it is resynced from the
                            // controller - TransportBar's scrubber's shape.
                            Connections {
                                target: HearthController
                                function onDecoderSettingsChanged() {
                                    enhancementSlider.value = root.settings.ac4DialogueEnhancementDb ?? 0;
                                }
                            }
                        }
                        Text { text: qsTr("%1 dB").arg(Number(root.settings.ac4DialogueEnhancementDb ?? 0).toFixed(0))
                               color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: Theme.fontNormal }
                        Item { Layout.fillWidth: true }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Raises dialogue against the rest of the mix where the stream carries "
                                  + "dialogue enhancement data, up to the stream's own limit. 0 to 12 dB.")
                              + (root.metadata.dialogueEnhancement !== undefined
                                 ? " " + qsTr("This stream allows up to %1 dB.")
                                             .arg(Number(root.metadata.dialogueEnhancement.maxGainDb).toFixed(0))
                                 : "")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Dialogue level"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                        AppSlider {
                            id: dialogueSlider
                            Layout.preferredWidth: Math.round(200 * Theme.fontScale)
                            from: -12; to: 12; stepSize: 1; snapMode: Slider.SnapAlways
                            value: root.settings.ac4DialogueDb ?? 0
                            Accessible.name: qsTr("Dialogue level, dB")
                            onMoved: root.set("ac4DialogueDb", value)
                            Connections {
                                target: HearthController
                                function onDecoderSettingsChanged() {
                                    dialogueSlider.value = root.settings.ac4DialogueDb ?? 0;
                                }
                            }
                        }
                        Text { text: qsTr("%1 dB").arg(root.signedDb(root.settings.ac4DialogueDb))
                               color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: Theme.fontNormal }
                        Item { Layout.fillWidth: true }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("The dialogue against the music and effects, where a presentation carries "
                                  + "them apart, up to the most the stream allows.")
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
                            note: qsTr("When the presentation carries an associated programme. A presentation "
                                      + "that carries one plays first.")
                            checked: root.settings.ac4AudioDescription ?? false
                            onToggled: function(on) { root.set("ac4AudioDescription", on); }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Its level"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                        AppSlider {
                            id: associatedSlider
                            Layout.preferredWidth: Math.round(200 * Theme.fontScale)
                            enabled: root.settings.ac4AudioDescription ?? false
                            from: -12; to: 0; stepSize: 1; snapMode: Slider.SnapAlways
                            value: root.settings.ac4AssociatedDb ?? 0
                            Accessible.name: qsTr("Audio description level, dB")
                            onMoved: root.set("ac4AssociatedDb", value)
                            Connections {
                                target: HearthController
                                function onDecoderSettingsChanged() {
                                    associatedSlider.value = root.settings.ac4AssociatedDb ?? 0;
                                }
                            }
                        }
                        Text { text: qsTr("%1 dB").arg(root.signedDb(root.settings.ac4AssociatedDb))
                               color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: Theme.fontNormal }
                        Item { Layout.fillWidth: true }
                    }
                }
            }

            // --- right column --------------------------------------------
            ColumnLayout {
                Layout.preferredWidth: 1
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: Theme.gap * 2

                Card {
                    ordinal: "03"
                    title: qsTr("Dynamic range")
                    framed: true
                    summary: qsTr("AC-4 only")

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Device"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                        AppComboBox {
                            id: deviceCombo
                            objectName: "ac4DeviceCombo"
                            Layout.preferredWidth: Math.round(259 * Theme.fontScale)
                            enabled: root.settings.ac4Normalise ?? true
                            model: [qsTr("Automatic"), qsTr("Home theatre"), qsTr("Flat panel TV"),
                                    qsTr("Portable speakers"), qsTr("Portable headphones"), qsTr("No compression")]
                            currentIndex: Math.max(0, root.drcValues.indexOf(root.settings.ac4Drc ?? "auto"))
                            Accessible.name: qsTr("Device")
                            onActivated: function(index) { root.set("ac4Drc", root.drcValues[index]); }
                            Binding on currentIndex {
                                value: Math.max(0, root.drcValues.indexOf(root.settings.ac4Drc ?? "auto"))
                                when: !deviceCombo.popup.visible
                                restoreMode: Binding.RestoreBindingOrValue
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("AC-4 carries a compression curve for each kind of device; this picks which "
                                  + "one applies. Automatic takes the one for the output level: home theatre "
                                  + "to −27 dBFS, flat panel TV to −17, portable above that.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Output level"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                        AppSlider {
                            id: levelSlider
                            Layout.preferredWidth: Math.round(200 * Theme.fontScale)
                            enabled: root.settings.ac4Normalise ?? true
                            from: -31; to: 0; stepSize: 1; snapMode: Slider.SnapAlways
                            value: root.settings.ac4OutputLevelDbfs ?? -31
                            Accessible.name: qsTr("Output level, dBFS")
                            onMoved: root.set("ac4OutputLevelDbfs", value)
                            Connections {
                                target: HearthController
                                function onDecoderSettingsChanged() {
                                    levelSlider.value = root.settings.ac4OutputLevelDbfs ?? -31;
                                }
                            }
                        }
                        Text { text: qsTr("%1 dBFS").arg(root.signedDb(root.settings.ac4OutputLevelDbfs ?? -31))
                               color: Theme.textMuted; font.family: Theme.monoFamily; font.pixelSize: Theme.fontNormal }
                        Item { Layout.fillWidth: true }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Item { Layout.preferredWidth: root.labelWidth }
                        AppCheckBox {
                            Layout.fillWidth: true
                            text: qsTr("Dialogue normalisation")
                            note: qsTr("Brings dialogue to the output level, cutting or boosting it. Off plays "
                                      + "the stream at its coded level, with no compression.")
                            checked: root.settings.ac4Normalise ?? true
                            onToggled: function(on) { root.set("ac4Normalise", on); }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: root.metadata.dialnormDbfs !== undefined || root.metadata.drcModes !== undefined
                        text: (root.metadata.dialnormDbfs !== undefined
                               ? qsTr("This stream's dialogue is at %1 dBFS.")
                                     .arg(Number(root.metadata.dialnormDbfs).toFixed(2).replace("-", "−"))
                               : "")
                              + (root.metadata.drcModes !== undefined
                                 ? " " + qsTr("It carries compression for: %1.")
                                             .arg(root.metadata.drcModes.map(function(m) {
                                                 return root.drcModeName(m.id);
                                             }).join(", "))
                                 : "")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }

                Card {
                    ordinal: "04"
                    title: qsTr("Stereo and mono")
                    framed: true
                    summary: qsTr("shared with AC-3 and E-AC-3")

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
                        Text { text: qsTr("Downmix"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
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
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Item { Layout.preferredWidth: root.labelWidth }
                        AppCheckBox {
                            Layout.fillWidth: true
                            text: qsTr("Follow the stream's preferred downmix")
                            note: qsTr("Where the stream names one, in place of the choice above. Lt/Rt takes its "
                                      + "Pro Logic II form where the stream prefers that. AC-4 only.")
                            checked: root.settings.ac4PreferredDownmix ?? false
                            onToggled: function(on) { root.set("ac4PreferredDownmix", on); }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Item { Layout.preferredWidth: root.labelWidth }
                        AppCheckBox {
                            Layout.fillWidth: true
                            text: qsTr("Mix the LFE in")
                            note: qsTr("At the stream's own LFE mix level, where it carries one. On for AC-4 "
                                      + "until set here or on the AC-3 and E-AC-3 tab.")
                            checked: root.settings.mixLfe ?? true
                            onToggled: function(on) { root.set("mixLfe", on); }
                        }
                    }
                }
            }
        }

        // Under both columns, full width: its three choices do not fit a
        // column beside their label at the window's narrowest.
        Card {
            Layout.fillWidth: true
            ordinal: "05"
            title: qsTr("Errors")
            framed: true
            summary: qsTr("shared with AC-3 and E-AC-3")

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                Text { text: qsTr("Bad frame"); color: Theme.text; font.pixelSize: Theme.fontNormal
                       elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
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
            Text {
                Layout.fillWidth: true
                text: qsTr("What plays in place of a frame that will not decode. AC-4 is back to what "
                          + "the stream carries at its next I-frame.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
        }
    }
}

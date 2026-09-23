import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The AC-3 and E-AC-3 decoder page (planning/hearth-design.md): every
// control DecoderSettings holds, read from and written straight back to
// HearthController.decoderSettings - a real, working decoder, not a
// settings-only form.
//
// Every control the design shows now has a field to bind to. "This stream"
// and "Programme" read HearthController.currentMedia - the playing item's
// own file, off a thread of its own (media_inspector.hpp) - the same source
// the Media page reads for any queue item, not just this one.
ScrollView {
    id: root

    // The control column the mockups measure: every row's control starts
    // 120 px after the card's content edge, which is this plus Theme.gap.
    // Scaled, so a label that grows keeps its column instead of pushing
    // through the control beside it.
    readonly property int labelWidth: Math.round(108 * Theme.fontScale)
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    readonly property var settings: HearthController.decoderSettings
    function set(key, value) {
        const next = Object.assign({}, settings);
        next[key] = value;
        HearthController.setDecoderSettings(next);
    }

    readonly property var media: HearthController.currentMedia
    readonly property var probe: root.media.probe ?? ({})
    readonly property var bitstream: root.media.bitstream ?? ({})
    function modeLabel() {
        const mode = root.settings.mode ?? "line";
        return mode === "rf" ? qsTr("RF") : (mode === "custom" ? qsTr("Custom") : qsTr("Line"));
    }

    // The playing file's own name, for "03 This stream"'s card header - the
    // path itself (root.media.path) is a full path, on whichever platform's
    // own separator.
    function fileNameOf(path) {
        if (!path) {
            return "";
        }
        const cut = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
        return cut >= 0 ? path.substring(cut + 1) : path;
    }

    // "Dialogue level"'s own row: dialnorm is a single dB value in most
    // streams, but not constant in every one (dialnormConstant, from
    // io::DialnormRange::constant()) - a stream whose dialnorm changes over
    // time gets a range rather than a single "so N dB down" reading.
    function dialogueLevelText() {
        if (root.probe.dialnormDb === undefined) {
            return qsTr("not carried");
        }
        if (root.probe.dialnormConstant === false) {
            const lo = Math.min(root.probe.dialnormDb, root.probe.dialnormMaxDb);
            const hi = Math.max(root.probe.dialnormDb, root.probe.dialnormMaxDb);
            return qsTr("dialnorm %1 to %2, so %3 to %4 dB down")
                       .arg(lo).arg(hi)
                       .arg(Number(31 + lo).toFixed(1)).arg(Number(31 + hi).toFixed(1));
        }
        return qsTr("dialnorm %1, so %2 dB down").arg(root.probe.dialnormDb)
                   .arg(Number(31 + root.probe.dialnormDb).toFixed(1));
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
                Layout.alignment: Qt.AlignTop
                spacing: Theme.gap * 2

                Card {
                    ordinal: "01"
                    title: qsTr("Dynamic range")
                    framed: true

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
                        // `enabled` belongs on the control, not the row: the
                        // design dims a disabled slider but keeps its label
                        // at full strength (measured on decoder-ac3-eac3.png
                        // with mode = Line).
                        Text { text: qsTr("Cut"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                        AppSlider {
                            id: cutSlider
                            Layout.preferredWidth: Math.round(200 * Theme.fontScale)
                            enabled: root.settings.mode === "custom"
                            from: 0; to: 1
                            value: root.settings.drcCut ?? 1.0
                            onMoved: root.set("drcCut", value)
                            // A drag writes `value` directly and the binding
                            // above is gone for good, so it is resynced from
                            // the controller whenever the settings change -
                            // the same shape TransportBar's scrubber uses.
                            Connections {
                                target: HearthController
                                function onDecoderSettingsChanged() {
                                    cutSlider.value = root.settings.drcCut ?? 1.0;
                                }
                            }
                        }
                        Text {
                            text: qsTr("%1%").arg(Math.round((root.settings.drcCut ?? 1.0) * 100))
                            color: Theme.textMuted
                            font.family: Theme.monoFamily
                            font.pixelSize: Theme.fontNormal
                            Layout.preferredWidth: Math.round(44 * Theme.fontScale)
                        }
                        Item { Layout.fillWidth: true }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        // `enabled` belongs on the control, not the row: the
                        // design dims a disabled slider but keeps its label
                        // at full strength (measured on decoder-ac3-eac3.png
                        // with mode = Line).
                        Text { text: qsTr("Boost"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                        AppSlider {
                            id: boostSlider
                            Layout.preferredWidth: Math.round(200 * Theme.fontScale)
                            enabled: root.settings.mode === "custom"
                            from: 0; to: 1
                            value: root.settings.drcBoost ?? 1.0
                            onMoved: root.set("drcBoost", value)
                            // A drag writes `value` directly and the binding
                            // above is gone for good, so it is resynced from
                            // the controller whenever the settings change -
                            // the same shape TransportBar's scrubber uses.
                            Connections {
                                target: HearthController
                                function onDecoderSettingsChanged() {
                                    boostSlider.value = root.settings.drcBoost ?? 1.0;
                                }
                            }
                        }
                        Text {
                            text: qsTr("%1%").arg(Math.round((root.settings.drcBoost ?? 1.0) * 100))
                            color: Theme.textMuted
                            font.family: Theme.monoFamily
                            font.pixelSize: Theme.fontNormal
                            Layout.preferredWidth: Math.round(44 * Theme.fontScale)
                        }
                        Item { Layout.fillWidth: true }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Item { Layout.preferredWidth: root.labelWidth }
                        AppCheckBox {
                            Layout.fillWidth: true
                            enabled: root.settings.mode === "custom"
                            text: qsTr("Heavy compression")
                            note: qsTr("Uses the compr words where the stream carries them.")
                            checked: root.settings.heavyCompression ?? false
                            onToggled: function(on) { root.set("heavyCompression", on); }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Item { Layout.preferredWidth: root.labelWidth }
                        AppCheckBox {
                            Layout.fillWidth: true
                            enabled: root.settings.mode === "custom"
                            text: qsTr("Dialogue normalisation")
                            note: qsTr("Brings dialogue to −31 dBFS and never raises it. Line and RF turn it on.")
                            checked: root.settings.normaliseDialogue ?? true
                            onToggled: function(on) { root.set("normaliseDialogue", on); }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        enabled: root.settings.mode === "rf"
                        Text { text: qsTr("RF ceiling"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                        AppTextField {
                            id: rfCeilingField
                            Layout.preferredWidth: Math.round(140 * Theme.fontScale)
                            enabled: root.settings.mode === "rf"
                            // The design writes the unit inside the box,
                            // right-aligned, rather than as a label after it.
                            unit: qsTr("dBFS")
                            // No lower bound: output.hpp's own comment on
                            // rf_ceiling treats more headroom than asked for
                            // as a valid choice, just a quieter one. The
                            // upper bound is real - full scale is as high as
                            // a ceiling means anything. StandardNotation
                            // because the default accepts "-1e3" as valid.
                            validator: DoubleValidator {
                                top: 0
                                decimals: 1
                                notation: DoubleValidator.StandardNotation
                            }
                            text: Number(root.settings.rfCeilingDb ?? 0).toFixed(1)
                            Accessible.name: qsTr("RF ceiling, dBFS")
                            onEditingFinished: {
                                const value = parseFloat(text);
                                if (!isNaN(value)) {
                                    root.set("rfCeilingDb", value);
                                }
                            }
                            // Typing writes `text` directly and the binding
                            // above is gone for good, so it is re-read
                            // whenever this is not the field being edited.
                            Binding on text {
                                value: Number(root.settings.rfCeilingDb ?? 0).toFixed(1)
                                when: !rfCeilingField.activeFocus
                                restoreMode: Binding.RestoreBindingOrValue
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("What RF mode holds the fold under. Full scale by default; no effect in "
                                      + "Line or Custom mode.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                Card {
                    ordinal: "02"
                    title: qsTr("Stereo and mono")
                    framed: true

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
                            text: qsTr("Phase-shift the surround sum")
                            note: qsTr("Lt/Rt only. Delays the output by 63 samples.")
                            checked: root.settings.ltrtPhaseShift ?? true
                            onToggled: function(on) { root.set("ltrtPhaseShift", on); }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Item { Layout.preferredWidth: root.labelWidth }
                        AppCheckBox {
                            Layout.fillWidth: true
                            text: qsTr("Mix the LFE in")
                            note: qsTr("At the stream's own LFE mix level where it carries one, "
                                      + "and +10 dB where it does not.")
                            checked: root.settings.mixLfe ?? false
                            onToggled: function(on) { root.set("mixLfe", on); }
                        }
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
                    title: qsTr("This stream")
                    framed: true
                    summary: root.fileNameOf(root.media.path)

                    Text {
                        Layout.fillWidth: true
                        visible: Object.keys(root.media).length === 0
                        text: qsTr("Nothing playing.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        visible: Object.keys(root.media).length > 0
                        columns: 2
                        columnSpacing: Theme.gap
                        rowSpacing: 4

                        Text { text: qsTr("Dialogue level"); color: Theme.textMuted; Layout.preferredWidth: 110 }
                        Text {
                            Layout.fillWidth: true
                            text: root.dialogueLevelText()
                            color: Theme.text
                            wrapMode: Text.WordWrap
                        }

                        Text { text: qsTr("Dynamic range"); color: Theme.textMuted }
                        Text {
                            Layout.fillWidth: true
                            text: root.probe.dynrngSeen
                                  ? qsTr("carried, applied in full in %1 mode").arg(root.modeLabel())
                                  : qsTr("not carried")
                            color: Theme.text
                            wrapMode: Text.WordWrap
                        }

                        Text { text: qsTr("Heavy compression"); color: Theme.textMuted }
                        Text {
                            Layout.fillWidth: true
                            text: root.probe.comprSeen
                                  ? (root.settings.mode === "rf"
                                     ? qsTr("carried, used in RF mode")
                                     : qsTr("carried, not used in %1 mode").arg(root.modeLabel()))
                                  : qsTr("not carried")
                            color: Theme.text
                            wrapMode: Text.WordWrap
                        }

                        Text { text: qsTr("Mix levels"); color: Theme.textMuted }
                        Text {
                            Layout.fillWidth: true
                            text: root.bitstream.mixLevels !== undefined
                                  ? qsTr("centre %1 dB · surround %2 dB%3")
                                        .arg(Number(root.bitstream.mixLevels.centreDb).toFixed(1))
                                        .arg(Number(root.bitstream.mixLevels.surroundDb).toFixed(1))
                                        .arg(root.bitstream.mixLevels.lfeDb !== undefined
                                             ? qsTr(" · LFE %1 dB").arg(Number(root.bitstream.mixLevels.lfeDb).toFixed(1))
                                             : "")
                                  : qsTr("not carried")
                            color: Theme.text
                            wrapMode: Text.WordWrap
                        }

                        Text { text: qsTr("Objects"); color: Theme.textMuted }
                        Text {
                            Layout.fillWidth: true
                            text: root.probe.objectCount === undefined
                                  ? qsTr("none")
                                  : (root.probe.joc && root.probe.objectCount === 0
                                     ? qsTr("%1 reconstructed from the %2, placed by position")
                                           .arg(root.probe.jocReconstructedCount ?? 0).arg(root.probe.bedLabel ?? "")
                                     : qsTr("%1 · %2").arg(root.probe.objectCount).arg(root.probe.bedLabel ?? ""))
                            color: Theme.text
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                Card {
                    ordinal: "04"
                    title: qsTr("Programme")
                    framed: true

                    Text {
                        Layout.fillWidth: true
                        visible: (root.media.programmes ?? []).length === 0
                        text: qsTr("Nothing playing.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: (root.media.programmes ?? []).length > 0
                        spacing: 2

                        // Read-only: Session::open()'s own comment says why -
                        // the first programme always plays; picking a
                        // different one is Session's own choice of units, not
                        // part of DecoderSettings, and has no setter here yet.
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gap
                            Text { text: qsTr("Programme"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                            AppComboBox {
                                Layout.preferredWidth: Math.round(319 * Theme.fontScale)
                                enabled: false
                                model: (root.media.programmes ?? []).map(function(p) {
                                    return qsTr("%1 · %2 · %3").arg(p.substreamId).arg(p.layoutLabel).arg(p.bsmodLabel);
                                })
                                currentIndex: 0
                                Accessible.name: qsTr("Programme")
                            }
                            Item { Layout.fillWidth: true }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: (root.media.programmes ?? []).length > 1
                                  ? qsTr("This stream carries %1 programmes. Not adjustable from this "
                                        + "build yet; the first one always plays.")
                                        .arg(root.media.programmes.length)
                                  : qsTr("This stream carries one programme.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        Layout.topMargin: Theme.gap / 2
                        text: qsTr("For a 1+1 stream: which of its two unrelated programmes plays, or both, one to each side.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Dual mono"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
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
                }

                Card {
                    ordinal: "05"
                    title: qsTr("Objects")
                    framed: true

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Reconstruct"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
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
                        Text { text: qsTr("Domain"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                        SegmentedControl {
                            accessibleName: qsTr("Domain")
                            currentValue: root.settings.jocDomain ?? "qmf"
                            model: [
                                { value: "qmf", label: qsTr("QMF") },
                                { value: "mdct", label: qsTr("MDCT band") }
                            ]
                            onSelected: function(value) { root.set("jocDomain", value); }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("QMF is the domain TS 103 420 specifies. MDCT band costs less, and its "
                                  + "objects lag the bed by 256 samples rather than 576.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }

                Card {
                    ordinal: "06"
                    title: qsTr("Errors and transform")
                    framed: true

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
                    // Flush with the card's content edge, unlike the boxes in
                    // 01 and 02: this one has no label column beside it in
                    // the mockup either.
                    AppCheckBox {
                        Layout.fillWidth: true
                        text: qsTr("Fast inverse transform")
                        note: qsTr("The FFT form. Off uses the reference form, to compare the two.")
                        checked: root.settings.fastInverseTransform ?? true
                        onToggled: function(on) { root.set("fastInverseTransform", on); }
                    }
                }
            }
        }
    }
}

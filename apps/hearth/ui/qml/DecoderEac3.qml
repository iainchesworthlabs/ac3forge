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
                        enabled: root.settings.mode === "rf"
                        Text { text: qsTr("RF ceiling"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                        TextField {
                            Layout.preferredWidth: 90
                            horizontalAlignment: Text.AlignRight
                            font.family: Theme.monoFamily
                            // No lower bound: output.hpp's own comment on
                            // rf_ceiling treats more headroom than asked for
                            // as a valid choice, just a quieter one. The
                            // upper bound is real - full scale is as high as
                            // a ceiling means anything.
                            validator: DoubleValidator { top: 0; decimals: 1 }
                            text: Number(root.settings.rfCeilingDb ?? 0).toFixed(1)
                            Accessible.name: qsTr("RF ceiling, dBFS")
                            onEditingFinished: {
                                const value = parseFloat(text);
                                if (!isNaN(value)) {
                                    root.set("rfCeilingDb", value);
                                }
                            }
                        }
                        Text { text: qsTr("dBFS"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall }
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
                    title: qsTr("03 This stream")
                    subtitle: root.fileNameOf(root.media.path)

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
                    title: qsTr("04 Programme")

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
                            Text { text: qsTr("Programme"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                            ComboBox {
                                Layout.fillWidth: true
                                enabled: false
                                model: (root.media.programmes ?? []).map(function(p) {
                                    return qsTr("%1 · %2 · %3").arg(p.substreamId).arg(p.layoutLabel).arg(p.bsmodLabel);
                                })
                                currentIndex: 0
                                Accessible.name: qsTr("Programme")
                            }
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
                        Text { text: qsTr("Dual mono"); color: Theme.textMuted; Layout.preferredWidth: 90 }
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
                    title: qsTr("05 Objects")

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
                    title: qsTr("06 Errors and transform")

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

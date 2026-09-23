import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The selected Hearth sink's own decoder settings (network-sink-decoder.png):
// the extension's DecoderSettings, the sink's own subset of DecoderEac3.qml's
// controls - no RF ceiling, dual mono, JOC domain or fast inverse transform,
// since _ac3forge_player@v1 carries none of those (ac3forge_player.hpp's own
// kDecoderSettingNames). Read from and written straight back to
// NetworkController.sinkDecoderSettings/setSinkDecoderSettings(), the same
// "whole map, apply what changed" shape DecoderEac3.qml already uses for
// this computer's own engine.
ScrollView {
    id: root
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    readonly property var settings: NetworkController.sinkDecoderSettings
    readonly property var acceptedKeys: root.settings.acceptedKeys ?? []
    function accepts(key) { return root.acceptedKeys.indexOf(key) >= 0; }
    function set(key, value) {
        const next = Object.assign({}, settings);
        next[key] = value;
        NetworkController.setSinkDecoderSettings(next);
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: Theme.gap * 2

        Card {
            title: qsTr("01 Dynamic range")

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                enabled: root.accepts("mode")
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
                          + "full. RF: heavy compression as well. Custom: the settings below.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                enabled: root.settings.mode === "custom" && root.accepts("drc_cut")
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
                enabled: root.settings.mode === "custom" && root.accepts("drc_boost")
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
                enabled: root.settings.mode === "custom" && root.accepts("heavy_compression")
                CheckBox {
                    text: qsTr("Heavy compression")
                    checked: root.settings.heavyCompression ?? false
                    onToggled: root.set("heavyCompression", checked)
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                enabled: root.settings.mode === "custom" && root.accepts("dialnorm")
                CheckBox {
                    text: qsTr("Dialogue normalisation")
                    checked: root.settings.normaliseDialogue ?? true
                    onToggled: root.set("normaliseDialogue", checked)
                }
            }
        }

        Card {
            title: qsTr("02 Stereo")

            Text {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: (NetworkController.sinkSpeakerSettings.labels ?? []).length === 2
                      ? qsTr("Used when this sink's layout is 2.0.")
                      : qsTr("Used when this sink's layout is 2.0. Its layout, %1, is rendered instead.")
                            .arg(NetworkController.sinkSpeakerSettings.layoutText ?? "")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                enabled: root.accepts("downmix")
                Text { text: qsTr("Downmix"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                SegmentedControl {
                    accessibleName: qsTr("Downmix")
                    currentValue: root.settings.downmix ?? "loro"
                    model: [
                        { value: "loro", label: qsTr("Lo/Ro") },
                        { value: "ltrt", label: qsTr("Lt/Rt") }
                    ]
                    onSelected: function(value) { root.set("downmix", value); }
                }
            }
            CheckBox {
                enabled: root.accepts("mix_lfe")
                text: qsTr("Mix the LFE in")
                checked: root.settings.mixLfe ?? false
                onToggled: root.set("mixLfe", checked)
            }
        }

        Card {
            title: qsTr("03 Objects and errors")

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                enabled: root.accepts("objects")
                Text { text: qsTr("Objects"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                SegmentedControl {
                    accessibleName: qsTr("Objects")
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
                text: qsTr("Auto places the objects when the layout has heights. %1 has none, so this "
                          + "sink plays the stream's downmix.")
                          .arg(NetworkController.sinkSpeakerSettings.layoutText ?? "")
                visible: NetworkController.sinkSpeakerSettings.hasHeight !== true
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                enabled: root.accepts("concealment")
                Text { text: qsTr("Bad frame"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                SegmentedControl {
                    accessibleName: qsTr("Bad frame")
                    currentValue: root.settings.concealment ?? "repeatFade"
                    model: [
                        { value: "none", label: qsTr("None") },
                        { value: "repeatFade", label: qsTr("Repeat and fade") },
                        { value: "mute", label: qsTr("Mute") }
                    ]
                    onSelected: function(value) { root.set("concealment", value); }
                }
            }
        }
    }
}

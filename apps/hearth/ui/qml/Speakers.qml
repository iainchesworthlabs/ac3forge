import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The Speakers page (planning/hearth-design.md, "Speaker setup"): routing,
// trim, delay, the bass-management crossover, and the speaker layout itself
// - the picker, the "As text" field, the Heights control and each speaker's
// own Size toggle (HearthController.setLayoutText()/setHeights()/
// setSpeakerSmall(), backed by the engine's own live Player::set_layout()) -
// all real and posted straight to the engine through HearthController. Two
// things the design shows are not built here: the plan diagram and the
// identify tone (render::IdentifyTone is not wired into the engine yet).
// Both are named rather than left silently missing.
ScrollView {
    id: root
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    readonly property var labels: HearthController.speakerLabels
    readonly property var smallFlags: HearthController.speakerSmall
    readonly property var isLfeFlags: HearthController.speakerIsLfe
    readonly property var routingList: HearthController.routing
    readonly property int outputs: HearthController.routingOutputs
    // The layout picker's own preset names - a plain "5.1.2" etc. is what
    // the picker's own SegmentedControl offers, and also what its currently-
    // selected segment is computed against (falling back to "List" once a
    // Heights or Size change has moved layoutText off any of these, exactly
    // as the mockup's own example does for a 5.1.2 room with small fronts).
    readonly property var layoutPresets: ["2.0", "5.1", "7.1", "5.1.2", "5.1.4", "7.1.4"]

    // speakerSmall() no longer decorates the name here - the Levels table's
    // own SIZE column shows it as a control now, not a suffix - but this
    // stays for anything else on the page that still wants a speaker
    // described by name and size together (the identify status line does).
    function speakerName(index) {
        const small = root.smallFlags[index] === true;
        return small ? qsTr("%1 (small)").arg(root.labels[index]) : root.labels[index];
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: Theme.gap * 2

        Text {
            Layout.fillWidth: true
            text: HearthController.deviceName.length > 0
                  ? qsTr("Setup for %1 · %2 outputs").arg(HearthController.deviceName).arg(root.outputs)
                  : qsTr("No output device chosen yet")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
        }

        Card {
            title: qsTr("01 Speaker layout")

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                Text { text: qsTr("Layout"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                SegmentedControl {
                    objectName: "speakersLayoutPreset"
                    accessibleName: qsTr("Speaker layout")
                    currentValue: root.layoutPresets.includes(HearthController.layoutText)
                                  ? HearthController.layoutText : "list"
                    model: [
                        { value: "2.0", label: qsTr("2.0") },
                        { value: "5.1", label: qsTr("5.1") },
                        { value: "7.1", label: qsTr("7.1") },
                        { value: "5.1.2", label: qsTr("5.1.2") },
                        { value: "5.1.4", label: qsTr("5.1.4") },
                        { value: "7.1.4", label: qsTr("7.1.4") },
                        { value: "list", label: qsTr("List") }
                    ]
                    // "List" has no preset of its own to switch to - it is
                    // only ever the picker's OWN read-out of a layout that
                    // does not match any of the other six (a custom list, or
                    // one with a small speaker or a re-tiered height, which
                    // the named form cannot express); use "As text" for that.
                    onSelected: function(value) {
                        if (value !== "list") {
                            HearthController.setLayoutText(value);
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                Text { text: qsTr("As text"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                TextField {
                    objectName: "speakersLayoutText"
                    Layout.fillWidth: true
                    font.family: Theme.monoFamily
                    text: HearthController.layoutText
                    Accessible.name: qsTr("Layout, as text")
                    // Unparseable text is simply dropped, the same as an
                    // out-of-range trim or delay elsewhere on this page -
                    // setLayoutText() parses before it ever reaches the
                    // engine, so there is no round trip to fail against, and
                    // the field falls back to showing the layout still in
                    // effect.
                    onEditingFinished: HearthController.setLayoutText(text)
                }
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("A name such as 7.1.4, or one token per output: a location, an angle such as "
                          + "30/0, or a dash for an output with no speaker. A speaker marked small sends "
                          + "its bass to the LFE output below the crossover instead of reproducing it; "
                          + "the plan diagram this design shows is a later slice.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                Text { text: qsTr("Heights"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                SegmentedControl {
                    objectName: "speakersHeights"
                    accessibleName: qsTr("Height speaker realization")
                    enabled: HearthController.layoutHasHeight
                    currentValue: HearthController.heightsRealization
                    model: [
                        { value: "wall", label: qsTr("On the wall") },
                        { value: "ceiling", label: qsTr("In the ceiling") },
                        { value: "upfiring", label: qsTr("Up-firing") }
                    ]
                    onSelected: function(value) { HearthController.setHeights(value); }
                }
            }
        }

        Card {
            title: qsTr("02 Routing · speaker to device output")

            Column {
                id: grid
                spacing: 1

                readonly property int cellSize: 32
                readonly property int labelWidth: 72
                readonly property int noneWidth: 56

                Row {
                    spacing: 1
                    Item { width: grid.labelWidth; height: grid.cellSize }
                    Repeater {
                        model: root.outputs
                        delegate: Text {
                            required property int index
                            width: grid.cellSize
                            height: grid.cellSize
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            text: String(index + 1)
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                        }
                    }
                    Text {
                        width: grid.noneWidth
                        height: grid.cellSize
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("NONE")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMicro
                    }
                }

                Repeater {
                    model: root.labels

                    delegate: Row {
                        id: rowItem
                        required property int index
                        required property string modelData
                        spacing: 1

                        Text {
                            width: grid.labelWidth
                            height: grid.cellSize
                            verticalAlignment: Text.AlignVCenter
                            text: rowItem.modelData
                            color: Theme.text
                            font.bold: true
                        }

                        Repeater {
                            model: root.outputs

                            delegate: Rectangle {
                                id: cell
                                required property int index
                                readonly property bool assigned: root.routingList[rowItem.index] === cell.index

                                width: grid.cellSize
                                height: grid.cellSize
                                color: assigned ? Theme.accent : "transparent"
                                border.color: Theme.divider
                                border.width: 1

                                Accessible.role: Accessible.RadioButton
                                Accessible.name: qsTr("%1 to output %2").arg(rowItem.modelData).arg(cell.index + 1)
                                Accessible.checkable: true
                                Accessible.checked: cell.assigned
                                Accessible.onPressAction:
                                    HearthController.setRoutingAssignment(rowItem.index, cell.index)

                                activeFocusOnTab: true
                                Keys.onSpacePressed: HearthController.setRoutingAssignment(rowItem.index, cell.index)
                                Keys.onReturnPressed: HearthController.setRoutingAssignment(rowItem.index, cell.index)

                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: -Theme.focusRingOffset
                                    visible: cell.activeFocus
                                    color: "transparent"
                                    border.color: Theme.focusRing
                                    border.width: Theme.focusRingWidth
                                    z: 100
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: HearthController.setRoutingAssignment(rowItem.index, cell.index)
                                }
                            }
                        }

                        Rectangle {
                            id: noneCell
                            readonly property bool assigned: (root.routingList[rowItem.index] ?? -1) < 0

                            width: grid.noneWidth
                            height: grid.cellSize
                            color: assigned ? Theme.accent : "transparent"
                            border.color: Theme.divider
                            border.width: 1

                            Accessible.role: Accessible.RadioButton
                            Accessible.name: qsTr("%1 to no output").arg(rowItem.modelData)
                            Accessible.checkable: true
                            Accessible.checked: noneCell.assigned
                            Accessible.onPressAction: HearthController.setRoutingAssignment(rowItem.index, -1)

                            activeFocusOnTab: true
                            Keys.onSpacePressed: HearthController.setRoutingAssignment(rowItem.index, -1)
                            Keys.onReturnPressed: HearthController.setRoutingAssignment(rowItem.index, -1)

                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -Theme.focusRingOffset
                                visible: noneCell.activeFocus
                                color: "transparent"
                                border.color: Theme.focusRing
                                border.width: Theme.focusRingWidth
                                z: 100
                            }

                            MouseArea {
                                anchors.fill: parent
                                onClicked: HearthController.setRoutingAssignment(rowItem.index, -1)
                            }
                        }
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("Every speaker reaches an output, and no output carries two speakers. A speaker "
                          + "sent to no output is not heard, and an output no speaker reaches stays silent. "
                          + "Tab reaches every cell; Space or Enter sends the speaker there.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }

            RowLayout {
                spacing: Theme.gap

                Button {
                    objectName: "speakersUseDeviceOrder"
                    text: qsTr("Use the device's order")
                    onClicked: HearthController.useDeviceOrder()
                }
                Button {
                    objectName: "speakersClearRouting"
                    text: qsTr("Clear")
                    onClicked: HearthController.clearRouting()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gap * 2

            Card {
                title: qsTr("03 Levels and delays")
                Layout.preferredWidth: 1
                Layout.fillWidth: true
                // Not Layout.fillHeight: true - a Rectangle (Card is one)
                // defaults that to false, and RowLayout centres a
                // non-filling child vertically rather than sitting it at
                // the top, so this card would float below its own title
                // once its row's other column (Bass management + Identify)
                // makes the row taller than this card's own content.
                Layout.alignment: Qt.AlignTop

                Column {
                    id: levels
                    Layout.fillWidth: true
                    spacing: Theme.gap / 2

                    readonly property int labelWidth: 96
                    readonly property int sizeWidth: 120
                    readonly property int fieldWidth: 64
                    readonly property int unitWidth: 24

                    Row {
                        spacing: Theme.gap / 2
                        Text {
                            width: levels.labelWidth
                            text: qsTr("SPEAKER"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro
                        }
                        Text {
                            width: levels.sizeWidth
                            text: qsTr("SIZE"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro
                        }
                        Text {
                            width: levels.fieldWidth
                            horizontalAlignment: Text.AlignRight
                            text: qsTr("TRIM"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro
                        }
                        Item { width: levels.unitWidth; height: 1 }
                        Text {
                            width: levels.fieldWidth
                            horizontalAlignment: Text.AlignRight
                            text: qsTr("DELAY"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro
                        }
                        Item { width: levels.unitWidth; height: 1 }
                    }

                    Repeater {
                        model: root.labels

                        delegate: Row {
                            id: row
                            required property int index
                            required property string modelData
                            spacing: Theme.gap / 2

                            Text {
                                width: levels.labelWidth
                                height: trimField.implicitHeight
                                verticalAlignment: Text.AlignVCenter
                                text: root.labels[row.index]
                                color: Theme.text
                            }

                            // A fixed-width cell so TRIM/DELAY still line up
                            // down the table regardless of which of the two
                            // children below is showing. Not a Row child
                            // itself with its own anchors - a positioner sets
                            // its children's x/y itself, so an anchor on a
                            // direct child of `row` would fight it; anchoring
                            // within this plain Item instead is safe.
                            Item {
                                width: levels.sizeWidth
                                height: trimField.implicitHeight

                                // LFE has no size to set - render::Speaker::
                                // small only means anything on a full-
                                // bandwidth speaker (layout.hpp's own header
                                // comment).
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    visible: root.isLfeFlags[row.index] === true
                                    text: "—"
                                    color: Theme.textMuted
                                }
                                SegmentedControl {
                                    objectName: "speakersSize-" + row.index
                                    anchors.verticalCenter: parent.verticalCenter
                                    visible: root.isLfeFlags[row.index] !== true
                                    // Nowhere for a small speaker's bass to go
                                    // without an LFE feed - OutputLayout::
                                    // with_small() itself would refuse
                                    // turning this on then.
                                    enabled: HearthController.layoutHasLfe
                                    segHeight: trimField.implicitHeight
                                    accessibleName: qsTr("Size for %1").arg(row.modelData)
                                    currentValue: root.smallFlags[row.index] === true ? "small" : "large"
                                    model: [
                                        { value: "large", label: qsTr("Large") },
                                        { value: "small", label: qsTr("Small") }
                                    ]
                                    onSelected: function(value) {
                                        HearthController.setSpeakerSmall(row.index, value === "small");
                                    }
                                }
                            }

                            TextField {
                                id: trimField
                                objectName: "speakersTrim-" + row.index
                                width: levels.fieldWidth
                                horizontalAlignment: Text.AlignRight
                                font.family: Theme.monoFamily
                                validator: DoubleValidator { bottom: -24; top: 12; decimals: 1 }
                                text: Number(HearthController.trimDb[row.index] ?? 0).toFixed(1)
                                Accessible.name: qsTr("Trim for %1, dB").arg(row.modelData)
                                onEditingFinished: {
                                    const value = parseFloat(text);
                                    if (!isNaN(value)) {
                                        HearthController.setTrimDb(row.index, value);
                                    }
                                }
                            }
                            Text {
                                width: levels.unitWidth
                                height: trimField.implicitHeight
                                verticalAlignment: Text.AlignVCenter
                                text: qsTr("dB"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro
                            }

                            TextField {
                                objectName: "speakersDelay-" + row.index
                                width: levels.fieldWidth
                                horizontalAlignment: Text.AlignRight
                                font.family: Theme.monoFamily
                                validator: DoubleValidator { bottom: 0; top: 40; decimals: 1 }
                                text: Number(HearthController.delayMs[row.index] ?? 0).toFixed(1)
                                Accessible.name: qsTr("Delay for %1, milliseconds").arg(row.modelData)
                                onEditingFinished: {
                                    const value = parseFloat(text);
                                    if (!isNaN(value)) {
                                        HearthController.setDelayMs(row.index, value);
                                    }
                                }
                            }
                            Text {
                                width: levels.unitWidth
                                height: trimField.implicitHeight
                                verticalAlignment: Text.AlignVCenter
                                text: qsTr("ms"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro
                            }
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.preferredWidth: 1
                Layout.fillWidth: true
                spacing: Theme.gap * 2

                Card {
                    title: qsTr("04 Bass management")

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Crossover"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                        SegmentedControl {
                            accessibleName: qsTr("Crossover preset")
                            currentValue: [80, 100, 120].includes(HearthController.crossoverHz)
                                          ? String(HearthController.crossoverHz) : ""
                            model: [
                                { value: "80", label: qsTr("80 Hz") },
                                { value: "100", label: qsTr("100 Hz") },
                                { value: "120", label: qsTr("120 Hz") }
                            ]
                            onSelected: function(value) { HearthController.setCrossoverHz(Number(value)); }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Exact"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                        TextField {
                            objectName: "speakersCrossoverExact"
                            Layout.preferredWidth: 64
                            horizontalAlignment: Text.AlignRight
                            font.family: Theme.monoFamily
                            validator: DoubleValidator { bottom: 40; top: 250; decimals: 0 }
                            text: Number(HearthController.crossoverHz).toFixed(0)
                            Accessible.name: qsTr("Crossover, Hz")
                            onEditingFinished: {
                                const value = parseFloat(text);
                                if (!isNaN(value)) {
                                    HearthController.setCrossoverHz(value);
                                }
                            }
                        }
                        Text { text: qsTr("Hz"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Any value from 40 to 250 Hz. Only speakers marked small above are "
                                  + "affected; with none, this has nothing to cross over.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }

                Card {
                    title: qsTr("05 Identify")
                    enabled: false

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Not wired in this build yet: playing a tone from one output at a time, "
                                  + "to hear where each speaker actually is, needs render::IdentifyTone, "
                                  + "which this engine does not run.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }
}

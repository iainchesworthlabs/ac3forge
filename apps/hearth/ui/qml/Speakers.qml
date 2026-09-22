import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The Speakers page (planning/hearth-design.md, "Speaker setup"): routing,
// trim, delay, the bass-management crossover and the identify tone, all
// real and posted straight to the engine through HearthController. One
// thing the design shows is not built here: the layout picker and plan
// diagram (the layout is fixed for this slice - HearthController.
// speakerLabels' own comment says why), named rather than left silently
// missing.
ScrollView {
    id: root
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    readonly property var labels: HearthController.speakerLabels
    readonly property var smallFlags: HearthController.speakerSmall
    readonly property var routingList: HearthController.routing
    readonly property int outputs: HearthController.routingOutputs

    function speakerName(index) {
        const small = root.smallFlags[index] === true;
        return small ? qsTr("%1 (small)").arg(root.labels[index]) : root.labels[index];
    }

    function identifyStatusText() {
        const slot = HearthController.identifySlot;
        if (slot < 0) {
            return qsTr("Nothing playing.");
        }
        const name = root.speakerName(slot);
        const output = HearthController.routing[slot] ?? -1;
        return output >= 0
            ? qsTr("Noise on %1, output %2. Escape stops it.").arg(name).arg(output + 1)
            : qsTr("Noise on %1, which reaches no output, so nothing will be heard. Escape stops it.")
                  .arg(name);
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

            Text {
                Layout.fillWidth: true
                text: root.labels.length > 0 ? root.labels.join(qsTr(", ")) : qsTr("Unknown")
                color: Theme.text
                font.family: Theme.monoFamily
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Fixed for this build; choosing a different layout, and the plan diagram that "
                          + "goes with it, are a later slice. A speaker marked small sends its bass to the "
                          + "LFE output below the crossover instead of reproducing it.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
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
                    readonly property int fieldWidth: 64
                    readonly property int unitWidth: 24

                    Row {
                        spacing: Theme.gap / 2
                        Text {
                            width: levels.labelWidth
                            text: qsTr("SPEAKER"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro
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
                        Item { width: Theme.gap; height: 1 }
                        Text {
                            text: qsTr("IDENTIFY"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro
                        }
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
                                text: root.speakerName(row.index)
                                color: Theme.text
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

                            Item { width: Theme.gap; height: 1 }

                            // While this row's slot is the one sounding the
                            // tone: "noise" beside a "Stop" button, as the
                            // design shows it - see docs/hearth/design/
                            // screenshots/speakers-setup.png's "Ls" row.
                            // Otherwise, an "Identify" button alone.
                            Text {
                                visible: HearthController.identifySlot === row.index
                                height: identifyButton.implicitHeight
                                verticalAlignment: Text.AlignVCenter
                                text: qsTr("noise")
                                color: Theme.textMuted
                                font.family: Theme.monoFamily
                                font.pixelSize: Theme.fontMicro
                            }
                            Button {
                                id: identifyButton
                                objectName: "speakersIdentify-" + row.index
                                text: HearthController.identifySlot === row.index
                                      ? qsTr("Stop") : qsTr("Identify")
                                onClicked: HearthController.identifySlot === row.index
                                           ? HearthController.stopIdentify()
                                           : HearthController.startIdentify(row.index)
                                Accessible.name: HearthController.identifySlot === row.index
                                                 ? qsTr("Stop the identify tone on %1").arg(row.modelData)
                                                 : qsTr("Identify %1, pink noise").arg(row.modelData)
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

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Pink noise on one output at a time, to hear where it comes out. "
                                  + "An LFE output gets 30 to 80 Hz.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Level"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                        SegmentedControl {
                            accessibleName: qsTr("Identify level")
                            currentValue: [-30, -20, -12].includes(HearthController.identifyLevelDb)
                                          ? String(HearthController.identifyLevelDb) : ""
                            model: [
                                { value: "-30", label: qsTr("-30 dB") },
                                { value: "-20", label: qsTr("-20 dB") },
                                { value: "-12", label: qsTr("-12 dB") }
                            ]
                            onSelected: function(value) { HearthController.setIdentifyLevelDb(Number(value)); }
                        }
                    }

                    Text {
                        objectName: "speakersIdentifyStatus"
                        Layout.fillWidth: true
                        text: root.identifyStatusText()
                        color: HearthController.identifySlot >= 0 ? Theme.bad : Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }
}

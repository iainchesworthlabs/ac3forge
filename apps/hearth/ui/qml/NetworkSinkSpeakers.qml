import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The selected Hearth sink's own speaker layout, routing and levels
// (network-sink-speakers.png) - Speakers.qml's own controls, read from and
// written to NetworkController.sinkSpeakerSettings/setSink*() instead of
// HearthController's, over the network rather than this computer's own
// engine. Two real differences from the local page, both because the wire
// (ac3forge_player.hpp) says so, not by choice here:
//   * trim/delay are indexed by SINK OUTPUT, not by render slot - the
//     "OUT" column below is what ties a speaker's row to which trimDb/
//     delayMs entry it reads (setSinkTrimDb()'s own comment,
//     network_controller.hpp).
//   * no Heights control or exact-Hz crossover field: the mockup does not
//     show either for a sink (fewer layout/crossover presets too - 2.0/
//     5.1/7.1/5.1.2 and 60/80/100/120 Hz, not the local page's six and
//     three) - matched here rather than copied from Speakers.qml's own set.
ScrollView {
    id: root
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    readonly property var speakers: NetworkController.sinkSpeakerSettings
    readonly property var labels: root.speakers.labels ?? []
    readonly property var smallFlags: root.speakers.small ?? []
    readonly property var isLfeFlags: root.speakers.isLfe ?? []
    readonly property var routingList: root.speakers.routing ?? []
    readonly property int outputs: root.speakers.outputs ?? 0
    readonly property var layoutPresets: ["2.0", "5.1", "7.1", "5.1.2"]

    function outputOf(slot) {
        return slot < root.routingList.length ? root.routingList[slot] : -1;
    }

    // "✓ Every speaker reaches a slot. Slots 7 and 8 are silent." /
    // "✓ Every speaker reaches a slot, and every slot has one." / a plain
    // (no ✓) warning when a speaker itself reaches no slot at all.
    function slotsStatusText() {
        let unassignedSpeakers = 0;
        const patched = {};
        for (const output of root.routingList) {
            if (output < 0) { unassignedSpeakers++; } else { patched[output] = true; }
        }
        if (unassignedSpeakers > 0) {
            return qsTr("%1 speaker%2 reaches no slot and will not be heard.")
                       .arg(unassignedSpeakers).arg(unassignedSpeakers === 1 ? "" : "s");
        }
        const silent = [];
        for (let o = 0; o < root.outputs; o++) {
            if (!patched[o]) { silent.push(o + 1); }
        }
        if (silent.length === 0) {
            return qsTr("✓ Every speaker reaches a slot, and every slot has one.");
        }
        const list = silent.length === 1
                     ? String(silent[0])
                     : silent.slice(0, -1).join(", ") + qsTr(" and ") + silent[silent.length - 1];
        return qsTr("✓ Every speaker reaches a slot. Slot%1 %2 %3 silent.")
                   .arg(silent.length === 1 ? "" : "s").arg(list).arg(silent.length === 1 ? qsTr("is") : qsTr("are"));
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: Theme.gap * 2

        Card {
            title: qsTr("01 Speaker layout")

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                Text { text: qsTr("Layout"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                SegmentedControl {
                    objectName: "networkSinkLayoutPreset"
                    accessibleName: qsTr("Speaker layout")
                    currentValue: root.layoutPresets.includes(root.speakers.layoutText)
                                  ? root.speakers.layoutText : "list"
                    model: [
                        { value: "2.0", label: qsTr("2.0") },
                        { value: "5.1", label: qsTr("5.1") },
                        { value: "7.1", label: qsTr("7.1") },
                        { value: "5.1.2", label: qsTr("5.1.2") },
                        { value: "list", label: qsTr("List") }
                    ]
                    onSelected: function(value) {
                        if (value !== "list") { NetworkController.setSinkLayoutText(value); }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                Text { text: qsTr(""); Layout.preferredWidth: 90 }
                TextField {
                    objectName: "networkSinkLayoutText"
                    Layout.fillWidth: true
                    font.family: Theme.monoFamily
                    text: root.speakers.layoutText ?? ""
                    Accessible.name: qsTr("Layout, as text")
                    onEditingFinished: NetworkController.setSinkLayoutText(text)
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                Text { text: qsTr("Crossover"); color: Theme.textMuted; Layout.preferredWidth: 90 }
                SegmentedControl {
                    objectName: "networkSinkCrossover"
                    accessibleName: qsTr("Crossover")
                    currentValue: [60, 80, 100, 120].includes(root.speakers.crossoverHz)
                                  ? String(root.speakers.crossoverHz) : ""
                    model: [
                        { value: "60", label: qsTr("60 Hz") },
                        { value: "80", label: qsTr("80 Hz") },
                        { value: "100", label: qsTr("100 Hz") },
                        { value: "120", label: qsTr("120 Hz") }
                    ]
                    onSelected: function(value) { NetworkController.setSinkCrossoverHz(Number(value)); }
                }
            }
            Text {
                Layout.fillWidth: true
                text: root.smallFlags.includes(true)
                      ? qsTr("Speakers marked small below send their bass here instead.")
                      : qsTr("No small speakers, so nothing is crossed over.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
        }

        Card {
            title: qsTr("02 Routing · speaker to sink slot")

            Column {
                id: grid
                spacing: 1
                activeFocusOnTab: true
                onActiveFocusChanged: {
                    if (grid.activeFocus) {
                        const target = grid.cellAt(0, 0);
                        if (target) { target.forceActiveFocus(); }
                    }
                }

                readonly property int cellSize: 32
                readonly property int labelWidth: 72
                readonly property int noneWidth: 56

                function cellAt(row, column) {
                    const rowCount = root.labels.length;
                    if (rowCount === 0) { return null; }
                    row = Math.max(0, Math.min(rowCount - 1, row));
                    const rowItem = rowsRepeater.itemAt(row);
                    if (!rowItem) { return null; }
                    column = Math.max(0, Math.min(root.outputs, column));
                    return column < root.outputs ? rowItem.outputsRepeater.itemAt(column) : rowItem.noneOutputCell;
                }

                function moveFocus(row, column, event) {
                    let dRow = 0, dColumn = 0;
                    switch (event.key) {
                    case Qt.Key_Left: dColumn = -1; break;
                    case Qt.Key_Right: dColumn = 1; break;
                    case Qt.Key_Up: dRow = -1; break;
                    case Qt.Key_Down: dRow = 1; break;
                    default: return;
                    }
                    const target = grid.cellAt(row + dRow, column + dColumn);
                    if (target) { target.forceActiveFocus(); }
                    event.accepted = true;
                }

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
                            font.pixelSize: Theme.fontMicro
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
                    id: rowsRepeater
                    model: root.labels

                    delegate: Row {
                        id: rowItem
                        required property int index
                        required property string modelData
                        spacing: 1
                        property alias outputsRepeater: cellsRepeater
                        property alias noneOutputCell: noneCell

                        Text {
                            width: grid.labelWidth
                            height: grid.cellSize
                            verticalAlignment: Text.AlignVCenter
                            text: rowItem.modelData
                            color: Theme.text
                            font.bold: true
                        }

                        Repeater {
                            id: cellsRepeater
                            model: root.outputs

                            delegate: Rectangle {
                                id: cell
                                required property int index
                                readonly property bool assigned: root.outputOf(rowItem.index) === cell.index

                                width: grid.cellSize
                                height: grid.cellSize
                                color: assigned ? Theme.accent : "transparent"
                                border.color: Theme.divider
                                border.width: 1

                                Accessible.role: Accessible.RadioButton
                                Accessible.name: qsTr("%1 to slot %2").arg(rowItem.modelData).arg(cell.index + 1)
                                Accessible.checkable: true
                                Accessible.checked: cell.assigned
                                Accessible.onPressAction:
                                    NetworkController.setSinkRoutingAssignment(rowItem.index, cell.index)

                                Keys.onPressed: function(event) { grid.moveFocus(rowItem.index, cell.index, event); }
                                Keys.onSpacePressed: NetworkController.setSinkRoutingAssignment(rowItem.index, cell.index)
                                Keys.onReturnPressed: NetworkController.setSinkRoutingAssignment(rowItem.index, cell.index)

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
                                    onClicked: {
                                        cell.forceActiveFocus();
                                        NetworkController.setSinkRoutingAssignment(rowItem.index, cell.index);
                                    }
                                }
                            }
                        }

                        Rectangle {
                            id: noneCell
                            readonly property bool assigned: root.outputOf(rowItem.index) < 0

                            width: grid.noneWidth
                            height: grid.cellSize
                            color: assigned ? Theme.accent : "transparent"
                            border.color: Theme.divider
                            border.width: 1

                            Accessible.role: Accessible.RadioButton
                            Accessible.name: qsTr("%1 to no slot").arg(rowItem.modelData)
                            Accessible.checkable: true
                            Accessible.checked: noneCell.assigned
                            Accessible.onPressAction: NetworkController.setSinkRoutingAssignment(rowItem.index, -1)

                            Keys.onPressed: function(event) { grid.moveFocus(rowItem.index, root.outputs, event); }
                            Keys.onSpacePressed: NetworkController.setSinkRoutingAssignment(rowItem.index, -1)
                            Keys.onReturnPressed: NetworkController.setSinkRoutingAssignment(rowItem.index, -1)

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
                                onClicked: {
                                    noneCell.forceActiveFocus();
                                    NetworkController.setSinkRoutingAssignment(rowItem.index, -1);
                                }
                            }
                        }
                    }
                }
            }

            Text {
                objectName: "networkSinkSlotsStatus"
                Layout.fillWidth: true
                text: root.slotsStatusText()
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("✓ %1 slots at %2-bit, as the sink reports. Slot width is set on the sink's "
                          + "own page, to match its DACs.")
                          .arg(root.outputs).arg(root.speakers.outputBitDepth ?? 0)
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
        }

        Card {
            title: qsTr("03 Levels and delays")

            Column {
                id: levels
                Layout.fillWidth: true
                spacing: Theme.gap / 2

                readonly property int labelWidth: 40
                readonly property int outWidth: 32
                readonly property int sizeWidth: 120
                readonly property int fieldWidth: 64
                readonly property int unitWidth: 24

                Row {
                    spacing: Theme.gap / 2
                    Text { width: levels.labelWidth; text: qsTr("SPK"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro }
                    Text { width: levels.outWidth; text: qsTr("OUT"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro }
                    Text { width: levels.sizeWidth; text: qsTr("SIZE"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro }
                    Text {
                        width: levels.fieldWidth; horizontalAlignment: Text.AlignRight
                        text: qsTr("TRIM"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro
                    }
                    Item { width: levels.unitWidth; height: 1 }
                    Text {
                        width: levels.fieldWidth; horizontalAlignment: Text.AlignRight
                        text: qsTr("DELAY"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro
                    }
                    Item { width: levels.unitWidth; height: 1 }
                    Item { width: Theme.gap; height: 1 }
                    Text { text: qsTr("IDENTIFY"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro }
                }

                Repeater {
                    model: root.labels

                    delegate: Row {
                        id: row
                        required property int index
                        required property string modelData
                        spacing: Theme.gap / 2
                        readonly property int output: root.outputOf(row.index)

                        Text {
                            width: levels.labelWidth
                            height: trimField.implicitHeight
                            verticalAlignment: Text.AlignVCenter
                            text: row.modelData
                            color: Theme.text
                        }
                        Text {
                            width: levels.outWidth
                            height: trimField.implicitHeight
                            verticalAlignment: Text.AlignVCenter
                            text: row.output >= 0 ? String(row.output + 1) : "—"
                            color: Theme.textMuted
                        }

                        Item {
                            width: levels.sizeWidth
                            height: trimField.implicitHeight
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: root.isLfeFlags[row.index] === true
                                text: "—"
                                color: Theme.textMuted
                            }
                            SegmentedControl {
                                objectName: "networkSinkSize-" + row.index
                                anchors.verticalCenter: parent.verticalCenter
                                visible: root.isLfeFlags[row.index] !== true
                                enabled: root.speakers.hasLfe === true
                                segHeight: trimField.implicitHeight
                                accessibleName: qsTr("Size for %1").arg(row.modelData)
                                currentValue: root.smallFlags[row.index] === true ? "small" : "large"
                                model: [
                                    { value: "large", label: qsTr("Large") },
                                    { value: "small", label: qsTr("Small") }
                                ]
                                onSelected: function(value) {
                                    NetworkController.setSinkSpeakerSmall(row.index, value === "small");
                                }
                            }
                        }

                        TextField {
                            id: trimField
                            objectName: "networkSinkTrim-" + row.index
                            width: levels.fieldWidth
                            horizontalAlignment: Text.AlignRight
                            font.family: Theme.monoFamily
                            enabled: row.output >= 0
                            validator: DoubleValidator {
                                bottom: (root.speakers.management ?? {}).trimMinDb ?? -24
                                top: (root.speakers.management ?? {}).trimMaxDb ?? 12
                                decimals: 1
                            }
                            text: Number(row.output >= 0 ? ((root.speakers.trimDb ?? [])[row.output] ?? 0) : 0).toFixed(1)
                            Accessible.name: qsTr("Trim for %1, dB").arg(row.modelData)
                            onEditingFinished: {
                                const value = parseFloat(text);
                                if (!isNaN(value) && row.output >= 0) {
                                    NetworkController.setSinkTrimDb(row.output, value);
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
                            objectName: "networkSinkDelay-" + row.index
                            width: levels.fieldWidth
                            horizontalAlignment: Text.AlignRight
                            font.family: Theme.monoFamily
                            enabled: row.output >= 0
                            validator: DoubleValidator {
                                bottom: 0
                                top: (root.speakers.management ?? {}).maxDelayMs ?? 40
                                decimals: 1
                            }
                            text: Number(row.output >= 0 ? ((root.speakers.delayMs ?? [])[row.output] ?? 0) : 0).toFixed(1)
                            Accessible.name: qsTr("Delay for %1, milliseconds").arg(row.modelData)
                            onEditingFinished: {
                                const value = parseFloat(text);
                                if (!isNaN(value) && row.output >= 0) {
                                    NetworkController.setSinkDelayMs(row.output, value);
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

                        // A plain Rectangle + MouseArea, not a native Button -
                        // Speakers.qml's own identical choice, for the same
                        // reason (qml-native-button-repeater-offscreen-hang).
                        Rectangle {
                            id: identifyButton
                            objectName: "networkSinkIdentify-" + row.index
                            readonly property bool active: (root.speakers.identifySlot ?? -1) === row.index

                            enabled: (root.speakers.management ?? {}).identify === true
                            opacity: enabled ? 1.0 : 0.5
                            implicitWidth: identifyLabel.implicitWidth + Theme.gap * 2
                            implicitHeight: identifyLabel.implicitHeight + Theme.gap
                            color: identifyArea.containsMouse ? Theme.neutral200 : Theme.bg
                            border.color: Theme.border
                            border.width: 1
                            radius: Theme.radius

                            Accessible.role: Accessible.Button
                            Accessible.name: identifyButton.active
                                             ? qsTr("Stop the identify tone on %1").arg(row.modelData)
                                             : qsTr("Identify %1, pink noise").arg(row.modelData)
                            Accessible.onPressAction: identifyButton.active
                                                       ? NetworkController.stopSinkIdentify()
                                                       : NetworkController.startSinkIdentify(row.index)

                            activeFocusOnTab: identifyButton.enabled
                            Keys.onSpacePressed: identifyButton.active
                                                  ? NetworkController.stopSinkIdentify()
                                                  : NetworkController.startSinkIdentify(row.index)
                            Keys.onReturnPressed: identifyButton.active
                                                   ? NetworkController.stopSinkIdentify()
                                                   : NetworkController.startSinkIdentify(row.index)

                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -Theme.focusRingOffset
                                visible: identifyButton.activeFocus
                                color: "transparent"
                                border.color: Theme.focusRing
                                border.width: Theme.focusRingWidth
                                z: 100
                            }
                            Text {
                                id: identifyLabel
                                anchors.centerIn: parent
                                text: identifyButton.active ? qsTr("Stop") : qsTr("Identify")
                                color: Theme.text
                            }
                            MouseArea {
                                id: identifyArea
                                anchors.fill: parent
                                enabled: identifyButton.enabled
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: identifyButton.active
                                           ? NetworkController.stopSinkIdentify()
                                           : NetworkController.startSinkIdentify(row.index)
                            }
                        }
                    }
                }
            }
        }
    }
}

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
    // Scaled, like every other page's label column: a fixed 90 clips at 150%.
    readonly property int labelColumn: Math.round(90 * Theme.fontScale)

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

        // Flat and untitled: network-sink-speakers.png puts the layout and
        // crossover rows straight onto the page under the Speakers/Decoder
        // tabs, with no box and no heading of their own - only the routing
        // grid and the levels table below are boxed. (The page's own numbered
        // heading is its column's, drawn by NetworkSinkSettings.qml.)
        Card {
            flat: true

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                Text {
                    text: qsTr("Layout")
                    color: Theme.textMuted
                    Layout.preferredWidth: root.labelColumn
                    elide: Text.ElideRight
                }
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
                Item { Layout.preferredWidth: root.labelColumn; Layout.preferredHeight: 1 }
                AppTextField {
                    id: layoutField
                    objectName: "networkSinkLayoutText"
                    Layout.fillWidth: true
                    font.family: Theme.monoFamily
                    // Typing writes `text` onto the control and the binding is
                    // destroyed for good, so the sink's own layout would never
                    // reach it again. Restored whenever the field is not being
                    // edited - Speakers.qml's own layout field does the same.
                    Binding on text {
                        when: !layoutField.activeFocus
                        value: root.speakers.layoutText ?? ""
                        restoreMode: Binding.RestoreBindingOrValue
                    }
                    Accessible.name: qsTr("Layout, as text")
                    onEditingFinished: NetworkController.setSinkLayoutText(text)
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                Text {
                    text: qsTr("Crossover")
                    color: Theme.textMuted
                    Layout.preferredWidth: root.labelColumn
                    elide: Text.ElideRight
                }
                // Bare numbers, no "Hz" per segment: the mockup puts the unit
                // in the sentence underneath instead, and four "NNN Hz"
                // segments do not fit this column at 150% text.
                SegmentedControl {
                    objectName: "networkSinkCrossover"
                    accessibleName: qsTr("Crossover, Hz")
                    currentValue: [60, 80, 100, 120].includes(root.speakers.crossoverHz)
                                  ? String(root.speakers.crossoverHz) : ""
                    model: [
                        { value: "60", label: "60" },
                        { value: "80", label: "80" },
                        { value: "100", label: "100" },
                        { value: "120", label: "120" }
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

        // Boxed, untitled, and the two status lines sit OUTSIDE the box: the
        // mockup's border starts at the SPEAKER header row - there is no
        // heading above it - and the ✓ sentences run on the page beneath.
        Card {
            Column {
                id: grid
                spacing: 0
                Layout.fillWidth: true
                activeFocusOnTab: true
                onActiveFocusChanged: {
                    if (grid.activeFocus) {
                        const target = grid.cellAt(0, 0);
                        if (target) { target.forceActiveFocus(); }
                    }
                }

                // Measured off network-sink-speakers.png: cells on a 26 px
                // pitch (borders land on single lines there, so the cells abut
                // - spacing 0, not 1, or every boundary draws twice), NONE the
                // same square as the rest, a 30 px header row and 40 px rows,
                // each closed by a full-width rule.
                readonly property int cellSize: Math.round(26 * Theme.fontScale)
                readonly property int labelWidth: Math.round(72 * Theme.fontScale)
                readonly property int noneWidth: grid.cellSize
                readonly property int headerHeight: Math.round(30 * Theme.fontScale)
                readonly property int rowHeight: Math.round(40 * Theme.fontScale)

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
                    spacing: 0
                    Text {
                        width: grid.labelWidth
                        height: grid.headerHeight
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("SPEAKER")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMicro
                    }
                    Repeater {
                        model: root.outputs
                        delegate: Text {
                            required property int index
                            width: grid.cellSize
                            height: grid.headerHeight
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            text: String(index + 1)
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontMicro
                        }
                    }
                    Text {
                        width: grid.noneWidth
                        height: grid.headerHeight
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("NONE")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMicro
                    }
                }

                Rectangle { width: grid.width; height: 1; color: Theme.divider }

                Repeater {
                    id: rowsRepeater
                    model: root.labels

                    // A Column, not the Row itself, so each row can close with
                    // its own rule - the id stays `rowItem` so everything
                    // inside still reads its index and label from here.
                    delegate: Column {
                        id: rowItem
                        required property int index
                        required property string modelData
                        spacing: 0
                        property alias outputsRepeater: cellsRepeater
                        property alias noneOutputCell: noneCell

                        Row {
                        spacing: 0
                        height: grid.rowHeight

                        Text {
                            width: grid.labelWidth
                            height: grid.rowHeight
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

                                anchors.verticalCenter: parent.verticalCenter
                                width: grid.cellSize
                                height: grid.cellSize
                                // A patched cell KEEPS its outline and fills a
                                // smaller square inside it - it does not flood
                                // the whole box. Speakers.qml had the same bug
                                // against the same mockup family.
                                color: "transparent"
                                border.color: Theme.divider
                                border.width: 1

                                Rectangle {
                                    anchors.centerIn: parent
                                    width: Math.round(parent.width / 2)
                                    height: width
                                    visible: cell.assigned
                                    color: Theme.accent
                                }

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

                            anchors.verticalCenter: parent.verticalCenter
                            width: grid.noneWidth
                            height: grid.cellSize
                            color: "transparent"
                            border.color: Theme.divider
                            border.width: 1

                            Rectangle {
                                anchors.centerIn: parent
                                width: Math.round(parent.width / 2)
                                height: width
                                visible: noneCell.assigned
                                color: Theme.accent
                            }

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

                        // The Card's own border closes the last row.
                        Rectangle {
                            width: grid.width
                            height: 1
                            color: Theme.divider
                            visible: rowItem.index < root.labels.length - 1
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

        // Untitled and ruled, like the routing grid above and for the same
        // reason: the mockup's box starts at the SPK header row, and a
        // full-width rule closes the header and every speaker after it
        // (measured at a 47 px pitch).
        Card {
            Column {
                id: levels
                Layout.fillWidth: true
                spacing: 0

                readonly property int rowHeight: Math.round(47 * Theme.fontScale)

                readonly property int labelWidth: Math.round(40 * Theme.fontScale)
                readonly property int outWidth: Math.round(32 * Theme.fontScale)
                readonly property int sizeWidth: Math.round(120 * Theme.fontScale)
                // The unit is drawn INSIDE the field now (AppTextField.unit),
                // which is where the mockup puts dB and ms, so the column is
                // the old field plus the old separate unit label.
                readonly property int fieldWidth: Math.round(88 * Theme.fontScale)

                Row {
                    spacing: Theme.gap / 2
                    height: Math.round(28 * Theme.fontScale)
                    Text { width: levels.labelWidth; height: parent.height; verticalAlignment: Text.AlignVCenter; text: qsTr("SPK"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro }
                    Text { width: levels.outWidth; height: parent.height; verticalAlignment: Text.AlignVCenter; text: qsTr("OUT"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro }
                    Text { width: levels.sizeWidth; height: parent.height; verticalAlignment: Text.AlignVCenter; text: qsTr("SIZE"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro }
                    Text {
                        width: levels.fieldWidth; height: parent.height
                        horizontalAlignment: Text.AlignRight; verticalAlignment: Text.AlignVCenter
                        text: qsTr("TRIM"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro
                    }
                    Text {
                        width: levels.fieldWidth; height: parent.height
                        horizontalAlignment: Text.AlignRight; verticalAlignment: Text.AlignVCenter
                        text: qsTr("DELAY"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro
                    }
                    Item { width: Theme.gap; height: 1 }
                    Text {
                        height: parent.height; verticalAlignment: Text.AlignVCenter
                        text: qsTr("IDENTIFY"); color: Theme.textMuted; font.pixelSize: Theme.fontMicro
                    }
                }

                Rectangle { width: levels.width; height: 1; color: Theme.divider }

                Repeater {
                    model: root.labels

                    // A Column so the row can close with its own rule; the id
                    // stays `row` so everything inside still reads its index,
                    // label and output from here.
                    delegate: Column {
                        id: row
                        required property int index
                        required property string modelData
                        spacing: 0
                        readonly property int output: root.outputOf(row.index)

                        Row {
                        spacing: Theme.gap / 2
                        height: levels.rowHeight

                        Text {
                            width: levels.labelWidth
                            height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            text: row.modelData
                            color: Theme.text
                        }
                        Text {
                            width: levels.outWidth
                            height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            text: row.output >= 0 ? String(row.output + 1) : "—"
                            color: Theme.textMuted
                        }

                        Item {
                            width: levels.sizeWidth
                            height: parent.height
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

                        // Typing writes `text` onto the control and destroys
                        // the declarative binding for good - so the sink's own
                        // trim and delay are restored whenever the field is
                        // not being edited, rather than never reaching it
                        // again after the first keystroke.
                        AppTextField {
                            id: trimField
                            anchors.verticalCenter: parent.verticalCenter
                            objectName: "networkSinkTrim-" + row.index
                            width: levels.fieldWidth
                            horizontalAlignment: Text.AlignRight
                            font.family: Theme.monoFamily
                            enabled: row.output >= 0
                            unit: qsTr("dB")
                            validator: DoubleValidator {
                                bottom: (root.speakers.management ?? {}).trimMinDb ?? -24
                                top: (root.speakers.management ?? {}).trimMaxDb ?? 12
                                decimals: 1
                                notation: DoubleValidator.StandardNotation
                            }
                            Binding on text {
                                when: !trimField.activeFocus
                                value: Number(row.output >= 0
                                              ? ((root.speakers.trimDb ?? [])[row.output] ?? 0) : 0).toFixed(1)
                                restoreMode: Binding.RestoreBindingOrValue
                            }
                            Accessible.name: qsTr("Trim for %1, dB").arg(row.modelData)
                            onEditingFinished: {
                                const value = parseFloat(text);
                                if (!isNaN(value) && row.output >= 0) {
                                    NetworkController.setSinkTrimDb(row.output, value);
                                }
                            }
                        }

                        AppTextField {
                            id: delayField
                            anchors.verticalCenter: parent.verticalCenter
                            objectName: "networkSinkDelay-" + row.index
                            width: levels.fieldWidth
                            horizontalAlignment: Text.AlignRight
                            font.family: Theme.monoFamily
                            enabled: row.output >= 0
                            unit: qsTr("ms")
                            validator: DoubleValidator {
                                bottom: 0
                                top: (root.speakers.management ?? {}).maxDelayMs ?? 40
                                decimals: 1
                                notation: DoubleValidator.StandardNotation
                            }
                            Binding on text {
                                when: !delayField.activeFocus
                                value: Number(row.output >= 0
                                              ? ((root.speakers.delayMs ?? [])[row.output] ?? 0) : 0).toFixed(1)
                                restoreMode: Binding.RestoreBindingOrValue
                            }
                            Accessible.name: qsTr("Delay for %1, milliseconds").arg(row.modelData)
                            onEditingFinished: {
                                const value = parseFloat(text);
                                if (!isNaN(value) && row.output >= 0) {
                                    NetworkController.setSinkDelayMs(row.output, value);
                                }
                            }
                        }

                        Item { width: Theme.gap; height: 1 }

                        // A plain Rectangle + MouseArea, not a native Button -
                        // Speakers.qml's own identical choice, for the same
                        // reason (qml-native-button-repeater-offscreen-hang).
                        Rectangle {
                            id: identifyButton
                            anchors.verticalCenter: parent.verticalCenter
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

                        // The Card's own border closes the last row.
                        Rectangle {
                            width: levels.width
                            height: 1
                            color: Theme.divider
                            visible: row.index < root.labels.length - 1
                        }
                    }
                }
            }
        }
    }
}

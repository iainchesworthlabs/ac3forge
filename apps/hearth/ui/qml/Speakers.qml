import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The Speakers page (planning/hearth-design.md, "Speaker setup"): routing,
// trim, delay and the bass-management crossover, all real and posted
// straight to the engine through HearthController. Two things the design
// shows are not built here: the layout picker (the layout is fixed for this
// slice - HearthController.speakerLabels' own comment says why) and the
// identify tone (render::IdentifyTone is not wired into the engine yet).
// Both are named rather than left silently missing.
//
// The "01 Speaker layout" plan diagram IS built here, even though the
// engine exposes no per-slot angle (same comment) - only the label and the
// small-speaker flag. Rather than extend the engine for one diagram, this
// file keeps its own label-to-angle/elevation table, ported by hand from
// ac3::spatial::direction_of (src/forge/src/spatial/spatial.cpp) - see
// speakerAngles/directionOf below. Keep the two in sync if that table ever
// changes.
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

    // ac3::spatial::kHeightThresholdDeg (spatial.hpp): half way to the
    // nominal height angle, so the plan diagram draws a slot hollow (the
    // height tier) exactly when OutputLayout::has_height() would count it.
    readonly property real heightThresholdDeg: 22.5

    // ac3::spatial::direction_of's own table (spatial.cpp): azimuth
    // counterclockwise from front (left positive), elevation above the
    // listener's plane. Ls/Rs are not here - their azimuth depends on the
    // rest of the layout, same as direction_of's own has_rears/
    // has_side_discrete arguments - see directionOf below. LFE/LFE2 are not
    // here either: direction_of documents them as having no direction at
    // all, and the plan diagram lists them beside the ring instead of
    // placing them on it.
    readonly property var speakerAngles: ({
        "L":   { azimuth: 30,    elevation: 0 },
        "C":   { azimuth: 0,     elevation: 0 },
        "R":   { azimuth: -30,   elevation: 0 },
        "Lc":  { azimuth: 15,    elevation: 0 },
        "Rc":  { azimuth: -15,   elevation: 0 },
        "Lrs": { azimuth: 150,   elevation: 0 },
        "Rrs": { azimuth: -150,  elevation: 0 },
        "Cs":  { azimuth: 180,   elevation: 0 },
        "Ts":  { azimuth: 180,   elevation: 90 },
        "Lsd": { azimuth: 90,    elevation: 0 },
        "Rsd": { azimuth: -90,   elevation: 0 },
        "Lw":  { azimuth: 60,    elevation: 0 },
        "Rw":  { azimuth: -60,   elevation: 0 },
        "Vhl": { azimuth: 45,    elevation: 45 },
        "Vhr": { azimuth: -45,   elevation: 45 },
        "Vhc": { azimuth: 0,     elevation: 45 },
        "Lts": { azimuth: 135,   elevation: 45 },
        "Rts": { azimuth: -135,  elevation: 45 }
    })

    // Mirrors direction_of(location, has_rears, has_side_discrete): without
    // a discrete rear pair, Ls/Rs sit at the 5.1 ring's own +-110 degrees;
    // with one, they move to the side (+-90) and the rear pair takes the
    // rest. Returns null for a label this table (and that special case)
    // does not cover - a raw angle token or "-" from a hand-written list
    // layout, which the plan diagram then leaves undrawn rather than guess.
    function directionOf(label, hasRears, hasSideDiscrete) {
        if (label === "Ls" || label === "Rs") {
            const azimuth = hasRears && !hasSideDiscrete ? 90 : 110;
            return { azimuth: label === "Ls" ? azimuth : -azimuth, elevation: 0 };
        }
        return root.speakerAngles[label] ?? null;
    }

    // The current layout's slots, split into what the plan diagram draws on
    // the ring (a label, its azimuth, and whether it belongs in the height
    // tier) and what it lists beside the ring instead (LFE/LFE2).
    readonly property var planEntries: {
        const hasRears = root.labels.indexOf("Lrs") >= 0 || root.labels.indexOf("Rrs") >= 0;
        const hasSideDiscrete = root.labels.indexOf("Lsd") >= 0 || root.labels.indexOf("Rsd") >= 0;
        const ring = [];
        const lfe = [];
        for (let i = 0; i < root.labels.length; i++) {
            const label = root.labels[i];
            if (label === "LFE" || label === "LFE2") {
                lfe.push(label);
                continue;
            }
            const direction = root.directionOf(label, hasRears, hasSideDiscrete);
            if (direction) {
                ring.push({ label: label, azimuth: direction.azimuth,
                            height: direction.elevation >= root.heightThresholdDeg });
            }
        }
        return { ring: ring, lfe: lfe };
    }

    readonly property string planDescription: {
        const ring = root.planEntries.ring;
        const heightCount = ring.filter(function(entry) { return entry.height; }).length;
        let text = qsTr("%1 speakers on the ring, %2 at height").arg(ring.length).arg(heightCount);
        if (root.planEntries.lfe.length > 0) {
            text += " " + qsTr("Plus %1 low-frequency, no direction.").arg(root.planEntries.lfe.length);
        }
        return text;
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
                text: qsTr("Fixed for this build; choosing a different layout is a later slice. A "
                          + "speaker marked small sends its bass to the LFE output below the crossover "
                          + "instead of reproducing it.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap * 2

                Item {
                    id: plan
                    Layout.preferredWidth: 230
                    Layout.preferredHeight: 230
                    Layout.alignment: Qt.AlignTop

                    readonly property real cx: width / 2
                    readonly property real cy: height / 2
                    readonly property real ringRadius: Math.min(width, height) / 2 - 40
                    // Distinctly inside ringRadius, so a height slot never
                    // lands on top of an ear-level one at the same azimuth
                    // (C/Vhc, Cs/Ts - both real ITU-R BS.2051 combinations).
                    readonly property real heightRadius: ringRadius * 0.6

                    function screenX(azimuthDeg, radius) {
                        return plan.cx - Math.sin(azimuthDeg * Math.PI / 180) * radius;
                    }
                    function screenY(azimuthDeg, radius) {
                        return plan.cy - Math.cos(azimuthDeg * Math.PI / 180) * radius;
                    }
                    function radiusFor(entry) {
                        return entry.height ? plan.heightRadius : plan.ringRadius;
                    }

                    // A plan view is a picture, not a set of discrete
                    // controls - one Graphic with a text alternative, the
                    // same shape SoundfieldView.qml uses for the same
                    // reason (see its own comment).
                    Accessible.role: Accessible.Graphic
                    Accessible.name: qsTr("Speaker plan")
                    Accessible.description: root.planDescription

                    Rectangle {
                        anchors.fill: parent
                        color: Theme.neutral100
                        border.color: Theme.divider
                        border.width: 1
                    }

                    // The ear-level ring. Dashed, matching SoundfieldView's
                    // ceiling ring, and for the same reason there: a Canvas
                    // is what draws a dashed stroke in QML.
                    Canvas {
                        id: ring
                        anchors.fill: parent
                        onPaint: {
                            const ctx = getContext("2d");
                            ctx.reset();
                            ctx.strokeStyle = String(Theme.neutral300);
                            ctx.lineWidth = 1;
                            ctx.setLineDash([3, 3]);
                            ctx.beginPath();
                            ctx.arc(plan.cx, plan.cy, plan.ringRadius, 0, 2 * Math.PI);
                            ctx.stroke();
                        }
                        // Repaint on a theme flip: onPaint reads a Theme
                        // colour, and a Canvas has no dependency tracking of
                        // its own.
                        Connections {
                            target: Theme
                            function onDarkChanged() { ring.requestPaint(); }
                            function onPaletteChoiceChanged() { ring.requestPaint(); }
                        }
                    }

                    // Front, always straight up - the diagram's own
                    // reference direction, not a speaker.
                    Rectangle {
                        x: plan.cx - 8
                        y: plan.cy - plan.ringRadius - 12
                        width: 16
                        height: 3
                        color: Theme.text
                    }

                    // The listening position, at the ring's centre.
                    Rectangle { x: plan.cx - 6; y: plan.cy - 1; width: 12; height: 1; color: Theme.neutral400 }
                    Rectangle { x: plan.cx - 1; y: plan.cy - 6; width: 1; height: 12; color: Theme.neutral400 }

                    Repeater {
                        model: root.planEntries.ring

                        delegate: Item {
                            id: entry
                            required property var modelData

                            Rectangle {
                                x: plan.screenX(entry.modelData.azimuth, plan.radiusFor(entry.modelData)) - width / 2
                                y: plan.screenY(entry.modelData.azimuth, plan.radiusFor(entry.modelData)) - height / 2
                                width: 10
                                height: 10
                                color: entry.modelData.height ? "transparent" : Theme.text
                                border.color: Theme.text
                                border.width: entry.modelData.height ? 1.5 : 0
                            }
                            Text {
                                text: entry.modelData.label
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontMicro
                                x: plan.screenX(entry.modelData.azimuth, plan.radiusFor(entry.modelData) + 16) - implicitWidth / 2
                                y: plan.screenY(entry.modelData.azimuth, plan.radiusFor(entry.modelData) + 16) - implicitHeight / 2
                            }
                        }
                    }

                    // The LFE feed is stated, not placed - direction_of's
                    // own comment says it has no direction at all, the same
                    // reason SoundfieldView.qml lists it in text rather than
                    // drawing it. The top-left corner specifically, not the
                    // ring's own left: Ls sits there too once a layout has
                    // rear surrounds (has_rears above), and every ring label
                    // stays within ringRadius/heightRadius plus its own
                    // outward offset - short of a corner - so a corner is
                    // the one spot nothing on the ring ever reaches.
                    Column {
                        visible: root.planEntries.lfe.length > 0
                        x: 6
                        y: 6
                        spacing: 4

                        Repeater {
                            model: root.planEntries.lfe

                            delegate: Row {
                                id: lfeRow
                                required property string modelData
                                spacing: 4
                                Rectangle {
                                    width: 10; height: 10; color: Theme.text
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Text {
                                    text: lfeRow.modelData
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fontMicro
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    spacing: Theme.gap / 2

                    RowLayout {
                        spacing: Theme.gap / 2
                        Rectangle { implicitWidth: 10; implicitHeight: 10; color: Theme.text }
                        Text {
                            text: qsTr("speaker at ear level")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                        }
                    }
                    RowLayout {
                        spacing: Theme.gap / 2
                        Rectangle {
                            implicitWidth: 10; implicitHeight: 10; color: "transparent"
                            border.color: Theme.text; border.width: 1.5
                        }
                        Text {
                            text: qsTr("height speaker")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                        }
                    }
                    RowLayout {
                        spacing: Theme.gap / 2
                        Rectangle { implicitWidth: 16; implicitHeight: 3; color: Theme.text }
                        Text {
                            text: qsTr("front")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        Layout.topMargin: Theme.gap / 2
                        text: qsTr("Directions follow ITU-R BS.2051: L and R sit 30° either side of "
                                  + "centre, Ls and Rs 110° back (90° with rear surrounds present), and "
                                  + "each height speaker sits above its own compass point, closer to the "
                                  + "centre. The LFE feed has no direction, so it is listed rather than "
                                  + "placed.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
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

                // Arrow keys move focus only (Space/Enter/click below still do the
                // assigning). Looked up fresh via Repeater.itemAt() on every press
                // rather than cached, so focus can't ever target a stale item.
                function cellAt(row, column) {
                    const rowCount = root.labels.length;
                    if (rowCount === 0) {
                        return null;
                    }
                    row = Math.max(0, Math.min(rowCount - 1, row));
                    const rowItem = rowsRepeater.itemAt(row);
                    if (!rowItem) {
                        return null;
                    }
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
                    if (target) {
                        target.forceActiveFocus();
                    }
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
                                Keys.onPressed: function(event) { grid.moveFocus(rowItem.index, cell.index, event); }
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
                            Keys.onPressed: function(event) { grid.moveFocus(rowItem.index, root.outputs, event); }
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
                          + "Tab reaches the grid; arrow keys move through it, and Space or Enter sends the "
                          + "speaker to the focused output.")
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

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// The Speakers page (planning/hearth-design.md, "Speaker setup"): routing,
// trim, delay, the bass-management crossover, the identify tone, the plan
// diagram, and the speaker layout itself - the picker, the "As text" field,
// the Heights control and each speaker's own Size toggle (HearthController.
// setLayoutText()/setHeights()/setSpeakerSmall(), backed by the engine's own
// live Player::set_layout()) - all real and posted straight to the engine
// through HearthController.
//
// The plan diagram has no per-slot angle from the engine to draw with -
// only the label and the small-speaker flag. Rather than extend the engine
// for one diagram, this file keeps its own label-to-angle/elevation table,
// ported by hand from ac3::spatial::direction_of
// (src/forge/src/spatial/spatial.cpp) - see speakerAngles/directionOf
// below. Keep the two in sync if that table ever changes.
ScrollView {
    id: root

    // The control column both mockups measure, scaled so a label that grows
    // keeps its column rather than pushing through the control beside it.
    readonly property int labelWidth: Math.round(108 * Theme.fontScale)
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    readonly property var labels: HearthController.speakerLabels
    readonly property var smallFlags: HearthController.speakerSmall
    readonly property var isLfeFlags: HearthController.speakerIsLfe
    readonly property var routingList: HearthController.routing
    readonly property int outputs: HearthController.routingOutputs
    readonly property var outputNames: HearthController.outputNames

    // The routing grid's column header for output `index`: the device's own
    // name beside its 1-based number ("1 FL", planning/hearth-design.md's
    // mockup) where HearthController.outputNames has one, else the bare
    // number alone - a device that cannot say, or a width no standard
    // arrangement fits, leaves that entry empty.
    function outputHeading(index) {
        const name = root.outputNames[index];
        return name ? qsTr("%1 %2").arg(index + 1).arg(name) : String(index + 1);
    }

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

    // The "01 Speaker layout" card's own read-out of whether the Decoder's
    // object-reconstruction policy (DecoderEac3.qml's "05 Objects" card,
    // HearthController.decoderSettings.objects) actually places objects for
    // the layout set up here. "auto" is the only policy where that depends
    // on this page's own Heights state (render::Serving::compute, serving.hpp) -
    // "always"/"never" place regardless of it, so those two phrasings don't
    // reference height at all.
    function objectsStatusText() {
        const policy = HearthController.decoderSettings.objects ?? "auto";
        if (policy === "always") {
            return qsTr("✓ Objects are always placed, regardless of height (Decoder: Always).");
        }
        if (policy === "never") {
            return qsTr("Objects are never placed (Decoder: Never).");
        }
        return HearthController.layoutHasHeight
            ? qsTr("✓ The layout has heights, so objects are placed (Decoder: Auto).")
            : qsTr("The layout has no heights, so objects are not placed (Decoder: Auto).");
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
        // implicitWidth as well as width, so the ScrollView derives a content
        // width from it: the routing grid below is a fixed-size Row tree
        // whose implicit width grows with the output count, and with
        // horizontal scrolling off it was the thing being clipped.
        width: root.availableWidth
        implicitWidth: root.availableWidth
        spacing: Theme.gap * 2

        Text {
            Layout.fillWidth: true
            Layout.topMargin: Theme.pad
            Layout.leftMargin: Theme.pad
            Layout.rightMargin: Theme.pad
            text: HearthController.deviceName.length > 0
                  ? qsTr("Setup for %1 · %2 outputs").arg(HearthController.deviceName).arg(root.outputs)
                  : qsTr("No output device chosen yet")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
        }

        Card {
            Layout.leftMargin: Theme.pad
            Layout.rightMargin: Theme.pad
            ordinal: "01"
            title: qsTr("Speaker layout")
            framed: true

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                Text { text: qsTr("Layout"); color: Theme.text; font.pixelSize: Theme.fontNormal
                       elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
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
                Text { text: qsTr("As text"); color: Theme.text; font.pixelSize: Theme.fontNormal
                       elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                AppTextField {
                    id: layoutTextField
                    objectName: "speakersLayoutText"
                    Layout.fillWidth: true
                    text: HearthController.layoutText
                    Accessible.name: qsTr("Layout, as text")
                    // Unparseable text is simply dropped, the same as an
                    // out-of-range trim or delay elsewhere on this page -
                    // setLayoutText() parses before it ever reaches the
                    // engine, so there is no round trip to fail against, and
                    // the field falls back to showing the layout still in
                    // effect.
                    onEditingFinished: HearthController.setLayoutText(text)
                    // Typing destroys the binding above for good, so the
                    // field is re-read whenever it is not the one being
                    // edited - otherwise picking a preset moved the engine
                    // and left this showing the old text.
                    Binding on text {
                        value: HearthController.layoutText
                        when: !layoutTextField.activeFocus
                        restoreMode: Binding.RestoreBindingOrValue
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("A name such as 7.1.4, or one token per output: a location, an angle such as "
                          + "30/0, or a dash for an output with no speaker. A speaker marked small sends "
                          + "its bass to the LFE output below the crossover instead of reproducing it.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                Text { text: qsTr("Heights"); color: Theme.text; font.pixelSize: Theme.fontNormal
                       elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
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

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                Text { text: qsTr("Objects"); color: Theme.text; font.pixelSize: Theme.fontNormal
                       elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                Text {
                    Layout.fillWidth: true
                    text: root.objectsStatusText()
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.WordWrap
                }
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
            Layout.leftMargin: Theme.pad
            Layout.rightMargin: Theme.pad
            ordinal: "02"
            title: qsTr("Routing · speaker to device output")
            framed: true

            Column {
                id: grid
                spacing: 1

                // The grid's own single Tab stop (matching SegmentedControl's
                // group-is-the-stop idiom, apps/gui/qml/SegmentedControl.qml) -
                // individual cells no longer declare activeFocusOnTab
                // themselves. Landing here from Tab hands real focus straight
                // to a cell below, so the focus ring and Space/Enter both work
                // immediately, with no arrow press needed first.
                activeFocusOnTab: true
                onActiveFocusChanged: {
                    if (grid.activeFocus) {
                        const target = grid.cellAt(0, 0);
                        if (target) {
                            target.forceActiveFocus();
                        }
                    }
                }

                readonly property int cellSize: 28
                readonly property int labelWidth: 72
                readonly property int noneWidth: 56
                // Taller than a body row: a header cell with a device name
                // wraps its number and name onto two lines (outputHeading()),
                // which a plain cellSize-square box is too short for.
                readonly property int headerHeight: cellSize + Theme.gap

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
                    Item { width: grid.labelWidth; height: grid.headerHeight }
                    Repeater {
                        model: root.outputs
                        delegate: Text {
                            required property int index
                            width: grid.cellSize
                            height: grid.headerHeight
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            text: root.outputHeading(index)
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
                                // The cell's outline is always drawn; a
                                // patched cell adds an accent square inside
                                // it at half its size, rather than flooding
                                // the whole box (components.png, "ROUTING
                                // GRID · EMPTY, PATCHED, FOCUSED", where the
                                // accent run measures exactly half the cell
                                // in both axes). Flooding it made a patched
                                // cell read twice as heavy as designed and
                                // swallowed the outline.
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
                                Accessible.name: qsTr("%1 to output %2").arg(rowItem.modelData).arg(cell.index + 1)
                                Accessible.checkable: true
                                Accessible.checked: cell.assigned
                                Accessible.onPressAction:
                                    HearthController.setRoutingAssignment(rowItem.index, cell.index)

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
                                    onClicked: {
                                        cell.forceActiveFocus();
                                        HearthController.setRoutingAssignment(rowItem.index, cell.index);
                                    }
                                }
                            }
                        }

                        Rectangle {
                            id: noneCell
                            readonly property bool assigned: (root.routingList[rowItem.index] ?? -1) < 0

                            // The NONE column is wide enough for the word in
                            // its header, but the cell itself is the same
                            // square every output cell is (measured on both
                            // mockups), with the same inner accent square.
                            width: grid.cellSize
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
                            Accessible.name: qsTr("%1 to no output").arg(rowItem.modelData)
                            Accessible.checkable: true
                            Accessible.checked: noneCell.assigned
                            Accessible.onPressAction: HearthController.setRoutingAssignment(rowItem.index, -1)

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
                                onClicked: {
                                    noneCell.forceActiveFocus();
                                    HearthController.setRoutingAssignment(rowItem.index, -1);
                                }
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

                AppButton {
                    objectName: "speakersUseDeviceOrder"
                    text: qsTr("Use the device's order")
                    onClicked: HearthController.useDeviceOrder()
                }
                AppButton {
                    objectName: "speakersClearRouting"
                    text: qsTr("Clear")
                    onClicked: HearthController.clearRouting()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.pad
            Layout.rightMargin: Theme.pad
            spacing: Theme.gap * 2

            Card {
                ordinal: "03"
                title: qsTr("Levels and delays")
                framed: true
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

                            AppTextField {
                                id: trimField
                                objectName: "speakersTrim-" + row.index
                                width: levels.fieldWidth + levels.unitWidth
                                // The design writes the unit inside the box,
                                // right-aligned, rather than after it.
                                unit: qsTr("dB")
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
                            AppTextField {
                                objectName: "speakersDelay-" + row.index
                                width: levels.fieldWidth + levels.unitWidth
                                unit: qsTr("ms")
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
                            // A plain Rectangle + MouseArea, not a native
                            // Button: this Repeater-backed delegate carries
                            // real, non-empty speaker data, and a native QQC2
                            // Button in that position has hung the offscreen
                            // Qt Quick Test binary on Windows elsewhere in
                            // this family (qml-native-button-repeater-
                            // offscreen-hang) - the routing grid's own cells
                            // above use the same shape for the same reason.
                            Rectangle {
                                id: identifyButton
                                objectName: "speakersIdentify-" + row.index
                                readonly property bool active: HearthController.identifySlot === row.index

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
                                                           ? HearthController.stopIdentify()
                                                           : HearthController.startIdentify(row.index)

                                activeFocusOnTab: true
                                Keys.onSpacePressed: identifyButton.active
                                                      ? HearthController.stopIdentify()
                                                      : HearthController.startIdentify(row.index)
                                Keys.onReturnPressed: identifyButton.active
                                                       ? HearthController.stopIdentify()
                                                       : HearthController.startIdentify(row.index)

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
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: identifyButton.active
                                               ? HearthController.stopIdentify()
                                               : HearthController.startIdentify(row.index)
                                }
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
                    ordinal: "04"
                title: qsTr("Bass management")
                framed: true

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Crossover"); color: Theme.text; font.pixelSize: Theme.fontNormal
                       elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                        SegmentedControl {
                            accessibleName: qsTr("Crossover preset")
                            currentValue: [80, 100, 120].includes(HearthController.crossoverHz)
                                          ? String(HearthController.crossoverHz) : ""
                            // Bare numbers: the unit is written once, inside
                            // the field beside them (speakers-setup.png).
                            model: [
                                { value: "80", label: "80" },
                                { value: "100", label: "100" },
                                { value: "120", label: "120" }
                            ]
                            onSelected: function(value) { HearthController.setCrossoverHz(Number(value)); }
                        }
                        AppTextField {
                            id: crossoverField
                            objectName: "speakersCrossoverExact"
                            Layout.preferredWidth: Math.round(88 * Theme.fontScale)
                            unit: qsTr("Hz")
                            validator: DoubleValidator { bottom: 40; top: 250; decimals: 0 }
                            text: Number(HearthController.crossoverHz).toFixed(0)
                            Accessible.name: qsTr("Crossover, Hz")
                            onEditingFinished: {
                                const value = parseFloat(text);
                                if (!isNaN(value)) {
                                    HearthController.setCrossoverHz(value);
                                }
                            }
                            // Picking a preset moves the engine; without this
                            // the field kept showing whatever was last typed.
                            Binding on text {
                                value: Number(HearthController.crossoverHz).toFixed(0)
                                when: !crossoverField.activeFocus
                                restoreMode: Binding.RestoreBindingOrValue
                            }
                        }
                        Item { Layout.fillWidth: true }
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
                    ordinal: "05"
                title: qsTr("Identify")
                framed: true

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
                        Text { text: qsTr("Level"); color: Theme.text; font.pixelSize: Theme.fontNormal
                       elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
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

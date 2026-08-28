import QtQuick

import Ac3Forge

// A small horizontal group of mutually exclusive text options - the
// ".seg"/".seg-opt" pattern the handoff uses throughout (Basic/Advanced,
// File/Live capture, Coded/Rendered, Author a path/Drive it live). Zero
// radius, one active fill in accent, everything else reads as plain text.
Row {
    id: root

    // [{ value: "basic", label: "Basic" }, ...]
    property var model: []
    property string currentValue: ""
    property int segHeight: 28
    property int fontSize: 13
    // What this particular instance is choosing between - callers set this
    // to the same qsTr() text their own adjacent label already shows (e.g.
    // "Theme", "Controls tier"), never a second, hand-typed copy of it, so
    // the accessible name can't drift from what's on screen. Left empty for
    // a caller that has no visible label of its own to borrow (Accessible
    // reports the group without a name rather than a placeholder).
    property string accessibleName: ""
    signal selected(string value)

    spacing: 0

    Accessible.role: Accessible.Grouping
    Accessible.name: root.accessibleName

    Repeater {
        model: root.model

        delegate: Rectangle {
            id: seg
            required property var modelData
            readonly property bool active: modelData.value === root.currentValue

            // One SegmentedControl instance backs several distinct choices
            // across the app (tier, coded/rendered, file/live...), so a
            // fixed name would collide; keying on the option's own value
            // gives every segment, everywhere it's used, a stable, unique
            // target for Qt Quick Test's findChild - not just the one this
            // patch's own tests happen to click.
            objectName: "seg-" + modelData.value

            height: root.segHeight
            implicitWidth: label.implicitWidth + 18
            color: active ? Theme.accent : "transparent"
            border.color: Theme.divider
            border.width: 1

            // A radio button, not a plain button: exactly one segment in
            // the group is ever active, and which one is what the group
            // exists to report - Accessible.checked reads the same `active`
            // binding the fill colour already does, so the two can never
            // disagree.
            Accessible.role: Accessible.RadioButton
            Accessible.name: modelData.label
            Accessible.checkable: true
            Accessible.checked: seg.active
            Accessible.onPressAction: root.selected(seg.modelData.value)

            Text {
                id: label
                anchors.centerIn: parent
                text: modelData.label
                color: seg.active ? Theme.bg : Theme.text
                font.pixelSize: root.fontSize
            }

            MouseArea {
                anchors.fill: parent
                onClicked: root.selected(seg.modelData.value)
            }
        }
    }
}

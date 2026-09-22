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
    property int fontSize: Theme.fontBody
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
    Accessible.focusable: true

    // One tab stop for the group, then the arrows choose within it - the
    // way a radio group behaves everywhere else, and the reason the segments
    // themselves report Accessible.RadioButton. Home and End take the first
    // and the last; Left from the first wraps to the last, so a group of two
    // toggles with either arrow.
    activeFocusOnTab: true
    function indexOfCurrentValue() {
        for (let i = 0; i < root.model.length; ++i) {
            if (root.model[i].value === root.currentValue) {
                return i;
            }
        }
        return -1;
    }
    Keys.onPressed: function(event) {
        const count = root.model.length;
        if (count === 0) {
            return;
        }
        let index = root.indexOfCurrentValue();
        if (event.key === Qt.Key_Left || event.key === Qt.Key_Up) {
            index = (index <= 0 ? count : index) - 1;
        } else if (event.key === Qt.Key_Right || event.key === Qt.Key_Down) {
            index = (index + 1) % count;
        } else if (event.key === Qt.Key_Home) {
            index = 0;
        } else if (event.key === Qt.Key_End) {
            index = count - 1;
        } else {
            return;
        }
        root.selected(root.model[index].value);
        event.accepted = true;
    }

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

            // The floor is the handoff's 28; a larger text size grows it
            // rather than clipping the label inside it.
            height: Math.max(root.segHeight, label.implicitHeight + 8)
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
                color: seg.active ? Theme.accentText : Theme.text
                font.pixelSize: root.fontSize
            }

            // Not a FocusRing: the focus belongs to the GROUP and the ring
            // has to sit on whichever segment is current, which is a
            // condition the shared component's parent-activeFocus default
            // cannot express.
            Rectangle {
                objectName: "segFocusRing"
                anchors.fill: parent
                anchors.margins: -Theme.focusRingOffset
                visible: root.activeFocus && seg.active
                color: "transparent"
                border.color: Theme.focusRing
                border.width: Theme.focusRingWidth
                z: 100
            }

            MouseArea {
                anchors.fill: parent
                onClicked: root.selected(seg.modelData.value)
            }
        }
    }
}

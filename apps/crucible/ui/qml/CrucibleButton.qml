import QtQuick

import Ac3ForgeCrucible

// The GUI's flat bordered button, as the mockups draw it: 30 px tall,
// 1 px divider border, accent fill when primary.
Rectangle {
    id: root
    property string text: ""
    property bool primary: false
    property bool enabled: true
    signal clicked()

    // The mockup's 30, and taller when the text is larger, rather than a
    // label clipped inside a fixed box.
    implicitHeight: Math.max(30, label.implicitHeight + 12)
    implicitWidth: label.implicitWidth + 24
    color: primary ? Theme.accent : "transparent"
    border.color: Theme.divider
    border.width: 1
    opacity: enabled ? 1.0 : 0.45
    Accessible.role: Accessible.Button
    Accessible.name: root.text
    Accessible.focusable: root.enabled
    Accessible.onPressAction: if (root.enabled) root.clicked()

    // A tab stop while it can be pressed, and Space or Return presses it -
    // the two keys every desktop uses on a button. A disabled button is
    // skipped by Tab and ignores both.
    // Qt refuses to take an item out of the tab chain while it is the active
    // focus item, so a control that is disabled under the keyboard would keep
    // both the focus and its place in the chain - the person's next Tab would
    // start from something they can no longer use. Hand the focus back first.
    onEnabledChanged: if (!root.enabled && root.activeFocus) root.focus = false;
    activeFocusOnTab: root.enabled
    Keys.onPressed: function(event) {
        if (!root.enabled) {
            return;
        }
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            root.clicked();
            event.accepted = true;
        }
    }

    Text {
        id: label
        anchors.centerIn: parent
        // A button narrower than its label (a long translation in a fixed
        // rail) elides rather than spills.
        width: Math.min(implicitWidth, root.width - 16)
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
        text: root.text
        // On an accent fill, whichever end of the palette reads better on
        // it (Theme.accentText); plain text otherwise.
        color: root.primary ? Theme.accentText : Theme.text
        font.pixelSize: Theme.fontBody
    }
    MouseArea {
        anchors.fill: parent
        enabled: root.enabled
        cursorShape: Qt.PointingHandCursor
        // No forceActiveFocus here on purpose: a click on a button leaves
        // the keyboard where it was, so a mouse user never sees a ring.
        onClicked: root.clicked()
    }
    FocusRing {}
}

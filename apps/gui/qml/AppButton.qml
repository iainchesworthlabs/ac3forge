import QtQuick
import QtQuick.Layouts

import Ac3Forge

// The family's flat bordered button, as the design system draws it
// (docs/hearth/design/screenshots/components.png, "BUTTONS · REST, PRIMARY,
// DISABLED, KEYBOARD FOCUS"): 30 px tall, a 1 px divider border, accent fill
// when primary, dimmed when disabled, focus ring outside the border.
//
// A plain Rectangle rather than a QQC2 Button: Basic's own Button draws a
// flat FILLED background from palette.button with no border at all, which is
// the opposite of what that sheet specifies, and the family already prefers
// hand-drawn controls for anything with a custom shape
// (qml-native-button-repeater-offscreen-hang is the other reason).
Rectangle {
    id: root
    property string text: ""
    property bool primary: false
    property bool enabled: true
    // One of Theme's icon tokens, drawn before the label - the sheet's
    // "＋ With icon" button. Empty for a plain one.
    property string glyph: ""
    signal clicked()

    // The design's 30, and taller when the text is larger, rather than a
    // label clipped inside a fixed box.
    implicitHeight: Math.max(30, row.implicitHeight + 12)
    implicitWidth: row.implicitWidth + 24
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

    RowLayout {
        id: row
        anchors.centerIn: parent
        // A button narrower than its content (a long translation in a fixed
        // rail) elides rather than spills.
        width: Math.min(implicitWidth, root.width - 16)
        spacing: Theme.space2

        Text {
            visible: root.glyph.length > 0
            text: root.glyph
            font.family: Theme.iconFamily
            font.pixelSize: Theme.iconSize
            color: root.primary ? Theme.accentText : Theme.text
        }
        Text {
            id: label
            Layout.fillWidth: true
            horizontalAlignment: root.glyph.length > 0 ? Text.AlignLeft : Text.AlignHCenter
            elide: Text.ElideRight
            text: root.text
            // On an accent fill, whichever end of the palette reads better on
            // it (Theme.accentText); plain text otherwise.
            color: root.primary ? Theme.accentText : Theme.text
            font.pixelSize: Theme.fontBody
        }
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

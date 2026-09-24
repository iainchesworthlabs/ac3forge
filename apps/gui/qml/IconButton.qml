import QtQuick

import Ac3Forge

// A square button whose whole label is one icon glyph - the design system's
// transport row (docs/hearth/design/screenshots/components.png, "TRANSPORT"):
// the same 30 px box, 1 px divider border and accent-when-primary fill
// AppButton draws, sized square and lettered from Theme.iconFamily instead of
// carrying a word.
//
// `glyph` is one of Theme's own icon tokens (Theme.iconPlayArrow and the
// rest), never a hand-typed codepoint - see Theme.qml's icon section.
Rectangle {
    id: root
    property string glyph: ""
    property bool primary: false
    property bool enabled: true
    // Icon-only, so there is no label for a screen reader to fall back on -
    // every instance has to say what it does.
    property string accessibleName: ""
    signal clicked()

    // Square, on the same floor AppButton uses, and growing with the icon
    // when the person's text size does.
    implicitHeight: Math.max(30, label.implicitHeight + 12)
    implicitWidth: implicitHeight
    color: primary ? Theme.accent : "transparent"
    border.color: Theme.divider
    border.width: 1
    opacity: enabled ? 1.0 : 0.45

    Accessible.role: Accessible.Button
    Accessible.name: root.accessibleName
    Accessible.focusable: root.enabled
    Accessible.onPressAction: if (root.enabled) root.clicked()

    // Hand the focus back before leaving the tab chain - AppButton's own
    // comment explains why Qt needs this.
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
        text: root.glyph
        font.family: Theme.iconFamily
        font.pixelSize: Theme.iconSize
        color: root.primary ? Theme.accentText : Theme.text
    }
    MouseArea {
        anchors.fill: parent
        enabled: root.enabled
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
    FocusRing {}
}

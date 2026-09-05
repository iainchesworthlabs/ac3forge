import QtQuick
import QtQuick.Layouts

import Ac3ForgeCrucible

// A checkbox with an optional note beneath, drawn to the mockups: a 16 px
// square that fills with the accent when checked.
Item {
    id: root
    property string text: ""
    property string note: ""
    property bool checked: false
    property bool enabled: true
    signal toggled(bool checked)

    implicitWidth: column.implicitWidth
    implicitHeight: column.implicitHeight
    opacity: enabled ? 1.0 : 0.45
    Accessible.role: Accessible.CheckBox
    Accessible.name: root.text
    Accessible.checked: root.checked
    Accessible.focusable: root.enabled
    Accessible.onPressAction: if (root.enabled) root.toggled(!root.checked)

    activeFocusOnTab: root.enabled
    Keys.onPressed: function(event) {
        if (!root.enabled) {
            return;
        }
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            root.toggled(!root.checked);
            event.accepted = true;
        }
    }

    ColumnLayout {
        id: column
        width: root.width
        spacing: Theme.space1
        RowLayout {
            spacing: 10
            Rectangle {
                width: 16
                height: 16
                color: root.checked ? Theme.accent : "transparent"
                border.color: Theme.divider
                border.width: 1
                Canvas {
                    id: tick
                    anchors.fill: parent
                    visible: root.checked
                    onPaint: {
                        const c = getContext("2d");
                        c.clearRect(0, 0, width, height);
                        c.strokeStyle = Theme.bg;
                        c.lineWidth = 1.8;
                        c.beginPath();
                        c.moveTo(3, 8.5);
                        c.lineTo(6.5, 12);
                        c.lineTo(13, 4);
                        c.stroke();
                    }
                    Connections { target: Theme; function onBgChanged() { tick.requestPaint(); } }
                }
                // Around the box rather than the whole row: the box is what
                // the key presses, and a ring around three lines of note
                // text would say the note had focus.
                FocusRing { active: root.activeFocus }
            }
            Text { text: root.text; color: Theme.text; font.pixelSize: Theme.fontNormal }
        }
        Text {
            visible: root.note.length > 0
            text: root.note
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            Layout.leftMargin: 26
        }
    }
    MouseArea {
        anchors.fill: parent
        enabled: root.enabled
        cursorShape: root.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: root.toggled(!root.checked)
    }
}

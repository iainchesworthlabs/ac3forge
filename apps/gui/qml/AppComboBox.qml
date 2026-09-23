import QtQuick
import QtQuick.Controls as QQC

import Ac3Forge

// A combo box drawn to the design system (components.png, "FIELDS AND
// LISTS"): the same 30 px neutral100 box AppTextField draws, with a text
// chevron rather than Basic's image indicator, and an accent border while it
// has the keyboard.
//
// Basic's own non-editable ComboBox is the furthest of any native control
// from this sheet: a flat palette.button block with NO border at rest. This
// is Crucible's own already-styled combo (apps/crucible/ui/qml/
// SettingsPage.qml's language box) lifted into the shared set, since Hearth's
// Settings, Decoder, Media and Speakers pages all need the same one.
QQC.ComboBox {
    id: control

    implicitHeight: Math.max(30, contentText.implicitHeight + 10)
    font.pixelSize: Theme.fontBody

    background: Rectangle {
        color: Theme.neutral100
        border.color: control.activeFocus ? Theme.focusRing : Theme.divider
        border.width: 1
        radius: Theme.radius
    }

    // The padding swaps with the chevron under right-to-left mirroring.
    contentItem: Text {
        id: contentText
        leftPadding: control.mirrored ? 26 : Theme.space3
        rightPadding: control.mirrored ? Theme.space3 : 26
        text: control.displayText
        color: control.enabled ? Theme.text : Theme.textMuted
        font: control.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Text {
        anchors.right: parent.right
        anchors.rightMargin: Theme.space3
        anchors.verticalCenter: parent.verticalCenter
        text: "⌄"
        color: Theme.textMuted
        font.pixelSize: Theme.fontNormal
    }

    popup.background: Rectangle {
        color: Theme.surface
        border.color: Theme.divider
        border.width: 1
        radius: Theme.radius
    }

    delegate: QQC.ItemDelegate {
        required property var modelData
        required property int index
        width: control.width
        highlighted: control.highlightedIndex === index
        contentItem: Text {
            text: control.textRole.length > 0 && typeof modelData === "object"
                  ? modelData[control.textRole] : modelData
            color: Theme.text
            font.pixelSize: Theme.fontBody
            elide: Text.ElideRight
        }
        background: Rectangle { color: highlighted ? Theme.neutral200 : "transparent" }
    }
}

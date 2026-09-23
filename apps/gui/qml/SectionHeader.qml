import QtQuick
import QtQuick.Layouts

import Ac3Forge

// A label, a rule filling the remaining width, and an optional right-aligned
// caption - Card's own title row (main-play.png's "02 NOW PLAYING ————"),
// factored out so a section that isn't inside a Card, like PlayPage.qml's
// Queue panel, can draw the identical header without duplicating it.
//
// The ordinal is its own run rather than part of the label: the design draws
// it in accent ink and a fixed-width face while the words beside it are
// full-strength text (sampled off main-play.png: "02" is #ae1800, which is
// accentInk in the signal light palette, and "NOW PLAYING" is #201e1d, which
// is text - not textMuted, which this header used to draw both in). Keeping
// it out of the label also keeps a bare number out of the translated string.
RowLayout {
    id: root

    property alias label: labelText.text
    property alias summary: summaryText.text
    // "01", "02"... Empty for a section the design does not number.
    property string ordinal: ""

    Layout.fillWidth: true
    spacing: Theme.gap
    visible: labelText.text.length > 0

    Text {
        id: ordinalText
        visible: root.ordinal.length > 0
        text: root.ordinal
        color: Theme.accentInk
        font.family: Theme.monoFamily
        font.pixelSize: Theme.fontSmall
        font.bold: true
    }

    Text {
        id: labelText
        color: Theme.text
        font.pixelSize: Theme.fontSmall
        font.bold: true
        font.capitalization: Font.AllUppercase
        font.letterSpacing: Theme.trackingWide
    }

    // Two pixels, not one: the card borders around it are single-pixel, and
    // the design draws this rule heavier than them on purpose (measured on
    // main-play.png).
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 2
        color: Theme.divider
    }

    Text {
        id: summaryText
        visible: text.length > 0
        color: Theme.textMuted
        font.family: Theme.monoFamily
        font.pixelSize: Theme.fontSmall
    }
}

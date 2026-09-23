import QtQuick
import QtQuick.Layouts

import Ac3Forge

// A label, a rule filling the remaining width, and an optional right-aligned
// caption - Card's own title row (main-play.png's "02 NOW PLAYING ————"),
// factored out so a section that isn't inside a Card, like PlayPage.qml's
// Queue panel, can draw the identical header without duplicating it.
RowLayout {
    id: root

    property alias label: labelText.text
    property alias summary: summaryText.text

    Layout.fillWidth: true
    spacing: Theme.gap
    visible: labelText.text.length > 0

    Text {
        id: labelText
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
        font.bold: true
        font.capitalization: Font.AllUppercase
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        color: Theme.divider
    }

    Text {
        id: summaryText
        visible: text.length > 0
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
    }
}

import QtQuick
import QtQuick.Layouts

import Ac3Forge

// A titled panel. Children are laid out vertically inside `content`.
Rectangle {
    id: root

    property alias title: heading.text
    // An optional secondary title, right of the main one - e.g. Hearth's
    // Decoder page names the playing file here (DecoderEac3.qml's "03 This
    // stream" card). Empty and invisible by default, the same as an unset
    // title.
    property alias subtitle: subheading.text
    default property alias content: column.data

    color: Theme.surface
    border.color: Theme.border
    border.width: 1
    radius: Theme.radius
    Layout.fillWidth: true
    implicitHeight: layout.implicitHeight + Theme.pad * 2

    // A titled group of controls - the same "what am I looking at" question
    // the heading answers visually, given to a screen reader too. Reading
    // root.title directly (rather than heading.visible/heading.text) is
    // deliberate, not just simpler - an untitled Card already produces "",
    // the same "no name worth announcing" outcome heading.visible's
    // text.length > 0 condition means, without a second property to depend
    // on for change notification.
    Accessible.role: Accessible.Grouping
    Accessible.name: root.title

    ColumnLayout {
        id: layout
        anchors.fill: parent
        anchors.margins: Theme.pad
        spacing: Theme.gap

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gap
            visible: heading.visible || subheading.visible

            Text {
                id: heading
                Layout.fillWidth: true
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                font.bold: true
                font.capitalization: Font.AllUppercase
                visible: text.length > 0
            }
            Text {
                id: subheading
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                font.family: Theme.monoFamily
                visible: text.length > 0
            }
        }

        ColumnLayout {
            id: column
            Layout.fillWidth: true
            spacing: Theme.gap
        }
    }
}

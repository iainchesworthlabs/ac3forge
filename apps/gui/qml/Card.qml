import QtQuick
import QtQuick.Layouts

import Ac3Forge

// A titled panel. Children are laid out vertically inside `content`.
Rectangle {
    id: root

    property alias title: header.label
    // A short right-aligned caption on the title's own row, e.g. "Onkyo
    // receiver · 8 outputs" - the handoff draws a rule between it and the
    // title on every card, whether or not a summary is set (an untitled
    // summary just leaves the rule running to the card's edge).
    property alias summary: header.summary
    // Drops the outer fill/border so the header and content sit flat on
    // whatever panel this Card is placed on, keeping only the title/rule
    // header - the design boxes individual stats and steps (StatTile,
    // PlaySignalPathCard's own decode/render/output boxes), never a whole
    // section (main-play.png: "02 NOW PLAYING" through "07 SIGNAL PATH" are
    // flat). Off by default so every existing boxed Card is unaffected.
    property bool flat: false
    default property alias content: column.data

    color: root.flat ? "transparent" : Theme.surface
    border.color: Theme.border
    border.width: root.flat ? 0 : 1
    radius: Theme.radius
    Layout.fillWidth: true
    implicitHeight: layout.implicitHeight + (root.flat ? 0 : Theme.pad * 2)

    // A titled group of controls - the same "what am I looking at" question
    // the heading answers visually, given to a screen reader too. Reading
    // root.title directly (rather than header.visible/header.label) is
    // deliberate, not just simpler - an untitled Card already produces "",
    // the same "no name worth announcing" outcome SectionHeader's own
    // text.length > 0 visibility condition means, without a second property
    // to depend on for change notification.
    Accessible.role: Accessible.Grouping
    Accessible.name: root.title

    ColumnLayout {
        id: layout
        anchors.fill: parent
        anchors.margins: root.flat ? 0 : Theme.pad
        spacing: Theme.gap

        SectionHeader {
            id: header
        }

        ColumnLayout {
            id: column
            Layout.fillWidth: true
            spacing: Theme.gap
        }
    }
}

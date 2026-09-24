import QtQuick
import QtQuick.Layouts

import Ac3Forge

// A titled panel. Children are laid out vertically inside `content`.
//
// Three shapes, because the family draws three:
//
//   default    one box around the header AND the content. ac3gui's older
//              shape; left as the default so that app is untouched.
//   flat       no box at all, just the header and the content under it -
//              the Play page's monitor column (main-play.png).
//   framed     the header sits ABOVE the box on the page background and the
//              box wraps only the content - what every other Hearth page's
//              design shows (settings.png, speakers-setup.png, media-*.png,
//              decoder-*.png all measure the rule ~19 px above the box's
//              top border).
Rectangle {
    id: root

    property alias title: header.label
    // A short right-aligned caption on the title's own row, e.g. "Onkyo
    // receiver · 8 outputs" - the handoff draws a rule between it and the
    // title on every card, whether or not a summary is set (an untitled
    // summary just leaves the rule running to the card's edge).
    property alias summary: header.summary
    // "01", "02"... drawn in accent ink beside the title, not folded into it.
    property alias ordinal: header.ordinal
    // Off for a card nested inside a section that already drew one.
    property alias rule: header.rule
    // Drops the outer fill/border so the header and content sit flat on
    // whatever panel this Card is placed on, keeping only the title/rule
    // header. Off by default so every existing boxed Card is unaffected.
    property bool flat: false
    // Header above, box around the content only. Off by default for the
    // same reason.
    property bool framed: false
    default property alias content: column.data

    readonly property bool _outerBox: !root.flat && !root.framed

    color: root._outerBox ? Theme.surface : "transparent"
    border.color: Theme.border
    border.width: root._outerBox ? 1 : 0
    radius: Theme.radius
    Layout.fillWidth: true
    implicitHeight: layout.implicitHeight + (root._outerBox ? Theme.pad * 2 : 0)

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
        anchors.margins: root._outerBox ? Theme.pad : 0
        spacing: Theme.gap

        SectionHeader {
            id: header
        }

        // Invisible unless framed, in which case this is the box the design
        // draws under the header.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: column.implicitHeight + (root.framed ? Theme.pad * 2 : 0)
            color: root.framed ? Theme.surface : "transparent"
            border.color: Theme.border
            border.width: root.framed ? 1 : 0
            radius: Theme.radius

            ColumnLayout {
                id: column
                anchors.fill: parent
                anchors.margins: root.framed ? Theme.pad : 0
                spacing: Theme.gap
            }
        }
    }
}

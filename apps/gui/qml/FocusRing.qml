import QtQuick

import Ac3Forge

// The ring that says where the keyboard is. Put one inside any hand-drawn
// control and it draws just outside that control's own border while the
// control has active focus:
//
//     Rectangle { ... CrucibleButton's fill and border ...
//         FocusRing {}
//     }
//
// Zero radius and the accent-derived focusRing colour, per the design
// system; the offset keeps it clear of the control's 1 px border instead of
// replacing it, so a focused control still reads as the control it was.
//
// `active` is the parent's active focus by default. A control whose ring
// belongs to something other than its immediate parent - a ListView, whose
// visual children are reparented into its scrolling content item, so the
// ring goes on a wrapper instead - overrides it.
//
// The ring shows for ANY active focus, not only for focus that arrived by
// Tab: a mouse click on a button or a segment does not take focus here (the
// MouseAreas do not ask for it), so a mouse user sees no rings, while a
// click on the application list or a room marker DOES take focus, because
// that is where the arrow keys continue from, and a ring there is the truth.
Rectangle {
    id: ring

    property bool active: ring.parent ? ring.parent.activeFocus : false

    objectName: "focusRing"
    anchors.fill: parent
    anchors.margins: -Theme.focusRingOffset
    visible: ring.active
    color: "transparent"
    border.color: Theme.focusRing
    border.width: Theme.focusRingWidth
    // Above the control's own content, and above a sibling marker or chip
    // that would otherwise be drawn over it.
    z: 100
}

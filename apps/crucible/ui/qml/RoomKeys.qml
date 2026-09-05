import QtQuick
import QtQuick.Layouts

import Ac3ForgeCrucible

// The room's keyboard: one tab stop that moves whichever application is
// selected, wrapped around the room views so that the ring says the keys
// are here.
//
// View-independent on purpose. The plan, the elevation and the 3D picture
// draw the same room, so a key means the same thing whichever is on screen:
// Left and Right move across, Up and Down move front to back (the plan's own
// axis, and the elevation's), Page Up and Page Down move up and down.
// Nothing here asks which picture is showing.
//
// Steps: 0.05 of the room, 0.01 with Shift, 0.25 with Control - one press,
// a nudge, or a quarter of the room. The commands go out as signals; the
// page turns them into controller calls, which is what lets a test drive
// this with a fake application and no engine.
FocusScope {
    id: keys

    // The room views. Held in a ColumnLayout so the two pictures keep their
    // Layout attached properties, exactly as they had them one level up.
    default property alias content: inner.data

    // The selected application - a live AppEntry, or a look-alike in a test.
    property var app: null
    // False while the stream is the 5.1 bed only, when height carries
    // nothing; the moves still happen, and the announcement says so once.
    property bool objectsEnabled: true

    readonly property real step: 0.05
    readonly property real fineStep: 0.01     // Shift
    readonly property real coarseStep: 0.25   // Control

    signal moved(int app, double x, double y, double z)
    signal placed(int app)
    signal returned(int app)
    signal resized(int app, double size)
    signal announced(string text)

    activeFocusOnTab: true
    Accessible.role: Accessible.Grouping
    Accessible.focusable: true
    Accessible.name: qsTr("Room")
    Accessible.description: keys.app
        ? qsTr("%1 selected. Arrow keys move it, Page Up and Page Down change its height, Home recentres it, Enter places it, Delete returns it to the bed, plus and minus change its size.").arg(keys.app.name)
        : qsTr("No application selected: choose one in the applications list.")

    implicitWidth: inner.implicitWidth
    implicitHeight: inner.implicitHeight

    // Where the last press asked for this application to go. The engine's
    // own answer arrives on the next poll, 60 ms later, so two presses
    // inside one poll would both start from the same stale position and the
    // second would undo the first. Forgotten when the selection changes, and
    // never used for an application that is not in the room.
    property int targetApp: -1
    property real targetX: 0.5
    property real targetY: 0.5
    property real targetZ: 0.0
    onAppChanged: keys.targetApp = -1

    function clamp(value, low, high) {
        return Math.max(low, Math.min(high, value));
    }

    // Where a move starts from: the pending target when there is one for
    // this application, the engine's position otherwise, and the centre of
    // the room for an application that is still in the bed - so one arrow
    // press both places it and moves it.
    function origin() {
        if (!keys.app || keys.app.slot < 0) {
            return { x: 0.5, y: 0.5, z: 0.0 };
        }
        if (keys.targetApp === keys.app.app) {
            return { x: keys.targetX, y: keys.targetY, z: keys.targetZ };
        }
        return { x: keys.app.x, y: keys.app.y, z: keys.app.z };
    }

    function send(x, y, z) {
        keys.targetX = keys.clamp(x, 0, 1);
        keys.targetY = keys.clamp(y, 0, 1);
        keys.targetZ = keys.clamp(z, -1, 1);
        keys.targetApp = keys.app.app;
        keys.moved(keys.app.app, keys.targetX, keys.targetY, keys.targetZ);
        keys.announced(qsTr("%1: %2, %3").arg(keys.app.name)
            .arg(RoomWords.describe(keys.targetX, keys.targetY, keys.targetZ))
            .arg(RoomWords.coords(keys.targetX, keys.targetY, keys.targetZ)));
    }

    function nudge(dx, dy, dz, modifiers) {
        if (!keys.app) {
            keys.announced(qsTr("No application selected"));
            return;
        }
        if (keys.app.fullscreen) {
            keys.announced(qsTr("%1 is full-screen and stays in the bed").arg(keys.app.name));
            return;
        }
        const size = (modifiers & Qt.ShiftModifier) ? keys.fineStep
                   : (modifiers & Qt.ControlModifier) ? keys.coarseStep
                   : keys.step;
        const from = keys.origin();
        if (dz !== 0 && !keys.objectsEnabled) {
            keys.announced(qsTr("height has no effect while the stream is bed only"));
        }
        keys.send(from.x + dx * size, from.y + dy * size, from.z + dz * size);
    }

    function resizeBy(by) {
        if (!keys.app) {
            return;
        }
        keys.resized(keys.app.app, keys.clamp(keys.app.size + by, 0, 1));
    }

    Keys.onPressed: function(event) {
        switch (event.key) {
        case Qt.Key_Left: keys.nudge(-1, 0, 0, event.modifiers); break;
        case Qt.Key_Right: keys.nudge(1, 0, 0, event.modifiers); break;
        // y 0 is the front of the room, so Up is towards the front.
        case Qt.Key_Up: keys.nudge(0, -1, 0, event.modifiers); break;
        case Qt.Key_Down: keys.nudge(0, 1, 0, event.modifiers); break;
        case Qt.Key_PageUp: keys.nudge(0, 0, 1, event.modifiers); break;
        case Qt.Key_PageDown: keys.nudge(0, 0, -1, event.modifiers); break;
        case Qt.Key_Home:
            if (keys.app && !keys.app.fullscreen) {
                keys.send(0.5, 0.5, 0);
            }
            break;
        case Qt.Key_Return:
        case Qt.Key_Enter:
        case Qt.Key_Space:
            if (!keys.app) {
                keys.announced(qsTr("No application selected"));
                break;
            }
            if (keys.app.fullscreen) {
                keys.announced(qsTr("%1 is full-screen and stays in the bed").arg(keys.app.name));
            } else if (keys.app.slot < 0) {
                keys.placed(keys.app.app);
                keys.announced(qsTr("%1 placed in the centre of the room").arg(keys.app.name));
            } else {
                // Already placed: Enter says where it is rather than moving it.
                keys.announced(qsTr("%1: %2").arg(keys.app.name)
                    .arg(RoomWords.describe(keys.app.x, keys.app.y, keys.app.z)));
            }
            break;
        case Qt.Key_Delete:
        case Qt.Key_Backspace:
            if (keys.app && keys.app.slot >= 0) {
                keys.returned(keys.app.app);
                keys.targetApp = -1;
                keys.announced(qsTr("%1 returned to the bed").arg(keys.app.name));
            }
            break;
        case Qt.Key_Plus:
        case Qt.Key_Equal:
            keys.resizeBy(0.05);
            break;
        case Qt.Key_Minus:
            keys.resizeBy(-0.05);
            break;
        default:
            return;
        }
        event.accepted = true;
    }

    ColumnLayout {
        id: inner
        anchors.fill: parent
        spacing: Theme.space4
    }

    FocusRing {}
}

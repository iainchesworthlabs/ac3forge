pragma Singleton

import QtQuick

// What the window says out loud.
//
// A screen reader is told about a change through an announcement, and Qt
// exposes that as Accessible.announce on an item inside a window (6.8).
// Anything in the window can want to announce something - the room's keys
// after a nudge, the shell after the engine started or the endpoint moved -
// and none of them has an item to hand, so they call this and the shell's
// one announcer item relays it (Main.qml). Tests watch `announced` and read
// `lastMessage`, which is also what the announcer item reports as its name,
// so the last thing said is readable rather than gone.
QtObject {
    id: a11y

    // The last message, and a counter that ticks even when the same text is
    // said twice, so a test can tell a repeat from a silence.
    property string lastMessage: ""
    property int serial: 0
    signal announced(string text, bool assertive)

    // QAccessible::AnnouncementPoliteness, by value rather than by name:
    // the enum is a scoped one reached through QML_EXTENDED_NAMESPACE, and
    // its QML spelling has changed between Qt versions while the values
    // have not. Polite waits for the reader to finish; assertive interrupts,
    // which is for an error and nothing else.
    readonly property int polite: 0
    readonly property int assertive: 1

    // Say something. Empty text says nothing at all rather than interrupting
    // a reader with silence.
    function announce(text, urgent) {
        if (!text || text.length === 0) {
            return;
        }
        a11y.lastMessage = text;
        a11y.serial = a11y.serial + 1;
        a11y.announced(text, urgent === true);
    }
}

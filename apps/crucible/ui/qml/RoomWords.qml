pragma Singleton

import QtQuick

// A position, in words and in numbers, in one place.
//
// The room card, the application rows, the markers a screen reader reads
// and the announcement the keyboard makes after every nudge all say where
// something is. They said it in four hand-written spellings, so a reader
// heard one form, the row showed another and the card a third. They come
// from here now, and a change to the wording changes all of them.
//
// Room coordinates are ac3::oba::Position's: x 0..1 left to right, y 0..1
// front to back, z -1..1 floor to ceiling.
QtObject {
    id: words

    // Plain words for a position: "up, in front of you, to the left".
    // Height and the left-right half are dropped when they say nothing -
    // ear level, and the middle third of the room - so what is left is what
    // is worth hearing.
    //
    // The floor is nine whole phrases (onTheFloor below) rather than a
    // depth with a side glued to it, so a language that inflects the one
    // inside the other, or says them the other way round, writes each of
    // the nine as its own sentence. Height frames what the floor gives,
    // which leaves two more strings instead of eighteen.
    function describe(x, y, z) {
        const placed = words.onTheFloor(x, y);
        if (z > 0.3) {
            //: %1 is a position ("in front of you, to the left"); this adds that it is above ear level
            return qsTr("up, %1").arg(placed);
        }
        if (z < -0.3) {
            //: %1 is a position ("in front of you, to the left"); this adds that it is below ear level
            return qsTr("low, %1").arg(placed);
        }
        return placed;
    }

    // Where something is on the floor of the room, as a screen reader hears
    // it: the front, middle or back third, and the left, middle or right
    // third, in one phrase each.
    function onTheFloor(x, y) {
        const front = y < 0.35;
        const back = y > 0.65;
        if (x < 0.35) {
            //: Position in the room: ahead of the listener and to their left
            if (front) { return qsTr("in front of you, to the left"); }
            //: Position in the room: behind the listener and to their left
            if (back) { return qsTr("behind you, to the left"); }
            //: Position in the room: level with the listener, to their left
            return qsTr("beside you, to the left");
        }
        if (x > 0.65) {
            //: Position in the room: ahead of the listener and to their right
            if (front) { return qsTr("in front of you, to the right"); }
            //: Position in the room: behind the listener and to their right
            if (back) { return qsTr("behind you, to the right"); }
            //: Position in the room: level with the listener, to their right
            return qsTr("beside you, to the right");
        }
        //: Position in the room: ahead of the listener, neither left nor right
        if (front) { return qsTr("in front of you"); }
        //: Position in the room: behind the listener, neither left nor right
        if (back) { return qsTr("behind you"); }
        //: Position in the room: level with the listener, neither left nor right
        return qsTr("beside you");
    }

    // The same position as figures, labelled: "x 0.50 · y 0.50 · z +0.00".
    // The card and the announcements use this.
    function coords(x, y, z) {
        return qsTr("x %1 · y %2 · z %3").arg(x.toFixed(2)).arg(y.toFixed(2)).arg(words.signed(z));
    }

    // The same figures without labels, for the application rows, where the
    // line is already narrow and elides: "0.50, 0.50, +0.00".
    function numbers(x, y, z) {
        return x.toFixed(2) + ", " + y.toFixed(2) + ", " + words.signed(z);
    }

    // Height carries its sign, so "+0.00" and "-0.40" line up and a reader
    // hears which way from ear level it is.
    function signed(z) {
        return (z >= 0 ? "+" : "") + z.toFixed(2);
    }

    // Where an application sits: which slot it holds, or the bed.
    //
    // The three places that show both (the room card, an application row, a
    // marker's description) put this and coords() or numbers() either side
    // of a " · ". That separator is punctuation between two whole
    // phrases rather than a sentence assembled from words, so it is not
    // itself translated; folding the pair into one string would take the
    // same phrase away from the other two callers.
    function placement(app) {
        if (!app || app.slot < 0) {
            return qsTr("in the bed");
        }
        return app.width === 2
            ? qsTr("slots %1+%2").arg(app.slot + 1).arg(app.slot + 2)
            : qsTr("slot %1").arg(app.slot + 1);
    }
}

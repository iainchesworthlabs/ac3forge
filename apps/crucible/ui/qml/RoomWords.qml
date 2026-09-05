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
    function describe(x, y, z) {
        const centred = x >= 0.35 && x <= 0.65;
        const side = x < 0.35 ? qsTr("left") : qsTr("right");
        const depth = y < 0.35 ? qsTr("in front of you")
                    : y > 0.65 ? qsTr("behind you")
                    : qsTr("beside you");
        const height = z > 0.3 ? qsTr("up") : z < -0.3 ? qsTr("low") : "";
        return [height, depth, centred ? "" : qsTr("to the %1").arg(side)]
            .filter(function(part) { return part.length > 0; })
            .join(", ");
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
    function placement(app) {
        if (!app || app.slot < 0) {
            return qsTr("in the bed");
        }
        return app.width === 2
            ? qsTr("slots %1+%2").arg(app.slot + 1).arg(app.slot + 2)
            : qsTr("slot %1").arg(app.slot + 1);
    }
}

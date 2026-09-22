import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3ForgeHearth

// Pinned to the bottom of every page, the way every round-1 artboard shows
// it (planning/hearth-design.md). None of these buttons is inside a
// Repeater, so the native-Button-in-a-Repeater hang the family has hit
// elsewhere does not apply here.
Rectangle {
    color: Theme.surface
    border.color: Theme.border
    border.width: 1
    implicitHeight: 48

    // mm:ss, the shape every other duration readout in the family uses
    // (apps/gui/qml/Main.qml's own formatTime()) and what the design's
    // footer shows (docs/hearth/design/screenshots/main-play.png) - no hour
    // rollover, since nothing this app plays runs that long.
    function formatMs(ms) {
        const totalSeconds = Math.max(0, Math.floor(ms / 1000));
        const mm = Math.floor(totalSeconds / 60);
        const ss = totalSeconds % 60;
        return String(mm).padStart(2, "0") + ":" + String(ss).padStart(2, "0");
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.pad
        anchors.rightMargin: Theme.pad
        spacing: Theme.gap

        Button {
            objectName: "transportPrevious"
            text: qsTr("Previous")
            enabled: HearthController.currentIndex > 0
            onClicked: HearthController.previous()
        }
        Button {
            objectName: "transportPlayPause"
            text: HearthController.playing ? qsTr("Pause") : qsTr("Play")
            enabled: HearthController.queue.length > 0
            onClicked: HearthController.playing ? HearthController.pause() : HearthController.play()
        }
        Button {
            objectName: "transportStop"
            text: qsTr("Stop")
            enabled: HearthController.state !== "stopped"
            onClicked: HearthController.stop()
        }
        Button {
            objectName: "transportNext"
            text: qsTr("Next")
            enabled: HearthController.currentIndex >= 0 &&
                     HearthController.currentIndex + 1 < HearthController.queue.length
            onClicked: HearthController.next()
        }

        Text {
            objectName: "transportElapsed"
            text: formatMs(HearthController.positionMs)
            color: Theme.textMuted
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontSmall
        }

        Slider {
            id: scrubber
            objectName: "transportPosition"
            Layout.fillWidth: true
            from: 0
            to: Math.max(1, HearthController.durationMs)
            stepSize: 1000
            enabled: HearthController.durationMs > 0
            value: HearthController.positionMs

            // `value` is a plain binding, re-synced explicitly from the
            // Connections below rather than trusted to survive a drag
            // (Slider's own drag handling writes `value` directly, which
            // breaks a declarative binding on it for good) - the same shape
            // apps/gui/qml/StreamPlayerDialog.qml's own scrub slider uses,
            // and for the same reason: pausing for the drag's duration is
            // what stops that resync fighting the user, since positionMs
            // then only moves in response to seek() below. play() resumes
            // it afterwards if it was playing when the drag began - a seek
            // is legal whichever state the transport is in, and leaves that
            // state standing (Player::seek()'s own comment).
            property bool resumeOnRelease: false
            onPressedChanged: {
                if (pressed) {
                    resumeOnRelease = HearthController.playing;
                    HearthController.pause();
                } else {
                    HearthController.seek(value);
                    if (resumeOnRelease) {
                        HearthController.play();
                    }
                }
            }
            onMoved: HearthController.seek(value)

            Accessible.name: qsTr("Position")
            Accessible.description: qsTr("%1 of %2")
                .arg(formatMs(HearthController.positionMs))
                .arg(formatMs(HearthController.durationMs))
        }
        Connections {
            target: HearthController
            function onPositionChanged() { scrubber.value = HearthController.positionMs; }
        }

        Text {
            objectName: "transportRemaining"
            text: formatMs(Math.max(0, HearthController.durationMs - HearthController.positionMs))
            color: Theme.textMuted
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontSmall
        }

        Text {
            objectName: "transportState"
            Layout.preferredWidth: 180
            text: HearthController.errorText.length > 0 ? HearthController.errorText
                  : HearthController.noteText.length > 0 ? HearthController.noteText
                  : HearthController.state
            color: HearthController.errorText.length > 0 ? Theme.bad : Theme.textMuted
            font.pixelSize: Theme.fontSmall
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }

        CheckBox {
            objectName: "transportGapless"
            text: qsTr("Gapless")
            checked: HearthController.gapless
            onToggled: HearthController.gapless = checked
        }
    }
}

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

        // The design system's own transport row (components.png, "TRANSPORT"):
        // square icon buttons, the play/pause one accent-filled as the
        // primary action and the rest drawn as plain outlines. IconButton
        // rather than a native Button, which Basic draws as a wide flat fill
        // with no border and sized to a word rather than a glyph.
        IconButton {
            objectName: "transportPrevious"
            glyph: Theme.iconSkipPrevious
            enabled: HearthController.currentIndex > 0
            onClicked: HearthController.previous()
            accessibleName: qsTr("Previous")
        }
        IconButton {
            objectName: "transportPlayPause"
            glyph: HearthController.playing ? Theme.iconPause : Theme.iconPlayArrow
            primary: true
            enabled: HearthController.queue.length > 0
            onClicked: HearthController.playing ? HearthController.pause() : HearthController.play()
            accessibleName: HearthController.playing ? qsTr("Pause") : qsTr("Play")
        }
        IconButton {
            objectName: "transportStop"
            glyph: Theme.iconStop
            enabled: HearthController.state !== "stopped"
            onClicked: HearthController.stop()
            accessibleName: qsTr("Stop")
        }
        IconButton {
            objectName: "transportNext"
            glyph: Theme.iconSkipNext
            enabled: HearthController.currentIndex >= 0 &&
                     HearthController.currentIndex + 1 < HearthController.queue.length
            onClicked: HearthController.next()
            accessibleName: qsTr("Next")
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

            // A slim progress bar with a tick handle (main-play.png's own
            // footer), not QQC2 Basic's stock groove-and-circle - the same
            // reason every custom-shaped control in this family draws its
            // own look rather than leaving a native control's default (this
            // file's own header comment on the plain-Rectangle queue-row
            // idiom, though here the native Slider is kept for its real
            // drag/keyboard handling and only its two visual delegates are
            // replaced).
            background: Rectangle {
                x: scrubber.leftPadding
                y: scrubber.topPadding + scrubber.availableHeight / 2 - height / 2
                width: scrubber.availableWidth
                height: 4
                color: Theme.neutral300

                Rectangle {
                    width: scrubber.visualPosition * parent.width
                    height: parent.height
                    color: Theme.accent
                }
            }
            handle: Rectangle {
                x: scrubber.leftPadding + scrubber.visualPosition * (scrubber.availableWidth - width)
                y: scrubber.topPadding + scrubber.availableHeight / 2 - height / 2
                width: 4
                height: 16
                color: Theme.text
            }
        }
        Connections {
            target: HearthController
            function onPositionChanged() { scrubber.value = HearthController.positionMs; }
        }

        // The item's whole length, not what is left of it - the design's
        // footer reads "03:12 ... 12:14" against a 12:14 item
        // (main-play.png), elapsed beside total, the pair every transport
        // shows.
        Text {
            objectName: "transportDuration"
            text: formatMs(HearthController.durationMs)
            color: Theme.textMuted
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontSmall
        }

        // Only when there is something to say. The design's footer carries no
        // running state word at all (main-play.png), and "playing" beside a
        // pause button that already shows the same thing is noise - but an
        // error or a note still needs somewhere to land, so this keeps those
        // and drops only the idle state.
        Text {
            objectName: "transportState"
            visible: text.length > 0
            Layout.preferredWidth: visible ? 180 : 0
            text: HearthController.errorText.length > 0 ? HearthController.errorText
                  : HearthController.noteText
            color: HearthController.errorText.length > 0 ? Theme.bad : Theme.textMuted
            font.pixelSize: Theme.fontSmall
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }

        // The design draws gapless as a small outlined chip, not a ticked
        // check box (main-play.png) - the box was reading as a stray Windows
        // control next to the icons. Still a real toggle, and still announced
        // as one; "off" dims it the same way every other disabled or
        // unchosen control in this design system dims.
        Rectangle {
            objectName: "transportGapless"
            implicitWidth: gaplessLabel.implicitWidth + 16
            implicitHeight: Math.max(18, gaplessLabel.implicitHeight + 6)
            color: "transparent"
            border.color: Theme.divider
            border.width: 1
            opacity: HearthController.gapless ? 1.0 : 0.45

            Accessible.role: Accessible.CheckBox
            Accessible.name: qsTr("Gapless")
            Accessible.checked: HearthController.gapless
            Accessible.focusable: true
            Accessible.onPressAction: HearthController.gapless = !HearthController.gapless

            activeFocusOnTab: true
            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    HearthController.gapless = !HearthController.gapless;
                    event.accepted = true;
                }
            }

            Text {
                id: gaplessLabel
                anchors.centerIn: parent
                text: qsTr("Gapless")
                color: Theme.textMuted
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontMicro
                font.capitalization: Font.AllUppercase
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: HearthController.gapless = !HearthController.gapless
            }
            FocusRing {}
        }

        // The transport bar's master volume - one gain applied after
        // everything else (Player::take_block()), not the per-speaker trim
        // on the Speakers page. -60..0 dB matches Player::kMinVolumeDb/
        // kMaxVolumeDb; a literal here rather than a shared binding, the way
        // Speakers.qml's own DoubleValidator ranges already are.
        Text {
            text: Theme.iconVolumeUp
            font.family: Theme.iconFamily
            font.pixelSize: Theme.iconSize
            color: Theme.textMuted
        }
        Slider {
            id: volumeSlider
            objectName: "transportVolume"
            Layout.preferredWidth: 120
            from: -60
            to: 0
            value: HearthController.volumeDb
            // A plain binding on `value` does not survive a drag - Slider's
            // own drag handling writes it directly, which breaks the binding
            // for good (StreamPlayerDialog.qml's scrub slider has the same
            // comment) - so it is resynced explicitly below rather than
            // trusted to still be bound after the first move.
            onMoved: HearthController.setVolumeDb(value)
            Accessible.name: qsTr("Volume")

            // Same slim-bar-and-tick look as the scrubber above, not QQC2
            // Basic's stock groove-and-circle.
            background: Rectangle {
                x: volumeSlider.leftPadding
                y: volumeSlider.topPadding + volumeSlider.availableHeight / 2 - height / 2
                width: volumeSlider.availableWidth
                height: 4
                color: Theme.neutral300

                Rectangle {
                    width: volumeSlider.visualPosition * parent.width
                    height: parent.height
                    color: Theme.accent
                }
            }
            handle: Rectangle {
                x: volumeSlider.leftPadding + volumeSlider.visualPosition * (volumeSlider.availableWidth - width)
                y: volumeSlider.topPadding + volumeSlider.availableHeight / 2 - height / 2
                width: 4
                height: 16
                color: Theme.text
            }
        }
        Connections {
            target: HearthController
            function onStateChanged() { volumeSlider.value = HearthController.volumeDb; }
        }
        Text {
            objectName: "transportVolumeReadout"
            Layout.preferredWidth: 56
            horizontalAlignment: Text.AlignRight
            text: qsTr("%1 dB").arg(HearthController.volumeDb.toFixed(1))
            color: Theme.textMuted
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontSmall
        }
    }
}

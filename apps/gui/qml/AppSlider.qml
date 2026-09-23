import QtQuick
import QtQuick.Controls as QQC

import Ac3Forge

// A slider drawn to the design system (components.png, "SLIDERS · REST AND
// FOCUSED"): a 4 px rail, the travelled part in accent, and a thin upright
// tick for the handle - not Basic's stock groove and circle.
//
// A real QQC2 Slider with its two delegates replaced, rather than a
// hand-drawn control: the drag, the keyboard steps and the accessibility all
// come free and are worth more than the shape, which is all Basic actually
// gets wrong. Basic ignores `handle`/`background` on a NATIVE style, so the
// window has to be on a non-native style for this to take - Hearth's
// main.cpp already calls QQuickStyle::setStyle("Basic").
//
// `value` does not survive a drag on any QQC2 Slider: the drag writes it
// directly and the declarative binding is gone for good. Every caller
// resyncs it explicitly from whatever it mirrors - see TransportBar.qml's
// scrubber and its own comment.
QQC.Slider {
    id: control

    // Basic dims nothing when a control with replaced delegates is
    // disabled, so the same 0.45 every other hand-drawn control in
    // this family uses.
    opacity: enabled ? 1.0 : 0.45

    background: Rectangle {
        x: control.leftPadding
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: control.availableWidth
        height: 4
        color: Theme.neutral300

        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            color: Theme.accent
        }
    }

    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: 4
        height: 16
        color: Theme.text
    }

    // The sheet rings the whole control, not the handle.
    FocusRing { active: control.visualFocus }
}

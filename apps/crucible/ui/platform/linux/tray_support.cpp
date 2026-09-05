#include "tray_support.hpp"

#include <QObject>

// Linux: no tray, and the reason is a defect rather than a preference.
//
// Publishing a StatusNotifierItem from this window kills the process. Read
// off a Raspberry Pi 4B running the Raspberry Pi OS desktop (labwc,
// wf-panel-pi, Qt 6.8.2) on 2026-09-06: SIGBUS on the main thread, inside
// libQt6Gui reached from libQt6DBus delivering the panel's GetLayout call on
// the tray's com.canonical.dbusmenu object. The faulting instruction is an
// ldaxr - a refcount - on a pointer holding two AArch64 instruction words,
// so an object is being used after something else has written over it. It is
// a race: measured over ten launches each, the window survived 0 to 2 with
// the tray published and 10 without it.
//
// What it is not. Ruled out by measurement, ten launches per arm: the icon
// format (SVG and PNG both die, and this system ships no Qt 6 SVG icon
// engine); the menu's contents (a menu of literals dies the same way); the
// application-icon provider and its QIcon::fromTheme calls; Qt's
// accessibility bridge; the Qt Quick render loop; PipeWire's RTKit D-Bus
// client; the engine itself, which need not be running; and any symbol this
// binary exports over Qt's own. A minimal Qt application publishing the same
// tray and the same menu on the same session survives ten launches out of
// ten, so the fault is reached through this window's shape rather than being
// the desktop's alone: removing any one of several unrelated QML blocks
// moves it, which is the signature of a timing window rather than of a
// culprit in our own data.
//
// So the window does not publish one here. Closing it quits, which
// Main.qml's onClosing does when this is false, and the Settings page says
// why rather than offering a setting that cannot work. The next step is a
// sanitiser build on a desktop of this kind; docs/crucible/promotion.md
// carries the record.

namespace ac3::crucible::ui {

bool tray_is_published() { return false; }

QString tray_absent_reason() {
    return QObject::tr(
        "This build has no tray icon on Linux, so closing the window quits. Publishing one "
        "crashes the window on the desktops it has been tried on.");
}

}  // namespace ac3::crucible::ui

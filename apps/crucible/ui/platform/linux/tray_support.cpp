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
// Four more explanations were tested on 2026-09-06 and none of them holds.
// glibc's own heap checking (MALLOC_CHECK_=3, glibc.malloc.check=3) reports
// nothing before the fault. The faulting pointer is byte-identical with
// MALLOC_PERTURB_ set, so it is not memory that was freed and read back. The
// binary is built against the same Qt it loads, 6.8.2, from the one kit on
// the machine, so it is not a layout mismatch. And a build configured and
// compiled from scratch in a fresh tree crashes the same way, one launch in
// eight, so it is not stale generated code in a long-lived build directory.
//
// What the pointer is, is the one solid clue. 0xf9000bf3910003fd is two
// AArch64 instructions - str x19, [sp, #16] and mov x29, sp - which is the
// opening of a function prologue. Something reads a field, gets code bytes
// back as a pointer, and dereferences it. That is a structure read through a
// pointer that names a function rather than an object, inside Qt's own
// dbusmenu path, reached by this window's object graph and not by a minimal
// one.
//
// So the window does not publish a tray here, and that stands until someone
// can run the next step, which needs a machine this size cannot give: Qt
// debug symbols, or an address sanitiser over Qt itself, on a desktop with a
// StatusNotifier host. The reproducer is small - restore `visible: true` on
// the SystemTrayIcon in Main.qml, run it ten times, and count. Closing the
// window quits, which Main.qml's onClosing does when this returns false, and
// the Settings page says why rather than offering a setting that cannot
// work. docs/crucible/promotion.md carries the measurements.

namespace ac3::crucible::ui {

bool tray_is_published() { return false; }

QString tray_absent_reason() {
    return QObject::tr(
        "This build has no tray icon on Linux, so closing the window quits. Publishing one "
        "crashes the window on the desktops it has been tried on.");
}

}  // namespace ac3::crucible::ui

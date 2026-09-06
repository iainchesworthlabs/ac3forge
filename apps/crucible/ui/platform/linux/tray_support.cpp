#include "tray_support.hpp"

#include <QObject>
#include <QSystemTrayIcon>

// Linux: the StatusNotifierItem the desktop's panel shows, published again
// since the crash that took it away was found. Qt's own question is the
// right one, as it is on Windows - a session with no StatusNotifier host
// answers no and the setting greys with the reason below.
//
// What the crash was, because the shape of this file's menu still depends on
// it. Publishing an item from this window killed the process before it drew
// a frame: on a Raspberry Pi 4B (labwc, wf-panel-pi, Qt 6.8.2, aarch64)
// SIGBUS on eight to ten launches out of ten, and on a Debian 13 desktop VM
// (labwc, waybar, Qt 6.8.2, x86_64) SIGSEGV on nine or ten out of ten. Same
// fault, and the signal differs only because the AArch64 read was an ldaxr,
// which faults unaligned as SIGBUS where x86-64 faults unmapped as SIGSEGV.
//
// It is a type confusion in Qt, not a race, and not anything in this
// application's data:
//
//   QQuickLabsPlatformMenu::create() gives a Menu nested inside another Menu
//   the handle its parent's handle makes, via QPlatformMenu::createSubMenu().
//   QDBusPlatformMenu does not implement createSubMenu(), so the base class
//   answers, nothing native comes back, and Qt Labs Platform falls through
//   to its own QWidget fallback: the submenu's handle is a
//   QWidgetPlatformMenu. QDBusPlatformMenuItem::setMenu() then
//   static_casts that to QDBusPlatformMenu and writes m_containingMenuItem
//   through it, off the end of the object - valgrind catches that write
//   during QQmlObjectCreator::finalize, before the window is on screen. The
//   dangling static_cast is kept, and when the panel asks the tray for its
//   layout, QDBusMenuLayoutItem::populate reads QDBusPlatformMenu::m_items
//   out of a QWidgetPlatformMenu. The QList's d pointer is whatever that
//   object holds at the offset, and refcounting it is the crash.
//
// So it is not a race and every launch is not a coin toss for the reason it
// looked like one. The read is always wrong; whether it faults depends on
// what the bytes at that offset happen to be, which is why removing an
// unrelated QML block or adding an empty Qt.callLater moved it - those
// change the heap, not the schedule. The measured arms, ten launches each,
// on the VM (apps/linux/tray-vm) on 2026-09-06:
//
//   the tray's menu as it was, with one nested Menu   0 of 10, then 1 of 10
//   the same menu with that submenu's items lifted   10 of 10
//   the tray with no menu at all                     10 of 10
//   no tray published                                10 of 10
//
// The fix is therefore in the menu's shape rather than here: Main.qml's tray
// menu is flat, and must stay flat while this Qt bug stands. That constraint
// is the reason this file is worth reading; there is nothing else to it.
// docs/crucible/promotion.md carries the measurements and the report.

namespace ac3::crucible::ui {

bool tray_is_published() { return QSystemTrayIcon::isSystemTrayAvailable(); }

QString tray_absent_reason() {
    return tray_is_published()
               ? QString{}
               : QObject::tr("This desktop has no system tray, so closing the window quits.");
}

}  // namespace ac3::crucible::ui

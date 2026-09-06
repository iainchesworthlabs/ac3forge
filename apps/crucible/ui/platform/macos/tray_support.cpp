#include "tray_support.hpp"

#include <QObject>
#include <QSystemTrayIcon>

// macOS: the menu bar's status area, which Qt reaches through NSStatusItem.
//
// **THIS HAS NEVER BEEN RUN.** Written 2026-09-06; the only thing that will
// read it before somebody has a Mac is the macOS CI compiler
// (docs/crucible/promotion.md, "What cannot be verified, and why"). No tray
// icon has been published on a Mac by this application, and the sentence below
// is not a report that one was.
//
// The answer here is Windows' shape rather than Linux's, and the Linux file's
// record beside this one is why that needed deciding rather than assuming.
//
// **Linux says no, and its reason does not carry to this platform.**
// ui/platform/linux/tray_support.cpp records a measured crash: publishing a
// StatusNotifierItem from this window kills the process, SIGBUS on the main
// thread inside libQt6Gui reached from libQt6DBus delivering the panel's
// GetLayout call on the tray's com.canonical.dbusmenu object, 0 to 2 survivals
// out of ten launches with the tray published against 10 out of 10 without it.
// Every part of that fault is in the D-Bus half of Qt's tray support - the
// StatusNotifierItem protocol, the com.canonical.dbusmenu object, libQt6DBus
// itself. macOS has none of them: Qt's cocoa platform plugin puts a
// QSystemTrayIcon in the menu bar as an NSStatusItem, with no D-Bus, no
// StatusNotifier host and no dbusmenu anywhere in the path. So the Linux
// refusal is about a mechanism this platform does not use, and copying it here
// would be refusing a feature for somebody else's reason.
//
// That is a reason not to inherit Linux's no. It is NOT evidence that this
// works, and nothing here should be read as saying so. **Whoever runs Crucible
// on a Mac first should run the Linux file's own reproducer before trusting
// this**: launch the window ten times with the tray published, count how many
// survive, and if any do not, this file is where the answer changes and the
// measurement goes.
//
// Qt's own question is the right one to ask, as it is on Windows.
// QSystemTrayIcon::isSystemTrayAvailable() asks the platform plugin whether
// there is anywhere to put an icon; on macOS that is the menu bar, and a
// session without one - the offscreen platform the Qt Quick Test suites and
// `--shot` run under, most obviously - answers no. Asking rather than
// returning a literal true is what keeps the suites reading the machine:
// under offscreen this returns false on every platform, the setting greys,
// and Main.qml's onClosing quits instead of hiding a window with no way back
// to it.

namespace ac3::crucible::ui {

bool tray_is_published() { return QSystemTrayIcon::isSystemTrayAvailable(); }

QString tray_absent_reason() {
    return tray_is_published()
               ? QString{}
               : QObject::tr(
                     "This session has no menu bar to put a status icon in, so closing the "
                     "window quits.");
}

}  // namespace ac3::crucible::ui

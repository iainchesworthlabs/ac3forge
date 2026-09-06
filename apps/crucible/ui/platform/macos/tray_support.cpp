#include "tray_support.hpp"

#include <QObject>
#include <QSystemTrayIcon>

// macOS: the menu bar's status area, which Qt reaches through NSStatusItem.
//
// **THIS RUNS ON CI, AND ON NOBODY'S DESK.** Written 2026-09-06 without a
// Mac; tst_platform.qml reads CrucibleController.trayAvailable on both macOS
// legs, which is tray_is_published() below
// (docs/crucible/promotion.md, Phase 5). On those runners it answers false -
// Qt reports no native SystemTrayIcon implementation under the offscreen
// platform - so what has been exercised is the refusal, not the publish. No
// tray icon has been published on a Mac by this application, and the sentence
// below is not a report that one was.
//
// The answer here is the same as Windows' and, since 2026-09-06, the same as
// Linux's. This file was written while Linux still refused, and the Linux
// file's record beside this one is why that needed deciding rather than
// assuming; what that record now says is worth reading here, because the near
// miss it describes is not a D-Bus one.
//
// **The Linux crash was a type confusion, and this platform escapes it by
// one step that has nothing to do with D-Bus.**
// QQuickLabsPlatformMenu::create() gives a Menu nested inside another Menu the
// handle its parent's handle makes, via QPlatformMenu::createSubMenu(). Neither
// QDBusPlatformMenu nor QCocoaMenu implements that - Qt Labs Platform's own
// source lists "QCocoaMenu::createSubMenu()" as a TODO beside the D-Bus one -
// so both fall to the next rung, the platform theme's createPlatformMenu().
// There the platforms part: the generic Unix theme returns nothing, so Labs
// Platform drops to its QWidget fallback and QDBusPlatformMenuItem::setMenu()
// static_casts a QWidgetPlatformMenu to QDBusPlatformMenu; QCocoaTheme returns
// a real QCocoaMenu, so the fallback is never reached. QCocoaMenuItem::setMenu()
// makes the identical unchecked static_cast<QCocoaMenu *>, so what protects
// this platform is that its theme makes menus, not that the cast is safe.
//
// None of which is evidence that a tray works here, and nothing above should be
// read as saying so. **Whoever runs Crucible on a Mac first should run the
// Linux file's own reproducer before trusting this**: launch the window ten
// times with the tray published, count how many survive, and if any do not,
// this file is where the answer changes and the measurement goes. The tray's
// menu in Main.qml is flat on every platform, which is what keeps the rung
// above out of the picture; keep it that way here too.
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

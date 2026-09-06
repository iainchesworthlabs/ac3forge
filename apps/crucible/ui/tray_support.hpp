#pragma once

#include <QString>

// Whether this build publishes a system tray icon, and what to say when it
// does not. A platform seam like ui/platform/<os>/app_icon_provider.cpp: one
// file per operating system, exactly one compiled, no #ifdefs
// (docs/crucible/promotion.md, "The platform tree").
//
// The question is whether this session has somewhere to put an icon, and Qt
// answers it: QSystemTrayIcon::isSystemTrayAvailable(). Both platforms ask
// it and neither adds anything, which is what a seam should look like when
// the platforms agree.
//
// It has not always. Linux answered a flat no for a while, because
// publishing an item killed the window - the Linux file has that record,
// which is still worth reading: the constraint it leaves behind is that the
// tray's menu in Main.qml must not nest a submenu.

namespace ac3::crucible::ui {

// True where the window may create a tray icon.
[[nodiscard]] bool tray_is_published();

// One sentence for the Settings page, in the platform's own words, saying
// why the "keep running in the tray" setting cannot apply. Empty where the
// tray is published.
[[nodiscard]] QString tray_absent_reason();

}  // namespace ac3::crucible::ui

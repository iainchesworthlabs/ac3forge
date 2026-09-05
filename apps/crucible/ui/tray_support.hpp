#pragma once

#include <QString>

// Whether this build publishes a system tray icon, and what to say when it
// does not. A platform seam like ui/platform/<os>/app_icon_provider.cpp: one
// file per operating system, exactly one compiled, no #ifdefs
// (docs/crucible/promotion.md, "The platform tree").
//
// The question is not "does this desktop have a tray". Qt answers that with
// QSystemTrayIcon::isSystemTrayAvailable(), and on the Raspberry Pi's own
// desktop the answer is yes - wf-panel-pi owns
// org.kde.StatusNotifierWatcher. The question is whether publishing one is
// safe here, and on Linux today it is not: see the Linux file for what was
// measured and what was ruled out.

namespace ac3::crucible::ui {

// True where the window may create a tray icon.
[[nodiscard]] bool tray_is_published();

// One sentence for the Settings page, in the platform's own words, saying
// why the "keep running in the tray" setting cannot apply. Empty where the
// tray is published.
[[nodiscard]] QString tray_absent_reason();

}  // namespace ac3::crucible::ui

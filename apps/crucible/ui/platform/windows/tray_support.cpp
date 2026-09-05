#include "tray_support.hpp"

#include <QSystemTrayIcon>

// Windows: the notification area, which the window has used since the demo.
// Qt's own question is the right one here - a session with the notification
// area turned off answers no, and the setting greys with the reason below.

namespace ac3::crucible::ui {

bool tray_is_published() { return QSystemTrayIcon::isSystemTrayAvailable(); }

QString tray_absent_reason() {
    return tray_is_published()
               ? QString{}
               : QObject::tr("This session has no notification area, so closing the window quits.");
}

}  // namespace ac3::crucible::ui

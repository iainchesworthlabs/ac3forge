#include "app_icon_provider.hpp"

#include <QMutexLocker>

// The Linux AppIconProvider: it has no icons yet, and says so by returning
// none (docs/crucible/promotion.md, Phase 4).
//
// The Windows one asks the shell for the icon Explorer would show for an
// executable, which is a single call. Linux has no equivalent single call:
// the icon a menu shows for an application comes from its .desktop entry's
// Icon= line, resolved against the current icon theme, and the mapping from
// a running process's executable back to its .desktop file is heuristic -
// by Exec= line, by StartupWMClass, by the application.name PipeWire reports.
// All of that is worth doing properly rather than approximately, and it is a
// follow-up.
//
// Until then this returns a null image for every path, which the header
// documents as the "no icon" answer, and the rail and the bed chips show the
// monogram they already show for an executable without an icon. Nothing in
// the window has to know the difference.

namespace ac3::crucible::ui {

AppIconProvider::AppIconProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage AppIconProvider::requestImage(const QString& /*id*/, QSize* size,
                                     const QSize& /*requested_size*/) {
    if (size != nullptr) {
        *size = QSize();
    }
    return {};
}

}  // namespace ac3::crucible::ui

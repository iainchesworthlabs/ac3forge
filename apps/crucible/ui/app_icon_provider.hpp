#pragma once

#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>
#include <QString>
#include <QHash>

namespace ac3::crucible::ui {

// image://appicon/<percent-encoded executable path>?icon=<icon-theme name>
// &app=<application id>&name=<display name>: the platform's own icon for an
// application, as a QImage, cached per id. One id grammar on every platform
// (AppIcon.qml builds it); each platform reads what it can use. Windows
// takes the path and asks the shell for the icon Explorer shows for that
// executable. Linux takes all four: the icon name through the icon theme,
// then a .desktop entry matched by application id, TryExec, Exec,
// StartupWMClass or Name, then the theme by the executable's own name. The
// rail, the bed chips and the room views use it in place of the monogram
// wherever the platform has one; an id with no icon yields a null image and
// the monogram stays.
class AppIconProvider final : public QQuickImageProvider {
public:
    AppIconProvider();

    QImage requestImage(const QString& id, QSize* size, const QSize& requested_size) override;

private:
    QMutex mutex_;
    QHash<QString, QImage> cache_;
};

}  // namespace ac3::crucible::ui

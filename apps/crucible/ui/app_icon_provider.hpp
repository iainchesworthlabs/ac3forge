#pragma once

#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>
#include <QString>
#include <QHash>

namespace ac3::crucible::ui {

// image://appicon/<percent-encoded executable path>: the icon Windows shows
// for that executable in Explorer, as a QImage, cached per path. The rail
// and the bed chips use it in place of the monogram wherever the shell has
// one; a path with no icon yields a null image and the monogram stays.
class AppIconProvider final : public QQuickImageProvider {
public:
    AppIconProvider();

    QImage requestImage(const QString& id, QSize* size, const QSize& requested_size) override;

private:
    QMutex mutex_;
    QHash<QString, QImage> cache_;
};

}  // namespace ac3::crucible::ui

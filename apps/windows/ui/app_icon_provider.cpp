#include "app_icon_provider.hpp"

#include <windows.h>

#include <shellapi.h>

#include <QMutexLocker>
#include <QUrl>

namespace ac3::desk {

namespace {

QImage icon_of(const wchar_t* path, DWORD attributes, bool large, bool by_attributes) {
    SHFILEINFOW info{};
    UINT flags = SHGFI_ICON | (large ? SHGFI_LARGEICON : SHGFI_SMALLICON);
    if (by_attributes) {
        flags |= SHGFI_USEFILEATTRIBUTES;
    }
    if (SHGetFileInfoW(path, attributes, &info, sizeof(info), flags) == 0 || info.hIcon == nullptr) {
        return {};
    }
    QImage image = QImage::fromHICON(info.hIcon);
    DestroyIcon(info.hIcon);
    return image;
}

// SHGetFileInfo hands back the icon Explorer would show for the file: the
// executable's own first icon, or, for an executable with none, the shell's
// generic one. The generic one is what the shell gives a name that is only
// an attribute set, so it is fetched once that way and compared pixel for
// pixel; a match means the monogram is the better picture.
QImage shell_icon(const QString& path, bool large) {
    static const QImage generic_large = icon_of(L"x.exe", FILE_ATTRIBUTE_NORMAL, true, true);
    static const QImage generic_small = icon_of(L"x.exe", FILE_ATTRIBUTE_NORMAL, false, true);
    const auto wide = path.toStdWString();
    QImage image = icon_of(wide.c_str(), 0, large, false);
    const QImage& generic = large ? generic_large : generic_small;
    if (!image.isNull() && !generic.isNull() && image == generic) {
        return {};
    }
    return image;
}

}  // namespace

AppIconProvider::AppIconProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage AppIconProvider::requestImage(const QString& id, QSize* size, const QSize& requested_size) {
    const QString path = QUrl::fromPercentEncoding(id.toUtf8());
    const bool large = requested_size.width() > 20 || requested_size.height() > 20 || !requested_size.isValid();
    const QString key = path + (large ? QStringLiteral("|L") : QStringLiteral("|S"));
    {
        const QMutexLocker lock(&mutex_);
        if (const auto it = cache_.constFind(key); it != cache_.constEnd()) {
            if (size != nullptr) {
                *size = it->size();
            }
            return *it;
        }
    }
    QImage image = path.isEmpty() ? QImage{} : shell_icon(path, large);
    if (!image.isNull() && requested_size.isValid()) {
        image = image.scaled(requested_size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (size != nullptr) {
        *size = image.size();
    }
    const QMutexLocker lock(&mutex_);
    cache_.insert(key, image);
    return image;
}

}  // namespace ac3::desk

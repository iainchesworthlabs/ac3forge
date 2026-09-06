#include "app_icon_provider.hpp"

#import <AppKit/AppKit.h>

#include <QByteArray>
#include <QGuiApplication>
#include <QImage>
#include <QLatin1Char>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSemaphore>
#include <QSize>
#include <QString>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

// The macOS AppIconProvider: an application's icon from its bundle, through
// NSWorkspace (docs/crucible/promotion.md, Phase 5).
//
// **THIS RUNS ON CI, AND ON NOBODY'S DESK.** Written 2026-09-06 without a
// Mac; since the same day tst_icons.qml creates AppIcon items on both macOS
// legs, so requestImage() below answers for real
// (docs/crucible/promotion.md, Phase 5). What no test can check is whether
// what it drew is the right icon: the suite asserts that an image came back,
// not what is in it, and nobody has looked at one.
//
// Objective-C++ because NSWorkspace and NSImage have no C entry point, the
// same wall engine/platform/macos/foreground.mm hits. The file's path is what
// says "macOS"; there is no #ifdef here, and
// tools/checks/check_platform_macros.ps1 holds that rule for the whole of
// apps/ (its extension list gained .mm on 2026-09-06, with these two files).
//
// Half the Qt includes below are read AFTER the AppKit import, which matters
// more than it looks: AppKit reaches Apple's <AssertMacros.h> through
// ApplicationServices, and that header defines a one-parameter macro called
// verify() while Qt's own qbytearray.h/qstring.h/qlist.h/qspan.h declare and
// call a two-parameter member verify(pos, n). A Qt header first read after the
// import would fail on that. The build defines
// __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES=0 for this target
// (apps/crucible/CMakeLists.txt and ui/tests/CMakeLists.txt), which is Apple's
// own opt-out, so the order below is safe rather than lucky - but it is safe
// because of that define and not on its own.
//
// ---------------------------------------------------------------------------
// The rungs
// ---------------------------------------------------------------------------
// macOS has a single call for "the icon this thing has" where Linux has none -
// -[NSWorkspace iconForFile:] hands back exactly what the Finder shows - so
// there are three rungs here rather than Linux's four, the first that yields a
// picture winning:
//
//   1. The .app bundle. The session monitor puts the OUTERMOST bundle in
//      image_path where a process has one (engine/platform/macos/
//      process_facts.hpp's icon_path_of, over bundle_facts.hpp), so a
//      browser's audio helper arrives here as the browser and gets the
//      browser's icon rather than a helper binary's.
//   2. The bundle identifier, through
//      -[NSWorkspace URLForApplicationWithBundleIdentifier:]. This is the rung
//      for a process whose executable path could not be read at all -
//      proc_pidpath fails with EPERM across a user boundary - while Core Audio
//      still named its bundle.
//   3. Nothing: a null image, and the monogram stays.
//
// **There is deliberately no rung for a bare executable.** iconForFile: on a
// Unix binary outside a bundle answers with the shell's generic executable
// icon, which is a worse picture than the monogram and is the same picture for
// every such process. The Windows provider fetches its own generic icon once
// and compares pixel for pixel to reject it; here the test is cheaper, because
// a path that is not a bundle is knowable without asking anybody - it does not
// end in ".app" - so it is never asked. A command-line player gets the
// monogram, which is the right answer for it.
//
// ---------------------------------------------------------------------------
// Which thread may touch what - the same rule as the Linux provider, for a
// different reason, and it is not optional
// ---------------------------------------------------------------------------
// The constructor runs on the GUI thread. Every request arrives on Qt Quick's
// pixmap-reader thread, because this provider asks for
// ForceAsynchronousImageLoading; the id is parsed and the bundle-or-identifier
// decision made there, and neither touches AppKit.
//
// **AppKit is main-thread-only, and that is the whole of the discipline
// here.** NSWorkspace's icon lookups and NSImage's drawing both reach AppKit's
// process-wide state, which Apple documents as usable from the main thread
// alone; NSGraphicsContext's current-context stack is per-thread but the
// image being drawn is not. So the lookup and the draw run on the GUI thread,
// one queued call per new identity, which the reader thread waits on - exactly
// what ui/platform/linux/app_icon_provider.cpp does for QIconLoader, whose own
// reason is different (one process-wide set of caches with no lock) and whose
// consequence is identical.
//
// The wait is bounded rather than a blocking connection, and for the reason
// the Linux file gives: the reader thread's own destructor, at engine
// teardown, runs on the GUI thread and waits for the reader, so a reader
// blocked on that same GUI thread would never return. On a timeout the answer
// is null and is NOT cached, so the next request asks again. The posted call
// owns its inputs and the slot for its answer through a shared block, so a
// call that outlives this wait writes into memory nobody reads any more. The
// frame thread never touches any of it.

namespace ac3::crucible::ui {

namespace {

// Off by default. QT_LOGGING_RULES="ac3crucible.icons.debug=true" prints the
// rung each application resolved on, which is what a first Mac run should
// quote. The same category name the Linux provider uses, so one rule turns on
// whichever platform is in front of you.
Q_LOGGING_CATEGORY(lcIcons, "ac3crucible.icons", QtWarningMsg)

// How long the reader thread waits for the GUI thread's answer. A GUI thread
// that has not served an event in this long is shutting down or stuck, and a
// monogram meanwhile costs nothing. The Linux provider's own figure.
constexpr int kGuiThreadWaitMs = 2000;

// A UTF-8 QString as an NSString, without leaning on Qt's Darwin-only
// QString::toNSString(): the conversion is two lines and this file already
// carries the autorelease pool it needs.
NSString* to_ns(const QString& text) {
    const QByteArray utf8 = text.toUtf8();
    return [NSString stringWithUTF8String:utf8.constData()];
}

// An NSImage as a QImage of exactly px by px, drawn rather than copied.
//
// NSImage is a resolution-independent set of representations, not a bitmap, so
// there is nothing to memcpy out of it: the way to get pixels is to draw it
// into a context whose format you chose. That context is a CGBitmapContext
// over the QImage's own scanlines, so there is one buffer and no intermediate
// copy.
//
// The pixel format pairs QImage::Format_ARGB32_Premultiplied with
// kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host. QImage's ARGB32
// is one native-endian 32-bit word per pixel with alpha in the top byte, which
// is what "alpha first, host byte order" names on the CoreGraphics side; both
// macOS architectures are little-endian, so the host order resolves the same
// way on each. Premultiplied on both sides, so no conversion pass is needed
// after the draw.
//
// **Orientation is the thing most likely to be wrong here, so here is the
// reasoning rather than a result.** A CGBitmapContext draws in a coordinate
// system whose origin is the lower left, while its backing memory begins with
// the image's TOP row - CoreGraphics maps user y = height to row 0 - and a
// QImage's scanline 0 is its top row too. So an image drawn upright in that
// coordinate system lands top row first in memory and reads upright as a
// QImage, with no transform applied: `flipped:NO` on the NSGraphicsContext
// says the context is the unflipped one it actually is, and the CTM is left
// alone. Qt's own qt_mac_cg_context() flips the CTM instead, because the Qt
// code after it paints in Qt's top-left coordinates, and then flips a second
// time inside qt_mac_drawCGImage() to put the picture back the right way up;
// two flips and none are the same picture. Nobody has looked at an icon this
// produced. If they come out upside down on a Mac, this paragraph is the one
// that was wrong and a CTM translate-and-scale is the fix.
//
// GUI thread only (the file header).
QImage image_from_ns(NSImage* icon, int px) {
    if (icon == nil || px <= 0) {
        return {};
    }
    QImage out(px, px, QImage::Format_ARGB32_Premultiplied);
    if (out.isNull()) {
        return {};
    }
    out.fill(Qt::transparent);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (space == nullptr) {
        return {};
    }
    // Each half cast before the OR rather than the result cast after it: the
    // two constants belong to different enumerations (CGImageAlphaInfo and
    // CGBitmapInfo), and arithmetic between two different enumeration types is
    // deprecated in C++20 and warned about from C++23 on.
    const std::uint32_t bitmap_info = static_cast<std::uint32_t>(kCGImageAlphaPremultipliedFirst) |
                                      static_cast<std::uint32_t>(kCGBitmapByteOrder32Host);
    CGContextRef context = CGBitmapContextCreate(
        out.bits(), static_cast<std::size_t>(px), static_cast<std::size_t>(px), 8,
        static_cast<std::size_t>(out.bytesPerLine()), space, bitmap_info);
    CGColorSpaceRelease(space);
    if (context == nullptr) {
        return {};
    }
    NSGraphicsContext* graphics = [NSGraphicsContext graphicsContextWithCGContext:context
                                                                          flipped:NO];
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:graphics];
    const CGFloat side = static_cast<CGFloat>(px);
    [icon drawInRect:NSMakeRect(0, 0, side, side)
            fromRect:NSZeroRect
           operation:NSCompositingOperationSourceOver
            fraction:1.0];
    [NSGraphicsContext restoreGraphicsState];
    CGContextRelease(context);
    return out;
}

// What one request resolves to, and the rung that answered.
struct Resolved {
    QImage image;
    const char* rung = "monogram";
};

// The two picture rungs, in order. GUI thread only (the file header).
Resolved resolve_on_gui_thread(const QString& bundle_path, const QString& bundle_id, int px) {
    Resolved result;
    @autoreleasepool {
        NSWorkspace* workspace = [NSWorkspace sharedWorkspace];
        if (!bundle_path.isEmpty()) {
            NSString* path = to_ns(bundle_path);
            if (path != nil) {
                result.image = image_from_ns([workspace iconForFile:path], px);
                if (!result.image.isNull()) {
                    result.rung = "bundle-path";
                    return result;
                }
            }
        }
        if (!bundle_id.isEmpty()) {
            NSString* identifier = to_ns(bundle_id);
            NSURL* url = identifier == nil
                             ? nil
                             : [workspace URLForApplicationWithBundleIdentifier:identifier];
            NSString* found = url == nil ? nil : [url path];
            if (found != nil) {
                result.image = image_from_ns([workspace iconForFile:found], px);
                if (!result.image.isNull()) {
                    result.rung = "bundle-id";
                }
            }
        }
    }
    return result;
}

// Runs resolve_on_gui_thread() where it must run and waits for the answer, or
// gives up after kGuiThreadWaitMs with `answered` false (the file header says
// why the wait is bounded). Written the same way as the Linux provider's own,
// down to the shared block, so the two cannot drift apart.
Resolved on_gui_thread(QString bundle_path, QString bundle_id, int px, bool& answered) {
    if (qGuiApp == nullptr) {
        answered = false;
        return {};
    }
    if (QThread::currentThread() == qGuiApp->thread()) {
        answered = true;
        return resolve_on_gui_thread(bundle_path, bundle_id, px);
    }
    struct Shared {
        QSemaphore done;
        QMutex mutex;
        Resolved result;
    };
    const auto shared = std::make_shared<Shared>();
    // The captures carry their own names rather than the parameters': an
    // init-capture is a declaration, and one named after a variable already in
    // scope is what -Wshadow is for.
    QMetaObject::invokeMethod(
        qGuiApp,
        [shared, bundle = std::move(bundle_path), identifier = std::move(bundle_id), px] {
            Resolved resolved = resolve_on_gui_thread(bundle, identifier, px);
            const QMutexLocker lock(&shared->mutex);
            shared->result = std::move(resolved);
            shared->done.release();
        },
        Qt::QueuedConnection);
    answered = shared->done.tryAcquire(1, kGuiThreadWaitMs);
    if (!answered) {
        return {};
    }
    const QMutexLocker lock(&shared->mutex);
    return shared->result;
}

}  // namespace

AppIconProvider::AppIconProvider()
    : QQuickImageProvider(QQuickImageProvider::Image,
                          QQuickImageProvider::ForceAsynchronousImageLoading) {}

QImage AppIconProvider::requestImage(const QString& id, QSize* size, const QSize& requested_size) {
    const QString path = QUrl::fromPercentEncoding(id.section(QLatin1Char('?'), 0, 0).toUtf8());
    const QUrlQuery query(id.section(QLatin1Char('?'), 1));
    // `app` is the bundle identifier here: one id grammar on every platform,
    // each reading what its own can use (ui/app_icon_provider.hpp). `icon` is
    // the freedesktop icon-theme name and is always empty on this platform, so
    // it is not read at all.
    const QString app_id = query.queryItemValue(QStringLiteral("app"), QUrl::FullyDecoded);
    const QString name = query.queryItemValue(QStringLiteral("name"), QUrl::FullyDecoded);
    const bool large =
        requested_size.width() > 20 || requested_size.height() > 20 || !requested_size.isValid();
    // The whole id is the key: two processes of one application, and a
    // restarted one, share a lookup (the per-process cache is the session
    // monitor's). What is kept under it is the picture at the size it was
    // drawn, and never a copy scaled to one caller's request: the rail asks for
    // 28 pixels and the 3D room for 160 against this one key, and whichever
    // arrived second would be handed the first one's scaled image. Scaling
    // happens per request, below; both other providers are written the same way.
    const QString key = id + (large ? QStringLiteral("|L") : QStringLiteral("|S"));
    QImage image;
    bool cached = false;
    {
        const QMutexLocker lock(&mutex_);
        if (const auto it = cache_.constFind(key); it != cache_.constEnd()) {
            image = *it;
            cached = true;  // a cached null is an answer: the monogram
        }
    }
    if (!cached) {
        const int px = large ? 256 : 32;
        // A bundle, or not. The session monitor already resolved the outermost
        // .app for a process that has one, so this is a test of what it sent
        // rather than a second search; a path that is not a bundle is left for
        // rung 2 or the monogram, never handed to iconForFile: (the file
        // header says why the generic executable icon is not wanted).
        const QString bundle_path = path.endsWith(QStringLiteral(".app")) ? path : QString{};
        bool answered = false;
        Resolved resolved = on_gui_thread(bundle_path, app_id, px, answered);
        image = std::move(resolved.image);
        qCDebug(lcIcons) << "icon for" << name << "path" << path << "app" << app_id << "->"
                         << (answered ? resolved.rung : "no answer from the GUI thread");
        if (answered) {
            // A null is cached too, as both other providers cache one: the
            // monogram is the answer for this identity for the life of the
            // process.
            const QMutexLocker lock(&mutex_);
            cache_.insert(key, image);
        }
    }
    if (!image.isNull() && requested_size.isValid()) {
        image = image.scaled(requested_size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (size != nullptr) {
        *size = image.size();
    }
    return image;
}

}  // namespace ac3::crucible::ui

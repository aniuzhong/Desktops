#include "wallpaper.h"

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QPaintEvent>
#include <QPainter>
#include <QRect>
#include <QScreen>
#include <QSettings>

Q_LOGGING_CATEGORY(lcWallpaper, "desktops.wallpaper")

namespace
{
    // Where Windows keeps the desktop image. A plain path on this machine
    // (verified), so no COM and no TranscodedImageCache parsing.
    QString wallpaperPath()
    {
        return QSettings(QStringLiteral("HKEY_CURRENT_USER\\Control Panel\\Desktop"), QSettings::NativeFormat)
            .value(QStringLiteral("WallPaper")).toString();
    }
}  // namespace

Wallpaper::Wallpaper(const QPixmap& image, const QRect& screen)
    : image_(image.scaled(screen.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation))
{
    // Tool: no taskbar button, no Alt+Tab entry. NoAcceptFocus and
    // TransparentForInput together mean a click can never raise it.
    // Staying behind every app window is not automatic on a shell-less
    // desktop (this window was once found above the dock bar), so the
    // caller sinks it at birth and the dock bar pins itself topmost.
    setWindowFlags(Qt::FramelessWindowHint |
                   Qt::Tool |
                   Qt::WindowDoesNotAcceptFocus |
                   Qt::WindowTransparentForInput);
    setGeometry(screen);
}

void Wallpaper::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.drawPixmap((width() - image_.width()) / 2, (height() - image_.height()) / 2, image_);
}

QWidget* composeWallpaper()
{
    const QString path = wallpaperPath();
    const QPixmap image(path);
    if (image.isNull())
    {
        qCInfo(lcWallpaper, "no wallpaper: '%s' did not load", path.toUtf8().constData());
        return nullptr;
    }
    const QRect screen = QGuiApplication::primaryScreen()->geometry();
    qCInfo(lcWallpaper, "wallpaper '%s' (%dx%d) on screen %dx%d", path.toUtf8().constData(),
           image.width(), image.height(), screen.width(), screen.height());
    return new Wallpaper(image, screen);
}

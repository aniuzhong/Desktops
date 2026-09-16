#pragma once

#include <QPixmap>
#include <QWidget>

// A desktop with no shell has no wallpaper: win32k fills its desktop
// window with COLOR_BACKGROUND, captured when the desktop was created
// (black under the dark theme, off-white under the light one, and it does
// not follow later theme changes - so two desktops can disagree).
// Painting it ourselves is the only way to have one.
//
// The window belongs to the dock process, which is already attached to
// the target desktop (see dock.h), so this needs no plumbing of its own:
// no process, no pipe, no protocol.
//
// Known gap: neither the geometry nor the image follows a resolution or
// DPI change after construction, and the image is scaled to logical
// pixels rather than device pixels (soft on a scaled display). The dock
// has the same gap, so both are meant to be fixed together.
class Wallpaper : public QWidget
{
public:
    // Fill is the only scale mode implemented: WallpaperStyle is 10 on the
    // machine this was written against, and covering the screen is what a
    // wallpaper is for. The image is scaled once, here, and the overflow
    // cropped off-centre - no rescaling per repaint.
    Wallpaper(const QPixmap& image, const QRect& screen);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QPixmap image_;
};

// Null when no image can be shown: the desktop then keeps the system
// colour, which is the honest fallback.
QWidget* composeWallpaper();

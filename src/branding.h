#pragma once

#include <QIcon>
#include <QSize>
#include <QString>

// Icons are embedded (data/icons, see CMakeLists.txt) and used directly
// rather than looked up in the icon theme: a theme lookup can pick up a
// stale icon left installed by an older build.
namespace Branding {

// The full logo (dachshund + calendar): window, notifications.
inline QIcon appIcon()
{
    QIcon icon;
    for (int s : {32, 48, 64, 128, 256, 512})
        icon.addFile(QStringLiteral(":/icons/lowacal-%1.png").arg(s), QSize(s, s));
    return icon;
}

// Simplified glyph for the system tray: the full logo turns to mush at
// 22 px, so the tray shows just the logo's calendar (purple header,
// rings), drawn on the pixel grid. Source: data/icons/lowacal-tray.svg.
inline QIcon trayIcon()
{
    QIcon icon;
    for (int s : {16, 22, 24, 32, 44, 48, 64})
        icon.addFile(QStringLiteral(":/icons/lowacal-tray-%1.png").arg(s), QSize(s, s));
    return icon;
}

} // namespace Branding

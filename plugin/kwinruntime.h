#pragma once
#include <QGuiApplication>
namespace Minimize2Tray {
// KWin renamed its internal QPA; do not infer compositor access from a process name.
inline bool internalKWinPlatform(const QString &name = QGuiApplication::platformName()) {
    return name == QLatin1String("org.kde.kwin.qpa") || name == QLatin1String("wayland-org.kde.kwin.qpa");
}
} // namespace Minimize2Tray

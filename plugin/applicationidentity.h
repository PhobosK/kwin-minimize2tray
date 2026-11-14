#pragma once

#include <KService>
#include <QIcon>
#include <QVariantMap>

namespace Minimize2Tray {
// Installed launcher metadata only: never execute a desktop file or a window-provided path.
KService::Ptr applicationService(const QVariantMap &window);
QVariantMap matchingProperties(QVariantMap window);
QIcon explicitTrayIcon(const QString &path);
bool validIconPath(const QString &path);
} // namespace Minimize2Tray

#pragma once

#include <KService>
#include <QIcon>
#include <QVariantMap>

namespace Minimize2Tray {
// Installed launcher metadata only: never execute a desktop file or a window-provided path.
QObject *applicationIndexNotifier();
// Cached lookups return empty until the worker publishes changed().
KService::Ptr installedApplication(const QString &id);
KService::Ptr applicationService(const QVariantMap &window);
QVariantMap matchingProperties(QVariantMap window);
QIcon explicitTrayIcon(const QString &path);
bool validIconPath(const QString &path);
} // namespace Minimize2Tray

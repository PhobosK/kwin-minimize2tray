#pragma once

#include <KConfig>
#include <QVariantList>
#include <QVariantMap>

namespace Minimize2Tray {
inline constexpr auto ScriptId = "com.github.luisbocanegra.minimize2tray";
inline constexpr auto ConfigGroup = "Script-com.github.luisbocanegra.minimize2tray";
inline constexpr int MaxRules = 1024;

QString configLockPath();
QVariantMap newRule();
QString encodeRules(const QVariantList &rules, bool indented = false);
bool decodeRules(const QString &json, QVariantList &rules, QString &error);
QVariantList readRules(KConfig &config, QString &error);
QVariantMap readSettings(KConfig &config);
QString desktopIdentity(QString value);
bool protectedIdentity(const QString &value);
bool protectedWindow(const QVariantMap &window);
QString windowType(const QVariantMap &window);
QString matchFailure(const QVariantMap &rule, const QVariantMap &window);
QString ruleSummary(const QVariantMap &rule);
QString searchText(const QVariantMap &rule);
} // namespace Minimize2Tray

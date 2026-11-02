#pragma once

#include <QByteArray>
#include <QVariantList>
#include <QVariantMap>

namespace Minimize2Tray {
inline constexpr int MaxConfigurationBackupBytes = 8 * 1024 * 1024;

struct ConfigurationBackup {
    QVariantList rules;
    QVariantMap settings;
    QString sortOrder;
};

bool mergeConfigurationRules(const QVariantList &current, const QVariantList &incoming, QVariantList &merged,
                             QString &error);

QByteArray encodeConfigurationBackup(const ConfigurationBackup &backup, QString &error);
bool decodeConfigurationBackup(const QByteArray &json, ConfigurationBackup &backup, QString &error);
bool readConfigurationBackup(const QString &path, ConfigurationBackup &backup, QString &error);
bool writeConfigurationBackup(const QString &path, const QByteArray &json, QString &error);
} // namespace Minimize2Tray

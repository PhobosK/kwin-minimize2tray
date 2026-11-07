#include "configbackup.h"
#include "rules.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <cmath>

namespace Minimize2Tray {
static QString text(const char *value) { return QCoreApplication::translate("Minimize2Tray", value); }

bool mergeConfigurationRules(const QVariantList &current, const QVariantList &incoming, QVariantList &merged,
                             QString &error) {
    QVariantList result, additions;
    if (!decodeRules(encodeRules(current), result, error) || !decodeRules(encodeRules(incoming), additions, error))
        return false;
    QSet<QUuid> ids;
    QSet<QByteArray> contents;
    auto content = [](QVariantMap rule) {
        rule.remove(QStringLiteral("id"));
        // Creation metadata alone does not justify adding a duplicate rule.
        rule.remove(QStringLiteral("autoCreated"));
        return QJsonDocument(QJsonObject::fromVariantMap(rule)).toJson(QJsonDocument::Compact);
    };
    for (const auto &value : result) {
        const auto rule = value.toMap();
        ids.insert(QUuid(rule.value(QStringLiteral("id")).toString()));
        contents.insert(content(rule));
    }
    for (const auto &value : additions) {
        auto rule = value.toMap();
        const auto key = content(rule);
        if (contents.contains(key))
            continue;
        if (result.size() >= MaxRules) {
            error = text("Adding these rules would exceed the maximum of 1024 rules. Choose fewer rules or replace the "
                         "current rules.");
            return false;
        }
        QUuid id(rule.value(QStringLiteral("id")).toString());
        if (ids.contains(id)) {
            do {
                id = QUuid::createUuid();
            } while (ids.contains(id));
            rule.insert(QStringLiteral("id"), id.toString(QUuid::WithoutBraces));
        }
        ids.insert(id);
        contents.insert(key);
        result.append(rule);
    }
    // Validate the combined document too: individually valid files can together
    // exceed the runtime's size limit. Do not modify the output on failure.
    return decodeRules(encodeRules(result), merged, error);
}

bool decodeConfigurationBackup(const QByteArray &json, ConfigurationBackup &backup, QString &error) {
    error.clear();
    if (json.size() > MaxConfigurationBackupBytes) {
        error = text("The configuration backup exceeds the 8 MiB size limit.");
        return false;
    }
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        error = text("The configuration backup is not valid JSON: ") + parseError.errorString();
        return false;
    }
    if (!doc.isObject()) {
        error = text("The configuration backup must be a JSON object.");
        return false;
    }
    const auto root = doc.object();
    if (root.size() != 5 || root.value(QStringLiteral("format")) != QJsonValue(QString::fromLatin1(ScriptId)) ||
        root.value(QStringLiteral("formatVersion")) != QJsonValue(1) ||
        !root.value(QStringLiteral("rules")).isObject() || !root.value(QStringLiteral("settings")).isObject() ||
        !QStringList{QStringLiteral("title"), QStringLiteral("newest"), QStringLiteral("oldest")}.contains(
            root.value(QStringLiteral("sortOrder")).toString())) {
        error = text("Unsupported Minimize2Tray configuration backup format or version.");
        return false;
    }
    const auto settings = root.value(QStringLiteral("settings")).toObject();
    const auto limit = settings.value(QStringLiteral("countDisplayLimit"));
    if (settings.size() != 4 || !settings.value(QStringLiteral("countUseDot")).isBool() ||
        !settings.value(QStringLiteral("hideOnMinimize")).isBool() ||
        !settings.value(QStringLiteral("debugEnabled")).isBool() || !limit.isDouble() || limit.toDouble() < 1 ||
        limit.toDouble() > 9999 || std::floor(limit.toDouble()) != limit.toDouble()) {
        error = text("The configuration backup has missing, unknown or invalid options.");
        return false;
    }
    ConfigurationBackup result;
    // Use the same strict validation and migrations as the editor and runtime.
    const auto rules = QJsonDocument(root.value(QStringLiteral("rules")).toObject()).toJson(QJsonDocument::Compact);
    if (!decodeRules(QString::fromUtf8(rules), result.rules, error))
        return false;
    result.settings = settings.toVariantMap();
    result.sortOrder = root.value(QStringLiteral("sortOrder")).toString();
    backup = result; // Leave the caller's state untouched on any failure.
    return true;
}

QByteArray encodeConfigurationBackup(const ConfigurationBackup &backup, QString &error) {
    const auto rules = QJsonDocument::fromJson(encodeRules(backup.rules).toUtf8()).object();
    const QJsonObject root{{QStringLiteral("format"), QString::fromLatin1(ScriptId)},
                           {QStringLiteral("formatVersion"), 1},
                           {QStringLiteral("rules"), rules},
                           {QStringLiteral("settings"), QJsonObject::fromVariantMap(backup.settings)},
                           {QStringLiteral("sortOrder"), backup.sortOrder}};
    const auto json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    ConfigurationBackup checked;
    return decodeConfigurationBackup(json, checked, error) ? json : QByteArray();
}

bool readConfigurationBackup(const QString &path, ConfigurationBackup &backup, QString &error) {
    error.clear();
    if (!QFileInfo(path).isFile()) {
        error = text("Select a regular configuration backup file.");
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = text("Could not open the configuration backup: ") + file.errorString();
        return false;
    }
    // Bound the read even if the file grows after selection.
    const auto bytes = file.read(MaxConfigurationBackupBytes + 1);
    if (file.error() != QFileDevice::NoError) {
        error = text("Could not read the configuration backup: ") + file.errorString();
        return false;
    }
    return decodeConfigurationBackup(bytes, backup, error);
}

bool writeConfigurationBackup(const QString &path, const QByteArray &json, QString &error) {
    ConfigurationBackup checked;
    if (!decodeConfigurationBackup(json, checked, error))
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(json) != json.size() || !file.commit()) {
        error = text("Could not export the configuration: ") + file.errorString();
        return false;
    }
    return true;
}
} // namespace Minimize2Tray

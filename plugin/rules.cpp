#include "rules.h"

#include <KConfigGroup>
#include <KService>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>

namespace Minimize2Tray {
static QString text(const char *value) { return QCoreApplication::translate("Minimize2Tray", value); }

QString desktopIdentity(QString value) {
    value = QFileInfo(value).fileName();
    if (value.endsWith(QLatin1String(".desktop"))) {
        value.chop(8);
    }
    return value;
}

bool protectedIdentity(const QString &value) {
    const QString id = desktopIdentity(value).toLower();
    static const QSet<QString> protectedIds{QString::fromLatin1(ScriptId),
                                            QStringLiteral("kcm_minimize2tray"),
                                            QStringLiteral("kwin-minimize2tray"),
                                            QStringLiteral("minimize2tray"),
                                            QStringLiteral("systemsettings"),
                                            QStringLiteral("systemsettings5"),
                                            QStringLiteral("systemsettings6"),
                                            QStringLiteral("org.kde.systemsettings"),
                                            QStringLiteral("kcmshell6"),
                                            QStringLiteral("org.kde.kcmshell6"),
                                            QStringLiteral("kwin"),
                                            QStringLiteral("kwin_wayland"),
                                            QStringLiteral("kwin_x11")};
    return protectedIds.contains(id);
}

bool protectedWindow(const QVariantMap &window) {
    return protectedIdentity(window.value(QStringLiteral("desktopFile")).toString()) ||
           protectedIdentity(window.value(QStringLiteral("resourceClass")).toString()) ||
           protectedIdentity(window.value(QStringLiteral("resourceName")).toString()) ||
           window.value(QStringLiteral("role")).toString().startsWith(QLatin1String("minimize2tray-"));
}

QString configLockPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           QStringLiteral("/kwin-minimize2tray.lock");
}

QVariantMap newRule() {
    return {
        {QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("title"), text("New rule")},
        {QStringLiteral("description"), QString()},
        {QStringLiteral("enabled"), true},
        {QStringLiteral("match"), QVariantMap{{QStringLiteral("windowTypes"), QStringList{QStringLiteral("normal")}},
                                              {QStringLiteral("includeChildren"), false}}}};
}

QString encodeRules(const QVariantList &rules, bool indented) {
    return QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("schemaVersion"), 1},
                                                       {QStringLiteral("rules"), QJsonArray::fromVariantList(rules)}})
                                 .toJson(indented ? QJsonDocument::Indented : QJsonDocument::Compact));
}

static bool validString(const QJsonValue &v, int maximum, bool empty = true) {
    return v.isString() && v.toString().size() <= maximum && !v.toString().contains(QChar::Null) &&
           (empty || !v.toString().trimmed().isEmpty());
}

static bool onlyKeys(const QJsonObject &o, const QSet<QString> &allowed) {
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (!allowed.contains(it.key())) {
            return false;
        }
    }
    return true;
}

bool decodeRules(const QString &json, QVariantList &rules, QString &error) {
    error.clear();
    auto invalid = [&error](const QString &reason) {
        error = reason;
        return false;
    };
    if (json.size() > 1024 * 1024) {
        return invalid(text("The rules exceed the 1 MiB size limit."));
    }
    const auto bytes = json.toUtf8();
    if (bytes.size() > 1024 * 1024)
        return invalid(text("The rules exceed the 1 MiB size limit."));
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return invalid(text("The rules are not valid JSON: ") + parseError.errorString());
    }
    const auto root = doc.object();
    if (!onlyKeys(root, {QStringLiteral("schemaVersion"), QStringLiteral("rules")}) ||
        root.value(QStringLiteral("schemaVersion")) != QJsonValue(1) ||
        !root.value(QStringLiteral("rules")).isArray()) {
        return invalid(text("Unsupported rule format or schema version."));
    }
    const auto array = root.value(QStringLiteral("rules")).toArray();
    if (array.size() > MaxRules) {
        return invalid(text("There are too many rules (maximum 1024)."));
    }
    QSet<QString> ids;
    QVariantList result;
    const QSet<QString> conditions{QStringLiteral("desktopFile"),  QStringLiteral("resourceClass"),
                                   QStringLiteral("resourceName"), QStringLiteral("legacyClass"),
                                   QStringLiteral("role"),         QStringLiteral("caption")};
    auto matchKeys = conditions;
    matchKeys.insert(QStringLiteral("windowTypes"));
    matchKeys.insert(QStringLiteral("includeChildren"));
    for (const auto &value : array) {
        const auto rule = value.toObject();
        const QString id = rule.value(QStringLiteral("id")).toString();
        const QString normalizedId = QUuid(id).toString(QUuid::WithoutBraces);
        if (!value.isObject() ||
            !onlyKeys(rule, {QStringLiteral("id"), QStringLiteral("title"), QStringLiteral("description"),
                             QStringLiteral("enabled"), QStringLiteral("match")}) ||
            !validString(rule.value(QStringLiteral("id")), 38, false) || QUuid(id).isNull() ||
            ids.contains(normalizedId) || !validString(rule.value(QStringLiteral("title")), 256, false) ||
            !validString(rule.value(QStringLiteral("description")), 4096) ||
            !rule.value(QStringLiteral("enabled")).isBool() || !rule.value(QStringLiteral("match")).isObject()) {
            return invalid(text("A rule has missing, invalid or duplicate fields."));
        }
        ids.insert(normalizedId);
        const auto match = rule.value(QStringLiteral("match")).toObject();
        const auto types = match.value(QStringLiteral("windowTypes"));
        if (!onlyKeys(match, matchKeys) || !types.isArray() || types.toArray().isEmpty() ||
            types.toArray().size() > 3 || !match.value(QStringLiteral("includeChildren")).isBool()) {
            return invalid(text("A rule has invalid window types or child-window settings."));
        }
        QSet<QString> seenTypes;
        for (const auto &type : types.toArray()) {
            if (!type.isString() ||
                !QStringList{QStringLiteral("normal"), QStringLiteral("dialog"), QStringLiteral("utility")}.contains(
                    type.toString()) ||
                seenTypes.contains(type.toString())) {
                return invalid(text("A rule contains an unsupported or repeated window type."));
            }
            seenTypes.insert(type.toString());
        }
        bool hasIdentity = false;
        for (const auto &key : conditions) {
            if (!match.contains(key)) {
                continue;
            }
            const auto condition = match.value(key).toObject();
            const QString op = condition.value(QStringLiteral("operator")).toString();
            const bool identity = key != QLatin1String("role") && key != QLatin1String("caption");
            if (!match.value(key).isObject() ||
                !onlyKeys(condition, {QStringLiteral("operator"), QStringLiteral("value")}) ||
                !validString(condition.value(QStringLiteral("value")), 1024, false) ||
                (op != QLatin1String("equals") && (identity || op != QLatin1String("contains")))) {
                return invalid(text("A matching condition is invalid. Application identities require an exact match."));
            }
            const QString pattern = condition.value(QStringLiteral("value")).toString();
            if (identity && (protectedIdentity(pattern) ||
                             (key != QLatin1String("legacyClass") &&
                              (pattern.contains(QLatin1Char('/')) || pattern.contains(QLatin1Char('\\')) ||
                               pattern.contains(QLatin1Char(':')))))) {
                return invalid(
                    text("Configuration hosts, Minimize2Tray itself and paths cannot be used as application targets."));
            }
            hasIdentity = hasIdentity || identity;
        }
        if (!hasIdentity) {
            return invalid(text("Select an application ID, window class or instance for each rule."));
        }
        result.append(rule.toVariantMap());
    }
    rules = result;
    return true;
}

static QVariantList legacyRules(const KConfigGroup &group, QString &error) {
    const QString legacy = group.readEntry("hideByDefaultClass", QString());
    QVariantList rules;
    QSet<QString> seen;
    for (const auto &entry : legacy.split(QLatin1Char(','))) {
        const QString name = entry.trimmed();
        if (name.isEmpty() || seen.contains(name) || protectedIdentity(name)) {
            continue;
        }
        seen.insert(name);
        auto rule = newRule();
        const bool installedIdentity =
            !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\')) && !name.contains(QLatin1Char(':'));
        const auto service = installedIdentity ? KService::serviceByStorageId(name.endsWith(QLatin1String(".desktop"))
                                                                                  ? name
                                                                                  : name + QStringLiteral(".desktop"))
                                               : KService::Ptr();
        rule[QStringLiteral("title")] =
            (service && service->isApplication() && !service->name().isEmpty() ? service->name() : name).left(256);
        rule[QStringLiteral("description")] =
            text("Imported from previous settings. Matches class or instance: ") + name;
        auto match = rule.value(QStringLiteral("match")).toMap();
        match[QStringLiteral("legacyClass")] =
            QVariantMap{{QStringLiteral("operator"), QStringLiteral("equals")}, {QStringLiteral("value"), name}};
        rule[QStringLiteral("match")] = match;
        rules.append(rule);
    }
    QVariantList checked;
    if (!decodeRules(encodeRules(rules), checked, error)) {
        return {};
    }
    return rules;
}

QVariantList readRules(KConfig &config, QString &error) {
    error.clear();
    KConfigGroup group(&config, ConfigGroup);
    QVariantList rules;
    if (group.hasKey("rules")) {
        decodeRules(group.readEntry("rules", QString()), rules, error);
        return rules;
    }
    // Serialize migration with native configuration saves.
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation));
    QLockFile lock(configLockPath());
    if (!lock.tryLock(0)) {
        error = text("Another Minimize2Tray window is updating the configuration. Try again.");
        QString legacyError;
        const auto fallback = legacyRules(group, legacyError);
        if (!legacyError.isEmpty())
            error = legacyError;
        return fallback;
    }
    config.reparseConfiguration();
    if (group.hasKey("rules")) {
        decodeRules(group.readEntry("rules", QString()), rules, error);
        return rules;
    }
    const QString legacy = group.readEntry("hideByDefaultClass", QString());
    rules = legacyRules(group, error);
    if (!error.isEmpty())
        return {};
    if (group.isEntryImmutable("rules") || !config.isConfigWritable(false)) {
        error = text("The previous settings are read-only. They remain active, but could not be migrated.");
        return rules;
    }
    if (!group.hasKey("legacyClassBackup")) {
        group.writeEntry("legacyClassBackup", legacy);
    }
    group.writeEntry("rules", encodeRules(rules));
    if (!config.sync()) {
        config.markAsClean();
        error = text("The previous settings could not be migrated. Check that kwinrc is writable.");
    }
    return rules;
}

QVariantMap readSettings(KConfig &config) {
    const KConfigGroup group(&config, ConfigGroup);
    return {{QStringLiteral("debugEnabled"), group.readEntry("debugEnabled", false)},
            {QStringLiteral("countUseDot"), group.readEntry("countUseDot", false)},
            {QStringLiteral("countDisplayLimit"), std::clamp(group.readEntry("countDisplayLimit", 99), 1, 9999)},
            {QStringLiteral("hideOnMinimize"), group.readEntry("hideOnMinimize", false)}};
}

QString windowType(const QVariantMap &window) {
    const auto type = window.value(QStringLiteral("type"));
    if (type.metaType().id() == QMetaType::QString) {
        return type.toString();
    }
    if (!type.isValid())
        return QStringLiteral("other");
    switch (type.toInt()) {
    case 0:
        return QStringLiteral("normal");
    case 5:
        return QStringLiteral("dialog");
    case 8:
        return QStringLiteral("utility");
    default:
        return QStringLiteral("other");
    }
}

QString matchFailure(const QVariantMap &rule, const QVariantMap &window) {
    if (protectedWindow(window)) {
        return text("Protected configuration or system window");
    }
    if (!rule.value(QStringLiteral("enabled")).toBool()) {
        return text("Rule disabled");
    }
    if (!window.contains(QStringLiteral("minimizable")) || !window.contains(QStringLiteral("modal")) ||
        !window.contains(QStringLiteral("specialWindow"))) {
        return text("Refresh with Minimize2Tray enabled to verify eligibility");
    }
    const QString type = windowType(window);
    if (!window.value(QStringLiteral("minimizable")).toBool() || window.value(QStringLiteral("modal")).toBool() ||
        window.value(QStringLiteral("specialWindow")).toBool() ||
        !QStringList{QStringLiteral("normal"), QStringLiteral("dialog"), QStringLiteral("utility")}.contains(type)) {
        return text("Window cannot safely be minimized");
    }
    const auto match = rule.value(QStringLiteral("match")).toMap();
    if (!match.value(QStringLiteral("windowTypes")).toStringList().contains(type)) {
        return text("Different window type");
    }
    if (!match.value(QStringLiteral("includeChildren")).toBool() &&
        (window.value(QStringLiteral("hasTransientParent")).toBool() || type == QLatin1String("dialog"))) {
        return text("Child window or dialog");
    }
    for (auto it = match.begin(); it != match.end(); ++it) {
        if (it.key() == QLatin1String("windowTypes") || it.key() == QLatin1String("includeChildren")) {
            continue;
        }
        const auto condition = it.value().toMap();
        QString actual = window.value(it.key()).toString();
        QString wanted = condition.value(QStringLiteral("value")).toString();
        if (it.key() == QLatin1String("legacyClass")) {
            if (window.value(QStringLiteral("resourceName")).toString() != wanted &&
                window.value(QStringLiteral("resourceClass")).toString() != wanted) {
                return text("Different class or instance");
            }
            continue;
        }
        if (it.key() == QLatin1String("desktopFile")) {
            actual = desktopIdentity(actual);
            wanted = desktopIdentity(wanted);
        }
        const bool matches = condition.value(QStringLiteral("operator")) == QLatin1String("contains")
                                 ? actual.contains(wanted, Qt::CaseSensitive)
                                 : actual == wanted;
        if (!matches) {
            if (it.key() == QLatin1String("desktopFile"))
                return text("Different application");
            if (it.key() == QLatin1String("resourceClass"))
                return text("Different window class");
            if (it.key() == QLatin1String("resourceName"))
                return text("Different window instance");
            if (it.key() == QLatin1String("role"))
                return text("Different window role");
            return text("Different window title");
        }
    }
    return {};
}

QString ruleSummary(const QVariantMap &rule) {
    const auto match = rule.value(QStringLiteral("match")).toMap();
    QStringList parts;
    for (const auto *key : {"desktopFile", "resourceClass", "resourceName", "legacyClass", "role", "caption"}) {
        const auto value = match.value(QLatin1String(key)).toMap().value(QStringLiteral("value")).toString();
        if (!value.isEmpty()) {
            QString label;
            if (qstrcmp(key, "desktopFile") == 0)
                label = text("App");
            else if (qstrcmp(key, "resourceClass") == 0)
                label = text("Class");
            else if (qstrcmp(key, "resourceName") == 0)
                label = text("Instance");
            else if (qstrcmp(key, "legacyClass") == 0)
                label = text("Class or instance");
            else if (qstrcmp(key, "role") == 0)
                label = text("Role");
            else
                label = text("Title");
            parts.append(label + QStringLiteral(": ") + value);
        }
    }
    QStringList types;
    for (const auto &type : match.value(QStringLiteral("windowTypes")).toStringList()) {
        types.append(type == QLatin1String("normal")   ? text("Normal window")
                     : type == QLatin1String("dialog") ? text("Dialog")
                                                       : text("Utility window"));
    }
    parts.append(types.join(QStringLiteral(", ")));
    if (!match.value(QStringLiteral("includeChildren")).toBool()) {
        parts.append(text("No child windows"));
    }
    return parts.join(QStringLiteral(" · "));
}

QString searchText(const QVariantMap &rule) {
    return rule.value(QStringLiteral("title")).toString() + QLatin1Char('\n') +
           rule.value(QStringLiteral("description")).toString() + QLatin1Char('\n') + ruleSummary(rule);
}
} // namespace Minimize2Tray

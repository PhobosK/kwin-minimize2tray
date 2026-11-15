#include "rules.h"
#include "applicationidentity.h"
#include <QCache>
#include <QRegularExpression>

#include <KConfigGroup>
#include <KService>
#include <QCoreApplication>
#include <QDateTime>
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

QVariantMap windowProperties(QObject *window) {
    if (!window)
        return {};
    const auto type = window->property("normalWindow").toBool() ? QStringLiteral("normal")
                      : window->property("dialog").toBool()     ? QStringLiteral("dialog")
                      : window->property("utility").toBool()    ? QStringLiteral("utility")
                                                                : QStringLiteral("other");
    return {{QStringLiteral("uuid"), window->property("internalId")},
            {QStringLiteral("pid"), window->property("pid")},
            {QStringLiteral("desktopFile"), window->property("desktopFileName")},
            {QStringLiteral("resourceClass"), window->property("resourceClass")},
            {QStringLiteral("resourceName"), window->property("resourceName")},
            {QStringLiteral("role"), window->property("windowRole")},
            {QStringLiteral("caption"), window->property("caption")},
            {QStringLiteral("type"), type},
            {QStringLiteral("hasTransientParent"), window->property("transient")},
            {QStringLiteral("modal"), window->property("modal")},
            {QStringLiteral("specialWindow"), window->property("specialWindow").toBool() ||
                                                  window->property("appletPopup").toBool() ||
                                                  window->property("splash").toBool()},
            {QStringLiteral("minimizable"), window->property("minimizable")}};
}

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
        {QStringLiteral("title"), QCoreApplication::translate("Minimize2Tray", "New rule")},
        {QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("description"), QString()},
        {QStringLiteral("enabled"), true},
        {QStringLiteral("autoCreated"), false},
        {QStringLiteral("inactiveMatch"), QVariantMap()},
        {QStringLiteral("iconOverride"),
         QVariantMap{{QStringLiteral("enabled"), false}, {QStringLiteral("path"), QString()}}},
        {QStringLiteral("behavior"), QVariantMap{{QStringLiteral("skipNativeTray"), false},
                                                 {QStringLiteral("inheritStartup"), true},
                                                 {QStringLiteral("autoPin"), false},
                                                 {QStringLiteral("startInTray"), false},
                                                 {QStringLiteral("minimizeToTray"), QStringLiteral("inherit")},
                                                 {QStringLiteral("closeToTray"), false},
                                                 {QStringLiteral("startupProtection"), false},
                                                 {QStringLiteral("terminateOnLogout"), false}}},
        {QStringLiteral("match"), QVariantMap{{QStringLiteral("windowTypes"), QStringList{QStringLiteral("normal")}},
                                              {QStringLiteral("includeChildren"), false}}}};
}

QVariantMap automaticRule(const QVariantMap &window, const QVariantMap &widerRule, const QVariantList &windows,
                          QString &error) {
    error.clear();
    if (QUuid(window.value(QStringLiteral("uuid")).toString()).isNull()) {
        error = QCoreApplication::translate("Minimize2Tray", "The selected window is no longer available. Select it again.");
        return {};
    }
    auto rule = newRule();
    rule[QStringLiteral("autoCreated")] = true;
    auto match = widerRule.isEmpty() ? rule.value(QStringLiteral("match")).toMap()
                                     : widerRule.value(QStringLiteral("match")).toMap();
    if (!widerRule.isEmpty()) {
        auto enabled = widerRule;
        enabled[QStringLiteral("enabled")] = true;
        if (!matchFailure(enabled, window).isEmpty()) {
            error = QCoreApplication::translate("Minimize2Tray", "The selected window no longer matches the existing rule. Select it again.");
            return {};
        }
    }
    match[QStringLiteral("windowTypes")] = QStringList{windowType(window)};
    match[QStringLiteral("includeChildren")] =
        window.value(QStringLiteral("hasTransientParent")).toBool() || windowType(window) == QLatin1String("dialog");
    for (const auto &key : {QStringLiteral("desktopFile"), QStringLiteral("resourceClass"),
                            QStringLiteral("resourceName"), QStringLiteral("role")}) {
        auto value = window.value(key).toString();
        if (key == QLatin1String("desktopFile"))
            value = desktopIdentity(value);
        if (!value.isEmpty())
            match[key] =
                QVariantMap{{QStringLiteral("operator"), QStringLiteral("equals")}, {QStringLiteral("value"), value}};
    }
    rule[QStringLiteral("match")] = match;
    auto behavior = rule.value(QStringLiteral("behavior")).toMap();
    behavior[QStringLiteral("inheritStartup")] = false;
    behavior[QStringLiteral("startInTray")] = true;
    behavior[QStringLiteral("minimizeToTray")] = QStringLiteral("enabled");
    rule[QStringLiteral("behavior")] = behavior;
    auto identity = window.value(QStringLiteral("resourceClass")).toString();
    if (identity.isEmpty())
        identity = desktopIdentity(window.value(QStringLiteral("desktopFile")).toString());
    if (identity.isEmpty())
        identity = window.value(QStringLiteral("resourceName")).toString();
    rule[QStringLiteral("title")] = identity.left(256);
    auto overlaps = [&]() {
        return std::any_of(windows.cbegin(), windows.cend(), [&](const auto &entry) {
            const auto other = entry.toMap();
            return other.value(QStringLiteral("uuid")) != window.value(QStringLiteral("uuid")) &&
                   matchFailure(rule, other).isEmpty();
        });
    };
    // Prefer stable roles. Exact titles are a fail-closed fallback for windows
    // that expose no distinct role, rather than guessing a title substring.
    const auto role = match.value(QStringLiteral("role")).toMap();
    const bool needsTitle =
        (!widerRule.isEmpty() &&
         (role.isEmpty() || widerRule.value(QStringLiteral("match")).toMap().value(QStringLiteral("role")) == role)) ||
        (role.isEmpty() && (windowType(window) != QLatin1String("normal") ||
                            window.value(QStringLiteral("hasTransientParent")).toBool()));
    if (needsTitle || overlaps()) {
        const auto caption = window.value(QStringLiteral("caption")).toString();
        if (caption.isEmpty()) {
            error = QCoreApplication::translate("Minimize2Tray", "This window has no distinct role or title. Create or edit its rule manually.");
            return {};
        }
        match[QStringLiteral("caption")] =
            QVariantMap{{QStringLiteral("operator"), QStringLiteral("equals")}, {QStringLiteral("value"), caption}};
        rule[QStringLiteral("match")] = match;
    }
    QVariantList checked;
    if (!decodeRules(encodeRules({rule}), checked, error))
        return {};
    const auto failure = matchFailure(rule, window);
    if (!failure.isEmpty()) {
        error = failure;
        return {};
    }
    if (overlaps()) {
        error = QCoreApplication::translate("Minimize2Tray", "These windows expose identical matching properties. An automatic rule cannot safely distinguish "
                     "the selected window.");
        return {};
    }
    // JSON treats QStringList and lists loaded from JSON arrays consistently.
    if (!widerRule.isEmpty() && QJsonObject::fromVariantMap(match) ==
                                    QJsonObject::fromVariantMap(widerRule.value(QStringLiteral("match")).toMap())) {
        error = QCoreApplication::translate("Minimize2Tray", "The existing rule already uses these specific properties. Edit that rule instead.");
        return {};
    }
    return rule;
}

static QRegularExpression boundedExpression(const QString &pattern) {
    // Qt uses PCRE2. Limits keep user patterns from monopolizing KWin's thread.
    static QCache<QString, QRegularExpression> cache(256);
    if (const auto cached = cache.object(pattern))
        return *cached;
    QRegularExpression expression(QStringLiteral("(*LIMIT_MATCH=10000)(*LIMIT_DEPTH=100)") + pattern);
    cache.insert(pattern, new QRegularExpression(expression));
    return expression;
}

QString encodeRules(const QVariantList &rules, bool indented) {
    return QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("schemaVersion"), 9},
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
        return invalid(QCoreApplication::translate("Minimize2Tray", "The rules exceed the 1 MiB size limit."));
    }
    const auto bytes = json.toUtf8();
    if (bytes.size() > 1024 * 1024)
        return invalid(QCoreApplication::translate("Minimize2Tray", "The rules exceed the 1 MiB size limit."));
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return invalid(QCoreApplication::translate("Minimize2Tray", "The rules are not valid JSON: ") + parseError.errorString());
    }
    const auto root = doc.object();
    const auto schema = root.value(QStringLiteral("schemaVersion"));
    if (!onlyKeys(root, {QStringLiteral("schemaVersion"), QStringLiteral("rules")}) ||
        (schema != QJsonValue(1) && schema != QJsonValue(2) && schema != QJsonValue(3) && schema != QJsonValue(4) &&
         schema != QJsonValue(5) && schema != QJsonValue(6) && schema != QJsonValue(7) && schema != QJsonValue(8) && schema != QJsonValue(9)) ||
        !root.value(QStringLiteral("rules")).isArray()) {
        return invalid(QCoreApplication::translate("Minimize2Tray", "Unsupported rule format or schema version."));
    }
    const int version = schema.toInt();
    const auto array = root.value(QStringLiteral("rules")).toArray();
    if (array.size() > MaxRules) {
        return invalid(QCoreApplication::translate("Minimize2Tray", "There are too many rules (maximum 1024)."));
    }
    QSet<QString> ids;
    QVariantList result;
    const QSet<QString> conditions{QStringLiteral("desktopFile"),  QStringLiteral("resourceClass"),
                                   QStringLiteral("resourceName"), QStringLiteral("legacyClass"),
                                   QStringLiteral("role"),         QStringLiteral("resourceClassComplete"),
                                   QStringLiteral("caption")};
    auto matchKeys = conditions;
    matchKeys.insert(QStringLiteral("windowTypes"));
    matchKeys.insert(QStringLiteral("includeChildren"));
    QSet<QString> ruleKeys{QStringLiteral("id"), QStringLiteral("title"), QStringLiteral("description"),
                           QStringLiteral("enabled"), QStringLiteral("match")};
    if (schema != QJsonValue(1))
        ruleKeys.insert(QStringLiteral("behavior"));
    if (version >= 5)
        ruleKeys.insert(QStringLiteral("createdAt"));
    if (version >= 9) ruleKeys.insert(QStringLiteral("inactiveMatch"));
    if (version >= 8)
        ruleKeys.insert(QStringLiteral("iconOverride"));
    if (version >= 7)
        ruleKeys.insert(QStringLiteral("autoCreated"));
    for (const auto &value : array) {
        const auto rule = value.toObject();
        const QString id = rule.value(QStringLiteral("id")).toString();
        const QString normalizedId = QUuid(id).toString(QUuid::WithoutBraces);
        if (!value.isObject() ||
            !onlyKeys(rule, ruleKeys) ||
            !validString(rule.value(QStringLiteral("id")), 38, false) || QUuid(id).isNull() ||
            ids.contains(normalizedId) || !validString(rule.value(QStringLiteral("title")), 256, false) ||
            !validString(rule.value(QStringLiteral("description")), 4096) ||
            !rule.value(QStringLiteral("enabled")).isBool() || !rule.value(QStringLiteral("match")).isObject()) {
            return invalid(QCoreApplication::translate("Minimize2Tray", "A rule has missing, invalid or duplicate fields."));
        }
        if (version >= 5) {
            const auto created = rule.value(QStringLiteral("createdAt"));
            const auto date = QDateTime::fromString(created.toString(), Qt::ISODateWithMs);
            // Null means the creation time of an older rule is unknown. Otherwise
            // require a canonical UTC timestamp, so exported dates are unambiguous.
            if (!created.isNull() && (!created.isString() || !date.isValid() ||
                created.toString().size() != 24 || date.toUTC().toString(Qt::ISODateWithMs) != created.toString()))
                return invalid(QCoreApplication::translate("Minimize2Tray", "A rule has an invalid creation timestamp."));
        }
        if (version >= 7 && !rule.value(QStringLiteral("autoCreated")).isBool())
            return invalid(QCoreApplication::translate("Minimize2Tray", "The automatically created flag must be a boolean setting."));
        if (version >= 8) {
            const auto icon = rule.value(QStringLiteral("iconOverride")).toObject();
            if (!rule.value(QStringLiteral("iconOverride")).isObject() || icon.size() != 2 ||
                !onlyKeys(icon, {QStringLiteral("enabled"), QStringLiteral("path")}) ||
                !icon.value(QStringLiteral("enabled")).isBool() ||
                !validString(icon.value(QStringLiteral("path")), 4096) ||
                (!icon.value(QStringLiteral("path")).toString().isEmpty() &&
                 !validIconPath(icon.value(QStringLiteral("path")).toString())) ||
                (icon.value(QStringLiteral("enabled")).toBool() &&
                 icon.value(QStringLiteral("path")).toString().isEmpty()))
                return invalid(QCoreApplication::translate("Minimize2Tray", "Choose a local desktop file or icon for the tray icon override."));
        }
        ids.insert(normalizedId);
        if (schema != QJsonValue(1)) {
            const auto behavior = rule.value(QStringLiteral("behavior"));
            const auto object = behavior.toObject();
            QSet<QString> keys = schema == QJsonValue(2)
                ? QSet<QString>{QStringLiteral("startInTray")}
                : QSet<QString>{QStringLiteral("autoPin"), QStringLiteral("startInTray"),
                                QStringLiteral("minimizeToTray"), QStringLiteral("closeToTray")};
            if (version >= 4)
                keys.insert(QStringLiteral("startupProtection"));
            if (version >= 9) keys.insert(QStringLiteral("skipNativeTray"));
            if (version >= 8)
                keys.insert(QStringLiteral("inheritStartup"));
            if (version >= 6)
                keys.insert(QStringLiteral("terminateOnLogout"));
            if (!behavior.isObject() || !onlyKeys(object, keys) ||
                !object.value(QStringLiteral("startInTray")).isBool()) {
                return invalid(QCoreApplication::translate("Minimize2Tray", "A rule has missing or invalid behavior settings."));
            }
            if (schema != QJsonValue(2) &&
                (!object.value(QStringLiteral("autoPin")).isBool() ||
                 !object.value(QStringLiteral("closeToTray")).isBool() ||
                 !QStringList{QStringLiteral("inherit"), QStringLiteral("enabled"), QStringLiteral("disabled")}
                      .contains(object.value(QStringLiteral("minimizeToTray")).toString()) ||
                 (object.value(QStringLiteral("autoPin")).toBool() &&
                  object.value(QStringLiteral("startInTray")).toBool()))) {
                return invalid(QCoreApplication::translate("Minimize2Tray", "A rule has invalid action settings or conflicting startup actions."));
            }
            if (version >= 9 && !object.value(QStringLiteral("skipNativeTray")).isBool())
                return invalid(QCoreApplication::translate("Minimize2Tray", "Native tray conflict handling must be a boolean setting."));
            if (version >= 8 && (!object.value(QStringLiteral("inheritStartup")).isBool() ||
                                 (object.value(QStringLiteral("inheritStartup")).toBool() &&
                                  (object.value(QStringLiteral("autoPin")).toBool() ||
                                   object.value(QStringLiteral("startInTray")).toBool()))))
                return invalid(QCoreApplication::translate("Minimize2Tray", "The global startup choice conflicts with an explicit startup action."));
            if (version >= 4 && (!object.value(QStringLiteral("startupProtection")).isBool() ||
                                 (object.value(QStringLiteral("startupProtection")).toBool() &&
                                  !object.value(QStringLiteral("startInTray")).toBool() &&
                                  !object.value(QStringLiteral("inheritStartup")).toBool()))) {
                return invalid(QCoreApplication::translate("Minimize2Tray", "Startup protection requires Start in tray and a boolean setting."));
            }
        }
        const auto match = rule.value(QStringLiteral("match")).toObject();
        if (version < 8 && match.contains(QStringLiteral("resourceClassComplete")))
            return invalid(QCoreApplication::translate("Minimize2Tray", "Whole window class requires rule schema version 8 or later."));
        const auto types = match.value(QStringLiteral("windowTypes"));
        if (!onlyKeys(match, matchKeys) || !types.isArray() || types.toArray().isEmpty() ||
            types.toArray().size() > 3 || !match.value(QStringLiteral("includeChildren")).isBool()) {
            return invalid(QCoreApplication::translate("Minimize2Tray", "A rule has invalid window types or child-window settings."));
        }
        QSet<QString> seenTypes;
        for (const auto &type : types.toArray()) {
            if (!type.isString() ||
                !QStringList{QStringLiteral("normal"), QStringLiteral("dialog"), QStringLiteral("utility")}.contains(
                    type.toString()) ||
                seenTypes.contains(type.toString())) {
                return invalid(QCoreApplication::translate("Minimize2Tray", "A rule contains an unsupported or repeated window type."));
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
                (!QStringList{QStringLiteral("equals"), QStringLiteral("contains"), QStringLiteral("regexp")}.contains(
                     op) ||
                 (version < 8 && op != QLatin1String("equals") && (identity || op != QLatin1String("contains"))))) {
                return invalid(QCoreApplication::translate("Minimize2Tray", "A matching condition has an invalid operator or value."));
            }
            const QString pattern = condition.value(QStringLiteral("value")).toString();
            if (op == QLatin1String("regexp") && !boundedExpression(pattern).isValid())
                return invalid(QCoreApplication::translate("Minimize2Tray", "A regular expression is invalid or exceeds the supported complexity."));
            if (identity && op == QLatin1String("equals") &&
                (protectedIdentity(pattern) ||
                 (key != QLatin1String("legacyClass") &&
                  (pattern.contains(QLatin1Char('/')) || pattern.contains(QLatin1Char('\\')) ||
                   pattern.contains(QLatin1Char(':')))))) {
                return invalid(
                    QCoreApplication::translate("Minimize2Tray", "Configuration hosts, Minimize2Tray itself and paths cannot be used as application targets."));
            }
            hasIdentity = hasIdentity || identity;
        }
        if (!hasIdentity) {
            return invalid(QCoreApplication::translate("Minimize2Tray", "Select an application ID, window class or instance for each rule."));
        }
        if (version >= 6 &&
            !rule.value(QStringLiteral("behavior")).toObject().value(QStringLiteral("terminateOnLogout")).isBool())
            return invalid(QCoreApplication::translate("Minimize2Tray", "Quit on logout must be a boolean setting."));
        if (version >= 9) {
            const auto inactive = rule.value(QStringLiteral("inactiveMatch")).toObject();
            if (!rule.value(QStringLiteral("inactiveMatch")).isObject() || !onlyKeys(inactive, conditions))
                return invalid(QCoreApplication::translate("Minimize2Tray", "The inactive matching properties are invalid."));
            for (auto it = inactive.begin(); it != inactive.end(); ++it) {
                const auto condition = it.value().toObject();
                // Inactive values never participate in matching. Validate a pattern when re-enabled.
                if (match.contains(it.key()) || !it.value().isObject() || condition.size() != 2 ||
                    !onlyKeys(condition, {QStringLiteral("operator"), QStringLiteral("value")}) ||
                    !validString(condition.value(QStringLiteral("value")), 1024, false) ||
                    !QStringList{QStringLiteral("equals"), QStringLiteral("contains"), QStringLiteral("regexp")}
                        .contains(condition.value(QStringLiteral("operator")).toString()))
                    return invalid(QCoreApplication::translate("Minimize2Tray", "An inactive matching property has invalid or conflicting fields."));
            }
        }
        auto decoded = rule.toVariantMap();
        if (version < 9) {
            decoded[QStringLiteral("inactiveMatch")] = QVariantMap();
        }
        if (version < 8) {
            decoded[QStringLiteral("iconOverride")] =
                QVariantMap{{QStringLiteral("enabled"), false}, {QStringLiteral("path"), QString()}};
        }
        if (schema == QJsonValue(1) || schema == QJsonValue(2)) {
            // Upgrade in memory; leave the stored document intact until Apply/OK.
            const bool start = schema == QJsonValue(1) ||
                               decoded.value(QStringLiteral("behavior")).toMap().value(QStringLiteral("startInTray")).toBool();
            decoded.insert(QStringLiteral("behavior"),
                           QVariantMap{{QStringLiteral("autoPin"), !start}, {QStringLiteral("startInTray"), start},
                                       {QStringLiteral("minimizeToTray"), QStringLiteral("inherit")},
                                       {QStringLiteral("closeToTray"), false}});
        }
        if (schema == QJsonValue(1) || schema == QJsonValue(2) || schema == QJsonValue(3)) {
            auto behavior = decoded.value(QStringLiteral("behavior")).toMap();
            behavior.insert(QStringLiteral("startupProtection"), false);
            decoded.insert(QStringLiteral("behavior"), behavior);
        }
        if (version < 6) {
            auto behavior = decoded.value(QStringLiteral("behavior")).toMap();
            behavior.insert(QStringLiteral("terminateOnLogout"), false);
            decoded.insert(QStringLiteral("behavior"), behavior);
        }
        if (version < 8) {
            auto behavior = decoded.value(QStringLiteral("behavior")).toMap();
            // Preserve every existing rule's explicit startup choice.
            behavior[QStringLiteral("inheritStartup")] = false;
            decoded[QStringLiteral("behavior")] = behavior;
        }
        if (version < 9) {
            auto behavior = decoded.value(QStringLiteral("behavior")).toMap();
            behavior[QStringLiteral("skipNativeTray")] = false;
            decoded[QStringLiteral("behavior")] = behavior;
        }
        if (version < 5)
            decoded.insert(QStringLiteral("createdAt"), QVariant());
        if (version < 7)
            decoded.insert(QStringLiteral("autoCreated"), false);
        result.append(decoded);
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
        rule[QStringLiteral("createdAt")] = QVariant(); // Original creation time is unknown.
        auto behavior = rule.value(QStringLiteral("behavior")).toMap();
        behavior[QStringLiteral("inheritStartup")] = false;
        behavior[QStringLiteral("startInTray")] = true;
        rule[QStringLiteral("behavior")] = behavior;
        const bool installedIdentity =
            !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\')) && !name.contains(QLatin1Char(':'));
        const auto service = installedIdentity ? installedApplication(name) : KService::Ptr();
        rule[QStringLiteral("title")] =
            (service && service->isApplication() && !service->name().isEmpty() ? service->name() : name).left(256);
        rule[QStringLiteral("description")] =
            QCoreApplication::translate("Minimize2Tray", "Imported from previous settings. Matches class or instance: ") + name;
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
        error = QCoreApplication::translate("Minimize2Tray", "Another Minimize2Tray window is updating the configuration. Try again.");
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
        error = QCoreApplication::translate("Minimize2Tray", "The previous settings are read-only. They remain active, but could not be migrated.");
        return rules;
    }
    if (!group.hasKey("legacyClassBackup")) {
        group.writeEntry("legacyClassBackup", legacy);
    }
    group.writeEntry("rules", encodeRules(rules));
    if (!config.sync()) {
        config.markAsClean();
        error = QCoreApplication::translate("Minimize2Tray", "The previous settings could not be migrated. Check that kwinrc is writable.");
    }
    return rules;
}

QVariantMap readSettings(KConfig &config) {
    const KConfigGroup group(&config, ConfigGroup);
    return {{QStringLiteral("debugEnabled"), group.readEntry("debugEnabled", false)},
            {QStringLiteral("startupDiagnostics"), group.readEntry("debugEnabled", false) && group.readEntry("startupDiagnostics", false)},
            {QStringLiteral("recoverTrayInput"), group.readEntry("recoverTrayInput", false)},
            {QStringLiteral("countUseDot"), group.readEntry("countUseDot", false)},
            {QStringLiteral("countDisplayLimit"), std::clamp(group.readEntry("countDisplayLimit", 99), 1, 9999)},
            {QStringLiteral("hideOnMinimize"), group.readEntry("hideOnMinimize", false)},
            {QStringLiteral("startupAction"),
             QStringList{QStringLiteral("none"), QStringLiteral("pin"), QStringLiteral("hide")}.contains(
                 group.readEntry("startupAction", QStringLiteral("hide")))
                 ? group.readEntry("startupAction", QStringLiteral("hide"))
                 : QStringLiteral("hide")}};
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
        return QCoreApplication::translate("Minimize2Tray", "Protected configuration or system window");
    }
    if (!rule.value(QStringLiteral("enabled")).toBool()) {
        return QCoreApplication::translate("Minimize2Tray", "Rule disabled");
    }
    if (!window.contains(QStringLiteral("minimizable")) || !window.contains(QStringLiteral("modal")) ||
        !window.contains(QStringLiteral("specialWindow"))) {
        return QCoreApplication::translate("Minimize2Tray", "Refresh with Minimize2Tray enabled to verify eligibility");
    }
    const QString type = windowType(window);
    if (!window.value(QStringLiteral("minimizable")).toBool() || window.value(QStringLiteral("modal")).toBool() ||
        window.value(QStringLiteral("specialWindow")).toBool() ||
        !QStringList{QStringLiteral("normal"), QStringLiteral("dialog"), QStringLiteral("utility")}.contains(type)) {
        return QCoreApplication::translate("Minimize2Tray", "Window cannot safely be minimized");
    }
    const auto match = rule.value(QStringLiteral("match")).toMap();
    if (!match.value(QStringLiteral("windowTypes")).toStringList().contains(type)) {
        return QCoreApplication::translate("Minimize2Tray", "Different window type");
    }
    if (!match.value(QStringLiteral("includeChildren")).toBool() &&
        (window.value(QStringLiteral("hasTransientParent")).toBool() || type == QLatin1String("dialog"))) {
        return QCoreApplication::translate("Minimize2Tray", "Child window or dialog");
    }
    for (auto it = match.begin(); it != match.end(); ++it) {
        if (it.key() == QLatin1String("windowTypes") || it.key() == QLatin1String("includeChildren")) {
            continue;
        }
        const auto condition = it.value().toMap();
        QString actual = matchingProperties(window).value(it.key()).toString();
        QString wanted = condition.value(QStringLiteral("value")).toString();
        if (it.key() == QLatin1String("legacyClass")) {
            const auto op = condition.value(QStringLiteral("operator")).toString();
            const auto matches = [&](const QString &actual) {
                return op == QLatin1String("contains") ? actual.contains(wanted)
                       : op == QLatin1String("regexp") ? boundedExpression(wanted).match(actual.left(4096)).hasMatch()
                                                       : actual == wanted;
            };
            if (!matches(window.value(QStringLiteral("resourceName")).toString()) &&
                !matches(window.value(QStringLiteral("resourceClass")).toString())) {
                return QCoreApplication::translate("Minimize2Tray", "Different class or instance");
            }
            continue;
        }
        if (it.key() == QLatin1String("desktopFile")) {
            const auto app = applicationService(window);
            actual = app ? app->desktopEntryName() : desktopIdentity(actual);
            if (condition.value(QStringLiteral("operator")) == QLatin1String("equals")) {
                wanted = desktopIdentity(wanted);
                const auto expected = installedApplication(wanted);
                if (expected && expected->isApplication()) wanted = expected->desktopEntryName();
            }
        }
        const auto op = condition.value(QStringLiteral("operator")).toString();
        const bool rawDesktopMatch = it.key() == QLatin1String("desktopFile") && op == QLatin1String("equals") &&
                                     desktopIdentity(window.value(QStringLiteral("desktopFile")).toString()) == wanted;
        const bool matches =
            rawDesktopMatch ||
            (op == QLatin1String("contains") ? actual.contains(wanted, Qt::CaseSensitive)
             : op == QLatin1String("regexp") ? boundedExpression(wanted).match(actual.left(4096)).hasMatch()
                                             : actual == wanted);
        if (!matches) {
            if (it.key() == QLatin1String("desktopFile"))
                return QCoreApplication::translate("Minimize2Tray", "Different application");
            if (it.key() == QLatin1String("resourceClass"))
                return QCoreApplication::translate("Minimize2Tray", "Different window class");
            if (it.key() == QLatin1String("resourceClassComplete"))
                return QCoreApplication::translate("Minimize2Tray", "Different whole window class");
            if (it.key() == QLatin1String("resourceName"))
                return QCoreApplication::translate("Minimize2Tray", "Different window instance");
            if (it.key() == QLatin1String("role"))
                return QCoreApplication::translate("Minimize2Tray", "Different window role");
            return QCoreApplication::translate("Minimize2Tray", "Different window title");
        }
    }
    return {};
}

QString ruleSummary(const QVariantMap &rule) {
    const auto match = rule.value(QStringLiteral("match")).toMap();
    QStringList parts;
    for (const auto *key :
         {"desktopFile", "resourceClass", "resourceName", "resourceClassComplete", "legacyClass", "role", "caption"}) {
        const auto value = match.value(QLatin1String(key)).toMap().value(QStringLiteral("value")).toString();
        if (!value.isEmpty()) {
            QString label;
            if (qstrcmp(key, "desktopFile") == 0)
                label = QCoreApplication::translate("Minimize2Tray", "App");
            else if (qstrcmp(key, "resourceClass") == 0)
                label = QCoreApplication::translate("Minimize2Tray", "Class");
            else if (qstrcmp(key, "resourceClassComplete") == 0)
                label = QCoreApplication::translate("Minimize2Tray", "Whole class");
            else if (qstrcmp(key, "resourceName") == 0)
                label = QCoreApplication::translate("Minimize2Tray", "Instance");
            else if (qstrcmp(key, "legacyClass") == 0)
                label = QCoreApplication::translate("Minimize2Tray", "Class or instance");
            else if (qstrcmp(key, "role") == 0)
                label = QCoreApplication::translate("Minimize2Tray", "Role");
            else
                label = QCoreApplication::translate("Minimize2Tray", "Title");
            parts.append(label + QStringLiteral(": ") + value);
        }
    }
    QStringList types;
    for (const auto &type : match.value(QStringLiteral("windowTypes")).toStringList()) {
        types.append(type == QLatin1String("normal")   ? QCoreApplication::translate("Minimize2Tray", "Normal window")
                     : type == QLatin1String("dialog") ? QCoreApplication::translate("Minimize2Tray", "Dialog")
                                                       : QCoreApplication::translate("Minimize2Tray", "Utility window"));
    }
    parts.append(types.join(QStringLiteral(", ")));
    if (!match.value(QStringLiteral("includeChildren")).toBool()) {
        parts.append(QCoreApplication::translate("Minimize2Tray", "No child windows"));
    }
    const auto behavior = rule.value(QStringLiteral("behavior")).toMap();
    parts.append(behavior.value(QStringLiteral("inheritStartup")).toBool() ? QCoreApplication::translate("Minimize2Tray", "Use global startup")
                 : behavior.value(QStringLiteral("startInTray")).toBool()  ? QCoreApplication::translate("Minimize2Tray", "Start in tray")
                 : behavior.value(QStringLiteral("autoPin")).toBool()      ? QCoreApplication::translate("Minimize2Tray", "Pin without initially hiding")
                                                                           : QCoreApplication::translate("Minimize2Tray", "No startup action"));
    if (behavior.value(QStringLiteral("startupProtection")).toBool())
        parts.append(QCoreApplication::translate("Minimize2Tray", "Retry early startup restoration once"));
    const QString minimize = behavior.value(QStringLiteral("minimizeToTray")).toString();
    parts.append(minimize == QLatin1String("enabled") ? QCoreApplication::translate("Minimize2Tray", "Minimize to tray: on")
                 : minimize == QLatin1String("disabled") ? QCoreApplication::translate("Minimize2Tray", "Minimize to tray: off")
                                                       : QCoreApplication::translate("Minimize2Tray", "Minimize to tray: use global setting"));
    parts.append(behavior.value(QStringLiteral("closeToTray")).toBool() ? QCoreApplication::translate("Minimize2Tray", "Close to tray: optional shortcut")
                                                                      : QCoreApplication::translate("Minimize2Tray", "Close to tray: off"));
    if (behavior.value(QStringLiteral("skipNativeTray")).toBool())
        parts.append(QCoreApplication::translate("Minimize2Tray", "Skip while a native tray is active"));
    if (behavior.value(QStringLiteral("terminateOnLogout")).toBool())
        parts.append(QCoreApplication::translate("Minimize2Tray", "Quit on logout when closing is disabled"));
    return parts.join(QStringLiteral(" · "));
}

QString searchText(const QVariantMap &rule) {
    return (rule.value(QStringLiteral("autoCreated")).toBool() ? QCoreApplication::translate("Minimize2Tray", "Auto-created") + QLatin1Char('\n') : QString()) +
           rule.value(QStringLiteral("title")).toString() + QLatin1Char('\n') +
           rule.value(QStringLiteral("description")).toString() + QLatin1Char('\n') + ruleSummary(rule);
}
} // namespace Minimize2Tray

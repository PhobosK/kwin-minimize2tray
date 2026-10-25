#include "scriptconfig.h"
#include "rules.h"

#include <QDBusConnection>
#include <QDebug>
#include <QStandardPaths>

ScriptConfig::ScriptConfig(QObject *parent) : QObject(parent), m_config(QStringLiteral("kwinrc")) {
    m_reloadTimer.setSingleShot(true);
    connect(&m_reloadTimer, &QTimer::timeout, this, &ScriptConfig::reload);
    const auto scheduleReload = [this]() { m_reloadTimer.start(); };
    connect(&m_watch, &KDirWatch::dirty, this, scheduleReload);
    connect(&m_watch, &KDirWatch::created, this, scheduleReload);
    connect(&m_watch, &KDirWatch::deleted, this, scheduleReload);
    for (const auto &path : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation)) {
        m_watch.addFile(path + QStringLiteral("/kwinrc"));
    }
    m_registered = QDBusConnection::sessionBus().registerObject(QStringLiteral("/Minimize2Tray"), this,
                                                                QDBusConnection::ExportScriptableSlots);
    reload();
}

ScriptConfig::~ScriptConfig() {
    if (m_registered) {
        QDBusConnection::sessionBus().unregisterObject(QStringLiteral("/Minimize2Tray"));
    }
}

void ScriptConfig::reload() {
    m_config.reparseConfiguration();
    QString error;
    const auto rules = Minimize2Tray::readRules(m_config, error);
    auto values = Minimize2Tray::readSettings(m_config);
    // Keep the last valid rules if an external edit is malformed.
    values.insert(QStringLiteral("rules"), error.isEmpty() ? rules : m_values.value(QStringLiteral("rules"), rules));
    if (error != m_error) {
        m_error = error;
        if (!error.isEmpty()) {
            qWarning().noquote() << "Minimize2Tray:" << error;
        }
    }
    if (values != m_values) {
        m_values = values;
        Q_EMIT valuesChanged();
    }
}

bool ScriptConfig::isProtected(const QVariantMap &window) const { return Minimize2Tray::protectedWindow(window); }

QStringList ScriptConfig::matchingRules(const QVariantMap &window) const {
    QStringList matches;
    for (const auto &value : m_values.value(QStringLiteral("rules")).toList()) {
        const auto rule = value.toMap();
        if (Minimize2Tray::matchFailure(rule, window).isEmpty()) {
            matches.append(rule.value(QStringLiteral("id")).toString());
        }
    }
    return matches;
}

QVariantList ScriptConfig::windows() {
    Q_EMIT windowsRequested();
    return m_windows;
}

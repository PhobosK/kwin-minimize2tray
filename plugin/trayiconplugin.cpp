#include "trayicon.h"

#include <KConfig>
#include <KConfigGroup>
#include <KDirWatch>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QStandardPaths>
#include <QTimer>
#include <QVariantMap>

class ScriptConfig : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap values READ values NOTIFY valuesChanged)

  public:
    explicit ScriptConfig(QObject *parent = nullptr) : QObject(parent), m_config(QStringLiteral("kwinrc")) {
        m_reloadTimer.setSingleShot(true);
        connect(&m_reloadTimer, &QTimer::timeout, this, &ScriptConfig::reload);
        const auto scheduleReload = [this]() { m_reloadTimer.start(); };
        connect(&m_watch, &KDirWatch::dirty, this, scheduleReload);
        connect(&m_watch, &KDirWatch::created, this, scheduleReload);
        connect(&m_watch, &KDirWatch::deleted, this, scheduleReload);
        // File watches survive atomic saves and files that do not exist yet.
        for (const auto &path : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation)) {
            m_watch.addFile(path + QStringLiteral("/kwinrc"));
        }
        reload();
    }

    QVariantMap values() const { return m_values; }

  signals:
    void valuesChanged();

  private:
    void reload() {
        m_config.reparseConfiguration();
        const KConfigGroup group(&m_config, QStringLiteral("Script-com.github.luisbocanegra.minimize2tray"));
        const QVariantMap values{
            {QStringLiteral("debugEnabled"), group.readEntry("debugEnabled", false)},
            {QStringLiteral("hideByDefaultClass"), group.readEntry("hideByDefaultClass", QString())},
            {QStringLiteral("countUseDot"), group.readEntry("countUseDot", false)},
            {QStringLiteral("countDisplayLimit"), group.readEntry("countDisplayLimit", 99)},
            {QStringLiteral("hideOnMinimize"), group.readEntry("hideOnMinimize", false)},
        };
        if (m_values != values) {
            m_values = values;
            Q_EMIT valuesChanged();
        }
    }

    KConfig m_config;
    KDirWatch m_watch;
    QTimer m_reloadTimer;
    QVariantMap m_values;
};

class TrayIconPlugin : public QQmlExtensionPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QQmlExtensionInterface")

  public:
    void registerTypes(const char *uri) override {
        qmlRegisterType<TrayIcon>(uri, 1, 0, "TrayIcon");
        qmlRegisterType<ScriptConfig>(uri, 1, 0, "ScriptConfig");
    }
};

#include "trayiconplugin.moc"

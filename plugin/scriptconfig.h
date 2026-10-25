#pragma once

#include <KConfig>
#include <KDirWatch>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

class ScriptConfig : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.github.luisbocanegra.Minimize2Tray")
    Q_PROPERTY(QVariantMap values READ values NOTIFY valuesChanged)

  public:
    explicit ScriptConfig(QObject *parent = nullptr);
    ~ScriptConfig() override;
    QVariantMap values() const { return m_values; }
    Q_INVOKABLE bool isProtected(const QVariantMap &window) const;
    Q_INVOKABLE QStringList matchingRules(const QVariantMap &window) const;
    Q_INVOKABLE void setWindows(const QVariantList &windows) { m_windows = windows; }

  public slots:
    Q_SCRIPTABLE QVariantList windows();

  signals:
    void valuesChanged();
    void windowsRequested();

  private:
    void reload();
    KConfig m_config;
    KDirWatch m_watch;
    QTimer m_reloadTimer;
    QVariantMap m_values;
    QVariantList m_windows;
    QString m_error;
    bool m_registered = false;
};

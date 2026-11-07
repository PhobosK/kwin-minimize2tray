#pragma once

#include <KConfig>
#include <KDirWatch>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>
#include <memory>

class QDialog;
class QProcess;
class SessionTermination;

class ScriptConfig : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.github.luisbocanegra.Minimize2Tray")
    Q_PROPERTY(QVariantMap values READ values NOTIFY valuesChanged)

  public:
    explicit ScriptConfig(QObject *parent = nullptr);
    ~ScriptConfig() override;
    QVariantMap values() const { return m_values; }
    // Monotonic deadlines for bounded startup protection, unaffected by wall-clock changes.
    Q_INVOKABLE qint64 monotonicMilliseconds() const { return m_elapsed.elapsed(); }
    Q_INVOKABLE void showAbout();
    Q_INVOKABLE void configure();
    Q_INVOKABLE void addAutomaticRule();
    Q_INVOKABLE bool isProtected(const QVariantMap &window) const;
    Q_INVOKABLE QStringList matchingRules(const QVariantMap &window) const;
    Q_INVOKABLE void terminateSessionWindows(const QVariantList &windows);
    Q_INVOKABLE QVariantMap matchingBehavior(const QVariantMap &window) const;
    Q_INVOKABLE void setWindows(const QVariantList &windows) { m_windows = windows; }

  public slots:
    Q_SCRIPTABLE QVariantList windows();
    Q_SCRIPTABLE bool applyAutomaticRule(const QString &windowId, const QString &ruleId);

  signals:
    void sessionClosing();
    void valuesChanged();
    void windowsRequested();
    void automaticRuleCreated(const QString &windowId);

  private:
    void reload();
    void openConfiguration(bool automaticRule);
    void showDialog(QDialog *dialog, const QString &role);
    KConfig m_config;
    std::unique_ptr<SessionTermination> m_sessionTermination;
    QPointer<QDialog> m_aboutDialog;
    QPointer<QDialog> m_configDialog; // Launch errors only; the editor runs in kcmshell6.
    QPointer<QProcess> m_configProcess;
    KDirWatch m_watch;
    QTimer m_reloadTimer;
    QElapsedTimer m_elapsed;
    QVariantMap m_values;
    QVariantList m_windows;
    QString m_error;
    QString m_rulesSource;
    bool m_hasRulesSource = false;
    bool m_registered = false;
    bool m_configRequestPending = false;
};

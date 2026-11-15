#pragma once
#include <QDBusConnection>
#include <QDBusServiceWatcher>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QVariantMap>

// Observe the session's tray registry. Matching is advisory; it never controls another application.
class NativeTrayMonitor : public QObject {
    Q_OBJECT
  public:
    explicit NativeTrayMonitor(QObject *parent = nullptr);
    ~NativeTrayMonitor() override;
    bool hasNativeTray(const QVariantMap &window) const;
    QStringList proxyPids() const;
    bool available() const { return m_available; }
  signals:
    void changed();
  private slots:
    void requestRefresh();
    void registryItemRemoved(const QString &registration);
    void itemPropertiesChanged(const QString &, const QVariantMap &changed, const QStringList &invalidated);
    void itemStatusChanged(const QString &);

  private:
    struct Item {
        QString owner, id, desktop;
        uint pid = 0;
        bool active = false;
    };
    void refresh();
    void pump();
    void finishItem();
    QDBusConnection m_bus;
    QDBusServiceWatcher m_watcher;
    QTimer m_refresh, m_deadline;
    QHash<QString, Item> m_items, m_next;
    QSet<QString> m_subscriptions;
    QQueue<QString> m_queue;
    quint64 m_generation = 0;
    uint m_kwinPid = 0;
    int m_pending = 0;
    bool m_busy = false, m_again = false, m_available = false;
};

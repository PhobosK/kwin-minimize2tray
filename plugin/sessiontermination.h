#pragma once

#include <QObject>
#include <QPointer>
#include <QVariantList>

class SessionProcess;

// Watches KWin's in-process session state, not a guessed logind or unload event.
class SessionTermination : public QObject {
    Q_OBJECT
  public:
    explicit SessionTermination(QObject *source, QObject *notifications, QObject *parent = nullptr);
    bool available() const { return m_property >= 0; }
    bool closing() const;
    void terminate(const QVariantList &windows, const QVariantList &rules, const SessionProcess &process);

  signals:
    void closingRequested();

  private slots:
    void stateChanged();

  private:
    bool setSource(QObject *source);
    QPointer<QObject> m_source;
    QPointer<QObject> m_notifications;
    int m_property = -1;
    int m_quitting = -1;
    quint64 m_generation = 0;
    bool m_wasClosing = false;
    bool m_handled = false;
};

QString sessionProcessPluginPath();

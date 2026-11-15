#pragma once
#include <QDebug>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <atomic>

namespace Minimize2Tray {
inline std::atomic_bool startupTimingEnabled{false};
inline QMutex timingMutex;
inline QSet<QString> timedOperations;
inline void setStartupTiming(bool enabled) {
    if (startupTimingEnabled.exchange(enabled, std::memory_order_relaxed) && !enabled) {
        QMutexLocker lock(&timingMutex);
        timedOperations.clear();
    }
}
inline void startupEvent(const char *event, const QString &window = {}) {
    if (startupTimingEnabled.load(std::memory_order_relaxed)) {
        QElapsedTimer now;
        now.start();
        qInfo().noquote() << "Minimize2Tray timing:" << now.msecsSinceReference() << event << window;
    }
}
class StartupSpan {
  public:
    explicit StartupSpan(const char *label, const QString &identity = {}) : label(label), identity(identity) {
        if (!startupTimingEnabled.load(std::memory_order_relaxed))
            return;
        QMutexLocker lock(&timingMutex);
        const auto key = QString::fromLatin1(label) + QLatin1Char(':') + identity;
        // One duration per operation/window per enabled diagnostic session, with a hard memory bound.
        if (timedOperations.contains(key) || timedOperations.size() >= 4096)
            return;
        timedOperations.insert(key);
        timer.start();
    }
    ~StartupSpan() {
        if (timer.isValid() && startupTimingEnabled.load(std::memory_order_relaxed)) {
            QElapsedTimer now;
            now.start();
            qInfo().noquote() << "Minimize2Tray timing:" << now.msecsSinceReference() << label << identity
                              << timer.nsecsElapsed() / 1000000.0 << "ms";
        }
    }

  private:
    const char *label;
    QString identity;
    QElapsedTimer timer;
};
} // namespace Minimize2Tray

#pragma once
#include "logging.h"
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QUuid>
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
        log(QtDebugMsg, "StartupDiagnostics", event, "observed", window);
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
            log(QtDebugMsg, "StartupDiagnostics", label, "measured", QUuid(identity).isNull() ? QString() : identity, 0,
                QStringLiteral("duration_ms=%1").arg(timer.nsecsElapsed() / 1000000.0));
        }
    }

  private:
    const char *label;
    QString identity;
    QElapsedTimer timer;
};
} // namespace Minimize2Tray

#pragma once
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <atomic>
namespace Minimize2Tray {
inline std::atomic_bool debugLogging{false};
inline QLoggingCategory &logCategory() {
    static QLoggingCategory category("minimize2tray");
    return category;
}
inline void setDebugLogging(bool enabled) {
    debugLogging.store(enabled, std::memory_order_relaxed);
    logCategory().setEnabled(QtDebugMsg, enabled);
    logCategory().setEnabled(QtWarningMsg, true);
}
inline void log(QtMsgType level, const char *component, const char *operation, const char *outcome,
                const QString &window = {}, qint64 pid = 0, const QString &detail = {}) {
    if (level == QtDebugMsg && !debugLogging.load(std::memory_order_relaxed))
        return;
    QJsonObject record{{QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                       {QStringLiteral("component"), QString::fromLatin1(component)},
                       {QStringLiteral("operation"), QString::fromLatin1(operation)},
                       {QStringLiteral("outcome"), QString::fromLatin1(outcome)}};
    if (!window.isEmpty())
        record.insert(QStringLiteral("window"), window.left(64));
    if (pid > 0)
        record.insert(QStringLiteral("pid"), pid);
    if (!detail.isEmpty())
        record.insert(QStringLiteral("detail"), detail.left(1024));
    const auto message = QString::fromUtf8(QJsonDocument(record).toJson(QJsonDocument::Compact));
    if (level == QtDebugMsg)
        qCDebug(logCategory).noquote() << message;
    else if (level == QtInfoMsg)
        qCInfo(logCategory).noquote() << message;
    else
        qCWarning(logCategory).noquote() << message;
}
} // namespace Minimize2Tray

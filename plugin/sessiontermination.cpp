#include "sessiontermination.h"
#include "rules.h"
#include "sessionprocess.h"

#include <QDBusConnection>
#include <QDebug>
#include <QFileInfo>
#include <QMetaEnum>
#include <QMetaProperty>
#include <QSet>
#include <QTimer>
#include <dlfcn.h>
#if __has_include(<linux/pidfd.h>)
#include <linux/pidfd.h>
#endif
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
const char libraryAnchor = 0;

QVariantMap windowInfo(QObject *window) {
    const auto type = window->property("normalWindow").toBool() ? QStringLiteral("normal")
                      : window->property("dialog").toBool()     ? QStringLiteral("dialog")
                      : window->property("utility").toBool()    ? QStringLiteral("utility")
                                                                : QStringLiteral("other");
    return {{QStringLiteral("desktopFile"), window->property("desktopFileName")},
            {QStringLiteral("resourceClass"), window->property("resourceClass")},
            {QStringLiteral("resourceName"), window->property("resourceName")},
            {QStringLiteral("role"), window->property("windowRole")},
            {QStringLiteral("caption"), window->property("caption")},
            {QStringLiteral("type"), type},
            {QStringLiteral("hasTransientParent"), window->property("transient")},
            {QStringLiteral("modal"), window->property("modal")},
            {QStringLiteral("specialWindow"), window->property("specialWindow")},
            {QStringLiteral("minimizable"), window->property("minimizable")}};
}

struct ProcessHandle {
    int fd;
    explicit ProcessHandle(int descriptor) : fd(descriptor) {}
    ~ProcessHandle() {
        if (fd >= 0)
            close(fd);
    }
    ProcessHandle(const ProcessHandle &) = delete;
    ProcessHandle &operator=(const ProcessHandle &) = delete;
};

// Kernel process credentials and pidfs inode identity are available together on
// Linux 6.13+. A missing ioctl or permission fails closed, without a PID fallback.
bool processIdentity(int descriptor, QByteArray &identity, qint64 &pid) {
#if defined(PIDFD_GET_INFO) && defined(SYS_pidfd_send_signal)
    struct pidfd_info info{};
    info.mask = PIDFD_INFO_PID | PIDFD_INFO_CREDS | PIDFD_INFO_EXIT;
    struct stat status{};
    if (descriptor < 0 || ioctl(descriptor, PIDFD_GET_INFO, &info) != 0 ||
        (info.mask & (PIDFD_INFO_PID | PIDFD_INFO_CREDS)) != (PIDFD_INFO_PID | PIDFD_INFO_CREDS) ||
        (info.mask & PIDFD_INFO_EXIT) || info.pid <= 1 || info.pid != info.tgid || info.pid == uint(getpid()) ||
        info.pid == uint(getppid()) || info.ruid != geteuid() || info.euid != geteuid() || info.suid != geteuid() ||
        fstat(descriptor, &status) != 0 || status.st_ino == 0)
        return false;
    pid = info.pid;
    identity = QByteArray::number(qulonglong(status.st_dev)) + ':' + QByteArray::number(qulonglong(status.st_ino));
    return true;
#else
    Q_UNUSED(descriptor)
    Q_UNUSED(identity)
    Q_UNUSED(pid)
    return false;
#endif
}
} // namespace

QString sessionProcessPluginPath() {
    // Resolve beside this loaded library, including user-prefix and staged installs.
    Dl_info info{};
    if (!dladdr(&libraryAnchor, &info) || !info.dli_fname)
        return {};
    return QFileInfo(QString::fromLocal8Bit(info.dli_fname)).absolutePath() + QStringLiteral("/sessionprocess.so");
}

bool SessionTermination::setSource(QObject *source) {
    m_source = source;
    m_property = -1;
    if (!source || source->thread() != thread() || !source->inherits("KWin::EffectsHandler"))
        return false;
    const int index = source->metaObject()->indexOfProperty("sessionState");
    if (index < 0)
        return false;
    const auto property = source->metaObject()->property(index);
    bool valid = false;
    const int quitting = property.enumerator().keyToValue("Quitting", &valid);
    if (!property.isReadable() || !property.isEnumType() || !property.hasNotifySignal() || !valid)
        return false;
    m_property = index;
    m_quitting = quitting;
    return true;
}

SessionTermination::SessionTermination(QObject *source, QObject *notifications, QObject *parent)
    : QObject(parent), m_notifications(notifications) {
    if (!setSource(source) || !notifications || notifications->thread() != thread() ||
        !notifications->inherits("KWin::SessionManager")) {
        m_property = -1;
        return;
    }
    // SessionManager survives effects/compositor recreation. Its signal is
    // emitted after updating the state read by EffectsHandler's property.
    const int index = notifications->metaObject()->indexOfSignal("stateChanged()");
    const auto slot = staticMetaObject.method(staticMetaObject.indexOfSlot("stateChanged()"));
    if (index < 0 || !QObject::connect(notifications, notifications->metaObject()->method(index), this, slot)) {
        m_property = -1;
        return;
    }
    // Loading/reloading the script during logout must never terminate applications.
    m_wasClosing = closing();
    m_handled = m_wasClosing;
}

bool SessionTermination::closing() const {
    if (!m_source || !m_notifications || m_property < 0)
        return false;
    bool valid = false;
    const int state = m_source->metaObject()->property(m_property).read(m_source).toInt(&valid);
    return valid && state == m_quitting;
}

void SessionTermination::stateChanged() {
    if (!m_source && !setSource(QDBusConnection::sessionBus().objectRegisteredAt(QStringLiteral("/Effects"))))
        return;
    const bool nowClosing = closing();
    if (nowClosing == m_wasClosing)
        return;
    m_wasClosing = nowClosing;
    m_handled = false;
    const auto generation = ++m_generation;
    if (nowClosing) {
        QTimer::singleShot(0, this, [this, generation]() {
            if (generation == m_generation && closing() && !m_handled)
                Q_EMIT closingRequested();
        });
    }
}

void SessionTermination::terminate(const QVariantList &windows, const QVariantList &rules,
                                   const SessionProcess &process) {
    if (!closing() || m_handled)
        return;
    m_handled = true;
    QVariantList activeRules;
    for (const auto &value : rules) {
        const auto rule = value.toMap();
        if (rule.value(QStringLiteral("enabled")).toBool() &&
            rule.value(QStringLiteral("behavior")).toMap().value(QStringLiteral("terminateOnLogout")).toBool())
            activeRules.append(value);
    }
    if (activeRules.isEmpty())
        return;
    QList<QPointer<QObject>> nativeWindows;
    QSet<qint64> protectedPids;
    for (const auto &value : windows) {
        auto window = value.value<QObject *>();
        if (!window || window->thread() != thread() || !window->inherits("KWin::Window") ||
            window->property("deleted").toBool())
            continue;
        nativeWindows.append(window);
        const auto info = windowInfo(window);
        if (Minimize2Tray::protectedWindow(info) || info.value(QStringLiteral("modal")).toBool() ||
            info.value(QStringLiteral("specialWindow")).toBool() ||
            info.value(QStringLiteral("type")) == QLatin1String("other"))
            protectedPids.insert(window->property("pid").toLongLong());
    }
    QSet<QByteArray> attempted;
    int skipped = 0;
    for (const auto &window : nativeWindows) {
        if (!closing() || !window)
            return;
        const auto closeable = window->property("closeable");
        if (closeable.metaType().id() != QMetaType::Bool || closeable.toBool() || window->property("deleted").toBool())
            continue;
        const auto info = windowInfo(window);
        bool selected = false;
        for (const auto &value : activeRules) {
            if (Minimize2Tray::matchFailure(value.toMap(), info).isEmpty()) {
                selected = true;
                break;
            }
        }
        if (!selected)
            continue;
        const ProcessHandle handle(process.openProcess(window));
        QByteArray identity;
        qint64 pid = -1;
        if (!processIdentity(handle.fd, identity, pid) || protectedPids.contains(pid)) {
            ++skipped;
            continue;
        }
        if (!closing() || !window || window->property("deleted").toBool() || window->property("closeable").toBool())
            return;
        if (attempted.contains(identity))
            continue;
        attempted.insert(identity);
#if defined(SYS_pidfd_send_signal)
        if (syscall(SYS_pidfd_send_signal, handle.fd, SIGTERM, nullptr, 0) != 0)
            qWarning() << "Minimize2Tray: logout SIGTERM was not delivered to process" << pid;
#endif
    }
    if (skipped)
        qWarning() << "Minimize2Tray: skipped logout termination for" << skipped << "unverified or protected windows";
}

#include "nativetray.h"
#include "applicationidentity.h"
#include "diagnostics.h"
#include "rules.h"
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFileInfo>
#include <QRegularExpression>

namespace {
constexpr auto Watcher = "org.kde.StatusNotifierWatcher";
QDBusMessage call(const QString &service, const QString &path, const QString &interface, const QString &method,
                  const QVariantList &arguments = {}) {
    auto message = QDBusMessage::createMethodCall(service, path, interface, method);
    message.setArguments(arguments);
    return message;
}
template <typename T, typename Done>
void query(const QDBusConnection &bus, const QDBusMessage &message, QObject *context, Done done) {
    auto watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, 2000), context);
    const auto finish = [watcher, done]() {
        QDBusPendingReply<T> reply = *watcher;
        watcher->deleteLater();
        done(reply);
    };
    if (watcher->isFinished())
        QTimer::singleShot(0, context, finish);
    else
        QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context, finish);
}
QDBusMessage daemon(const QString &method, const QString &name) {
    return call(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                QStringLiteral("org.freedesktop.DBus"), method, {name});
}
} // namespace

NativeTrayMonitor::NativeTrayMonitor(QObject *parent)
    : QObject(parent), m_bus(QDBusConnection::sessionBus()),
      m_watcher(QString::fromLatin1(Watcher), m_bus, QDBusServiceWatcher::WatchForOwnerChange, this) {
    m_refresh.setSingleShot(true);
    m_refresh.setInterval(100);
    connect(&m_refresh, &QTimer::timeout, this, &NativeTrayMonitor::refresh);
    connect(&m_watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this]() {
        ++m_generation; // Discard replies from a previous watcher owner.
        m_deadline.stop();
        m_items.clear();
        m_next.clear();
        m_queue.clear();
        m_busy = false;
        m_pending = 0;
        m_available = false;
        emit changed();
        requestRefresh();
    });
    m_bus.connect(QString::fromLatin1(Watcher), QStringLiteral("/StatusNotifierWatcher"), QString::fromLatin1(Watcher),
                  QStringLiteral("StatusNotifierItemRegistered"), this, SLOT(requestRefresh()));
    m_bus.connect(QString::fromLatin1(Watcher), QStringLiteral("/StatusNotifierWatcher"), QString::fromLatin1(Watcher),
                  QStringLiteral("StatusNotifierItemUnregistered"), this, SLOT(registryItemRemoved(QString)));
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(3500);
    connect(&m_deadline, &QTimer::timeout, this, [this]() {
        ++m_generation;
        m_pending = 0;
        m_queue.clear();
        m_busy = false;
        // Do not publish replies invalidated by a concurrent registration change.
        if (!m_again)
            m_items = std::move(m_next);
        emit changed();
        if (m_again)
            requestRefresh();
    });
    QTimer::singleShot(0, this, &NativeTrayMonitor::requestRefresh);
}

NativeTrayMonitor::~NativeTrayMonitor() {
    m_bus.disconnect(QString::fromLatin1(Watcher), QStringLiteral("/StatusNotifierWatcher"),
                     QString::fromLatin1(Watcher), QStringLiteral("StatusNotifierItemRegistered"), this,
                     SLOT(requestRefresh()));
    m_bus.disconnect(QString::fromLatin1(Watcher), QStringLiteral("/StatusNotifierWatcher"),
                     QString::fromLatin1(Watcher), QStringLiteral("StatusNotifierItemUnregistered"), this,
                     SLOT(registryItemRemoved(QString)));
    for (const auto &entry : std::as_const(m_subscriptions)) {
        const auto slash = entry.indexOf(QLatin1Char('/'));
        m_bus.disconnect(entry.left(slash), entry.mid(slash), QStringLiteral("org.freedesktop.DBus.Properties"),
                         QStringLiteral("PropertiesChanged"), this,
                         SLOT(itemPropertiesChanged(QString, QVariantMap, QStringList)));
        m_bus.disconnect(entry.left(slash), entry.mid(slash), QStringLiteral("org.kde.StatusNotifierItem"),
                         QStringLiteral("NewStatus"), this, SLOT(itemStatusChanged(QString)));
    }
}
void NativeTrayMonitor::registryItemRemoved(const QString &registration) {
    m_next.remove(registration);
    if (m_items.remove(registration))
        emit changed();
    requestRefresh();
}
void NativeTrayMonitor::requestRefresh() {
    if (m_busy) {
        m_again = true;
        return;
    }
    if (!m_refresh.isActive())
        m_refresh.start();
}
void NativeTrayMonitor::itemPropertiesChanged(const QString &, const QVariantMap &changed,
                                              const QStringList &invalidated) {
    for (const auto *key : {"Id", "DesktopEntry", "Status"})
        if (changed.contains(QLatin1String(key)) || invalidated.contains(QLatin1String(key)))
            requestRefresh();
}
void NativeTrayMonitor::itemStatusChanged(const QString &) { requestRefresh(); }

void NativeTrayMonitor::refresh() {
    if (m_busy) {
        m_again = true;
        return;
    }
    m_deadline.start();
    m_busy = true;
    m_again = false;
    m_pending = 0;
    m_next.clear();
    m_queue.clear();
    for (const auto &entry : std::as_const(m_subscriptions)) {
        const auto slash = entry.indexOf(QLatin1Char('/'));
        const auto owner = entry.left(slash), path = entry.mid(slash);
        m_bus.disconnect(owner, path, QStringLiteral("org.freedesktop.DBus.Properties"),
                         QStringLiteral("PropertiesChanged"), this,
                         SLOT(itemPropertiesChanged(QString, QVariantMap, QStringList)));
        m_bus.disconnect(owner, path, QStringLiteral("org.kde.StatusNotifierItem"), QStringLiteral("NewStatus"), this,
                         SLOT(itemStatusChanged(QString)));
    }
    m_subscriptions.clear();
    const auto generation = ++m_generation;
    query<uint>(m_bus, daemon(QStringLiteral("GetConnectionUnixProcessID"), QStringLiteral("org.kde.KWin")), this,
                [this, generation](const auto &reply) {
                    if (generation != m_generation)
                        return;
                    m_kwinPid = reply.isError() ? 0 : reply.value();
                    query<QDBusVariant>(
                        m_bus,
                        call(QString::fromLatin1(Watcher), QStringLiteral("/StatusNotifierWatcher"),
                             QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"),
                             {QString::fromLatin1(Watcher), QStringLiteral("RegisteredStatusNotifierItems")}),
                        this, [this, generation](const auto &reply) {
                            if (generation != m_generation)
                                return;
                            m_available = !reply.isError();
                            if (!reply.isError()) {
                                const auto data = reply.value().variant();
                                const auto entries = data.metaType().id() == qMetaTypeId<QDBusArgument>()
                                                         ? qdbus_cast<QStringList>(data)
                                                         : data.toStringList();
                                // Bound memory and concurrent calls even for a malformed or overloaded registry.
                                for (const auto &entry : entries.mid(0, 256))
                                    if (entry.size() <= 512)
                                        m_queue.enqueue(entry);
                            }
                            pump();
                        });
                });
}

void NativeTrayMonitor::pump() {
    const auto generation = m_generation;
    while (m_pending < 4 && !m_queue.isEmpty()) {
        const auto registration = m_queue.dequeue();
        const auto slash = registration.indexOf(QLatin1Char('/'));
        const auto service = slash < 0 ? registration : registration.left(slash);
        const auto path = slash < 0 ? QStringLiteral("/StatusNotifierItem") : registration.mid(slash);
        static const QRegularExpression name(
            QStringLiteral("^(?::[0-9]+\\.[0-9]+|[A-Za-z_][A-Za-z0-9_-]*(?:\\.[A-Za-z0-9_-]+)+)$"));
        static const QRegularExpression object(QStringLiteral("^/(?:[A-Za-z0-9_]+(?:/[A-Za-z0-9_]+)*)?$"));
        if (service.size() > 255 || !name.match(service).hasMatch() || !object.match(path).hasMatch())
            continue;
        ++m_pending;
        query<QString>(
            m_bus, daemon(QStringLiteral("GetNameOwner"), service), this,
            [this, generation, registration, path](const auto &reply) {
                if (generation != m_generation)
                    return;
                if (reply.isError()) {
                    finishItem();
                    return;
                }
                const auto owner = reply.value();
                query<uint>(m_bus, daemon(QStringLiteral("GetConnectionUnixProcessID"), owner), this,
                            [this, generation, registration, path, owner](const auto &pidReply) {
                                if (generation != m_generation)
                                    return;
                                const uint pid = pidReply.isError() ? 0 : pidReply.value();
                                // Exclude this process and KWin's own Minimize2Tray registrations, also in the KCM
                                // host.
                                if (!pid || pid == uint(QCoreApplication::applicationPid()) || pid == m_kwinPid) {
                                    finishItem();
                                    return;
                                }
                                // Subscribe before reading to catch identity/status changes during the query.
                                m_subscriptions.insert(owner + path);
                                // Identity/status changes are uncommon; icon/progress updates do not trigger
                                // rescans.
                                m_bus.disconnect(owner, path, QStringLiteral("org.freedesktop.DBus.Properties"),
                                                 QStringLiteral("PropertiesChanged"), this,
                                                 SLOT(itemPropertiesChanged(QString, QVariantMap, QStringList)));
                                m_bus.connect(owner, path, QStringLiteral("org.freedesktop.DBus.Properties"),
                                              QStringLiteral("PropertiesChanged"), this,
                                              SLOT(itemPropertiesChanged(QString, QVariantMap, QStringList)));
                                m_bus.disconnect(owner, path, QStringLiteral("org.kde.StatusNotifierItem"),
                                                 QStringLiteral("NewStatus"), this, SLOT(itemStatusChanged(QString)));
                                m_bus.connect(owner, path, QStringLiteral("org.kde.StatusNotifierItem"),
                                              QStringLiteral("NewStatus"), this, SLOT(itemStatusChanged(QString)));
                                query<QVariantMap>(
                                    m_bus,
                                    call(owner, path, QStringLiteral("org.freedesktop.DBus.Properties"),
                                         QStringLiteral("GetAll"), {QStringLiteral("org.kde.StatusNotifierItem")}),
                                    this, [this, generation, registration, path, owner, pid](const auto &properties) {
                                        if (generation != m_generation)
                                            return;
                                        if (!properties.isError()) {
                                            const auto data = properties.value();
                                            const auto status = data.value(QStringLiteral("Status")).toString();
                                            Item item{owner, data.value(QStringLiteral("Id")).toString().left(1024),
                                                      data.value(QStringLiteral("DesktopEntry")).toString().left(1024),
                                                      pid,
                                                      status == QLatin1String("Active") ||
                                                          status == QLatin1String("NeedsAttention")};
                                            m_next.insert(registration, item);
                                        }
                                        finishItem();
                                    });
                            });
            });
    }
    if (!m_pending && m_queue.isEmpty()) {
        // Never publish a snapshot invalidated by registration changes during its asynchronous reads.
        if (!m_again)
            m_items = std::move(m_next);
        m_deadline.stop();
        m_busy = false;
        emit changed();
        if (m_again)
            requestRefresh();
    }
}
void NativeTrayMonitor::finishItem() {
    --m_pending;
    pump();
}

bool NativeTrayMonitor::hasNativeTray(const QVariantMap &window) const {
    const auto app = Minimize2Tray::applicationService(window);
    QStringList identities{Minimize2Tray::desktopIdentity(window.value(QStringLiteral("desktopFile")).toString()),
                           window.value(QStringLiteral("resourceClass")).toString(),
                           window.value(QStringLiteral("resourceName")).toString()};
    if (app)
        identities.append(app->desktopEntryName());
    identities.removeAll(QString());
    const auto pid = window.value(QStringLiteral("pid")).toUInt();
    for (const auto &item : m_items) {
        if (!item.active)
            continue;
        if (pid > 1 && pid == item.pid)
            return true;
        // Exact reported identities only. Titles, substrings and guessed PID names cannot establish a match.
        if (identities.contains(Minimize2Tray::desktopIdentity(item.desktop)) || identities.contains(item.id))
            return true;
    }
    return false;
}
QStringList NativeTrayMonitor::proxyPids() const {
    QStringList pids;
    for (const auto &item : m_items) {
        const auto executable = QFileInfo(QStringLiteral("/proc/%1/exe").arg(item.pid)).symLinkTarget();
        if (QFileInfo(executable).fileName() == QLatin1String("xembedsniproxy"))
            pids.append(QString::number(item.pid));
    }
    pids.removeDuplicates();
    return pids;
}

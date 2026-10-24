#include "trayicon.h"
#include <KService>
#include <KSycoca>
#include <QAction>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QTimer>
#include <QVariantMap>
#include <cmath>
#include <limits>
#include <utility>

static QString normalizedLauncherUrl(QString id) {
    const QString prefix = QStringLiteral("application://");
    if (id.startsWith(prefix)) {
        id.remove(0, prefix.size());
    }
    // Desktop IDs are case-sensitive filenames, not paths or arbitrary URLs.
    if (id.isEmpty() || id.contains(QLatin1Char('/')) || id.contains(QLatin1Char('\\')) ||
        id.contains(QLatin1Char(':')) || id.contains(QLatin1Char('?')) || id.contains(QLatin1Char('#')) ||
        id.contains(QChar::Null)) {
        return {};
    }
    if (!id.endsWith(QLatin1String(".desktop"))) {
        id.append(QLatin1String(".desktop"));
    }
    return prefix + id;
}

static bool isInteger(const QVariant &value) {
    const int type = value.metaType().id();
    return type == QMetaType::Int || type == QMetaType::UInt || type == QMetaType::LongLong ||
           type == QMetaType::ULongLong || type == QMetaType::Short || type == QMetaType::UShort;
}

static QVariantMap validatedLauncherProperties(const QVariantMap &properties) {
    QVariantMap result;
    const QVariant count = properties.value(QStringLiteral("count"));
    if (isInteger(count)) {
        bool ok = false;
        const qint64 number = count.toLongLong(&ok);
        if (ok && number >= 0 && number <= std::numeric_limits<int>::max()) {
            result.insert(QStringLiteral("count"), number);
        }
    }
    const QVariant progress = properties.value(QStringLiteral("progress"));
    if (isInteger(progress) || progress.metaType().id() == QMetaType::Double ||
        progress.metaType().id() == QMetaType::Float) {
        bool ok = false;
        const double fraction = progress.toDouble(&ok);
        if (ok && std::isfinite(fraction)) {
            result.insert(QStringLiteral("progress"), std::clamp(fraction, 0.0, 1.0));
        }
    }
    for (const auto *key : {"count-visible", "progress-visible", "urgent"}) {
        const QVariant value = properties.value(QLatin1String(key));
        if (value.metaType().id() == QMetaType::Bool) {
            result.insert(QLatin1String(key), value);
        }
    }
    return result;
}

static QVariantMap emptyLauncherProperties() {
    return {{QStringLiteral("count"), 0},
            {QStringLiteral("count-visible"), false},
            {QStringLiteral("progress"), 0.0},
            {QStringLiteral("progress-visible"), false},
            {QStringLiteral("urgent"), false}};
}

// One Unity receiver shares application state between all managed windows.
class LauncherAPI : public QObject {
    Q_OBJECT
  public:
    static QSharedPointer<LauncherAPI> instance() {
        static QWeakPointer<LauncherAPI> shared;
        auto instance = shared.toStrongRef();
        if (!instance) {
            instance = QSharedPointer<LauncherAPI>::create();
            shared = instance;
        }
        return instance;
    }

    LauncherAPI() : m_watcher(QString(), QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForUnregistration) {
        QDBusConnection::sessionBus().connect(QString(), QString(), QStringLiteral("com.canonical.Unity.LauncherEntry"),
                                              QStringLiteral("Update"), this,
                                              SLOT(update(QString, QVariantMap, QDBusMessage)));
        connect(&m_watcher, &QDBusServiceWatcher::serviceUnregistered, this, &LauncherAPI::resetSource);
    }

    QVariantMap watch(const QString &url) {
        if (url.isEmpty()) {
            return emptyLauncherProperties();
        }
        auto &entry = m_entries[url];
        ++entry.references;
        return entry.properties;
    }

    void unwatch(const QString &url) {
        auto entry = m_entries.find(url);
        if (entry != m_entries.end() && --entry->references == 0 && entry->sender.isEmpty()) {
            m_entries.erase(entry);
        }
    }

    QVariantMap properties(const QString &url) const { return m_entries.value(url).properties; }

  signals:
    void updated(const QString &url);

  private slots:
    void update(const QString &uri, const QVariantMap &properties, const QDBusMessage &message) {
        const QString url = normalizedLauncherUrl(uri);
        auto entry = m_entries.find(url);
        if (entry == m_entries.end() || message.service().isEmpty()) {
            return;
        }
        const auto values = validatedLauncherProperties(properties);
        if (values.isEmpty()) {
            return;
        }
        const QString previousSender = entry->sender;
        if (previousSender != message.service()) {
            // A new publisher must not inherit the previous publisher's fields.
            entry->properties = emptyLauncherProperties();
            entry->sender = message.service();
        }
        for (auto value = values.cbegin(); value != values.cend(); ++value) {
            entry->properties.insert(value.key(), value.value());
        }
        watchSource(message.service());
        if (!previousSender.isEmpty() && previousSender != message.service()) {
            bool stillUsed = false;
            for (const auto &item : std::as_const(m_entries)) {
                stillUsed |= item.sender == previousSender;
            }
            if (!stillUsed) {
                m_watcher.removeWatchedService(previousSender);
            }
        }
        emit updated(url);
    }

  private:
    struct Entry {
        QVariantMap properties = emptyLauncherProperties();
        QString sender;
        int references = 0;
    };
    QHash<QString, Entry> m_entries;
    QDBusServiceWatcher m_watcher;

    void watchSource(const QString &sender) {
        if (m_watcher.watchedServices().contains(sender)) {
            return;
        }
        m_watcher.addWatchedService(sender);
        // Close the race where the publisher exits before the watch is installed.
        auto *call = new QDBusPendingCallWatcher(
            QDBusConnection::sessionBus().interface()->asyncCall(QStringLiteral("GetNameOwner"), sender), this);
        connect(call, &QDBusPendingCallWatcher::finished, this, [this, call, sender]() {
            const QDBusPendingReply<QString> reply = *call;
            if (reply.isError()) {
                resetSource(sender);
            }
            call->deleteLater();
        });
    }

    void resetSource(const QString &sender) {
        QStringList changed;
        for (auto entry = m_entries.begin(); entry != m_entries.end();) {
            if (entry->sender != sender) {
                ++entry;
                continue;
            }
            if (entry->references == 0) {
                entry = m_entries.erase(entry);
            } else {
                entry->sender.clear();
                entry->properties = emptyLauncherProperties();
                changed.append(entry.key());
                ++entry;
            }
        }
        m_watcher.removeWatchedService(sender);
        for (const auto &url : changed) {
            emit updated(url);
        }
    }
};

TrayIcon::TrayIcon(QObject *parent) : QObject(parent), m_launcherAPI(LauncherAPI::instance()) {
    connect(m_launcherAPI.data(), &LauncherAPI::updated, this, [this](const QString &url) {
        if (url == m_launcherUrl) {
            launcherAPIUpdate(url, m_launcherAPI->properties(url));
        }
    });
    qGuiApp->installEventFilter(this);
    connect(this, &TrayIcon::xdgNameChanged, this, &TrayIcon::updateApplication);
    // Queue cache refreshes to avoid re-entering KService lookups.
    connect(KSycoca::self(), &KSycoca::databaseChanged, this, &TrayIcon::updateApplication, Qt::QueuedConnection);
    // Wait until QML has assigned all initial window properties.
    QTimer::singleShot(0, this, [this]() {
        initializeTrayIcon();
        updateApplication();
    });
}

TrayIcon::~TrayIcon() { m_launcherAPI->unwatch(m_launcherUrl); }

void TrayIcon::setIcon(const QIcon newIcon) {
    if (m_icon.cacheKey() != newIcon.cacheKey()) {
        m_icon = newIcon;
        emit iconChanged();
    }
}

void TrayIcon::setWindowId(const QUuid window) { m_windowId = window; }
void TrayIcon::setToolTipText(const QString toolTipText) {
    if (m_toolTipText != toolTipText) {
        m_toolTipText = toolTipText;
        emit toolTipTextChanged();
    }
}

void TrayIcon::launcherAPIUpdate(const QString &uri, const QMap<QString, QVariant> &properties) {
    const QString url = normalizedLauncherUrl(uri);
    if (url.isEmpty() || url != m_launcherUrl) {
        return;
    }
    const auto values = validatedLauncherProperties(properties);
    if (values.contains(QStringLiteral("count"))) {
        setCount(values.value(QStringLiteral("count")).toInt());
    }
    if (values.contains(QStringLiteral("count-visible"))) {
        setCountVisible(values.value(QStringLiteral("count-visible")).toBool());
    }
    if (values.contains(QStringLiteral("progress"))) {
        setProgress(qRound(values.value(QStringLiteral("progress")).toDouble() * 100));
    }
    if (values.contains(QStringLiteral("progress-visible"))) {
        setProgressVisible(values.value(QStringLiteral("progress-visible")).toBool());
    }
    if (values.contains(QStringLiteral("urgent"))) {
        setUrgent(values.value(QStringLiteral("urgent")).toBool());
    }
}

QString TrayIcon::launcherUrl() const { return m_launcherUrl; }

void TrayIcon::setLauncherUrl(const QString &launcherUrl) {
    const QString url = normalizedLauncherUrl(launcherUrl);
    if (url != m_launcherUrl) {
        m_launcherAPI->unwatch(m_launcherUrl);
        m_launcherUrl = url;
        const auto values = m_launcherAPI->watch(url);
        // State belongs to the desktop ID, not to the window that used it before.
        setCount(0);
        setCountVisible(false);
        setProgress(0);
        setProgressVisible(false);
        setUrgent(false);
        launcherAPIUpdate(url, values);
        Q_EMIT launcherUrlChanged(url);
    }
}

int TrayIcon::count() const { return m_count; }

void TrayIcon::setCount(int count) {
    if (m_count != count) {
        m_count = count;
        Q_EMIT countChanged(count);
    }
}

bool TrayIcon::countVisible() const { return m_countVisible; }

void TrayIcon::setCountVisible(bool countVisible) {
    if (m_countVisible != countVisible) {
        m_countVisible = countVisible;
        Q_EMIT countVisibleChanged(countVisible);
    }
}

int TrayIcon::progress() const { return m_progress; }

void TrayIcon::setProgress(int progress) {
    int boundedProgress = std::clamp(progress, 0, 100);

    if (progress != boundedProgress) {
        qDebug() << qUtf8Printable(m_launcherUrl) << ": Progress value " << progress << " is out of bounds!";
    }

    if (m_progress != boundedProgress) {
        m_progress = boundedProgress;
        Q_EMIT progressChanged(boundedProgress);
    }
}

bool TrayIcon::progressVisible() const { return m_progressVisible; }

void TrayIcon::setProgressVisible(bool progressVisible) {
    if (m_progressVisible != progressVisible) {
        m_progressVisible = progressVisible;
        Q_EMIT progressVisibleChanged(progressVisible);
    }
}

bool TrayIcon::urgent() const { return m_urgent; }

void TrayIcon::setUrgent(bool urgent) {
    if (m_urgent != urgent) {
        m_urgent = urgent;
        Q_EMIT urgentChanged(urgent);
    }
}

void TrayIcon::setXdgName(const QString id) {
    if (id != m_xdgName) {
        m_xdgName = id;
        // Keep early updates while tray creation and desktop lookup are deferred.
        if (!trayIcon) {
            setLauncherUrl(QFileInfo(id).fileName());
        }
        emit xdgNameChanged();
    }
}

void TrayIcon::initializeTrayIcon() {
    if (trayIcon) {
        return;
    }

    qDebug() << "New Tray Icon" << m_xdgName;
    trayIcon = new KStatusNotifierItem(m_xdgName.isEmpty() ? m_windowId.toString() : m_xdgName, this);
    m_menu = new QMenu();

    QAction *showAction = new QAction("Show/Hide", m_menu);
    connect(showAction, &QAction::triggered, this, [this]() { emit requestShowHide(m_windowId); });
    showAction->setIcon(QIcon::fromTheme(QStringLiteral("view-visible-symbolic")));
    m_menu->addAction(showAction);

    QAction *unpinAction = new QAction("Unpin", m_menu);
    connect(unpinAction, &QAction::triggered, this, [this]() { emit requestUnpin(m_windowId); });
    unpinAction->setIcon(QIcon::fromTheme(QStringLiteral("window-unpin-symbolic")));
    m_menu->addAction(unpinAction);

    connect(trayIcon, &KStatusNotifierItem::quitRequested, this, [this]() {
        emit requestClose(m_windowId);
        // wants to kill KWin otherwise
        trayIcon->abortQuit();
    });

    connect(trayIcon, &KStatusNotifierItem::activateRequested, this, [this]() { emit requestShowHide(m_windowId); });

    connect(this, &TrayIcon::toolTipTextChanged, this, [this]() { trayIcon->setToolTipTitle(m_toolTipText); });

    connect(this, &TrayIcon::appNameChanged, this, [this]() { trayIcon->setTitle(m_appName); });

    connect(this, &TrayIcon::iconChanged, this, &TrayIcon::scheduleBadgeUpdate);
    connect(this, &TrayIcon::urgentChanged, this, &TrayIcon::updateStatus);
    connect(this, &TrayIcon::demandsAttentionChanged, this, &TrayIcon::updateStatus);
    connect(this, &TrayIcon::countVisibleChanged, this, &TrayIcon::scheduleBadgeUpdate);
    connect(this, &TrayIcon::progressVisibleChanged, this, &TrayIcon::scheduleBadgeUpdate);
    connect(this, &TrayIcon::countChanged, this, &TrayIcon::scheduleBadgeUpdate);
    connect(this, &TrayIcon::progressChanged, this, &TrayIcon::scheduleBadgeUpdate);
    connect(this, &TrayIcon::countUseDotChanged, this, &TrayIcon::scheduleBadgeUpdate);
    connect(this, &TrayIcon::countDisplayLimitChanged, this, &TrayIcon::scheduleBadgeUpdate);

    trayIcon->setContextMenu(m_menu);
    trayIcon->setToolTipTitle(m_toolTipText);
    trayIcon->setToolTipSubTitle(m_xdgName);
    trayIcon->setTitle(m_appName);
    updateStatus();
}

void TrayIcon::updateApplication() {
    if (!trayIcon) {
        return;
    }
    // Resolve installed identities, never open a path supplied by a window.
    QString desktopId = QFileInfo(m_xdgName).fileName();
    if (!desktopId.isEmpty() && !desktopId.endsWith(QLatin1String(".desktop"))) {
        desktopId.append(QLatin1String(".desktop"));
    }
    const auto service = desktopId.isEmpty() ? KService::Ptr() : KService::serviceByStorageId(desktopId);
    const QString newName = service ? service->name() : m_xdgName;
    if (m_appName != newName) {
        m_appName = newName;
        emit appNameChanged();
    }

    m_desktopIcon = QIcon();
    m_desktopIconName.clear();
    if (service && service->isApplication()) {
        desktopId = service->storageId();
        const QString iconName = service->icon();
        const bool isAbsolute = QDir::isAbsolutePath(iconName);
        const bool isThemeIcon =
            !iconName.isEmpty() && !iconName.contains(QLatin1Char('/')) && QIcon::hasThemeIcon(iconName);
        if (isAbsolute || isThemeIcon) {
            const QIcon icon = isAbsolute ? QIcon(iconName) : QIcon::fromTheme(iconName);
            // QIcon can be non-null even when the image cannot be decoded.
            if (!icon.pixmap(22, 22).isNull()) {
                m_desktopIcon = icon;
                if (!isAbsolute) {
                    m_desktopIconName = iconName;
                }
            }
        }
    }
    setLauncherUrl(desktopId.isEmpty() ? QString() : QStringLiteral("application://") + desktopId);
    trayIcon->setToolTipSubTitle(m_xdgName);
    scheduleBadgeUpdate();
}

void TrayIcon::scheduleBadgeUpdate() {
    if (m_badgeUpdatePending) {
        return;
    }
    // Paint once after a batch of properties or rapid updates has settled.
    m_badgeUpdatePending = true;
    QTimer::singleShot(0, this, [this]() {
        m_badgeUpdatePending = false;
        updateBadges();
    });
}

bool TrayIcon::eventFilter(QObject *watched, QEvent *event) {
    if (watched == qGuiApp &&
        (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::ApplicationFontChange)) {
        scheduleBadgeUpdate();
    }
    return QObject::eventFilter(watched, event);
}

void TrayIcon::updateStatus() {
    if (trayIcon) {
        trayIcon->setStatus(m_urgent || m_demandsAttention ? KStatusNotifierItem::NeedsAttention
                                                           : KStatusNotifierItem::Active);
    }
}

void TrayIcon::updateBadges() {
    if (!trayIcon) {
        return;
    }
    const QIcon &baseIcon = m_desktopIcon.isNull() ? m_icon : m_desktopIcon;
    const bool showCount = m_countVisible && m_count > 0;
    if (!showCount && !m_progressVisible) {
        if (!m_desktopIconName.isEmpty()) {
            trayIcon->setIconByName(m_desktopIconName);
        } else {
            // Absolute paths are images, not SNI theme names.
            trayIcon->setIconByPixmap(baseIcon);
        }
        return;
    }

    const QString text = m_count > m_countDisplayLimit ? QString::number(m_countDisplayLimit) + QLatin1Char('+')
                                                       : QString::number(m_count);
    const QPalette palette = QGuiApplication::palette();
    QIcon badgedIcon;
    // Render each SNI size from the original icon, including HiDPI sizes.
    for (const int size : {16, 22, 32, 48, 64, 128}) {
        QPixmap original = baseIcon.pixmap(size, size);
        if (original.isNull()) {
            original = m_icon.pixmap(size, size);
        }
        if (original.isNull()) {
            continue;
        }
        QPixmap canvas(size, size);
        canvas.fill(Qt::transparent);
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const QSize imageSize = original.size().scaled(canvas.size(), Qt::KeepAspectRatio);
        painter.drawPixmap(QRect(QPoint((size - imageSize.width()) / 2, (size - imageSize.height()) / 2), imageSize),
                           original);
        const qreal unit = size / 22.0;
        painter.setPen(Qt::NoPen);
        if (m_progressVisible) {
            const QRectF track(unit, size - 3 * unit, size - 2 * unit, 2 * unit);
            painter.setBrush(palette.color(QPalette::Window));
            painter.drawRoundedRect(track, unit, unit);
            if (m_progress > 0) {
                painter.setBrush(palette.color(QPalette::Highlight));
                painter.drawRoundedRect(
                    QRectF(track.topLeft(), QSizeF(track.width() * m_progress / 100.0, track.height())), unit, unit);
            }
        }
        if (showCount) {
            painter.setPen(QPen(palette.color(QPalette::Window), unit));
            painter.setBrush(palette.color(QPalette::Highlight));
            if (m_countUseDot) {
                painter.drawEllipse(QRectF(size - 6 * unit, unit, 5 * unit, 5 * unit));
            } else {
                QFont font = QGuiApplication::font();
                font.setWeight(QFont::Bold);
                font.setPixelSize(qMax(1, qRound(size * 0.42)));
                const qreal padding = 1.2 * unit;
                const qreal available = size - 2 * unit - 2 * padding;
                const qreal width = QFontMetricsF(font).horizontalAdvance(text);
                if (width > available) {
                    font.setPixelSize(qMax(1, static_cast<int>(font.pixelSize() * available / width)));
                }
                const QFontMetricsF metrics(font);
                const qreal badgeHeight = qMin(size - 2 * unit, metrics.height() + 2 * unit);
                const qreal badgeWidth = qMax(badgeHeight, metrics.horizontalAdvance(text) + 2 * padding);
                const QRectF badge(size - badgeWidth - unit, unit, badgeWidth, badgeHeight);
                painter.drawRoundedRect(badge, badgeHeight / 2, badgeHeight / 2);
                painter.setFont(font);
                painter.setPen(palette.color(QPalette::HighlightedText));
                painter.drawText(badge, Qt::AlignCenter, text);
            }
        }
        painter.end();
        badgedIcon.addPixmap(canvas);
    }
    trayIcon->setIconByPixmap(badgedIcon.isNull() ? m_icon : badgedIcon);
}

void TrayIcon::setDemandsAttention(bool demandsAttention) {
    if (m_demandsAttention != demandsAttention) {
        m_demandsAttention = demandsAttention;
        emit demandsAttentionChanged();
    }
}

void TrayIcon::setCountUseDot(bool countUseDot) {
    if (m_countUseDot != countUseDot) {
        m_countUseDot = countUseDot;
        emit countUseDotChanged();
    }
}

void TrayIcon::setCountDisplayLimit(int limit) {
    limit = std::clamp(limit, 1, 9999);
    if (m_countDisplayLimit != limit) {
        m_countDisplayLimit = limit;
        emit countDisplayLimitChanged();
    }
}

#include "trayicon.moc"

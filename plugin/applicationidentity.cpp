#include "applicationidentity.h"
#include "rules.h"

#include "diagnostics.h"
#include <KDesktopFile>
#include <KShell>
#include <KSycoca>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrentRun>

namespace Minimize2Tray {
namespace {
bool installedId(const QString &id) {
    return !id.isEmpty() && id.size() <= 1024 && !id.contains(QLatin1Char('/')) && !id.contains(QLatin1Char('\\')) &&
           !id.contains(QLatin1Char(':')) && !id.contains(QChar::Null);
}

class ApplicationIndex : public QObject {
    Q_OBJECT
  public:
    ApplicationIndex() {
        pool.setMaxThreadCount(1);
        pool.setExpiryTimeout(1000);
        connect(KSycoca::self(), &KSycoca::databaseChanged, this, [this]() {
            ++generation;
            ready = false;
            data = {};
            start();
        });
        connect(&watcher, &QFutureWatcher<Index>::finished, this, [this]() {
            if (requestedGeneration == generation) {
                data = watcher.result();
                ready = true;
                emit changed();
            } else
                start();
        });
    }
    KService::Ptr find(const QString &alias) {
        if (!ready)
            start();
        return data.aliases.value(alias.toCaseFolded());
    }
    KService::Ptr desktop(const QString &id) {
        if (!ready)
            start();
        return data.desktops.value(id + QStringLiteral(".desktop"));
    }
  signals:
    void changed();

  private:
    struct Index {
        QHash<QString, KService::Ptr> aliases;
        QHash<QString, KService::Ptr> desktops;
    };
    void start() {
        if (ready || watcher.isRunning())
            return;
        requestedGeneration = generation;
        watcher.setFuture(QtConcurrent::run(&pool, []() {
            StartupSpan timing("application metadata index");
            KSycoca::disableAutoRebuild();
            Index result;
            const auto add = [&result](const QString &alias, const KService::Ptr &app) {
                if (!installedId(alias))
                    return;
                const auto key = alias.toCaseFolded();
                if (!result.aliases.contains(key))
                    result.aliases.insert(key, app);
                else if (const auto previous = result.aliases.value(key);
                         previous && previous->storageId() != app->storageId())
                    result.aliases[key] = {}; // Never resolve an ambiguous launcher alias.
            };
            // KSycoca is thread-local; do not perform the full scan on KWin's event loop.
            for (const auto &app : KService::allServices()) {
                if (!app->isApplication())
                    continue;
                result.desktops.insert(app->storageId(), app);
                // Exact desktop IDs remain usable even for launchers hidden in this desktop.
                if (!app->showInCurrentDesktop())
                    continue;
                add(app->desktopEntryName(), app);
                add(desktopIdentity(app->storageId()), app);
                add(app->property<QString>(QStringLiteral("StartupWMClass")), app);
                KShell::Errors error;
                const auto args = KShell::splitArgs(app->exec(), KShell::AbortOnMeta, &error);
                if (error == KShell::NoError && !args.isEmpty())
                    add(QFileInfo(args.first()).fileName(), app);
            }
            return result;
        }));
    }
    Index data;
    QThreadPool pool;
    QFutureWatcher<Index> watcher;
    quint64 generation = 0;
    quint64 requestedGeneration = 0;
    bool ready = false;
};
} // namespace

static ApplicationIndex &index() {
    static ApplicationIndex instance;
    return instance;
}
QObject *applicationIndexNotifier() { return &index(); }

KService::Ptr installedApplication(const QString &id) {
    if (!installedId(id))
        return {};
    return index().desktop(desktopIdentity(id));
}

KService::Ptr applicationService(const QVariantMap &window) {
    StartupSpan timing("application metadata lookup", window.value(QStringLiteral("desktopFile")).toString() +
                                                          QLatin1Char(':') +
                                                          window.value(QStringLiteral("resourceClass")).toString());
    const auto id = window.value(QStringLiteral("desktopFile")).toString();
    if (installedId(id)) {
        const auto app = installedApplication(id);
        if (app && app->isApplication())
            return app;
    }
    KService::Ptr result;
    for (const auto *key : {"desktopFile", "resourceClass", "resourceName"}) {
        const auto alias = window.value(QLatin1String(key)).toString();
        if (!installedId(alias))
            continue;
        const auto app = index().find(desktopIdentity(alias));
        if (!app)
            continue;
        if (result && result->storageId() != app->storageId())
            return {};
        result = app;
    }
    return result;
}

QVariantMap matchingProperties(QVariantMap window) {
    const auto instance = window.value(QStringLiteral("resourceName")).toString();
    const auto className = window.value(QStringLiteral("resourceClass")).toString();
    window[QStringLiteral("resourceClassComplete")] =
        instance.isEmpty() && className.isEmpty() ? QString() : instance + QLatin1Char(' ') + className;
    return window;
}

bool validIconPath(const QString &path) {
    return !path.isEmpty() && path.size() <= 4096 && QDir::isAbsolutePath(path) && !path.contains(QChar::Null);
}

QIcon explicitTrayIcon(const QString &path) {
    if (!validIconPath(path))
        return {};
    const QFileInfo file(path);
    // Reject devices/FIFOs and bound metadata reads. Selection never launches Exec=.
    if (!file.isFile() || !file.isReadable() || file.size() > 16 * 1024 * 1024)
        return {};
    if (path.endsWith(QLatin1String(".desktop"), Qt::CaseInsensitive)) {
        KDesktopFile desktop(path);
        const auto name = desktop.readIcon();
        if (QDir::isAbsolutePath(name)) {
            const QFileInfo image(name);
            if (!image.isFile() || image.size() > 16 * 1024 * 1024)
                return {};
            return QIcon(name);
        }
        if (!name.contains(QLatin1Char('/')))
            return QIcon::fromTheme(name);
        return {};
    }
    return QIcon(path);
}
} // namespace Minimize2Tray

#include "applicationidentity.moc"

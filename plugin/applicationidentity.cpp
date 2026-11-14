#include "applicationidentity.h"
#include "rules.h"

#include <KApplicationTrader>
#include <KDesktopFile>
#include <KShell>
#include <KSycoca>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QObject>

namespace Minimize2Tray {
namespace {
bool installedId(const QString &id) {
    return !id.isEmpty() && id.size() <= 1024 && !id.contains(QLatin1Char('/')) && !id.contains(QLatin1Char('\\')) &&
           !id.contains(QLatin1Char(':')) && !id.contains(QChar::Null);
}

class ApplicationIndex : public QObject {
  public:
    ApplicationIndex() {
        connect(KSycoca::self(), &KSycoca::databaseChanged, this, [this]() {
            ready = false;
            aliases.clear();
        });
    }
    KService::Ptr find(const QString &alias) {
        if (!ready) {
            // Index once per desktop database update, never scan per window event.
            for (const auto &app : KApplicationTrader::query([](const KService::Ptr &) { return true; })) {
                add(app->desktopEntryName(), app);
                add(desktopIdentity(app->storageId()), app);
                add(app->property<QString>(QStringLiteral("StartupWMClass")), app);
                KShell::Errors error;
                const auto args = KShell::splitArgs(app->exec(), KShell::AbortOnMeta, &error);
                if (error == KShell::NoError && !args.isEmpty())
                    add(QFileInfo(args.first()).fileName(), app);
            }
            ready = true;
        }
        return aliases.value(alias.toCaseFolded());
    }

  private:
    void add(const QString &alias, const KService::Ptr &app) {
        if (!installedId(alias))
            return;
        const auto key = alias.toCaseFolded();
        if (!aliases.contains(key))
            aliases.insert(key, app);
        else if (const auto previous = aliases.value(key); previous && previous->storageId() != app->storageId())
            aliases[key] = {}; // Ambiguous aliases never select an arbitrary launcher.
    }
    QHash<QString, KService::Ptr> aliases;
    bool ready = false;
};
} // namespace

KService::Ptr applicationService(const QVariantMap &window) {
    const auto id = window.value(QStringLiteral("desktopFile")).toString();
    if (installedId(id)) {
        const auto app = KService::serviceByStorageId(desktopIdentity(id) + QStringLiteral(".desktop"));
        if (app && app->isApplication())
            return app;
    }
    static ApplicationIndex index;
    KService::Ptr result;
    for (const auto *key : {"desktopFile", "resourceClass", "resourceName"}) {
        const auto alias = window.value(QLatin1String(key)).toString();
        if (!installedId(alias))
            continue;
        const auto app = index.find(desktopIdentity(alias));
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

#include "scriptconfig.h"
#include "rules.h"
#include "sessionprocess.h"
#include "sessiontermination.h"

#include <KAboutApplicationDialog>
#include <KAboutData>
#include <KPluginMetaData>
#include <KConfigGroup>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonObject>
#include <QPluginLoader>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>
#include <QDialog>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <algorithm>

ScriptConfig::ScriptConfig(QObject *parent) : QObject(parent), m_config(QStringLiteral("kwinrc")) {
    m_elapsed.start();
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(50);
    connect(&m_reloadTimer, &QTimer::timeout, this, &ScriptConfig::reload);
    const auto scheduleReload = [this]() {
        if (!m_reloadTimer.isActive()) m_reloadTimer.start();
    };
    connect(&m_watch, &KDirWatch::dirty, this, scheduleReload);
    connect(&m_watch, &KDirWatch::created, this, scheduleReload);
    connect(&m_watch, &KDirWatch::deleted, this, scheduleReload);
    for (const auto &path : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation)) {
        m_watch.addFile(path + QStringLiteral("/kwinrc"));
    }
    m_registered = QDBusConnection::sessionBus().registerObject(QStringLiteral("/Minimize2Tray"), this,
                                                                QDBusConnection::ExportScriptableSlots);
    reload();
}

ScriptConfig::~ScriptConfig() {
    // QProcess belongs to the application until it exits: never wait for a child
    // process on KWin's event loop, and never terminate an independently opened host.
    if (m_configProcess)
        m_configProcess->terminate();
    delete m_configDialog.data();
    delete m_aboutDialog.data();
    if (m_registered) {
        QDBusConnection::sessionBus().unregisterObject(QStringLiteral("/Minimize2Tray"));
    }
}

void ScriptConfig::reload() {
    m_config.reparseConfiguration();
    auto rules = m_values.value(QStringLiteral("rules")).toList();
    QString error = m_error;
    const KConfigGroup group(&m_config, Minimize2Tray::ConfigGroup);
    // Geometry and other KWin settings share this file. Decode rules only when
    // their document changed; preserve the last valid rules on malformed edits.
    if (!m_hasRulesSource || !group.hasKey("rules") ||
        group.readEntry("rules", QString()) != m_rulesSource) {
        const auto decoded = Minimize2Tray::readRules(m_config, error);
        if (error.isEmpty() || !m_values.contains(QStringLiteral("rules")))
            rules = decoded;
        const bool terminationEnabled =
            error.isEmpty() && std::any_of(rules.cbegin(), rules.cend(), [](const auto &value) {
                const auto rule = value.toMap();
                const auto behavior = rule.value(QStringLiteral("behavior")).toMap();
                return rule.value(QStringLiteral("enabled")).toBool() &&
                       behavior.value(QStringLiteral("terminateOnLogout")).toBool();
            });
        if (!terminationEnabled) {
            m_sessionTermination.reset();
        } else if (!m_sessionTermination && QGuiApplication::platformName() == QLatin1String("org.kde.kwin.qpa")) {
            const auto bus = QDBusConnection::sessionBus();
            auto source = bus.objectRegisteredAt(QStringLiteral("/Effects"));
            auto notifications = bus.objectRegisteredAt(QStringLiteral("/Session"));
            m_sessionTermination = std::make_unique<SessionTermination>(source, notifications, this);
            if (m_sessionTermination->available())
                connect(m_sessionTermination.get(), &SessionTermination::closingRequested, this,
                        &ScriptConfig::sessionClosing);
            else
                qWarning()
                    << "Minimize2Tray: KWin session-closing state is unavailable; logout termination is disabled";
        }
        m_hasRulesSource = group.hasKey("rules");
        m_rulesSource = group.readEntry("rules", QString());
    }
    auto values = Minimize2Tray::readSettings(m_config);
    values.insert(QStringLiteral("rules"), rules);
    if (error != m_error) {
        m_error = error;
        if (!error.isEmpty()) {
            qWarning().noquote() << "Minimize2Tray:" << error;
        }
    }
    if (values != m_values) {
        m_values = values;
        Q_EMIT valuesChanged();
    }
}

void ScriptConfig::terminateSessionWindows(const QVariantList &windows) {
    if (!m_sessionTermination || !m_sessionTermination->closing())
        return;
    // Recheck persisted intent at the last possible moment, including a pending
    // watcher reload. Malformed rules never authorize process termination.
    KConfig config(QStringLiteral("kwinrc"));
    QString error;
    const auto rules = Minimize2Tray::readRules(config, error);
    if (!error.isEmpty()) {
        qWarning() << "Minimize2Tray: skipping logout termination because the current rules are invalid";
        return;
    }
    QPluginLoader loader(sessionProcessPluginPath());
    const auto metadata = loader.metaData();
    if (metadata.value(QStringLiteral("IID")) != QJsonValue(QStringLiteral(SessionProcess_iid)) ||
        metadata.value(QStringLiteral("MetaData")).toObject().value(QStringLiteral("KWinVersion")) !=
            QJsonValue(QCoreApplication::applicationVersion())) {
        qWarning() << "Minimize2Tray: logout termination needs an adapter built for this KWin version";
        return;
    }
    auto process = qobject_cast<SessionProcess *>(loader.instance());
    if (!process) {
        qWarning() << "Minimize2Tray: could not load the logout process adapter:" << loader.errorString();
        return;
    }
    m_sessionTermination->terminate(windows, rules, *process);
}

bool ScriptConfig::isProtected(const QVariantMap &window) const { return Minimize2Tray::protectedWindow(window); }

QStringList ScriptConfig::matchingRules(const QVariantMap &window) const {
    QStringList matches;
    for (const auto &value : m_values.value(QStringLiteral("rules")).toList()) {
        const auto rule = value.toMap();
        if (Minimize2Tray::matchFailure(rule, window).isEmpty()) {
            matches.append(rule.value(QStringLiteral("id")).toString());
        }
    }
    return matches;
}

QVariantMap ScriptConfig::matchingBehavior(const QVariantMap &window) const {
    bool matched = false;
    bool autoPin = false;
    bool startInTray = false;
    bool startupProtection = false;
    bool closeToTray = false;
    bool minimizeEnabled = false;
    bool minimizeDisabled = false;
    for (const auto &value : m_values.value(QStringLiteral("rules")).toList()) {
        const auto rule = value.toMap();
        if (!Minimize2Tray::matchFailure(rule, window).isEmpty())
            continue;
        matched = true;
        const auto behavior = rule.value(QStringLiteral("behavior")).toMap();
        autoPin = autoPin || behavior.value(QStringLiteral("autoPin")).toBool();
        const bool start = behavior.value(QStringLiteral("startInTray")).toBool();
        startInTray = startInTray || start;
        startupProtection = startupProtection || (start && behavior.value(QStringLiteral("startupProtection")).toBool());
        closeToTray = closeToTray || behavior.value(QStringLiteral("closeToTray")).toBool();
        const QString minimize = behavior.value(QStringLiteral("minimizeToTray")).toString();
        minimizeEnabled = minimizeEnabled || minimize == QLatin1String("enabled");
        minimizeDisabled = minimizeDisabled || minimize == QLatin1String("disabled");
    }
    // Explicit enable wins across matching rules; explicit disable overrides inherited defaults.
    const bool minimizeToTray = minimizeEnabled ||
        (!minimizeDisabled && m_values.value(QStringLiteral("hideOnMinimize")).toBool());
    return {{QStringLiteral("matched"), matched}, {QStringLiteral("autoPin"), autoPin},
            {QStringLiteral("startInTray"), startInTray}, {QStringLiteral("minimizeToTray"), minimizeToTray},
            {QStringLiteral("closeToTray"), closeToTray}, {QStringLiteral("startupProtection"), startupProtection}};
}

QVariantList ScriptConfig::windows() {
    Q_EMIT windowsRequested();
    return m_windows;
}

void ScriptConfig::showDialog(QDialog *dialog, const QString &role) {
    dialog->setAttribute(Qt::WA_QuitOnClose, false);
    dialog->setWindowRole(QStringLiteral("minimize2tray-") + role);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void ScriptConfig::showAbout() {
    if (!m_aboutDialog) {
        const auto path =
            QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                   QStringLiteral("kwin/scripts/") + QString::fromLatin1(Minimize2Tray::ScriptId) +
                                       QStringLiteral("/metadata.json"));
        const auto metadata = KPluginMetaData::fromJsonFile(path);
        if (metadata.isValid() && metadata.pluginId() == QLatin1String(Minimize2Tray::ScriptId)) {
            KAboutData aboutData(
                metadata.pluginId(), metadata.name(), metadata.version(), metadata.description(),
                KAboutLicense::byKeyword(metadata.license()).key(), metadata.copyrightText(),
                tr("PhobosK fork, based on <a href=\"https://github.com/luisbocanegra/kwin-minimize2tray\">"
                   "KWin Minimize2Tray by Luis Bocanegra</a>."),
                metadata.website(), metadata.bugReportUrl());
            aboutData.setProgramLogo(QIcon::fromTheme(metadata.iconName()));
            for (const auto &author : metadata.authors())
                aboutData.addAuthor(author);
            for (const auto &contributor : metadata.otherContributors())
                aboutData.addCredit(contributor);
            QStringList translatorNames, translatorEmails;
            for (const auto &translator : metadata.translators()) {
                translatorNames.append(translator.name());
                translatorEmails.append(translator.emailAddress());
            }
            aboutData.setTranslator(translatorNames.join(QLatin1Char(',')), translatorEmails.join(QLatin1Char(',')));
            m_aboutDialog = new KAboutApplicationDialog(aboutData, KAboutApplicationDialog::HideLibraries);
        } else {
            m_aboutDialog =
                new QMessageBox(QMessageBox::Warning, tr("Minimize to Tray"),
                                tr("The installed Minimize2Tray metadata could not be loaded."), QMessageBox::Close);
        }
        m_aboutDialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    showDialog(m_aboutDialog.data(), QStringLiteral("about"));
}

void ScriptConfig::configure() {
    const auto request = QDBusMessage::createMethodCall(
        QString::fromLatin1(Minimize2Tray::ConfigService), QStringLiteral("/Minimize2Tray/Configuration"),
        QString::fromLatin1(Minimize2Tray::ConfigService), QStringLiteral("activate"));
    auto watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(request), this);
    const auto finished = [this, watcher]() {
        const QDBusPendingReply<> reply = *watcher;
        watcher->deleteLater();
        if (!reply.isError() || (m_configProcess && m_configProcess->state() != QProcess::NotRunning))
            return;
        const auto showError = [this](const QString &detail) {
            if (!m_configDialog) {
                m_configDialog = new QMessageBox(QMessageBox::Warning, tr("Minimize to Tray"),
                                                 tr("Could not open configuration: %1").arg(detail), QMessageBox::Close);
                m_configDialog->setAttribute(Qt::WA_DeleteOnClose);
            }
            showDialog(m_configDialog.data(), QStringLiteral("configuration-error"));
        };
        const auto metadata = KPluginMetaData::findPluginById(QStringLiteral("kwin/effects/configs"),
                                                              QStringLiteral("kcm_minimize2tray"));
        const auto executable = QStandardPaths::findExecutable(QStringLiteral("kcmshell6"));
        if (!metadata.isValid() || executable.isEmpty()) {
            showError(tr("Check that kcmshell6 and the Minimize2Tray configuration module are installed."));
            return;
        }
        // In-process widgets use KWin's internal QPA and its overlay layer. The
        // standard KDE host provides normal Wayland/X11 window management instead.
        auto process = new QProcess(QCoreApplication::instance());
        m_configProcess = process;
        auto environment = QProcessEnvironment::systemEnvironment();
        if (environment.value(QStringLiteral("QT_QPA_PLATFORM")).contains(QLatin1String("org.kde.kwin.qpa")))
            environment.remove(QStringLiteral("QT_QPA_PLATFORM"));
        process->setProcessEnvironment(environment);
        process->setProcessChannelMode(QProcess::ForwardedChannels);
        connect(process, &QProcess::started, process, [owner = QPointer<ScriptConfig>(this), process]() {
            if (!owner)
                process->terminate();
        });
        connect(process, &QProcess::finished, process, &QObject::deleteLater);
        connect(process, &QProcess::errorOccurred, process,
                [owner = QPointer<ScriptConfig>(this), process, showError](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                if (owner)
                    showError(process->errorString());
                process->deleteLater();
            }
        });
        process->start(executable, {QStringLiteral("--caption"), tr("Configure Minimize to Tray"),
                                   QStringLiteral("--args"), QString::fromLatin1(Minimize2Tray::ScriptId) +
                                       QStringLiteral(" KWin/Script"),
                                   QStringLiteral("--"), metadata.fileName()});
    };
    // A disconnected bus can complete before the watcher signal is connected.
    if (watcher->isFinished())
        QTimer::singleShot(0, this, finished);
    else
        connect(watcher, &QDBusPendingCallWatcher::finished, this, finished);
}

#include "scriptconfig.h"
#include "rules.h"

#include <KAboutApplicationDialog>
#include <KAboutData>
#include <KCMultiDialog>
#include <KPluginMetaData>
#include <QDBusConnection>
#include <QDebug>
#include <QDialog>
#include <QMessageBox>
#include <QStandardPaths>

ScriptConfig::ScriptConfig(QObject *parent) : QObject(parent), m_config(QStringLiteral("kwinrc")) {
    m_reloadTimer.setSingleShot(true);
    connect(&m_reloadTimer, &QTimer::timeout, this, &ScriptConfig::reload);
    const auto scheduleReload = [this]() { m_reloadTimer.start(); };
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
    delete m_configDialog.data();
    delete m_aboutDialog.data();
    if (m_registered) {
        QDBusConnection::sessionBus().unregisterObject(QStringLiteral("/Minimize2Tray"));
    }
}

void ScriptConfig::reload() {
    m_config.reparseConfiguration();
    QString error;
    const auto rules = Minimize2Tray::readRules(m_config, error);
    auto values = Minimize2Tray::readSettings(m_config);
    // Keep the last valid rules if an external edit is malformed.
    values.insert(QStringLiteral("rules"), error.isEmpty() ? rules : m_values.value(QStringLiteral("rules"), rules));
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
        startInTray = startInTray || behavior.value(QStringLiteral("startInTray")).toBool();
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
            {QStringLiteral("closeToTray"), closeToTray}};
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
    if (!m_configDialog) {
        const auto metadata = KPluginMetaData::findPluginById(QStringLiteral("kwin/effects/configs"),
                                                              QStringLiteral("kcm_minimize2tray"));
        if (metadata.isValid()) {
            auto dialog = new KCMultiDialog;
            dialog->setWindowTitle(tr("Configure Minimize to Tray"));
            // Use the same full module and arguments as the KWin Scripts configuration host.
            dialog->addModule(metadata, {QString::fromLatin1(Minimize2Tray::ScriptId), QStringLiteral("KWin/Script")});
            m_configDialog = dialog;
        } else {
            m_configDialog = new QMessageBox(QMessageBox::Warning, tr("Minimize to Tray"),
                                             tr("The Minimize2Tray configuration module could not be loaded. Check "
                                                "that the native plugins are installed."),
                                             QMessageBox::Close);
        }
        m_configDialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    showDialog(m_configDialog.data(), QStringLiteral("configuration"));
}

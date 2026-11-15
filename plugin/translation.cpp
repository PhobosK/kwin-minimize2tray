#include "translation.h"
#include <KLocalizedString>
#include <QCoreApplication>
#include <QLocale>
#include <QStandardPaths>
#include <QTranslator>

void Minimize2Tray::loadTranslations() {
    static bool attempted = false;
    if (attempted)
        return;
    attempted = true;
    const auto application = QCoreApplication::instance();
    const auto name = QStringLiteral("minimize2tray-translation");
    // Both plugin libraries share this application-owned catalog, including KCM reopenings.
    if (application->findChild<QTranslator *>(name, Qt::FindDirectChildrenOnly))
        return;
    auto translator = new QTranslator(application);
    auto languages = KLocalizedString::languages();
    if (languages.isEmpty())
        languages = QLocale().uiLanguages();
    languages.removeDuplicates();
    const auto locations = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    for (const auto &language : languages) {
        if (QLocale(language).language() == QLocale::English)
            break;
        for (const auto &location : locations) {
            if (translator->load(QLocale(language), QStringLiteral("minimize2tray"), QStringLiteral("_"),
                                 location + QStringLiteral("/kwin-minimize2tray/translations"))) {
                translator->setObjectName(name);
                QCoreApplication::installTranslator(translator);
                return;
            }
        }
    }
    delete translator; // English source strings remain the fallback.
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include <QApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

#include "main_window.h"
#include "portable.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("KartRecomp"));
    QApplication::setApplicationName(QStringLiteral("KartRecomp"));
    QApplication::setApplicationVersion(QStringLiteral(PROJECT_VERSION_STRING));
    // Windows: everything is read and written relative to this folder, so it can be moved anywhere as it is.
    Portable::EnterAppDir();
    Portable::UseIniSettings();
    // Qt's own texts (Yes / No / Cancel buttons, standard dialogs) in Spanish too: translations/qtbase_es.qm beside the launcher.
    QTranslator qt_spanish;
    if (qt_spanish.load(QLocale(QLocale::Spanish, QLocale::Spain), QStringLiteral("qtbase"), QStringLiteral("_"), QStringLiteral("translations")) ||
        qt_spanish.load(QLocale(QLocale::Spanish, QLocale::Spain), QStringLiteral("qtbase"), QStringLiteral("_"),
                        QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(&qt_spanish);

    MainWindow window;
    // --nolauncher (or --play): start the built game straight away with the saved settings, without showing the launcher, and
    // quit when the game exits. For playing from a TV or Steam (issue #17); the in-game menu (F10, L3+R3) has the settings.
    const QStringList args = QCoreApplication::arguments();
    if (args.contains(QStringLiteral("--nolauncher")) || args.contains(QStringLiteral("--play"))) {
        if (!window.launchWithoutWindow()) return 1;
        return app.exec();
    }
    window.show();
    return app.exec();
}

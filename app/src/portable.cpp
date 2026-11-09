// SPDX-License-Identifier: GPL-3.0-or-later
#include "portable.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace Portable {

void EnterAppDir() {
#ifdef Q_OS_WIN
    const QString app = QCoreApplication::applicationDirPath();
    if (!app.isEmpty() && QDir::currentPath() != app) QDir::setCurrent(app);
#endif
}

QString Store(const QString& path) {
#ifdef Q_OS_WIN
    if (path.isEmpty()) return path;
    const QString app = QCoreApplication::applicationDirPath();
    const QString absolute = QDir::cleanPath(QFileInfo(QDir(app), path).absoluteFilePath());
    const QString rel = QDir(app).relativeFilePath(absolute);
    // Another drive (D:\ while the launcher is on C:\) has no relative form: the path stays as it is.
    if (QDir::isAbsolutePath(rel)) return QDir::fromNativeSeparators(path);
    return rel.isEmpty() ? QStringLiteral(".") : rel;
#else
    return path;
#endif
}

QStringList Store(const QStringList& paths) {
    QStringList out;
    for (const QString& p : paths) out << Store(p);
    return out;
}

QString RelativeTo(const QString& dir, const QString& path) {
    if (path.isEmpty() || dir.isEmpty()) return path;
    const QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QString rel = QDir(QFileInfo(dir).absoluteFilePath()).relativeFilePath(absolute);
    if (QDir::isAbsolutePath(rel)) return path;
    return rel.isEmpty() ? QStringLiteral(".") : rel;
}

void UseIniSettings() {
#ifdef Q_OS_WIN
    QSettings::setDefaultFormat(QSettings::IniFormat);
    // Relative to the launcher's folder (the current directory, see EnterAppDir).
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("userdata/config"));
    QSettings ini;
    if (ini.value(QStringLiteral("portable/migrated")).toBool()) return;
    QSettings registry(QSettings::NativeFormat, QSettings::UserScope, QCoreApplication::organizationName(), QCoreApplication::applicationName());
    const QStringList pathKeys{QStringLiteral("setup/base"), QStringLiteral("setup/update"), QStringLiteral("setup/keys"),
                               QStringLiteral("setup/firmware"), QStringLiteral("suyu_home")};
    for (const QString& key : registry.allKeys()) {
        if (ini.contains(key) || key == QLatin1String("game_root")) continue; // Windows has no game folder: never bring one back
        QVariant value = registry.value(key);
        if (key == QLatin1String("setup/dlc")) value = Store(value.toStringList());
        else if (pathKeys.contains(key)) value = Store(value.toString());
        ini.setValue(key, value);
    }
    ini.setValue(QStringLiteral("portable/migrated"), true);
#endif
}

} // namespace Portable

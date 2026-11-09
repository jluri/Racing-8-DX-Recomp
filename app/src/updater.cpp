// SPDX-License-Identifier: GPL-3.0-or-later
#include "updater.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSettings>
#include <QUrl>
#include <QVersionNumber>
#include <QWidget>

namespace {
// The AppImage's release asset. 0.2.3 renamed it to match the application's name (AppImage catalog); older releases used
// the old name, which is still accepted.
constexpr char kLauncherAsset[] = "Racing_8_DX_Recomp-x86_64.AppImage";
constexpr char kOldLauncherAsset[] = "KartRecomp-Launcher-x86_64.AppImage";

QNetworkRequest Request(const QUrl& url) {
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("KartRecomp-Launcher/%1").arg(QCoreApplication::applicationVersion()));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return req;
}

/// The source checkout this launcher was built from (<repo>/build/app/kartrecomp), if any.
QString CheckoutRoot() {
    const QDir dir(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../..")));
    return QFileInfo::exists(dir.filePath(QStringLiteral(".git"))) && QFileInfo::exists(dir.filePath(QStringLiteral("CMakeLists.txt")))
               ? dir.absolutePath()
               : QString{};
}
} // namespace

Updater::Updater(QWidget* parent) : QObject(parent), parent_widget(parent), net(new QNetworkAccessManager(this)) {}

void Updater::check(bool quiet) {
    QSettings settings;
    if (quiet) {
        const auto last = settings.value(QStringLiteral("update_checked")).toDateTime();
        if (last.isValid() && last.secsTo(QDateTime::currentDateTimeUtc()) < 24 * 3600) return;
    }
    settings.setValue(QStringLiteral("update_checked"), QDateTime::currentDateTimeUtc());
    auto* reply = net->get(Request(QUrl(QStringLiteral("https://api.github.com/repos/%1/releases/latest").arg(QLatin1String(kRepo)))));
    connect(reply, &QNetworkReply::finished, this, [this, reply, quiet] {
        reply->deleteLater();
        const auto json = QJsonDocument::fromJson(reply->readAll()).object();
        const QString tag = json.value(QStringLiteral("tag_name")).toString();
        if (reply->error() != QNetworkReply::NoError || tag.isEmpty()) {
            if (!quiet) QMessageBox::information(parent_widget, tr("Actualizaciones"), tr("No se pudo buscar actualizaciones: %1").arg(reply->errorString()));
            return;
        }
        const auto latest = QVersionNumber::fromString(tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : tag);
        const auto current = QVersionNumber::fromString(QCoreApplication::applicationVersion());
        if (latest <= current) {
            if (!quiet) QMessageBox::information(parent_widget, tr("Actualizaciones"), tr("Tienes la última versión (%1).").arg(current.toString()));
            return;
        }
        QString asset;
        for (const auto& a : json.value(QStringLiteral("assets")).toArray()) {
            const QString name = a.toObject().value(QStringLiteral("name")).toString();
            if (name == QLatin1String(kLauncherAsset) || (asset.isEmpty() && name == QLatin1String(kOldLauncherAsset))) {
                asset = a.toObject().value(QStringLiteral("browser_download_url")).toString();
            }
        }
        offer(tag, json.value(QStringLiteral("body")).toString(), json.value(QStringLiteral("html_url")).toString(), asset);
    });
}

void Updater::offer(const QString& tag, const QString& notes, const QString& page, const QString& asset_url) {
    const QString root = CheckoutRoot();
    const QString appimage = qEnvironmentVariable("APPIMAGE");
    QString how;
    if (!root.isEmpty()) how = tr("Actualizar descarga el código nuevo y recompila el launcher.");
    else if (!appimage.isEmpty() && !asset_url.isEmpty()) how = tr("Actualizar descarga el launcher nuevo y sustituye a este.");
    else how = tr("Actualizar abre la página de la versión. Ojo: la versión oficial está en inglés y usa rutas absolutas.");
    QMessageBox box(parent_widget);
    box.setWindowTitle(tr("Actualización disponible"));
    box.setText(tr("Racing 8 DX Recomp %1 está disponible (tienes la %2).").arg(tag, QCoreApplication::applicationVersion()));
    box.setInformativeText(how + QLatin1Char('\n') +
                           tr("Los cambios en el propio juego se aplican cuando vuelves a construirlo con tus archivos."));
    box.setDetailedText(notes);
    box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    box.button(QMessageBox::Ok)->setText(tr("Actualizar"));
    box.button(QMessageBox::Cancel)->setText(tr("Más tarde"));
    if (box.exec() != QMessageBox::Ok) return;
    if (!root.isEmpty()) updateCheckout(root);
    else if (!appimage.isEmpty() && !asset_url.isEmpty()) updateAppImage(asset_url);
    else QDesktopServices::openUrl(QUrl(page));
}

void Updater::updateCheckout(const QString& root) {
    // git pull --ff-only (never touches local changes it can't fast-forward over), then rebuild the launcher in its build folder.
    auto* pull = new QProcess(this);
    pull->setWorkingDirectory(root);
    pull->setProcessChannelMode(QProcess::MergedChannels);
    connect(pull, &QProcess::readyRead, this, [this, pull] { emit log(QString::fromUtf8(pull->readAll()).trimmed()); });
    connect(pull, &QProcess::finished, this, [this, pull, root](int code) {
        pull->deleteLater();
        if (code != 0) {
            QMessageBox::warning(parent_widget, tr("Actualizar"), tr("git pull ha fallado (mira la salida). ¿Hay cambios locales en %1?").arg(root));
            return;
        }
        auto* build = new QProcess(this);
        build->setWorkingDirectory(root);
        build->setProcessChannelMode(QProcess::MergedChannels);
        connect(build, &QProcess::readyRead, this, [this, build] { emit log(QString::fromUtf8(build->readAll()).trimmed()); });
        connect(build, &QProcess::finished, this, [this, build](int c) {
            build->deleteLater();
            if (c != 0) {
                QMessageBox::warning(parent_widget, tr("Actualizar"), tr("El launcher no se pudo compilar (mira la salida)."));
                return;
            }
            if (QMessageBox::question(parent_widget, tr("Actualizar"), tr("Actualizado. ¿Reiniciar el launcher ahora?")) == QMessageBox::Yes) {
                QProcess::startDetached(QCoreApplication::applicationFilePath(), {});
                QCoreApplication::quit();
            }
        });
        emit log(tr("Recompilando el launcher…"));
        build->start(QStringLiteral("cmake"), {QStringLiteral("--build"), QStringLiteral("build"), QStringLiteral("--target"), QStringLiteral("kartrecomp")});
    });
    emit log(tr("Descargando la versión nueva…"));
    pull->start(QStringLiteral("git"), {QStringLiteral("pull"), QStringLiteral("--ff-only")});
}

void Updater::updateAppImage(const QString& url) {
    const QString target = qEnvironmentVariable("APPIMAGE");
    auto* reply = net->get(Request(QUrl(url)));
    emit log(tr("Descargando el launcher nuevo…"));
    connect(reply, &QNetworkReply::finished, this, [this, reply, target] {
        reply->deleteLater();
        const QByteArray data = reply->readAll();
        QFile out(target + QStringLiteral(".new"));
        if (reply->error() != QNetworkReply::NoError || data.size() < 1024 * 1024 || !out.open(QIODevice::WriteOnly) ||
            out.write(data) != data.size()) {
            QMessageBox::warning(parent_widget, tr("Actualizar"), tr("La descarga ha fallado: %1").arg(reply->errorString()));
            return;
        }
        out.close();
        out.setPermissions(out.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther);
        QFile::remove(target);
        if (!QFile::rename(out.fileName(), target)) {
            QMessageBox::warning(parent_widget, tr("Actualizar"), tr("No se pudo sustituir %1.").arg(target));
            return;
        }
        if (QMessageBox::question(parent_widget, tr("Actualizar"), tr("Actualizado. ¿Reiniciar el launcher ahora?")) == QMessageBox::Yes) {
            QProcess::startDetached(target, {});
            QCoreApplication::quit();
        }
    });
}

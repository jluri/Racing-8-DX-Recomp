// SPDX-License-Identifier: GPL-3.0-or-later
#include "setup_validate.h"

#include <QDir>
#include <QFileInfo>

#include "keys_check.h"

namespace {
QString Hex(quint64 id) { return QStringLiteral("%1").arg(id, 16, 16, QLatin1Char('0')); }
} // namespace

QString ValidateBase(const NspInfo& info) {
    if (!info.error.isEmpty()) return QStringLiteral("No se puede leer este archivo: %1.").arg(info.error);
    if (info.titleId != kBaseTitleId)
        return QStringLiteral("Esto no es el juego base de Mario Kart 8 Deluxe (title id %1, se esperaba %2).").arg(Hex(info.titleId), Hex(kBaseTitleId));
    return {};
}

QString ValidateUpdate(const NspInfo& info) {
    if (!info.error.isEmpty()) return QStringLiteral("No se puede leer este archivo: %1.").arg(info.error);
    if (info.titleId != kUpdateTitleId)
        return QStringLiteral("Esto no es la actualización de Mario Kart 8 Deluxe (title id %1, se esperaba %2).").arg(Hex(info.titleId), Hex(kUpdateTitleId));
    if (!info.aarch64)
        return QStringLiteral("Esta actualización es de 32 bits (3.0.3 o anterior; los archivos llamados \"v4.0.0\" suelen ser esta). Solo se pueden "
                              "recompilar las actualizaciones 3.0.4 y 4.0.1.");
    if (UpdateName(info.version).isEmpty())
        return QStringLiteral("Es la actualización versión %1; solo valen las actualizaciones 3.0.4 (versión %2) y 4.0.1 (versión %3).")
            .arg(info.version).arg(kSupportedUpdates[0].version).arg(kSupportedUpdates[1].version);
    return {};
}

QString UpdateName(quint32 version) {
    for (const auto& u : kSupportedUpdates)
        if (u.version == version) return QString::fromLatin1(u.name);
    return {};
}

QString HotListFor(const QString& list304, const QString& updateName) {
    if (updateName.isEmpty() || updateName == QLatin1String("3.0.4")) return list304;
    QString out = list304;
    return out.replace(QStringLiteral("mk8d-3.0.4"), QStringLiteral("mk8d-") + updateName);
}

QString ValidateFirmware(const QString& path) {
    const QFileInfo fi(path);
    if (!fi.exists()) return QStringLiteral("No se encontró el firmware.");
    if (fi.isFile()) return path.endsWith(".zip", Qt::CaseInsensitive) ? QString() : QStringLiteral("El firmware tiene que ser una carpeta con archivos .nca o un .zip.");
    if (QDir(path).entryList({"*.nca", "*.cnmt.nca"}, QDir::Files).isEmpty()) return QStringLiteral("Esta carpeta no tiene firmware (archivos .nca).");
    return {};
}

QString ValidateDlc(const QStringList& files) {
    for (const QString& f : files) {
        if (!f.endsWith(".nsp", Qt::CaseInsensitive) || !QFileInfo(f).isFile())
            return QStringLiteral("%1 no es un archivo .nsp.").arg(QFileInfo(f).fileName());
    }
    return {};
}

QString ValidateInputs(const SetupInputs& in, const std::function<NspInfo(const QString&)>& reader) {
    if (const auto p = CheckKeys(in.keys); p != KeyProblem::None) return KeyProblemText(p, MissingKeyName(in.keys));
    if (const QString why = ValidateBase(reader(in.base)); !why.isEmpty()) return why;
    if (const QString why = ValidateUpdate(reader(in.update)); !why.isEmpty()) return why;
    if (const QString why = ValidateFirmware(in.firmware); !why.isEmpty()) return why;
    return ValidateDlc(in.dlc);
}

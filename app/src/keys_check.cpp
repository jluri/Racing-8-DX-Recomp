// SPDX-License-Identifier: GPL-3.0-or-later
#include "keys_check.h"

#include <QFile>
#include <QSet>

namespace {
const char* const kDerivationKeys[] = {"aes_kek_generation_source", "key_area_key_application_source", "aes_key_generation_source", "titlekek_source"};

struct Scan {
    bool opened = false;
    QSet<QByteArray> names;
    bool master = false;
};
Scan ScanKeys(const QString& path) {
    Scan scan;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return scan;
    scan.opened = true;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine();
        const int eq = line.indexOf('=');
        if (eq <= 0) continue;
        const QByteArray name = line.left(eq).trimmed().toLower();
        if (line.mid(eq + 1).trimmed().isEmpty()) continue; // a name with no value is not a key
        scan.names.insert(name);
        if (name.startsWith("master_key_") && name.size() == 13) scan.master = true;
    }
    return scan;
}
} // namespace

KeyProblem CheckKeys(const QString& path) {
    const Scan scan = ScanKeys(path);
    if (!scan.opened) return KeyProblem::NoFile;
    if (!scan.names.contains("header_key")) return KeyProblem::MissingHeaderKey;
    for (const char* k : kDerivationKeys)
        if (!scan.names.contains(k)) return KeyProblem::MissingDerivationKey;
    return scan.master ? KeyProblem::None : KeyProblem::MissingMasterKey;
}

QString MissingKeyName(const QString& path) {
    const Scan scan = ScanKeys(path);
    if (!scan.opened) return {};
    if (!scan.names.contains("header_key")) return QStringLiteral("header_key");
    for (const char* k : kDerivationKeys)
        if (!scan.names.contains(k)) return QString::fromLatin1(k);
    return scan.master ? QString() : QStringLiteral("master_key_XX");
}

QString KeyProblemText(KeyProblem p, const QString& name) {
    switch (p) {
    case KeyProblem::None: return {};
    case KeyProblem::NoFile: return QStringLiteral("No se pudo leer prod.keys.");
    case KeyProblem::MissingHeaderKey: return QStringLiteral("Este prod.keys no tiene header_key; vuelve a volcarlo desde tu propia consola.");
    case KeyProblem::MissingMasterKey: return QStringLiteral("Este prod.keys no tiene entradas master_key_XX; vuelve a volcarlo desde tu propia consola.");
    case KeyProblem::MissingDerivationKey:
        return QStringLiteral("A este prod.keys le falta la entrada %1; vuelve a volcarlo desde tu propia consola.").arg(name.isEmpty() ? QStringLiteral("(una fuente de derivación de claves)") : name);
    }
    return {};
}

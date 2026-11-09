// SPDX-License-Identifier: GPL-3.0-or-later
#include "setup_pipeline.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFile>
#include <QProcess>
#include <QThread>

#include "build_runner.h"
#include "nsp_info.h"
#include "verify_build.h"

namespace {
qint64 AvailableRam() { return BuildRunner::AvailableMemoryBytes(); }
bool Cancelled(const std::atomic<bool>* c) { return c && c->load(); }
QString FileText(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
}
QString FileHash(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromLatin1(QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex().left(16));
}
} // namespace

PipelineResult RunSetupPipeline(const SetupInputs& in, const DataLayout& layout, const BuilderPaths& paths,
                                const std::function<void(int, const QString&)>& progress, const std::atomic<bool>* cancel, const PipelineLimits& limits) {
    const auto report = [&](int permille, const QString& text) { if (progress) progress(permille, text); };
    QString missing;
    if (!paths.complete(&missing)) return {false, missing};

    report(0, QStringLiteral("Comprobando tus archivos"));
    const auto reader = [&](const QString& p) { return ReadNspInfo(paths.exporter, p, in.keys); };
    if (const QString why = ValidateInputs(in, reader); !why.isEmpty()) return {false, why};
    // Each supported update has its own hot block list (data/mk8d-<version>/hot_blocks.txt); without one every block is compiled cold.
    const QString updateName = UpdateName(reader(in.update).version);
    QString hotList = HotListFor(paths.hotList, updateName);
    if (!QFileInfo::exists(hotList)) hotList.clear();
    if (Cancelled(cancel)) return {false, QStringLiteral("Cancelado.")};
    const int jobs = BuildRunner::JobsForRam(AvailableRam(), QThread::idealThreadCount());
    if (jobs == 0) return {false, QStringLiteral("No hay suficiente memoria libre: cierra otros programas (la construcción necesita al menos 3 GB libres) y vuelve a intentarlo.")};
    // The build's work files (about 6 GB) go to the cache folder; the finished game (about 1.5 GB) and the installed files to the data folder.
    if (!BuildRunner::HasDiskSpace(layout.cacheRoot, limits.cacheBytes))
        return {false, QStringLiteral("No hay suficiente espacio libre en %1: la construcción necesita unos 7 GB ahí.").arg(layout.cacheRoot)};
    if (!BuildRunner::HasDiskSpace(layout.dataRoot, limits.dataBytes))
        return {false, QStringLiteral("No hay suficiente espacio libre en %1: el juego necesita unos 2 GB ahí.").arg(layout.dataRoot)};

    report(5, QStringLiteral("Instalando tus archivos"));
    if (const QString err = InstallInputs(in, layout); !err.isEmpty()) return {false, err};
    QDir(layout.moduleDir()).removeRecursively(); // a half-finished earlier build must not look built
    QDir(layout.exportDir()).removeRecursively(); // always regenerated (about 2 s): never mix two exports
    QDir().mkpath(layout.exportDir());

    report(10, QStringLiteral("Leyendo el juego"));
    {
        QProcess ex;
        ex.setProcessChannelMode(QProcess::SeparateChannels);
        BuildRunner::DieWithParent(ex);
        ex.start(paths.exporter, {"--base", layout.base(), "--update", layout.update(), "--keys", layout.keysFile(), "--out", layout.exportDir(), "--progress-fd", "1"});
        if (!ex.waitForStarted()) return {false, QStringLiteral("No se puede iniciar el exportador (%1).").arg(paths.exporter)};
        while (!ex.waitForFinished(200)) {
            if (Cancelled(cancel)) { ex.kill(); ex.waitForFinished(); return {false, QStringLiteral("Cancelado.")}; }
            while (ex.canReadLine()) {
                const auto parts = ex.readLine().trimmed().split(' ');
                if (parts.value(0) == "PROGRESS") report(10 + parts.value(1).toInt() * 90 / 100, QString::fromUtf8(parts.mid(2).join(' ')));
            }
        }
        if (ex.exitCode() != 0) {
            const QString err = QString::fromLocal8Bit(ex.readAllStandardError()).trimmed();
            return {false, QStringLiteral("Ha fallado la lectura del juego: %1").arg(err.isEmpty() ? QStringLiteral("código de salida %1").arg(ex.exitCode()) : err)};
        }
    }

    BuildPlan plan;
    plan.useToolchain(paths.toolchain);
    plan.exportDir = layout.exportDir();
    plan.workDir = layout.workDir();
    plan.outDir = layout.moduleDir();
    plan.hotList = hotList;
    plan.jobs = jobs;
    plan.stamp = paths.engineVersion + "|" + FileText(paths.toolchain + "/TOOLCHAIN_VERSION") + "|" + FileHash(hotList) + "|" + updateName;
    const auto built = BuildRunner::Build(plan, [&](int done, int total, const QString& what) {
        report(100 + 850 * done / std::max(total, 1), QStringLiteral("Compiling %1 of %2: %3").arg(done).arg(total).arg(what));
    }, cancel);
    if (!built.ok) return {false, built.message};

    report(950, QStringLiteral("Probando el juego"));
    const VerifyResult verdict = VerifyBuild(paths.engine, layout, 150, cancel);
    if (!verdict.ok) return {false, verdict.message};

    QFile stamp(layout.moduleDir() + "/module.json");
    if (!stamp.open(QIODevice::WriteOnly)) return {false, QStringLiteral("No se puede escribir %1.").arg(stamp.fileName())};
    stamp.write("{ \"engineVersion\": \"" + paths.engineVersion.toUtf8() + "\", \"update\": \"" + updateName.toUtf8() + "\" }\n");
    report(1000, QStringLiteral("El juego está construido"));
    return {true, {}};
}

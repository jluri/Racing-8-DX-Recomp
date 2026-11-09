// SPDX-License-Identifier: GPL-3.0-or-later
// Windows portable mode with relative paths only: the launcher (and its command-line tools) always work from the folder that holds
// kartrecomp.exe, and every path they keep (settings, game files, the engine's sdl2-config.ini) is written relative to that folder.
// The whole folder can then be moved, copied or renamed and keeps working. Other platforms keep their XDG behaviour.
#pragma once

#include <QString>
#include <QStringList>

namespace Portable {

/// Windows: makes the folder of the executable the current directory (call it after QCoreApplication exists, and again after a file
/// dialog, which may change it). Elsewhere: nothing.
void EnterAppDir();

/// Windows: the launcher's settings go to userdata/config/KartRecomp/KartRecomp.ini (an INI file, not the registry); values left
/// in the registry by an older launcher are copied over once, their paths made relative. Elsewhere: nothing.
void UseIniSettings();

/// Windows: `path` relative to the launcher's folder when that is possible (same drive), with '/' separators. Elsewhere: unchanged.
QString Store(const QString& path);
QStringList Store(const QStringList& paths);

/// `path` (relative to the launcher's folder, or absolute) relative to `dir` (a folder a child process runs in), when possible.
QString RelativeTo(const QString& dir, const QString& path);

} // namespace Portable

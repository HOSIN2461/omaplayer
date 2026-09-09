#pragma once

// Helper binary lookup (ffmpeg/ffprobe/node): bundled next to the executable
// first (macOS .app Contents/MacOS, Windows portable dir), then PATH, then
// explicit fallbacks. Keeps the app working from a bundle without installers.
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>

inline QString appBundledExecutable(const QString &name,
                                    const QStringList &fallbacks = {})
{
    const QString appDir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
    const QStringList candidates = { name + QStringLiteral(".exe"), name };
#else
    const QStringList candidates = { name };
#endif
    for (const QString &c : candidates) {
        const QFileInfo info(appDir + QDir::separator() + c);
        if (info.isExecutable() && !info.isDir())
            return info.absoluteFilePath();
    }
    const QString inPath = QStandardPaths::findExecutable(name);
    if (!inPath.isEmpty())
        return inPath;
    for (const QString &candidate : fallbacks) {
        const QFileInfo info(candidate);
        if (info.isExecutable() && !info.isDir())
            return candidate;
    }
    return {};
}

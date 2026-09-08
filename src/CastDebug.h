#pragma once

// File logging for cast diagnostics. Qt message output (qInfo/console.log)
// does not reach our capture in this environment, so cast traffic goes here
// instead: one line per event, low volume (discovery excluded).
//
// Debug scaffolding — remove once Google Cast / AirPlay are stable.

#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QString>

inline void castDebug(const QString &s)
{
    QFile f(QStringLiteral("/tmp/opencode/cast-debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream t(&f);
        t << QDateTime::currentDateTime().toString(
                 QStringLiteral("hh:mm:ss.zzz"))
          << ' ' << s << '\n';
    }
}

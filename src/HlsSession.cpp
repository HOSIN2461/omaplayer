#include "HlsSession.h"

#include <QProcess>
#include <QTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUuid>

namespace {
constexpr int kSegTime = 4;
constexpr int kWindow = 60; // segments kept ≈ 4 min ≈ ≤100 MB
constexpr int kPollMs = 300;
constexpr int kMaxPolls = 50; // 15 s startup budget
} // namespace

HlsSession::HlsSession(QObject *parent)
    : QObject(parent)
{
    m_id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    m_dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/hls/") + m_id;
    QDir().mkpath(m_dir);
}

HlsSession::~HlsSession()
{
    stop();
}

void HlsSession::start(const QString &srcPath, double position,
                       const QString &vcodec)
{
    stop();
    QDir().mkpath(m_dir);
    const bool vcopy = (vcodec == QLatin1String("h264"));
    QStringList args = {QStringLiteral("-y"), QStringLiteral("-nostdin"),
                        QStringLiteral("-v"), QStringLiteral("error")};
    if (position > 2.0)
        args += {QStringLiteral("-ss"), QString::number(position, 'f', 1)};
    args += {QStringLiteral("-i"), srcPath, QStringLiteral("-map"),
             QStringLiteral("0:v:0"), QStringLiteral("-map"),
             QStringLiteral("0:a?")};
    if (vcopy)
        args += {QStringLiteral("-c:v"), QStringLiteral("copy")};
    else
        args += {QStringLiteral("-c:v"), QStringLiteral("libx264"),
                 QStringLiteral("-preset"), QStringLiteral("veryfast"),
                 QStringLiteral("-crf"), QStringLiteral("21")};
    args += {QStringLiteral("-c:a"), QStringLiteral("aac"),
             QStringLiteral("-ac"), QStringLiteral("2"),
             QStringLiteral("-b:a"), QStringLiteral("160k"),
             QStringLiteral("-f"), QStringLiteral("hls"),
             QStringLiteral("-hls_time"), QString::number(kSegTime),
             QStringLiteral("-hls_list_size"), QString::number(kWindow),
             QStringLiteral("-hls_flags"), QStringLiteral("delete_segments"),
             QStringLiteral("-hls_segment_filename"),
             m_dir + QStringLiteral("/seg%05d.ts"),
             m_dir + QStringLiteral("/index.m3u8")};
    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::readyReadStandardError, this, [this] {
        if (m_proc)
            m_lastError = QString::fromUtf8(m_proc->readAllStandardError())
                              .trimmed().right(300);
    });
    connect(m_proc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus status) {
                if (m_proc && m_polls >= 0) {
                    // Died during startup (or mid-stream): report once.
                    m_polls = -1;
                    Q_EMIT failed(m_lastError.isEmpty()
                                      ? tr("ffmpeg kilépett (%1)").arg(code)
                                      : m_lastError);
                }
                stop();
            });
    m_proc->start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")),
                  args);
    m_polls = 0;
    m_poll = new QTimer(this);
    m_poll->setInterval(kPollMs);
    connect(m_poll, &QTimer::timeout, this, &HlsSession::pollReady);
    m_poll->start();
}

void HlsSession::pollReady()
{
    if (!m_proc) {
        if (m_poll)
            m_poll->stop();
        return;
    }
    // Ready when the playlist references at least one materialized segment.
    const QString pl = m_dir + QStringLiteral("/index.m3u8");
    bool haveSeg = false;
    if (QFileInfo(pl).size() > 0) {
        QFile f(pl);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray body = f.readAll();
            for (const QByteArray &line : body.split('\n')) {
                const QByteArray t = line.trimmed();
                if (!t.isEmpty() && !t.startsWith('#')
                    && QFileInfo(m_dir + QLatin1Char('/') + t).size() > 0) {
                    haveSeg = true;
                    break;
                }
            }
        }
    }
    if (haveSeg) {
        m_polls = -1; // done polling (session continues streaming)
        if (m_poll)
            m_poll->stop();
        Q_EMIT ready(QStringLiteral("index.m3u8"));
        return;
    }
    if (++m_polls > kMaxPolls) {
        Q_EMIT failed(tr("HLS nem állt fel 15 mp alatt"));
        stop();
    }
}

void HlsSession::stop()
{
    if (m_poll) {
        m_poll->stop();
        m_poll->deleteLater();
        m_poll = nullptr;
    }
    m_polls = -1;
    if (m_proc) {
        QProcess *p = m_proc;
        m_proc = nullptr;
        p->disconnect(this);
        p->kill();
        p->waitForFinished(2000);
        p->deleteLater();
    }
    if (!m_dir.isEmpty())
        QDir(m_dir).removeRecursively();
}

bool HlsSession::serve(const QString &rel, QByteArray *contentType,
                       QByteArray *data)
{
    // Only playlist + our own segments, no path tricks.
    if (rel.contains(QLatin1String("..")) || rel.contains(QLatin1Char('/')))
        return false;
    const bool isPl = (rel == QLatin1String("index.m3u8"));
    const bool isTs = rel.startsWith(QLatin1String("seg")) && rel.endsWith(QLatin1String(".ts"));
    if (!isPl && !isTs)
        return false;
    QFile f(m_dir + QLatin1Char('/') + rel);
    if (!f.open(QIODevice::ReadOnly))
        return false; // not generated yet (live edge) or windowed out
    *data = f.readAll();
    if (data->isEmpty())
        return false;
    *contentType = isPl ? QByteArray("application/x-mpegURL")
                        : QByteArray("video/MP2T");
    return true;
}

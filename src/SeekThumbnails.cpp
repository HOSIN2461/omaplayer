#include "SeekThumbnails.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QQuickImageProvider>
#include <QSize>

#include <algorithm>
#include <cmath>

namespace {

// Sprite sheet geometry: 8 columns x N rows of 192px wide tiles. 24 target
// thumbs keeps generation snappy even for long movies (each frame is a decode
// seek), while the strip is still dense enough to preview on.
constexpr int kTargetThumbs = 24;
constexpr int kColumns = 8;
constexpr int kThumbWidth = 192;
// Files shorter than this get no preview (nothing useful to show).
constexpr double kMinDuration = 8.0;

bool isRemote(const QString &path)
{
    return path.startsWith(QLatin1String("http://"))
        || path.startsWith(QLatin1String("https://"))
        || path.startsWith(QLatin1String("ytdl://"))
        || path.startsWith(QLatin1String("dvd://"))
        || path.startsWith(QLatin1String("bd://"));
}

} // namespace

SeekThumbnails::SeekThumbnails(QObject *parent)
    : QObject(parent)
    , m_ffmpeg(ffmpegPath())
{
}

bool SeekThumbnails::enabled() const
{
    return QSettings().value(QStringLiteral("seekPreview/enabled"), true).toBool();
}

void SeekThumbnails::setEnabled(bool on)
{
    if (on == enabled())
        return;
    QSettings().setValue(QStringLiteral("seekPreview/enabled"), on);
    Q_EMIT enabledChanged();
    if (on)
        prepare(m_path, m_duration);
    else
        clear();
}

void SeekThumbnails::preload(const QString &filePath, double duration)
{
    // Warm the cache only — never decode. Mirrors prepare() up to the point
    // where it would spawn ffmpeg, then quietly falls back to the time bubble.
    if (filePath.isEmpty() || isRemote(filePath)
        || duration < kMinDuration || !enabled()) {
        clear();
        return;
    }
    const QString canonical = QFileInfo(filePath).canonicalFilePath();
    if (canonical.isEmpty() || !QFileInfo(canonical).isFile()) {
        clear();
        return;
    }
    m_path = canonical;
    m_duration = duration;
    if (loadFromCache(canonical, duration)) {
        m_failedPath.clear();
        return;
    }
    if (m_proc && m_path == canonical)
        return;
    if (m_ready || m_generating)
        return;
    m_ready = false;
    m_generating = false;
    m_imageUrl.clear();
}

void SeekThumbnails::prepare(const QString &filePath, double duration)
{
    // No file / remote stream / too short / feature off -> fall back to the
    // plain time bubble.
    if (filePath.isEmpty() || isRemote(filePath)
        || duration < kMinDuration || !enabled()) {
        clear();
        return;
    }
    const QString canonical = QFileInfo(filePath).canonicalFilePath();
    if (canonical.isEmpty() || !QFileInfo(canonical).isFile()) {
        clear();
        return;
    }

    // Never hammer ffmpeg: once a file's generation has failed (missing
    // decoder, corrupt stream, …), keep the quiet time-bubble until the media
    // actually changes or a cache appears. Without this, every hover / pointer
    // move re-triggers ffmpeg, stalling the UI and making the video feel
    // unusable ("can't scrub into it with the mouse").
    if (m_failedPath == canonical) {
        m_path = canonical;
        m_duration = duration;
        if (loadFromCache(canonical, duration)) {
            m_failedPath.clear();
            return;
        }
        m_ready = false;
        m_generating = false;
        return;
    }

    if (m_proc && m_path == canonical) {
        m_duration = duration; // already working on this file
        return;
    }

    m_path = canonical;
    m_duration = duration;

    if (loadFromCache(canonical, duration)) {
        m_failedPath.clear();
        return;
    }
    if (m_ffmpeg.isEmpty()) {
        // No ffmpeg on the system — idle, time bubble stays.
        m_failedPath = canonical;
        clear();
        return;
    }
    startGeneration(canonical, duration);
}

void SeekThumbnails::clear()
{
    cancelProcess();
    if (m_proc) {
        delete m_proc;
        m_proc = nullptr;
    }
    m_duration = 0.0;
    m_count = 0;
    m_columns = 1;
    m_tileWidth = 1;
    m_tileHeight = 1;
    m_imageUrl.clear();
    m_failedPath.clear();
    if (m_generating) {
        m_generating = false;
        Q_EMIT generatingChanged();
    }
    if (m_ready) {
        m_ready = false;
        Q_EMIT readyChanged();
    }
}

int SeekThumbnails::tileIndex(double position) const
{
    if (!m_ready || m_count <= 0 || m_duration <= 0.0)
        return 0;
    const double ratio = std::clamp(position / m_duration, 0.0, 1.0);
    return qBound(0, static_cast<int>(ratio * m_count), m_count - 1);
}

QString SeekThumbnails::spritePath() const
{
    return cacheStem(m_path) + QStringLiteral(".png");
}

QImage SeekThumbnails::tileImage(int index) const
{
    if (!m_ready || m_count <= 0 || m_tileWidth < 1 || m_tileHeight < 1)
        return {};
    index = qBound(0, index, m_count - 1);
    const QString sp = spritePath();
    QImage sprite;
    {
        QMutexLocker lock(&m_spriteMutex);
        if (m_spriteKey != sp) {
            m_sprite = QImage(sp);
            m_spriteKey = sp;
        }
        sprite = m_sprite;
    }
    if (sprite.isNull())
        return {};
    const int col = index % m_columns;
    const int row = index / m_columns;
    return sprite.copy(QRect(col * m_tileWidth, row * m_tileHeight,
                             m_tileWidth, m_tileHeight));
}

QImage SeekThumbProvider::requestImage(const QString &id, QSize *size,
                                       const QSize &requestedSize)
{
    bool ok = false;
    const int idx = id.toInt(&ok);
    QImage img = (ok && m_thumbs) ? m_thumbs->tileImage(idx) : QImage();
    if (img.isNull()) {
        if (size)
            *size = QSize(176, 99);
        return img;
    }
    if (requestedSize.isValid() && requestedSize.width() > 0) {
        img = img.scaled(requestedSize.width(),
                         requestedSize.height() > 0 ? requestedSize.height()
                                                     : img.height(),
                         Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (size)
        *size = img.size();
    return img;
}

bool SeekThumbnails::loadFromCache(const QString &filePath, double duration)
{
    const QString stem = cacheStem(filePath);
    const QString jsonPath = stem + QStringLiteral(".json");
    const QString pngPath = stem + QStringLiteral(".png");
    if (!QFile::exists(jsonPath) || !QFile::exists(pngPath))
        return false;

    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject meta = QJsonDocument::fromJson(f.readAll()).object();
    const qint64 cachedMtime = qint64(meta.value(QStringLiteral("mtime")).toDouble());
    const double cachedDur = meta.value(QStringLiteral("duration")).toDouble();
    if (cachedMtime != QFileInfo(filePath).lastModified().toSecsSinceEpoch()
        || std::abs(cachedDur - duration) > 2.0)
        return false;

    m_columns = meta.value(QStringLiteral("cols")).toInt(1);
    m_tileWidth = meta.value(QStringLiteral("tw")).toInt(1);
    m_tileHeight = meta.value(QStringLiteral("th")).toInt(1);
    m_count = meta.value(QStringLiteral("count")).toInt(0);
    if (m_columns < 1 || m_tileWidth < 1 || m_tileHeight < 1 || m_count < 1)
        return false;

    m_imageUrl = QUrl::fromLocalFile(pngPath).toString();
    m_ready = true;
    m_generating = false;
    Q_EMIT readyChanged();
    Q_EMIT generatingChanged();
    qInfo() << "seek-thumbs: cache hit" << QFileInfo(filePath).fileName()
            << m_count << "frames";
    return true;
}

void SeekThumbnails::startGeneration(const QString &filePath, double duration)
{
    cancelProcess();
    m_imageUrl.clear();
    if (m_ready) {
        m_ready = false;
        Q_EMIT readyChanged();
    }
    if (!m_generating) {
        m_generating = true;
        Q_EMIT generatingChanged();
    }

    // Sample the whole duration into ~kTargetThumbs frames spread evenly (never
    // closer than 4 seconds apart), then tile them into an 8-wide grid.
    // Bounding the frame count by the target — NOT by the duration — keeps
    // generation at a handful of decode seeks even for long movies, so the
    // preview appears almost immediately instead of spinning while ffmpeg
    // crawls through the whole file.
    const double step = std::max(4.0, duration / kTargetThumbs);
    int frameCount = 1 + static_cast<int>(std::floor(duration / step));
    frameCount = std::min(frameCount, int(kTargetThumbs));
    const int rows = (frameCount + kColumns - 1) / kColumns;
    m_columns = kColumns;
    m_tileWidth = kThumbWidth;
    m_tileHeight = 1;
    m_count = std::min(frameCount, int(kColumns) * rows);

    const QString pngPath = cacheStem(filePath) + QStringLiteral(".png");

    // tile pads a partially filled last row with black, so the grid can be
    // generated in one pass without knowing the exact produced frame count.
    QStringList args;
    args << QStringLiteral("-y") << QStringLiteral("-nostdin")
         << QStringLiteral("-hide_banner") << QStringLiteral("-loglevel")
         << QStringLiteral("error") << QStringLiteral("-threads") << QStringLiteral("2")
         << QStringLiteral("-i") << filePath
         << QStringLiteral("-an") << QStringLiteral("-sn") << QStringLiteral("-dn")
         << QStringLiteral("-vf")
         << QStringLiteral("fps=1/%1,scale=%2:-2,tile=%3x%4")
                .arg(step).arg(kThumbWidth).arg(kColumns).arg(rows)
         << pngPath;

    if (!m_proc)
        m_proc = new QProcess(this);
    connect(m_proc, &QProcess::finished, this, &SeekThumbnails::onProcessFinished,
            Qt::UniqueConnection);
    // A failed launch (e.g. missing ffmpeg) never emits finished, which would
    // leave the generating spinner spinning forever — treat it as a failure.
    connect(m_proc, &QProcess::errorOccurred, this, &SeekThumbnails::onProcessFinished,
            Qt::UniqueConnection);
    qInfo() << "seek-thumbs: generating" << QFileInfo(filePath).fileName()
            << frameCount << "frames, tile" << kColumns << "x" << rows;
    m_proc->start(m_ffmpeg, args);
}

void SeekThumbnails::onProcessFinished()
{
    if (!m_proc)
        return;
    const QString pngPath = cacheStem(m_path) + QStringLiteral(".png");
    if (m_proc->exitStatus() == QProcess::NormalExit
        && m_proc->exitCode() == 0 && QFile::exists(pngPath)) {
        // The tile grid was cols x rows; read the real tile height back from
        // the produced sprite (the scale=-2 filter keeps the aspect ratio).
        const int rows = (m_count + m_columns - 1) / m_columns;
        const QImage sprite(pngPath);
        m_tileHeight = !sprite.isNull() && rows > 0
            ? std::max(1, sprite.height() / rows)
            : m_tileHeight;
        if (m_tileHeight < 1)
            m_tileHeight = 1;

        QJsonObject meta;
        meta.insert(QStringLiteral("count"), m_count);
        meta.insert(QStringLiteral("cols"), m_columns);
        meta.insert(QStringLiteral("rows"), rows);
        meta.insert(QStringLiteral("tw"), m_tileWidth);
        meta.insert(QStringLiteral("th"), m_tileHeight);
        meta.insert(QStringLiteral("duration"), m_duration);
        meta.insert(QStringLiteral("mtime"),
                    double(QFileInfo(m_path).lastModified().toSecsSinceEpoch()));
        QFile jf(cacheStem(m_path) + QStringLiteral(".json"));
        if (jf.open(QIODevice::WriteOnly))
            jf.write(QJsonDocument(meta).toJson(QJsonDocument::Compact));

        m_imageUrl = QUrl::fromLocalFile(pngPath).toString();
        m_ready = true;
        m_generating = false;
        Q_EMIT readyChanged();
        Q_EMIT generatingChanged();
        qInfo() << "seek-thumbs: done" << m_count << "frames, tile"
                << m_tileWidth << "x" << m_tileHeight;
    } else {
        qWarning() << "seek-thumbs: ffmpeg failed" << m_proc->exitCode();
        // Per-file latch so a dead/undecodable file never re-spawns ffmpeg on
        // every pointer move (which pegs the CPU and makes the video unusable).
        m_failedPath = m_path;
        m_generating = false;
        Q_EMIT generatingChanged();
    }
}

void SeekThumbnails::cancelProcess()
{
    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->waitForFinished(500);
    }
}

QString SeekThumbnails::ffmpegPath()
{
    const QString inPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!inPath.isEmpty())
        return inPath;
    return QStringLiteral("/usr/bin/ffmpeg");
}

QString SeekThumbnails::cacheDir()
{
    const QString dir = QStandardPaths::writableLocation(
                            QStandardPaths::CacheLocation)
        + QStringLiteral("/thumbnails");
    QDir().mkpath(dir);
    return dir;
}

QString SeekThumbnails::cacheStem(const QString &filePath)
{
    const QByteArray hash = QCryptographicHash::hash(
                                filePath.toUtf8(), QCryptographicHash::Md5)
                                .toHex().left(16);
    return cacheDir() + QStringLiteral("/thumb_") + QString::fromLatin1(hash);
}
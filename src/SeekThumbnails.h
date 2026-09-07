#pragma once

#include <QObject>
#include <QString>
#include <QImage>
#include <QRect>
#include <QMutex>
#include <QQuickImageProvider>
#include <QtQmlIntegration>

class QProcess;
class QSize;

// Seek previews for the timeline: while hovering or scrubbing the scrubber a
// still of the frame under the pointer floats above the track (replacing the
// bare time bubble).
//
// Thumbnails come from a single sprite sheet generated once per local file
// with ffmpeg — frames sampled evenly across the duration are tiled into one
// PNG and cached in ~/.cache/omarchy/omaplayer/thumbnails (keyed by path +
// mtime). Remote streams (Jellyfin, YouTube…) skip generation and keep the
// plain time tooltip. Generation runs in the background so playback is never
// interrupted; while it is busy the time bubble is shown as before.
class SeekThumbnails : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool generating READ generating NOTIFY generatingChanged)
    Q_PROPERTY(QString imageUrl READ imageUrl NOTIFY readyChanged)
    Q_PROPERTY(int count READ count NOTIFY readyChanged)
    Q_PROPERTY(int columns READ columns NOTIFY readyChanged)
    Q_PROPERTY(int tileWidth READ tileWidth NOTIFY readyChanged)
    Q_PROPERTY(int tileHeight READ tileHeight NOTIFY readyChanged)
    Q_PROPERTY(double duration READ duration NOTIFY readyChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)

public:
    explicit SeekThumbnails(QObject *parent = nullptr);

    bool ready() const { return m_ready; }
    bool generating() const { return m_generating; }
    QString imageUrl() const { return m_imageUrl; }
    int count() const { return m_count; }
    int columns() const { return m_columns; }
    int tileWidth() const { return m_tileWidth; }
    int tileHeight() const { return m_tileHeight; }
    double duration() const { return m_duration; }
    bool enabled() const;
    void setEnabled(bool on);

    // (Re)prepare previews for a file. Called when the media changes; a busy
    // generation is cancelled and restarted, a cached sheet is reused. Remote
    // or nonexistent paths reset the state so the UI falls back to the time
    // bubble.
    Q_INVOKABLE void prepare(const QString &filePath, double duration);
    // Cache-only warm-up: load a ready sheet at media-open so a cached file
    // shows an instant preview on hover even while playing. NEVER spawns
    // ffmpeg — for uncached files it just idles (generation still happens
    // on demand from the scrubber while paused).
    Q_INVOKABLE void preload(const QString &filePath, double duration);
    // Tile index (row-major) for the frame at `position` seconds; QML feeds it
    // to the seek-thumbs image provider URL.
    Q_INVOKABLE int tileIndex(double position) const;
    // Cropped tile for `index` (row-major position in the sprite sheet). The
    // sheet PNG is loaded lazily on each request — tiles are only fetched when
    // the scrub position actually changes index.
    QImage tileImage(int index) const;
    Q_INVOKABLE void clear();

signals:
    void readyChanged();
    void generatingChanged();
    void enabledChanged();

private:
    bool loadFromCache(const QString &filePath, double duration);
    void startGeneration(const QString &filePath, double duration);
    void onProcessFinished();
    void cancelProcess();

    static QString ffmpegPath();
    static QString cacheDir();
    static QString cacheStem(const QString &filePath);

    QString spritePath() const;

    QProcess *m_proc = nullptr;
    mutable QMutex m_spriteMutex;
    mutable QImage m_sprite;
    mutable QString m_spriteKey;
    QString m_ffmpeg;
    QString m_path;
    QString m_failedPath;   // last file whose generation failed (per-file latch)
    double m_duration = 0.0;
    QString m_imageUrl;
    int m_count = 0;
    int m_columns = 1;
    int m_tileWidth = 1;
    int m_tileHeight = 1;
    bool m_ready = false;
    bool m_generating = false;
};

// QML image provider ("image://seekthumbs/<index>") that serves a single
// cropped tile from the generated sprite sheet. QmlImage's sourceClipRect
// renders black in this Qt, so the crop moves into C++.
class SeekThumbProvider : public QQuickImageProvider
{
public:
    explicit SeekThumbProvider(const SeekThumbnails *thumbs)
        : QQuickImageProvider(QQuickImageProvider::Image), m_thumbs(thumbs)
    {
    }
    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;

private:
    const SeekThumbnails *m_thumbs;
};
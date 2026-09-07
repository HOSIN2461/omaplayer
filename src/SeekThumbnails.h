#pragma once

#include <QObject>
#include <QString>
#include <QRect>
#include <QtQmlIntegration>

class QProcess;

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
    // Source rect of the frame at `position` seconds inside the sprite sheet;
    // QML binds it to the Image's sourceClipRect while scrubbing.
    Q_INVOKABLE QRect sourceRect(double position) const;
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

    QProcess *m_proc = nullptr;
    QString m_ffmpeg;
    QString m_path;
    double m_duration = 0.0;
    QString m_imageUrl;
    int m_count = 0;
    int m_columns = 1;
    int m_tileWidth = 1;
    int m_tileHeight = 1;
    bool m_ready = false;
    bool m_generating = false;
};
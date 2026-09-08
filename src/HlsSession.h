#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QTimer>

class QProcess;

// Live HLS session for casting: ffmpeg turns any file into ~4 s segments
// (video copied when H.264, audio → stereo AAC) under a session dir. The
// playlist starts AT the requested position (input -ss), so playback begins
// in seconds with a bounded sliding window (~60 segments ≈ 4 min ≈ ≤100 MB)
// instead of a full pre-transcode. Seeking stays valid inside the window
// and forward (live edge); far backward seeks can 404 (documented).
class HlsSession : public QObject
{
    Q_OBJECT

public:
    explicit HlsSession(QObject *parent = nullptr);
    ~HlsSession() override;

    // Spawns ffmpeg; emits ready() once playlist + first segment exist
    // (or failed() on early exit / timeout). Non-blocking.
    void start(const QString &srcPath, double position, const QString &vcodec);
    void stop();
    bool running() const { return m_proc != nullptr; }
    // Servable: streaming, or finished but window retained (replay/seek).
    bool available() const { return m_proc != nullptr || m_done; }
    QString id() const { return m_id; }
    // Serve playlist/segment bytes for a session-relative name.
    // Returns false (→ caller 404s) for unknown/not-yet-ready names.
    bool serve(const QString &rel, QByteArray *contentType, QByteArray *data);

signals:
    void ready(const QString &playlistRel); // e.g. "index.m3u8"
    void failed(const QString &why);

private:
    void pollReady();

    QString m_id;
    QString m_dir;
    QProcess *m_proc = nullptr;
    QTimer *m_poll = nullptr;
    int m_polls = 0;
    bool m_done = false; // ffmpeg reached EOF; window retained for replay
    QString m_lastError;
};

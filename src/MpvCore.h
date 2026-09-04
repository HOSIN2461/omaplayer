#pragma once

#include <QObject>
#include <QString>
#include <QPointer>
#include <QtQmlIntegration>

class QOpenGLContext;
class QQuickItem;
class QQuickWindow;

extern "C" {
#include <mpv/client.h>
#include <mpv/render_gl.h>
}

// The libmpv handle lives on the main thread; the render() calls come from
// the Qt Quick render thread. mpv render contexts are not thread-bound for
// calls — a render call serializes with the mpv core, and property calls go
// through the mpv event pipe, so mixing both is the documented usage.
class MpvCore : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(double volume READ volume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY mutedChanged)
    Q_PROPERTY(QString mediaTitle READ mediaTitle NOTIFY mediaTitleChanged)
    Q_PROPERTY(QString filePath READ filePath NOTIFY filePathChanged)

public:
    // The QML side instantiates the type (Main.qml holds one as `mpv`), and the
    // renderer reaches the same instance through here. The constructor records
    // s_instance, and instance() creates one lazily when nothing exists yet.
    static MpvCore *instance();

    explicit MpvCore(QObject *parent = nullptr);
    ~MpvCore() override;
    Q_DISABLE_COPY(MpvCore)

    bool playing() const { return m_playing; }
    double position() const { return m_position; }
    double duration() const { return m_duration; }
    double volume() const { return m_volume; }
    bool muted() const { return m_muted; }
    QString mediaTitle() const { return m_mediaTitle; }
    QString filePath() const { return m_filePath; }

    Q_INVOKABLE void open(const QString &location);
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void togglePause();
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE void seekRelative(double seconds);
    Q_INVOKABLE void setVolume(double volume);
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void frameStep();
    Q_INVOKABLE void toggleFullscreen();
    Q_INVOKABLE bool isFullscreen();
    Q_INVOKABLE void takeScreenshot();

    // Renderer-facing API (called on the Qt Quick render thread).
    mpv_render_context *renderContext();
    void renderFrame(int fbo, int width, int height);

    // The item that owns the FBO tells whichever top-level window it currently
    // renders into; the mpv frame-update callback repaints exactly that window.
    void setRenderWindow(QQuickWindow *window);
    void setRenderItem(QQuickItem *item);

signals:
    void playingChanged(bool playing);
    void positionChanged(double position);
    void durationChanged(double duration);
    void volumeChanged(double volume);
    void mutedChanged(bool muted);
    void mediaTitleChanged(const QString &mediaTitle);
    void filePathChanged(const QString &filePath);

public:
    static void wakeupCallback(void *context);
    static void renderUpdateCallback(void *context);

    void handleWakeup();
    void updateFromEvents();

    mpv_handle *m_handle = nullptr;
    mpv_render_context *m_renderContext = nullptr;
    QOpenGLContext *m_renderGlContext = nullptr; // context the render ctx was built on
    QPointer<QQuickWindow> m_renderWindow;
    QPointer<QQuickItem> m_renderItem;
    bool m_pendingOpen = false;
    QString m_pendingLocation;
    bool m_contextRebootPending = false; // deferred loadfile after ctx recreation
    QString m_contextRebootLocation;
    double m_contextRebootPosition = 0.0;

    bool m_playing = false;
    double m_position = 0.0;
    double m_duration = 0.0;
    double m_volume = 100.0;
    bool m_muted = false;
    QString m_mediaTitle;
    QString m_filePath;

    friend class MpvVideoItem;

private:
    static MpvCore *s_instance;
};
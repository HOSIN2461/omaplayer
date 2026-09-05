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
    Q_PROPERTY(double speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(bool subtitlesVisible READ subtitlesVisible NOTIFY subtitlesVisibleChanged)
    Q_PROPERTY(double brightness READ brightness WRITE setBrightness NOTIFY brightnessChanged)
    Q_PROPERTY(double contrast READ contrast WRITE setContrast NOTIFY contrastChanged)
    Q_PROPERTY(double saturation READ saturation WRITE setSaturation NOTIFY saturationChanged)
    Q_PROPERTY(double gamma READ gamma WRITE setGamma NOTIFY gammaChanged)
    Q_PROPERTY(double subScale READ subScale WRITE setSubScale NOTIFY subScaleChanged)
    Q_PROPERTY(double audioDelay READ audioDelay WRITE setAudioDelay NOTIFY audioDelayChanged)

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
    double speed() const { return m_speed; }
    bool subtitlesVisible() const { return m_subVisible; }
    double brightness() const { return m_brightness; }
    double contrast() const { return m_contrast; }
    double saturation() const { return m_saturation; }
    double gamma() const { return m_gamma; }
    double subScale() const { return m_subScale; }
    double audioDelay() const { return m_audioDelay; }

    Q_INVOKABLE void open(const QString &location);
    Q_INVOKABLE void openList(const QStringList &files);
    Q_INVOKABLE QVariantList playlistItems();
    Q_INVOKABLE bool hasNext();
    Q_INVOKABLE bool hasPrevious();
    Q_INVOKABLE void playlistNext();
    Q_INVOKABLE void playlistPrevious();
    Q_INVOKABLE void setLoopStatus(const QString &status);
    Q_INVOKABLE QString loopStatus();
    Q_INVOKABLE void removePlaylistItem(int index);
    Q_INVOKABLE void movePlaylistItem(int from, int to);
    Q_INVOKABLE QString savePlaylist(const QString &filePath);
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
    Q_INVOKABLE void toggleMinimize();
    Q_INVOKABLE void windowFullscreen(bool on);
    Q_INVOKABLE void takeScreenshot();
    Q_INVOKABLE void toggleSubtitles();
    void setSpeed(double speed);
    void setBrightness(double value);
    void setContrast(double value);
    void setSaturation(double value);
    void setGamma(double value);
    void setSubScale(double value);
    void setAudioDelay(double value);

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
    void speedChanged(double speed);
    void subtitlesVisibleChanged(bool visible);
    void brightnessChanged(double value);
    void contrastChanged(double value);
    void saturationChanged(double value);
    void gammaChanged(double value);
    void subScaleChanged(double value);
    void audioDelayChanged(double value);

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
    QStringList m_pendingFiles;
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
    double m_speed = 1.0;
    bool m_subVisible = true;
    double m_brightness = 0.0;
    double m_contrast = 0.0;
    double m_saturation = 0.0;
    double m_gamma = 0.0;
    double m_subScale = 1.0;
    double m_audioDelay = 0.0;

    friend class MpvVideoItem;

private:
    static MpvCore *s_instance;
};
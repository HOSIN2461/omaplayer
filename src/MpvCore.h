#pragma once

#include <QObject>
#include <QString>
#include <QPointer>
#include <QPair>
#include <QSet>
#include <QTimer>
#include <QVector>
#include <QImage>
#include <QVariantMap>
#include <QtQmlIntegration>
#include <functional>

class AudioIntroMatcher;
class QMenu;
class QOpenGLContext;
class QQuickItem;
class QQuickWindow;
class QSystemTrayIcon;

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
    Q_PROPERTY(bool skipPromptVisible READ skipPromptVisible NOTIFY skipPromptChanged)
    Q_PROPERTY(QString skipPromptLabel READ skipPromptLabel NOTIFY skipPromptChanged)
    Q_PROPERTY(bool autoSkip READ autoSkip WRITE setAutoSkip NOTIFY autoSkipChanged)
    Q_PROPERTY(bool audioDetection READ audioDetection WRITE setAudioDetection NOTIFY audioDetectionChanged)

    // --- convenience / playback aids ------------------------------------
    Q_PROPERTY(int sleepRemaining READ sleepRemaining NOTIFY sleepRemainingChanged)
    Q_PROPERTY(bool normalizeVolume READ normalizeVolume WRITE setNormalizeVolume NOTIFY normalizeVolumeChanged)
    Q_PROPERTY(bool resumeEnabled READ resumeEnabled WRITE setResumeEnabled NOTIFY resumeEnabledChanged)
    Q_PROPERTY(QVariantMap mediaInfo READ mediaInfo NOTIFY mediaInfoChanged)

    // --- video settings -------------------------------------------------
    Q_PROPERTY(QString videoAspect READ videoAspect WRITE setVideoAspect NOTIFY videoAspectChanged)
    Q_PROPERTY(int videoRotate READ videoRotate WRITE setVideoRotate NOTIFY videoRotateChanged)
    Q_PROPERTY(bool hwdecEnabled READ hwdecEnabled WRITE setHwdecEnabled NOTIFY hwdecEnabledChanged)
    Q_PROPERTY(bool deinterlaceEnabled READ deinterlaceEnabled WRITE setDeinterlaceEnabled NOTIFY deinterlaceEnabledChanged)
    Q_PROPERTY(bool hdrEnabled READ hdrEnabled WRITE setHdrEnabled NOTIFY hdrEnabledChanged)
    Q_PROPERTY(double hue READ hue WRITE setHue NOTIFY hueChanged)
    Q_PROPERTY(QString videoTrackLabel READ videoTrackLabel NOTIFY videoTrackLabelChanged)

    // --- audio settings -------------------------------------------------
    Q_PROPERTY(QVariantList audioEqGains READ audioEqGains NOTIFY audioEqGainsChanged)
    Q_PROPERTY(int currentAudioId READ currentAudioId NOTIFY currentAudioTrackChanged)
    Q_PROPERTY(QString audioTrackLabel READ audioTrackLabel NOTIFY currentAudioTrackChanged)

    // --- subtitle settings ---------------------------------------------
    Q_PROPERTY(double subDelay READ subDelay WRITE setSubDelay NOTIFY subDelayChanged)
    Q_PROPERTY(double subPos READ subPos WRITE setSubPos NOTIFY subPosChanged)
    Q_PROPERTY(double subFontSize READ subFontSize WRITE setSubFontSize NOTIFY subFontSizeChanged)
    Q_PROPERTY(QString subFontFamily READ subFontFamily WRITE setSubFontFamily NOTIFY subFontFamilyChanged)
    Q_PROPERTY(QString subColor READ subColor WRITE setSubColor NOTIFY subColorChanged)
    Q_PROPERTY(QString subBorderColor READ subBorderColor WRITE setSubBorderColor NOTIFY subBorderColorChanged)
    Q_PROPERTY(double subBorderSize READ subBorderSize WRITE setSubBorderSize NOTIFY subBorderSizeChanged)
    Q_PROPERTY(QString subBackColor READ subBackColor WRITE setSubBackColor NOTIFY subBackColorChanged)
    Q_PROPERTY(int currentSubtitleId READ currentSubtitleId NOTIFY currentSubtitleTrackChanged)

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

    QString videoAspect() const { return m_videoAspect; }
    int videoRotate() const { return m_videoRotate; }
    bool hwdecEnabled() const { return m_hwdecEnabled; }
    bool deinterlaceEnabled() const { return m_deinterlaceEnabled; }
    bool hdrEnabled() const { return m_hdrEnabled; }
    double hue() const { return m_hue; }
    QString videoTrackLabel() const { return m_videoTrackLabel; }

    QVariantList audioEqGains() const { return m_audioEqGains; }
    int currentAudioId() const { return m_currentAudioId; }
    QString audioTrackLabel() const { return m_audioTrackLabel; }

    double subDelay() const { return m_subDelay; }
    double subPos() const { return m_subPos; }
    double subFontSize() const { return m_subFontSize; }
    QString subFontFamily() const { return m_subFontFamily; }
    QString subColor() const { return m_subColor; }
    QString subBorderColor() const { return m_subBorderColor; }
    double subBorderSize() const { return m_subBorderSize; }
    QString subBackColor() const { return m_subBackColor; }
    int currentSubtitleId() const { return m_currentSubtitleId; }

    bool skipPromptVisible() const { return m_skipPromptVisible; }
    QString skipPromptLabel() const { return m_skipPromptLabel; }
    bool autoSkip() const { return m_autoSkip; }
    void setAutoSkip(bool on);
    bool audioDetection() const { return m_audioDetection; }
    void setAudioDetection(bool on);

    int sleepRemaining() const { return m_sleepRemaining; }
    Q_INVOKABLE void setSleepTimer(int seconds);
    bool normalizeVolume() const { return m_normalizeVolume; }
    void setNormalizeVolume(bool on);
    bool resumeEnabled() const { return m_resumeEnabled; }
    void setResumeEnabled(bool on);
    QVariantMap mediaInfo() const { return m_mediaInfo; }

    // Live playback stats for the on-demand statistics overlay (mpv's stats
    // script equivalent): render FPS, A/V sync, codecs/pixelformat, current
    // bitrates, hwdec and dropped frames. Polled freshly on each call while
    // the overlay is on screen; missing properties are simply omitted.
    Q_INVOKABLE QVariantMap stats() const;

    Q_INVOKABLE void open(const QString &location);
    Q_INVOKABLE void openList(const QStringList &files);
    Q_INVOKABLE void appendToPlaylist(const QStringList &files);
    Q_INVOKABLE QVariantList playlistItems();
    Q_INVOKABLE bool hasNext();
    Q_INVOKABLE bool hasPrevious();
    Q_INVOKABLE void playlistNext();
    Q_INVOKABLE void playlistPrevious();
    Q_INVOKABLE void playAt(int index);
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
    // Compact picture-in-picture mode ("Kis méret"): shrink the floating
    // window to a small corner box and back. Qt's OS-level minimize is a no-op
    // on Hyprland without a special workspace, so the key does this instead.
    Q_INVOKABLE void toggleMiniMode();
    Q_INVOKABLE void hideToTray();
    Q_INVOKABLE void windowFullscreen(bool on);
    Q_INVOKABLE void takeScreenshot();
    // True when a real media file is loaded. Asks mpv directly instead of the
    // observed "duration" property, which can lag on first load.
    Q_INVOKABLE bool mediaReady() const;
    // Receiver for the Qt-side grabToImage result (rendered video frame incl.
    // mpv subtitles), which works regardless of the hwdec/GL interop that the
    // mpv-native screenshot fails on with NVIDIA.
    Q_INVOKABLE void saveScreenshotImage(const QImage &img);
    Q_INVOKABLE void toggleSubtitles();
    Q_INVOKABLE void skipCurrent();
    Q_INVOKABLE void dismissSkipPrompt();
    void setSpeed(double speed);
    void setBrightness(double value);
    void setContrast(double value);
    void setSaturation(double value);
    void setGamma(double value);
    void setSubScale(double value);
    void setAudioDelay(double value);
    // Override the stream title mpv shows (Jellyfin direct-play URLs are
    // meaningless api_key URLs; set a readable one after opening).
    void setMediaTitle(const QString &title);
    // --- video in/out ----------------------------------------------------
    void setVideoAspect(const QString &aspect);
    Q_INVOKABLE void setVideoCropAspect(const QString &aspect);
    Q_INVOKABLE void setCustomVideoCrop(int w, int h);
    Q_INVOKABLE void clearVideoCrop();
    void setVideoRotate(int deg);
    void setHwdecEnabled(bool on);
    void setDeinterlaceEnabled(bool on);
    void setHdrEnabled(bool on);
    void setHue(double value);

    // --- audio -----------------------------------------------------------
    Q_INVOKABLE QVariantList audioTracks();
    Q_INVOKABLE void setAudioTrack(int id);
    Q_INVOKABLE void applyAudioEq();
    Q_INVOKABLE void setAudioEqBand(int index, double gain);
    Q_INVOKABLE void resetAudioEq();
    Q_INVOKABLE void loadExternalAudio(const QString &path);
    Q_INVOKABLE void setAudioEqGains(const QVariantList &gains);

    // --- subtitles -------------------------------------------------------
    Q_INVOKABLE QVariantList subtitleTracks();
    Q_INVOKABLE void setSubtitleTrack(int id);
    void setSubDelay(double value);
    void setSubPos(double value);
    void setSubFontSize(double value);
    void setSubFontFamily(const QString &family);
    void setSubColor(const QString &color);
    void setSubBorderColor(const QString &color);
    void setSubBorderSize(double value);
    void setSubBackColor(const QString &color);
    Q_INVOKABLE void setSubtitleColor(const QString &color);
    Q_INVOKABLE void loadExternalSubtitle(const QString &path);
    void refreshTracks();

    // Renderer-facing API (called on the Qt Quick render thread).
    mpv_render_context *renderContext();
    void renderFrame(int fbo, int width, int height);

    // The item that owns the FBO tells whichever top-level window it currently
    // renders into; the mpv frame-update callback repaints exactly that window.
    void setRenderWindow(QQuickWindow *window);
    void setRenderItem(QQuickItem *item);

private:
    void setupTray();
    void restoreFromTray();

    enum class SkipType { Intro, Recap, Credits };
    struct SkipRange {
        double start = 0.0;
        double end = 0.0;
        SkipType type = SkipType::Intro;
        bool prompted = false;
    };
    void recomputeSkipRanges(const mpv_node *list);
    void rebuildSkipRanges();
    void collectTitleSections(QVector<SkipRange> &ranges,
                              const QVector<QPair<double, QString>> &chapters,
                              double total, bool movie) const;
    void collectTimingSection(QVector<SkipRange> &ranges,
                              const QVector<QPair<double, QString>> &chapters,
                              double total) const;
    static QString normalizeChapterTitle(const QString &title);
    static bool classifyTitle(const QString &title, SkipType &type);
    void checkSkipPrompt();
    void skipRange(int index);
    void setSkipPromptVisible(bool visible);
    QString promptLabel(SkipType type) const;
    void startAudioDetection();
    void onAudioSectionFound(double start, double end);
    void onAudioOutroFound(double start, double end);
    void onAudioRecapFound(double start, double end);
    void onAudioNoMatch(const QString &reason);
    QString currentPlaylistPath();
    QVector<SkipRange> m_skipRanges;
    QVector<QPair<double, QString>> m_rawChapters;
    int m_skipPromptRange = -1;
    bool m_skipPromptVisible = false;
    bool m_autoSkip = false;
    bool m_audioDetection = true;
    QString m_skipPromptLabel;
    QTimer *m_skipTimer = nullptr;
    AudioIntroMatcher *m_audioMatcher = nullptr;
    QString m_audioDetectionFile; // file the running/completed detection serves
    QSet<QString> m_audioScanned;  // local files already probed this session

    // Sleep timer, loudness normalization (ReplayGain), position resume and
    // on-demand media specs.
    QTimer *m_sleepTimer = nullptr;
    int m_sleepRemaining = 0;
    bool m_normalizeVolume = false;
    bool m_resumeEnabled = false;
    QVariantMap m_mediaInfo;
    QString m_resumeTrackedPath;  // path whose position we're tracking
    QString m_resumeSeekedPath;   // path already auto-resumed this session
    qint64 m_lastResumeSaveMs = 0;

    void buildAudioEqFilter();
    QTimer *m_eqTimer = nullptr;
    void applyNormalizeOptions();
    void saveResumePosition();
    void seekSavedPosition(const QString &path);
    void buildMediaInfo();
    void clearMediaInfo();
    void refreshVideoLabel();
    void setCurrentAudioId(int id);
    void setCurrentSubtitleId(int id);
    void readOptions();
    QVariantList m_audioEqGains = QVariantList() << 0.0 << 0.0 << 0.0 << 0.0 << 0.0
                                                 << 0.0 << 0.0 << 0.0 << 0.0 << 0.0;

    QString m_videoAspect = QStringLiteral("no");
    int m_videoRotate = 0;
    bool m_hwdecEnabled = true;
    bool m_deinterlaceEnabled = false;
    bool m_hdrEnabled = true;
    double m_hue = 0.0;
    QString m_videoTrackLabel;
    int m_currentAudioId = -1;
    QString m_audioTrackLabel;
    int m_currentSubtitleId = -1;
    double m_subDelay = 0.0;
    double m_subPos = 100.0;
    double m_subFontSize = 55.0;
    QString m_subFontFamily = QStringLiteral("Sans");
    QString m_subColor = QStringLiteral("#FFFFFFFF");
    QString m_subBorderColor = QStringLiteral("#FF000000");
    double m_subBorderSize = 3.0;
    QString m_subBackColor = QStringLiteral("#80000000");

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
    void currentIndexChanged(int index);
    void skipPromptChanged();
    void autoSkipChanged(bool autoSkip);
    void audioDetectionChanged(bool audioDetection);
    void sleepRemainingChanged(int remaining);
    void screenshotRequested();
    void screenshotSaved(const QString &path);
    void sleepTimerFired();
    void normalizeVolumeChanged(bool normalizeVolume);
    void resumeEnabledChanged(bool resumeEnabled);
    void mediaInfoChanged();

    // --- video/audio/subtitle signals -------------------------------------
    void videoAspectChanged();
    void videoRotateChanged();
    void hwdecEnabledChanged();
    void deinterlaceEnabledChanged();
    void hdrEnabledChanged();
    void hueChanged();
    void videoTrackLabelChanged();
    void audioEqGainsChanged();
    void currentAudioTrackChanged();
    void currentSubtitleTrackChanged();
    void subDelayChanged();
    void subPosChanged();
    void subFontSizeChanged();
    void subFontFamilyChanged();
    void subColorChanged();
    void subBorderColorChanged();
    void subBorderSizeChanged();
    void subBackColorChanged();

public:
    static void wakeupCallback(void *context);
    static void renderUpdateCallback(void *context);

    void handleWakeup();
    // Brings the focused Hyprland window to `targetW` x `targetH` via the
    // relative-resize dispatcher (the Lua config has no absolute variant).
    void applyWmResizeTo(int targetW, int targetH);
    void updateFromEvents();
    // Jellyfin server-side segments (intro/recap/outro).
    void onJellyfinSegments(const QVariantList &segments);

    mpv_handle *m_handle = nullptr;
    mpv_render_context *m_renderContext = nullptr;
    QOpenGLContext *m_renderGlContext = nullptr; // context the render ctx was built on
    QPointer<QQuickWindow> m_renderWindow;
    QPointer<QQuickItem> m_renderItem;
    QPointer<QSystemTrayIcon> m_tray;
    bool m_miniMode = false;
    bool m_miniTransitioning = false;
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
    void whenHyprState(const QString &key, const char *value, bool isBool,
                       int timeoutMs, const std::function<void()> &then);
    bool m_fsTransitioning = false;
};
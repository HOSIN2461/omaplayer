#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QObject>
#include <QVariantMap>

class MpvCore;
class QDBusConnection;

// org.mpris.MediaPlayer2 (the "root" interface): Identify/Raise/Quit plus a
// few capability flags and the desktop-entry icon.
class MprisRoot : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")

    Q_PROPERTY(bool CanQuit READ canQuit CONSTANT)
    Q_PROPERTY(bool CanRaise READ canRaise CONSTANT)
    Q_PROPERTY(bool CanSetFullscreen READ canSetFullscreen CONSTANT)
    Q_PROPERTY(bool Fullscreen READ fullscreen WRITE setFullscreen)
    Q_PROPERTY(bool HasTrackList READ hasTrackList CONSTANT)
    Q_PROPERTY(QString Identity READ identity CONSTANT)
    Q_PROPERTY(QStringList SupportedUriSchemes READ supportedUriSchemes CONSTANT)
    Q_PROPERTY(QStringList SupportedMimeTypes READ supportedMimeTypes CONSTANT)

public:
    explicit MprisRoot(MpvCore *core, QObject *parent);

    bool canQuit() const { return true; }
    bool canRaise() const { return true; }
    bool canSetFullscreen() const { return false; }
    bool fullscreen() const { return false; }
    void setFullscreen(bool on);
    bool hasTrackList() const { return true; }
    QString identity() const { return QStringLiteral("Omaplayer"); }
    QStringList supportedUriSchemes() const;
    QStringList supportedMimeTypes() const;

public Q_SLOTS:
    void Raise();
    void Quit();

private:
    MpvCore *m_core;
};

// org.mpris.MediaPlayer2.Player: transport + metadata + status the desktop
// shell and media keys listen to.
class MprisPlayer : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")

    Q_PROPERTY(QString PlaybackStatus READ playbackStatus NOTIFY playbackStatusChanged)
    Q_PROPERTY(QString LoopStatus READ loopStatus WRITE setLoopStatus NOTIFY loopStatusChanged)
    Q_PROPERTY(double Rate READ rate WRITE setRate NOTIFY rateChanged)
    Q_PROPERTY(bool Shuffle READ shuffle)
    Q_PROPERTY(QVariantMap Metadata READ metadata NOTIFY metadataChanged)
    Q_PROPERTY(double Volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(qint64 Position READ position)
    Q_PROPERTY(double MinimumRate READ minimumRate CONSTANT)
    Q_PROPERTY(double MaximumRate READ maximumRate CONSTANT)
    Q_PROPERTY(bool CanGoNext READ canGoNext)
    Q_PROPERTY(bool CanGoPrevious READ canGoPrevious)
    Q_PROPERTY(bool CanPlay READ canPlay CONSTANT)
    Q_PROPERTY(bool CanPause READ canPause CONSTANT)
    Q_PROPERTY(bool CanSeek READ canSeek CONSTANT)
    Q_PROPERTY(bool CanControl READ canControl CONSTANT)

public:
    explicit MprisPlayer(MpvCore *core, QObject *parent);

    QString playbackStatus() const;
    QString loopStatus() const;
    void setLoopStatus(const QString &status);
    double rate() const;
    void setRate(double rate);
    bool shuffle() const;
    QVariantMap metadata() const;
    double volume() const;
    void setVolume(double volume);
    qint64 position() const;
    double minimumRate() const { return 0.05; }
    double maximumRate() const { return 4.0; }
    bool canGoNext() const;
    bool canGoPrevious() const;
    bool canPlay() const { return true; }
    bool canPause() const { return true; }
    bool canSeek() const { return true; }
    bool canControl() const { return true; }

public Q_SLOTS:
    void Next();
    void Previous();
    void Pause();
    void PlayPause();
    void Stop();
    void Play();
    void Seek(qint64 offset);
    void SetPosition(const QDBusObjectPath &trackId, qint64 position);
    void OpenUri(const QString &uri);

signals:
    void playbackStatusChanged(const QString &status);
    void loopStatusChanged(const QString &status);
    void rateChanged(double rate);
    void volumeChanged(double volume);
    void metadataChanged(const QVariantMap &metadata);
    void seeked(qint64 position);

private:
    void onPlayingChanged(bool playing);
    void onPausedChanged(bool paused);
    void onPositionChanged();
    void onDurationChanged();
    void onVolumeChanged(double volume);
    void onMediaTitleChanged(const QString &title);
    void onMetadataChanged();

    MpvCore *m_core;
};

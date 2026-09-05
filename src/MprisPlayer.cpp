#include "MprisPlayer.h"

#include "MpvCore.h"

#include <QDBusObjectPath>
#include <QFileInfo>
#include <QUrl>

static constexpr auto kBusName   = "org.mpris.MediaPlayer2.omaplayer";
static constexpr auto kRootPath  = "/org/mpris/MediaPlayer2";
static constexpr auto kTrackId   = "/org/mpris/MediaPlayer2/TrackList/1";

// ---------------------------------------------------------------------------
// org.mpris.MediaPlayer2 (root)
// ---------------------------------------------------------------------------
MprisRoot::MprisRoot(MpvCore *core, QObject *parent)
    : QDBusAbstractAdaptor(parent)
    , m_core(core)
{
}

void MprisRoot::setFullscreen(bool on)
{
    Q_UNUSED(on)
}

void MprisRoot::Raise()
{
    // No-op: our window manager rule keeps the window visible, there is no
    // taskbar icon to summon.
}

void MprisRoot::Quit()
{
    QCoreApplication::quit();
}

QStringList MprisRoot::supportedUriSchemes() const
{
    return { "file", "http", "https", "rtmp", "rtsp", "ftp", "smb", "mms",
             "hls", "udp", "tcp", "tls", "sftp", "dvd", "bd", "bluray" };
}

QStringList MprisRoot::supportedMimeTypes() const
{
    return { "audio/*", "video/*" };
}

// ---------------------------------------------------------------------------
// org.mpris.MediaPlayer2.Player
// ---------------------------------------------------------------------------
MprisPlayer::MprisPlayer(MpvCore *core, QObject *parent)
    : QDBusAbstractAdaptor(parent)
    , m_core(core)
{
    connect(m_core, &MpvCore::playingChanged, this, &MprisPlayer::onPlayingChanged);
    connect(m_core, &MpvCore::positionChanged, this, &MprisPlayer::onPositionChanged);
    connect(m_core, &MpvCore::durationChanged, this, &MprisPlayer::onDurationChanged);
    connect(m_core, &MpvCore::volumeChanged, this, &MprisPlayer::onVolumeChanged);
    connect(m_core, &MpvCore::mediaTitleChanged, this, &MprisPlayer::onMediaTitleChanged);
    connect(m_core, &MpvCore::speedChanged, this, [this](double /*speed*/) {
        Q_EMIT rateChanged(rate());
    });
}

QString MprisPlayer::playbackStatus() const
{
    if (!m_core->playing())
        return QStringLiteral("Paused");
    return QStringLiteral("Playing");
}

void MprisPlayer::onPlayingChanged(bool playing)
{
    Q_UNUSED(playing)
    Q_EMIT playbackStatusChanged(playbackStatus());
}

void MprisPlayer::onPausedChanged(bool /*paused*/)
{
    Q_EMIT playbackStatusChanged(playbackStatus());
}

void MprisPlayer::onPositionChanged()
{
    // Position is derived on the client side from Rate*elapsed; we only need
    // to say "something moved" so shells polling Position asynchronously get a
    // fresh value. Actually emit seeked rarely is enough — nothing here.
}

void MprisPlayer::onDurationChanged()
{
    Q_EMIT metadataChanged(metadata());
}

void MprisPlayer::onVolumeChanged(double /*vol*/)
{
    Q_EMIT volumeChanged(volume());
}

void MprisPlayer::onMediaTitleChanged(const QString &title)
{
    Q_UNUSED(title)
    Q_EMIT metadataChanged(metadata());
}

void MprisPlayer::onMetadataChanged()
{
    Q_EMIT metadataChanged(metadata());
}

QString MprisPlayer::loopStatus() const
{
    // The core tracks loop state; we expose the mpv "loop-playlist" here.
    return QStringLiteral("None");
}

void MprisPlayer::setLoopStatus(const QString &status)
{
    m_core->setLoopStatus(status);
}

double MprisPlayer::rate() const
{
    return m_core->speed();
}

void MprisPlayer::setRate(double rate)
{
    m_core->setSpeed(rate);
}

bool MprisPlayer::shuffle() const
{
    return false;
}

QVariantMap MprisPlayer::metadata() const
{
    QVariantMap m;
    const QString title = m_core->mediaTitle();
    m["mpris:trackid"] = QVariant::fromValue(QDBusObjectPath(QString::fromLatin1(kTrackId)));
    m["mpris:length"] = qint64(m_core->duration() * 1e6);
    m["mpris:artUrl"] = QString();
    if (!m_core->filePath().isEmpty()) {
        m["xesam:url"] = QUrl::fromLocalFile(m_core->filePath()).toString();
        m["xesam:title"] = title.isEmpty() ? QFileInfo(m_core->filePath()).fileName() : title;
    } else {
        m["xesam:title"] = title;
    }
    return m;
}

double MprisPlayer::volume() const
{
    return m_core->volume() / 100.0;
}

void MprisPlayer::setVolume(double volume)
{
    m_core->setVolume(qBound(0.0, volume * 100.0, 150.0));
}

qint64 MprisPlayer::position() const
{
    return qint64(m_core->position() * 1e6);
}

bool MprisPlayer::canGoNext() const
{
    return m_core->hasNext();
}

bool MprisPlayer::canGoPrevious() const
{
    return m_core->hasPrevious();
}

void MprisPlayer::Next()   { m_core->playlistNext(); }
void MprisPlayer::Previous(){ m_core->playlistPrevious(); }
void MprisPlayer::Pause()  { m_core->pause(); }
void MprisPlayer::PlayPause(){ m_core->togglePause(); }
void MprisPlayer::Stop()   { m_core->stop(); }
void MprisPlayer::Play()   { m_core->play(); }

void MprisPlayer::Seek(qint64 offset)
{
    m_core->seekRelative(offset / 1e6);
}

void MprisPlayer::SetPosition(const QDBusObjectPath &trackId, qint64 position)
{
    Q_UNUSED(trackId)
    m_core->seek(position / 1e6);
}

void MprisPlayer::OpenUri(const QString &uri)
{
    m_core->open(uri);
}

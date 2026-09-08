#pragma once

#include <QObject>
#include <QString>

class QSslSocket;
class QTimer;

// Minimal Google Cast (CastV2) sender — no protobuf dependency.
//
// Discovery (`_googlecast._tcp`) lives in CastManager; this class owns one
// TLS session to port 8009 and speaks the DefaultMediaReceiver (CC1AD845)
// dialect sufficient for local-file casting:
//
//   CONNECT → GET_STATUS → LAUNCH → (session) → LOAD http-url → PLAY/PAUSE…
//
// The local HTTP URL is produced by CastManager's shared file server and
// passed in as `contentId`, so Range/DirectPlay behaviour is identical to
// DLNA casting. Only STRING payloads are used; protobuf framing is
// hand-rolled (4-byte BE length + CastMessage with fields 1,2,3,4,5,6).
class GoogleCastClient : public QObject
{
    Q_OBJECT

public:
    explicit GoogleCastClient(QObject *parent = nullptr);
    ~GoogleCastClient() override;

    bool isConnected() const { return m_connected; }
    QString host() const { return m_host; }

    // Open TLS session (self-signed device cert is expected → ignored).
    void connectTo(const QString &host, quint16 port = 8009);
    void disconnectFrom();
    // Forget the app session (transport/session/media ids) so the next
    // load() re-runs GET_STATUS → LAUNCH instead of talking to a dead
    // transport. CastManager calls this on every new cast.
    void resetSession();
    // LOAD contentId right after (re)connect; `position` in seconds.
    void load(const QString &contentId, const QString &mime,
              const QString &title, double position = 0.0);
    void play();
    void pause();
    void seek(double seconds);
    void stop();

signals:
    void connected();
    void disconnected();
    void loaded(bool ok);
    void notice(const QString &text, const QString &kind);

private:
    void sendMessage(const QString &ns, const QString &dest,
                     const QString &json);
    void sendConnect(const QString &dest = QStringLiteral("receiver-0"));
    void sendGetStatus();
    void sendLaunch();
    void sendLoad();
    void onEncrypted();
    void onReadyRead();
    void onSocketError();
    void handlePayload(const QString &ns, const QString &dest,
                       const QString &jsonText);
    void heartbeatPong();

    QSslSocket *m_socket = nullptr;
    QTimer *m_heartbeat = nullptr;
    QByteArray m_buf;
    int m_requestId = 1;
    bool m_connected = false;

    QString m_host;
    // Pending LOAD (set by load(), flushed once a session exists).
    QString m_contentId, m_mime, m_title;
    double m_position = 0.0;
    bool m_loadPending = false;
    bool m_loadSent = false; // a LOAD is already in flight for this request
    // Session state learned from RECEIVER STATUS.
    QString m_transportId;   // e.g. "web-12"
    QString m_sessionId;
    int m_mediaSessionId = -1;
    QString m_lastDetail;    // last MEDIA_STATUS detailedErrorCode, if any
};

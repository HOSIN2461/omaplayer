#pragma once

#include "AirPlayPairing.h" // Credentials (ltpk:ltsk:atvId:clientId)

#include <QByteArray>
#include <QMap>
#include <QObject>
#include <QString>
#include <QVariant>

class QTcpSocket;
class QUdpSocket;
class QTimer;

// AirPlay 2 playback session (pyatv AP2 flow — NOT the legacy open /play):
//
//   TCP connect :7000 → /pair-verify (saved creds) → HAP-encrypted control
//   channel (HKDF Control-Salt keys + ChaCha20-Poly1305 framing, 1024 B
//   frames) → RTSP SETUP (bplist) → event channel (reversed Events keys,
//   200 OK replies) → RECORD → POST /play (binary plist) → setProperty ×2 →
//   /rate=1 → endTime ×2 → periodic /feedback + /playback-info polls.
//
// Mirrors pyatv ap2_session.py / airplayv2.py / rtsp.py / http.py /
// hap_channel.py / hap_session.py / auth/__init__.py (Control-Salt,
// Control-Write/Read-Encryption-Key, Events-Salt, ...).
//
// Blocking API with nested event loops (same pattern as AirPlayPairing):
// call play()/stop() outside QML handlers (QTimer::singleShot defer).
class AirPlaySession : public QObject
{
    Q_OBJECT

public:
    explicit AirPlaySession(QObject *parent = nullptr);
    ~AirPlaySession() override;

    void setDevice(const QString &host, quint16 port,
                   const AirPlayPairing::Credentials &creds);
    bool isPlaying() const { return m_playing; }

    // Full connect+verify+setup+play flow. True when the TV accepted /play
    // and playback-info shows no error.
    bool play(const QString &url, double position);
    // Mirror-handshake probe (grows into file-push playback): verify →
    // fp-setup → mirror SETUP#1 → RECORD → SETUP#2 video stream. Logs every
    // step; no RTP yet. Diagnostic scaffolding for LG V2-only TVs.
    bool mirrorProbe();
    // Best-effort playback rate (resume uses 1.0).
    void setRate(double rate);
    // TEARDOWN + close everything.
    void stop();

signals:
    void notice(const QString &text, const QString &kind);

private:
    struct Msg {
        bool ok = false;
        QString proto; // "HTTP/1.1" / "RTSP/1.0"
        int code = 0;
        QString message;
        QString error; // transport-level failure (timeout, disconnect, ...)
        QMap<QString, QString> headers; // lower-cased keys
        QByteArray body;
    };
    // HAP session crypto (one instance per channel).
    struct Crypt {
        bool on = false;
        QByteArray outKey, inKey;
        quint64 outCount = 0, inCount = 0;
        QByteArray feed;  // raw encrypted bytes not yet framed
        QByteArray plain; // decrypted stream not yet parsed
    };

    // Transport: pump the event loop until `pred` holds or timeout.
    bool pump(int timeoutMs);
    bool waitReadable(QTcpSocket *sock, int timeoutMs);
    // Control channel exchange (encrypted once verify enables it).
    Msg exchange(const QString &method, const QString &uri, const QString &proto,
                 const QList<QPair<QString, QString>> &headers,
                 const QByteArray &body, const QString &contentType,
                 const QString &userAgent, int timeoutMs = 10000);
    Msg rtsp(const QString &method, const QString &uri, const QVariant &bodyDict,
             const QList<QPair<QString, QString>> &extraHeaders = {},
             int timeoutMs = 10000);
    Msg rtspRaw(const QString &method, const QString &uri, const QByteArray &body,
                const QString &contentType,
                const QList<QPair<QString, QString>> &extraHeaders = {},
                int timeoutMs = 10000);
    QList<QPair<QString, QString>> rtspHeaders(
        const QList<QPair<QString, QString>> &extra);
    QString rtspUri() const;
    Msg http(const QString &method, const QString &uri,
             const QList<QPair<QString, QString>> &headers, const QByteArray &body,
             const QString &contentType);
    static bool tryParseMsg(QByteArray &buf, Msg &out, bool request);
    static QString dumpMsg(const Msg &m); // AP2DUMP=1 error detail
    static void feedFrames(Crypt &c, const QByteArray &chunk, bool &broken);
    static QByteArray frameOut(Crypt &c, const QByteArray &data);

    // Verify handshake on the raw socket; keeps the shared secret for the
    // control/event channel keys. Leaves plain-mode leftovers in m_leftover.
    bool verifyRaw(QByteArray &shared);
    // Enable HAP framing on the control channel.
    void enableControl(const QByteArray &shared);
    // Event channel (reversed Events keys) + handlers.
    bool connectEvent(int eventPort, const QByteArray &shared);
    void onEventData();
    // Minimal NTP responder for the timing port we announce in SETUP.
    bool bindTiming();
    void onTimingData();

    void postFeedback();
    void closeAll();

    QString m_host;
    quint16 m_port = 7000;
    AirPlayPairing::Credentials m_creds;

    QTcpSocket *m_ctrl = nullptr;
    QTcpSocket *m_event = nullptr;
    QUdpSocket *m_timing = nullptr;
    QTimer *m_feedback = nullptr;

    Crypt m_cc; // control channel
    Crypt m_ec; // event channel
    QByteArray m_leftover; // plain bytes read past the verify handshake
    // X25519 shared secret from pair-verify (kept: mirror video keys bind
    // it on some stacks — C# receiver mixes ecdhShared into the video KDF).
    QByteArray m_shared;

    // RTSP dialog state (pyatv RtspSession mirrors).
    int m_cseq = 0;
    QString m_uaRtsp = QStringLiteral("AirPlay/550.10");
    QString m_dacp;
    quint32 m_activeRemote = 0;
    quint32 m_sessionId = 0;
    QString m_localIp;
    QString m_playUuid;

    bool m_playing = false;
};

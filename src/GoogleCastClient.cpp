#include "GoogleCastClient.h"
#include "CastDebug.h"

#include <QSslSocket>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QtEndian>

namespace {

constexpr const char *kNsConnection = "urn:x-cast:com.google.cast.tp.connection";
constexpr const char *kNsHeartbeat = "urn:x-cast:com.google.cast.tp.heartbeat";
constexpr const char *kNsReceiver = "urn:x-cast:com.google.cast.receiver";
constexpr const char *kNsMedia = "urn:x-cast:com.google.cast.media";
constexpr const char *kAppDefaultMedia = "CC1AD845";
constexpr const char *kSenderId = "sender-0";

void putVarint(QByteArray &out, quint64 v)
{
    while (v > 0x7F) {
        out.append(char(0x80 | (v & 0x7F)));
        v >>= 7;
    }
    out.append(char(v & 0x7F));
}

void putStringField(QByteArray &out, int field, const QByteArray &s)
{
    // tag = field<<3 | 2 (length-delimited)
    putVarint(out, quint64((field << 3) | 2));
    putVarint(out, quint64(s.size()));
    out.append(s);
}

void putVarintField(QByteArray &out, int field, quint64 v)
{
    putVarint(out, quint64((field << 3) | 0));
    putVarint(out, v);
}

// CastMessage{1:protocol_version=0, 2:source, 3:dest, 4:ns, 5:STRING=0, 6:json}
QByteArray encodeMessage(const QString &source, const QString &dest,
                         const QString &ns, const QString &json)
{
    QByteArray body;
    putVarintField(body, 1, 0); // CASTV2_1_0
    putStringField(body, 2, source.toUtf8());
    putStringField(body, 3, dest.toUtf8());
    putStringField(body, 4, ns.toUtf8());
    putVarintField(body, 5, 0); // STRING
    putStringField(body, 6, json.toUtf8());
    QByteArray frame;
    const quint32 len = quint32(body.size());
    frame.append(char((len >> 24) & 0xFF));
    frame.append(char((len >> 16) & 0xFF));
    frame.append(char((len >> 8) & 0xFF));
    frame.append(char(len & 0xFF));
    frame.append(body);
    return frame;
}

// Minimal protobuf scan: extract field-6 (payload_utf8) plus fields 3,4 so
// the caller can route by namespace/destination. Returns false if the frame
// is not a CastMessage we understand.
bool decodeMessage(const QByteArray &body, QString *dest, QString *ns,
                   QString *payload)
{
    QString f2, f3, f4, f6;
    int pos = 0;
    auto readVarint = [&](quint64 *v) -> bool {
        quint64 r = 0;
        int shift = 0;
        while (pos < body.size()) {
            const quint8 b = quint8(body.at(pos++));
            r |= quint64(b & 0x7F) << shift;
            if (!(b & 0x80)) {
                *v = r;
                return true;
            }
            shift += 7;
            if (shift > 63)
                return false;
        }
        return false;
    };
    while (pos < body.size()) {
        quint64 tag = 0;
        if (!readVarint(&tag))
            return false;
        const int field = int(tag >> 3);
        const int wire = int(tag & 7);
        if (wire == 0) {
            quint64 dummy = 0;
            if (!readVarint(&dummy))
                return false;
        } else if (wire == 2) {
            quint64 len = 0;
            if (!readVarint(&len))
                return false;
            if (len > quint64(body.size() - pos))
                return false;
            const QString s = QString::fromUtf8(body.mid(pos, int(len)));
            pos += int(len);
            if (field == 2)
                f2 = s;
            else if (field == 3)
                f3 = s;
            else if (field == 4)
                f4 = s;
            else if (field == 6)
                f6 = s;
        } else {
            return false;
        }
    }
    if (f6.isEmpty())
        return false;
    if (dest)
        *dest = f3;
    if (ns)
        *ns = f4;
    if (payload)
        *payload = f6;
    return true;
}

QString mimeDefault(const QString &m)
{
    return m.isEmpty() ? QStringLiteral("video/mp4") : m;
}

} // namespace

GoogleCastClient::GoogleCastClient(QObject *parent)
    : QObject(parent)
    , m_socket(new QSslSocket(this))
    , m_heartbeat(new QTimer(this))
{
    m_heartbeat->setInterval(20000);
    m_heartbeat->setSingleShot(false);
    connect(m_heartbeat, &QTimer::timeout, this, [this] {
        if (m_connected)
            sendMessage(kNsHeartbeat, QStringLiteral("receiver-0"),
                        QStringLiteral(R"({"type":"PING"})"));
    });
    connect(m_socket, &QSslSocket::encrypted, this,
            &GoogleCastClient::onEncrypted);
    connect(m_socket, &QSslSocket::readyRead, this,
            &GoogleCastClient::onReadyRead);
    connect(m_socket, &QSslSocket::disconnected, this, [this] {
        m_connected = false;
        m_heartbeat->stop();
        Q_EMIT disconnected();
    });
    connect(m_socket,
            QOverload<const QList<QSslError> &>::of(
                &QSslSocket::sslErrors),
            this, [this](const QList<QSslError> &) {
                // Cast devices present a self-signed cert; pinning it is
                // out of scope for a LAN sender — proceed.
                m_socket->ignoreSslErrors();
            });
    connect(m_socket, &QSslSocket::errorOccurred, this,
            &GoogleCastClient::onSocketError);
}

GoogleCastClient::~GoogleCastClient()
{
    disconnectFrom();
}

void GoogleCastClient::connectTo(const QString &host, quint16 port)
{
    if (m_connected && host == m_host)
        return;
    disconnectFrom();
    qInfo() << "gcast: TLS connect" << host << (port ? port : 8009);
    m_host = host;
    m_buf.clear();
    m_transportId.clear();
    m_sessionId.clear();
    m_mediaSessionId = -1;
    m_socket->connectToHostEncrypted(host, port ? port : 8009);
}

void GoogleCastClient::disconnectFrom()
{
    m_heartbeat->stop();
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->disconnectFromHost();
    m_connected = false;
    m_loadPending = false;
    m_loadSent = false;
    resetSession();
}

void GoogleCastClient::resetSession()
{
    m_transportId.clear();
    m_sessionId.clear();
    m_mediaSessionId = -1;
}

void GoogleCastClient::load(const QString &contentId, const QString &mime,
                            const QString &title, double position)
{
    m_contentId = contentId;
    m_mime = mimeDefault(mime);
    m_title = title.isEmpty() ? contentId : title;
    m_position = qMax(0.0, position);
    m_loadPending = true;
    m_loadSent = false;
    m_lastDetail.clear();
    if (m_connected && !m_transportId.isEmpty()) {
        sendLoad();
    } else if (m_connected) {
        sendGetStatus(); // re-arm session discovery, LOAD follows STATUS
    }
    // else: flushed from onEncrypted() once TLS is up.
}

void GoogleCastClient::play()
{
    if (!m_connected || m_mediaSessionId < 0)
        return;
    QJsonObject o{{QStringLiteral("type"), QStringLiteral("PLAY")},
                  {QStringLiteral("requestId"), m_requestId++},
                  {QStringLiteral("mediaSessionId"), m_mediaSessionId}};
    sendMessage(kNsMedia, m_transportId,
                QString::fromUtf8(QJsonDocument(o).toJson(
                    QJsonDocument::Compact)));
}

void GoogleCastClient::pause()
{
    if (!m_connected || m_mediaSessionId < 0)
        return;
    QJsonObject o{{QStringLiteral("type"), QStringLiteral("PAUSE")},
                  {QStringLiteral("requestId"), m_requestId++},
                  {QStringLiteral("mediaSessionId"), m_mediaSessionId}};
    sendMessage(kNsMedia, m_transportId,
                QString::fromUtf8(QJsonDocument(o).toJson(
                    QJsonDocument::Compact)));
}

void GoogleCastClient::seek(double seconds)
{
    if (!m_connected || m_mediaSessionId < 0)
        return;
    QJsonObject o{{QStringLiteral("type"), QStringLiteral("SEEK")},
                  {QStringLiteral("requestId"), m_requestId++},
                  {QStringLiteral("mediaSessionId"), m_mediaSessionId},
                  {QStringLiteral("currentTime"), qMax(0.0, seconds)}};
    sendMessage(kNsMedia, m_transportId,
                QString::fromUtf8(QJsonDocument(o).toJson(
                    QJsonDocument::Compact)));
}

void GoogleCastClient::stop()
{
    if (!m_connected)
        return;
    if (m_mediaSessionId >= 0) {
        QJsonObject o{{QStringLiteral("type"), QStringLiteral("STOP")},
                      {QStringLiteral("requestId"), m_requestId++},
                      {QStringLiteral("mediaSessionId"), m_mediaSessionId}};
        sendMessage(kNsMedia, m_transportId,
                    QString::fromUtf8(QJsonDocument(o).toJson(
                        QJsonDocument::Compact)));
    }
    QJsonObject o{{QStringLiteral("type"), QStringLiteral("STOP")},
                  {QStringLiteral("requestId"), m_requestId++},
                  {QStringLiteral("sessionId"), m_sessionId}};
    sendMessage(kNsReceiver, QStringLiteral("receiver-0"),
                QString::fromUtf8(QJsonDocument(o).toJson(
                    QJsonDocument::Compact)));
    m_loadPending = false;
    m_loadSent = false;
    resetSession(); // the app is gone; next load() relaunches it
}

void GoogleCastClient::sendMessage(const QString &ns, const QString &dest,
                                   const QString &json)
{
    if (m_socket->state() != QAbstractSocket::ConnectedState)
        return;
    castDebug(QStringLiteral("gcast> %1 :: %2").arg(ns, json.left(220)));
    m_socket->write(encodeMessage(QString::fromLatin1(kSenderId), dest, ns,
                                  json));
}

void GoogleCastClient::sendConnect(const QString &dest)
{
    // AirConnect-compatible CONNECT (with origin) — some receivers ignore
    // the bare {"type":"CONNECT"} form.
    sendMessage(kNsConnection, dest,
                QStringLiteral(R"({"type":"CONNECT","origin":{}})"));
}

void GoogleCastClient::sendGetStatus()
{
    QJsonObject o{{QStringLiteral("type"), QStringLiteral("GET_STATUS")},
                  {QStringLiteral("requestId"), m_requestId++}};
    sendMessage(kNsReceiver, QStringLiteral("receiver-0"),
                QString::fromUtf8(QJsonDocument(o).toJson(
                    QJsonDocument::Compact)));
}

void GoogleCastClient::sendLaunch()
{
    QJsonObject o{{QStringLiteral("type"), QStringLiteral("LAUNCH")},
                  {QStringLiteral("requestId"), m_requestId++},
                  {QStringLiteral("appId"),
                   QString::fromLatin1(kAppDefaultMedia)}};
    sendMessage(kNsReceiver, QStringLiteral("receiver-0"),
                QString::fromUtf8(QJsonDocument(o).toJson(
                    QJsonDocument::Compact)));
}

void GoogleCastClient::sendLoad()
{
    // One in-flight LOAD per request: the receiver broadcasts RECEIVER_STATUS
    // on every change (launch progress, volume…), and each duplicate LOAD
    // cancels the previous one (LOAD_CANCELLED) — so gate on m_loadSent.
    if (!m_loadPending || m_loadSent || m_transportId.isEmpty())
        return;
    m_loadSent = true;
    QJsonObject meta{{QStringLiteral("type"), 0},
                     {QStringLiteral("metadataType"), 0},
                     {QStringLiteral("title"), m_title}};
    QJsonObject media{{QStringLiteral("contentId"), m_contentId},
                      {QStringLiteral("streamType"), QStringLiteral("BUFFERED")},
                      {QStringLiteral("contentType"), m_mime},
                      {QStringLiteral("metadata"), meta}};
    QJsonObject o{{QStringLiteral("type"), QStringLiteral("LOAD")},
                  {QStringLiteral("requestId"), m_requestId++},
                  {QStringLiteral("sessionId"), m_sessionId},
                  {QStringLiteral("media"), media},
                  {QStringLiteral("autoplay"), false},
                  {QStringLiteral("currentTime"), m_position}};
    sendMessage(kNsMedia, m_transportId,
                QString::fromUtf8(QJsonDocument(o).toJson(
                    QJsonDocument::Compact)));
}

void GoogleCastClient::onEncrypted()
{
    m_connected = true;
    qInfo() << "gcast: TLS up, sending CONNECT";
    Q_EMIT connected();
    sendConnect();
    m_heartbeat->start();
    if (m_loadPending && m_transportId.isEmpty())
        sendGetStatus();
    else if (m_loadPending)
        sendLoad();
    else
        sendGetStatus();
}

void GoogleCastClient::onReadyRead()
{
    m_buf.append(m_socket->readAll());
    while (m_buf.size() >= 4) {
        const quint32 len = (quint8(m_buf.at(0)) << 24)
            | (quint8(m_buf.at(1)) << 16) | (quint8(m_buf.at(2)) << 8)
            | quint8(m_buf.at(3));
        if (len > 64 * 1024 * 1024) {
            m_buf.clear();
            return;
        }
        if (m_buf.size() < 4 + int(len))
            return;
        const QByteArray body = m_buf.mid(4, int(len));
        m_buf.remove(0, 4 + int(len));
        QString dest, ns, payload;
        if (decodeMessage(body, &dest, &ns, &payload))
            handlePayload(ns, dest, payload);
    }
}

void GoogleCastClient::onSocketError()
{
    qInfo() << "gcast: socket error" << m_socket->errorString();
    Q_EMIT notice(tr("Google Cast kapcsolat hiba: %1")
                      .arg(m_socket->errorString()),
                  QStringLiteral("err"));
}

void GoogleCastClient::handlePayload(const QString &ns, const QString &dest,
                                     const QString &jsonText)
{
    Q_UNUSED(dest);
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(jsonText.toUtf8(),
                                                      &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return;
    const QJsonObject o = doc.object();
    const QString type = o.value(QStringLiteral("type")).toString();
    castDebug(QStringLiteral("gcast< %1 :: %2 :: %3")
                  .arg(ns, type, jsonText.left(300)));

    if (ns == QLatin1String(kNsHeartbeat) && type == QLatin1String("PING")) {
        heartbeatPong();
        return;
    }
    if (ns == QLatin1String(kNsConnection)
        && type == QLatin1String("CLOSE")) {
        // Receiver tore down our app transport (e.g. after STOP or app
        // exit). Forget the dead ids; a pending load re-runs discovery.
        castDebug(QStringLiteral("gcast: transport CLOSE, session reset"));
        resetSession();
        if (m_loadPending && !m_loadSent)
            sendGetStatus();
        return;
    }
    if (ns == QLatin1String(kNsReceiver)) {
        if (type == QLatin1String("RECEIVER_STATUS")) {
            const QJsonObject status = o.value(QStringLiteral("status"))
                                           .toObject();
            const QJsonArray apps = status.value(QStringLiteral("applications"))
                                        .toArray();
            QString transport, session;
            for (const QJsonValue &v : apps) {
                const QJsonObject app = v.toObject();
                if (app.value(QStringLiteral("appId")).toString()
                    == QLatin1String(kAppDefaultMedia)) {
                    transport = app.value(QStringLiteral("transportId"))
                                    .toString();
                    session = app.value(QStringLiteral("sessionId")).toString();
                    break;
                }
            }
            if (!transport.isEmpty()) {
                const bool isNew = transport != m_transportId;
                m_transportId = transport;
                m_sessionId = session;
                if (isNew)
                    sendConnect(m_transportId); // join the app transport
                if (m_loadPending && !m_loadSent)
                    sendLoad();
            } else if (m_loadPending) {
                sendLaunch(); // DefaultMediaReceiver not running → start it
            }
        } else if (type == QLatin1String("LAUNCH_ERROR")) {
            m_loadPending = false;
            m_loadSent = false;
            Q_EMIT loaded(false);
            Q_EMIT notice(tr("Google Cast indítás elutasítva"), "err");
        }
        return;
    }
    if (ns == QLatin1String(kNsMedia)) {
        if (type == QLatin1String("MEDIA_STATUS")) {
            const QJsonArray st = o.value(QStringLiteral("status")).toArray();
            if (!st.isEmpty()) {
                const QJsonObject first = st.first().toObject();
                if (first.contains(QStringLiteral("mediaSessionId")))
                    m_mediaSessionId = first
                                           .value(QStringLiteral("mediaSessionId"))
                                           .toInt(m_mediaSessionId);
                // Remember receiver-side complaints (e.g. detailedErrorCode
                // 104 = format not supported) so LOAD_FAILED can report them.
                const int detailed = first.value(QStringLiteral("detailedErrorCode"))
                                         .toInt(-1);
                if (detailed >= 0)
                    m_lastDetail = QStringLiteral("részletes kód: %1")
                                       .arg(detailed);
            }
            if (m_loadPending) {
                m_loadPending = false;
                m_loadSent = false;
                Q_EMIT loaded(true);
                Q_EMIT notice(tr("Kivetítve Google Cast-ra"), "ok");
                // Explicit PLAY like AirConnect (its LOAD uses autoplay:0):
                // some firmwares stay IDLE on autoplay:true + currentTime>0.
                play();
            }
        } else if (type == QLatin1String("LOAD_FAILED")
                   || type == QLatin1String("INVALID_REQUEST")) {
            m_loadPending = false;
            m_loadSent = false;
            Q_EMIT loaded(false);
            const QString reason = o.value(QStringLiteral("reason")).toString();
            QString why = !reason.isEmpty()
                ? reason
                : !m_lastDetail.isEmpty()
                    ? m_lastDetail + QStringLiteral(" · ")
                        + jsonText.left(120)
                    : jsonText.left(160);
            Q_EMIT notice(tr("Google Cast betöltés sikertelen (%1)").arg(why),
                          "err");
        } else if (type == QLatin1String("LOAD_CANCELLED")) {
            // Our own superseding request (or a STOP) killed this LOAD; the
            // replacement's MEDIA_STATUS will settle m_loadPending. Keep
            // waiting instead of reporting failure.
            m_loadSent = false;
        }
    }
}

void GoogleCastClient::heartbeatPong()
{
    sendMessage(kNsHeartbeat, QStringLiteral("receiver-0"),
                QStringLiteral(R"({"type":"PONG"})"));
}

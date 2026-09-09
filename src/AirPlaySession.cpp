#include "AirPlaySession.h"
#include "Bplist.h"
#include "CastDebug.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QCryptographicHash>
#include <QDateTime>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>
#include <QElapsedTimer>

#include <sodium.h>

namespace {

QByteArray hkdf(const QByteArray &salt, const QByteArray &info,
                const QByteArray &ikm)
{
    // HKDF-SHA512 extract+expand, 32 B — same as AirPlayPairing::hkdf and
    // pyatv hap_srp.hkdf_expand (cryptography lib, SHA512, length 32).
    QByteArray realSalt = salt.isEmpty() ? QByteArray(64, 0) : salt;
    QMessageAuthenticationCode prk(QCryptographicHash::Sha512, realSalt);
    prk.addData(ikm);
    const QByteArray prkBytes = prk.result();
    QByteArray t, okm;
    quint8 counter = 1;
    while (okm.size() < 32) {
        QMessageAuthenticationCode mac(QCryptographicHash::Sha512, prkBytes);
        mac.addData(t + info + QByteArray(1, char(counter)));
        t = mac.result();
        okm += t;
        ++counter;
    }
    return okm.left(32);
}

QByteArray nonce8(quint64 ctr)
{
    QByteArray n;
    for (int i = 0; i < 8; ++i)
        n.append(char((ctr >> (8 * i)) & 0xFF));
    return n;
}

// Helpers take the 8-byte nonce (counter or "PV-Msg02"-style tag) and
// pad to 12 bytes like AirPlayPairing (4 zero bytes + 8).
QByteArray padNonce(const QByteArray &nonce8b)
{
    QByteArray n(4, 0);
    n += nonce8b.left(8);
    return n;
}

QByteArray aeadEncrypt(const QByteArray &key, const QByteArray &nonce8b,
                       const QByteArray &plain, const QByteArray &ad = {})
{
    const QByteArray nonce = padNonce(nonce8b);
    QByteArray out(plain.size()
                       + crypto_aead_chacha20poly1305_ietf_ABYTES,
                   0);
    unsigned long long outLen = 0;
    if (crypto_aead_chacha20poly1305_ietf_encrypt(
            reinterpret_cast<unsigned char *>(out.data()), &outLen,
            reinterpret_cast<const unsigned char *>(plain.constData()),
            plain.size(),
            ad.isEmpty() ? nullptr
                         : reinterpret_cast<const unsigned char *>(ad.constData()),
            ad.size(), nullptr,
            reinterpret_cast<const unsigned char *>(nonce.constData()),
            reinterpret_cast<const unsigned char *>(key.constData()))
        != 0)
        return {};
    out.resize(outLen);
    return out;
}

QByteArray aeadDecrypt(const QByteArray &key, const QByteArray &nonce8b,
                       const QByteArray &cipher, const QByteArray &ad = {})
{
    const QByteArray nonce = padNonce(nonce8b);
    QByteArray out(cipher.size(), 0);
    unsigned long long outLen = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char *>(out.data()), &outLen, nullptr,
            reinterpret_cast<const unsigned char *>(cipher.constData()),
            cipher.size(),
            ad.isEmpty() ? nullptr
                         : reinterpret_cast<const unsigned char *>(ad.constData()),
            ad.size(),
            reinterpret_cast<const unsigned char *>(nonce.constData()),
            reinterpret_cast<const unsigned char *>(key.constData()))
        != 0)
        return {};
    out.resize(outLen);
    return out;
}

QMap<int, QByteArray> tlvRead(const QByteArray &data)
{
    QMap<int, QByteArray> out;
    int pos = 0;
    while (pos + 2 <= data.size()) {
        const int tag = quint8(data.at(pos));
        const int len = quint8(data.at(pos + 1));
        if (pos + 2 + len > data.size())
            break;
        out[tag] += data.mid(pos + 2, len);
        pos += 2 + len;
    }
    return out;
}

QByteArray tlvWrite(const QList<QPair<int, QByteArray>> &data)
{
    QByteArray out;
    for (const auto &kv : data) {
        const QByteArray &v = kv.second;
        int pos = 0, left = v.size();
        do {
            const int chunk = qMin(left, 255);
            out.append(char(kv.first));
            out.append(char(chunk));
            out.append(v.mid(pos, chunk));
            pos += chunk;
            left -= chunk;
        } while (left > 0);
    }
    return out;
}

QString shortBody(const QByteArray &b)
{
    // One-line summary for the debug log (bplist keys, no bulk).
    const QVariant v = Bplist::decode(b);
    if (v.typeId() == QMetaType::QVariantMap) {
        const QVariantMap m = v.toMap();
        QStringList keys = m.keys();
        QString s = QStringLiteral("bplist{%1}").arg(keys.join(QLatin1Char(',')));
        if (m.contains(QStringLiteral("error")))
            s += QStringLiteral(" ERROR=%1").arg(
                QString::fromUtf8(QByteArray::number(m.value(QStringLiteral("error"))
                                                         .toString().size())));
        return s.left(160);
    }
    if (b.size() <= 120)
        return QString::fromLatin1(b.toHex(' ')).left(160);
    return QStringLiteral("%1B bin").arg(b.size());
}

} // namespace

QString AirPlaySession::dumpMsg(const Msg &m)
{
    if (qgetenv("AP2DUMP") != "1")
        return {};
    QByteArray h;
    for (auto it = m.headers.constBegin(); it != m.headers.constEnd(); ++it)
        h += it.key().toLatin1() + "=" + it.value().toLatin1() + ";";
    return QStringLiteral(" hdrs=%1 body=%2")
        .arg(QString::fromLatin1(h).left(400))
        .arg(QString::fromLatin1(m.body.toHex(' ')).left(400));
}

AirPlaySession::AirPlaySession(QObject *parent)
    : QObject(parent)
    , m_feedback(new QTimer(this))
{
    sodium_init();
    m_feedback->setInterval(2000);
    connect(m_feedback, &QTimer::timeout, this, &AirPlaySession::postFeedback);
}

AirPlaySession::~AirPlaySession()
{
    closeAll();
}

void AirPlaySession::setDevice(const QString &host, quint16 port,
                               const AirPlayPairing::Credentials &creds)
{
    if (m_playing)
        stop();
    m_host = host;
    m_port = port ? port : 7000;
    m_creds = creds;
}

void AirPlaySession::closeAll()
{
    m_feedback->stop();
    m_playing = false;
    if (m_ctrl) {
        m_ctrl->disconnectFromHost();
        m_ctrl->deleteLater();
        m_ctrl = nullptr;
    }
    if (m_event) {
        m_event->disconnectFromHost();
        m_event->deleteLater();
        m_event = nullptr;
    }
    if (m_timing) {
        m_timing->close();
        m_timing->deleteLater();
        m_timing = nullptr;
    }
    m_cc = Crypt();
    m_ec = Crypt();
    m_leftover.clear();
}

bool AirPlaySession::pump(int timeoutMs)
{
    QEventLoop loop;
    QTimer t;
    t.setSingleShot(true);
    t.setInterval(timeoutMs);
    connect(&t, &QTimer::timeout, &loop, &QEventLoop::quit);
    t.start();
    loop.exec();
    return true;
}

bool AirPlaySession::waitReadable(QTcpSocket *sock, int timeoutMs)
{
    if (!sock || sock->state() != QAbstractSocket::ConnectedState)
        return false;
    if (sock->bytesAvailable() > 0)
        return true;
    QEventLoop loop;
    QTimer t;
    t.setSingleShot(true);
    t.setInterval(timeoutMs);
    connect(&t, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(sock, &QTcpSocket::readyRead, &loop, &QEventLoop::quit);
    connect(sock, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    t.start();
    loop.exec();
    return sock->bytesAvailable() > 0;
}

bool AirPlaySession::tryParseMsg(QByteArray &buf, Msg &out, bool request)
{
    const int hlen = buf.indexOf("\r\n\r\n");
    if (hlen < 0)
        return false;
    const QList<QByteArray> lines =
        buf.left(hlen).split('\n'); // keep \r handling simple below
    if (lines.isEmpty())
        return false;
    const QString first =
        QString::fromLatin1(lines.first().trimmed());
    const QStringList parts = first.split(QLatin1Char(' '));
    QMap<QString, QString> headers;
    for (int i = 1; i < lines.size(); ++i) {
        const QString line = QString::fromLatin1(lines.at(i).trimmed());
        if (line.isEmpty())
            continue;
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon < 0)
            continue;
        headers.insert(line.left(colon).trimmed().toLower(),
                       line.mid(colon + 1).trimmed());
    }
    const int bodyLen = headers.value(QStringLiteral("content-length")).toInt();
    const int total = hlen + 4 + bodyLen;
    if (buf.size() < total)
        return false;
    out.body = buf.mid(hlen + 4, bodyLen);
    out.headers = headers;
    buf.remove(0, total);
    if (request) {
        if (parts.size() < 3)
            return false;
        out.ok = true;
        out.proto = parts.at(2);
        out.message = parts.at(0) + QLatin1Char(' ') + parts.at(1); // METHOD path
        out.code = 0;
        return true;
    }
    if (parts.size() < 3)
        return false;
    out.ok = true;
    out.proto = parts.at(0);
    out.code = parts.at(1).toInt();
    out.message = parts.mid(2).join(QLatin1Char(' '));
    return true;
}

void AirPlaySession::feedFrames(AirPlaySession::Crypt &c,
                                const QByteArray &chunk, bool &broken)
{
    c.feed += chunk;
    while (c.feed.size() >= 2) {
        const int len = quint8(c.feed.at(0)) | (quint8(c.feed.at(1)) << 8);
        if (c.feed.size() < 2 + len + 16)
            break;
        const QByteArray lenBytes = c.feed.left(2);
        const QByteArray block = c.feed.mid(2, len + 16);
        const QByteArray dec =
            aeadDecrypt(c.inKey, nonce8(c.inCount), block, lenBytes);
        if (dec.isEmpty() && len > 0) {
            broken = true;
            return;
        }
        ++c.inCount;
        c.plain += dec;
        c.feed.remove(0, 2 + len + 16);
    }
}

QByteArray AirPlaySession::frameOut(AirPlaySession::Crypt &c,
                                     const QByteArray &data)
{
    QByteArray out;
    int pos = 0;
    while (pos < data.size()) {
        const int n = qMin(1024, data.size() - pos);
        QByteArray len;
        len.append(char(n & 0xFF));
        len.append(char((n >> 8) & 0xFF));
        out += len + aeadEncrypt(c.outKey, nonce8(c.outCount),
                                 data.mid(pos, n), len);
        ++c.outCount;
        pos += n;
    }
    return out;
}

AirPlaySession::Msg AirPlaySession::exchange(
    const QString &method, const QString &uri, const QString &proto,
    const QList<QPair<QString, QString>> &headers, const QByteArray &body,
    const QString &contentType, const QString &userAgent, int timeoutMs)
{
    Msg fail;
    if (!m_ctrl || m_ctrl->state() != QAbstractSocket::ConnectedState) {
        fail.error = tr("nincs kapcsolat");
        return fail;
    }
    QByteArray req = (method + QLatin1Char(' ') + uri + QLatin1Char(' ') + proto
                      + QStringLiteral("\r\nUser-Agent: ") + userAgent
                      + QStringLiteral("\r\n"))
                         .toLatin1();
    if (!contentType.isEmpty())
        req += "Content-Type: " + contentType.toLatin1() + "\r\n";
    if (!body.isEmpty())
        req += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    for (const auto &h : headers)
        req += h.first.toLatin1() + ": " + h.second.toLatin1() + "\r\n";
    req += "\r\n";
    const QByteArray head = req;
    req += body;
    if (qgetenv("AP2DUMP") == "1")
        castDebug(QStringLiteral("ap2> %1 %2 head=%3")
                      .arg(method, uri,
                           QString::fromLatin1(head.left(500))));
    if (m_cc.on)
        m_ctrl->write(frameOut(m_cc, req));
    else
        m_ctrl->write(req);
    m_ctrl->flush();

    QElapsedTimer deadline;
    deadline.start();
    bool broken = false;
    while (deadline.elapsed() < timeoutMs) {
        if (m_cc.on) {
            Msg m;
            if (tryParseMsg(m_cc.plain, m, false))
                return m;
        } else {
            Msg m;
            if (tryParseMsg(m_leftover, m, false))
                return m;
        }
        const int left = int(timeoutMs - deadline.elapsed());
        if (!waitReadable(m_ctrl, qMin(left, 500)))
            continue;
        const QByteArray chunk = m_ctrl->readAll();
        if (!chunk.isEmpty()) {
            if (m_cc.on)
                feedFrames(m_cc, chunk, broken);
            else
                m_leftover += chunk;
        }
        if (broken) {
            fail.error = tr("titkosítási hiba");
            return fail;
        }
        if (m_ctrl->state() != QAbstractSocket::ConnectedState
            && m_ctrl->bytesAvailable() == 0) {
            fail.error = tr("a TV bontotta a kapcsolatot");
            return fail;
        }
    }
    fail.error = tr("időtúllépés");
    return fail;
}

QList<QPair<QString, QString>> AirPlaySession::rtspHeaders(
    const QList<QPair<QString, QString>> &extra)
{
    QList<QPair<QString, QString>> h;
    h += {QStringLiteral("CSeq"), QString::number(m_cseq++)};
    h += {QStringLiteral("DACP-ID"), m_dacp};
    h += {QStringLiteral("Active-Remote"),
          QString::number(m_activeRemote)};
    h += {QStringLiteral("Client-Instance"), m_dacp};
    h += extra;
    return h;
}

QString AirPlaySession::rtspUri() const
{
    return QStringLiteral("rtsp://%1/%2").arg(m_localIp).arg(m_sessionId);
}

AirPlaySession::Msg AirPlaySession::rtsp(
    const QString &method, const QString &uri, const QVariant &bodyDict,
    const QList<QPair<QString, QString>> &extraHeaders, int timeoutMs)
{
    const QByteArray body = Bplist::encode(bodyDict);
    return rtspRaw(method, uri, body,
                   QStringLiteral("application/x-apple-binary-plist"),
                   extraHeaders, timeoutMs);
}

AirPlaySession::Msg AirPlaySession::rtspRaw(
    const QString &method, const QString &uri, const QByteArray &body,
    const QString &contentType,
    const QList<QPair<QString, QString>> &extraHeaders, int timeoutMs)
{
    Msg m = exchange(method, uri.isEmpty() ? rtspUri() : uri,
                     QStringLiteral("RTSP/1.0"), rtspHeaders(extraHeaders), body,
                     contentType, m_uaRtsp, timeoutMs);
    castDebug(QStringLiteral("ap2< %1 %2 code=%3 %4%5")
                  .arg(method, uri.isEmpty() ? rtspUri() : uri).arg(m.code)
                  .arg(m.ok ? shortBody(m.body) : m.error)
                  .arg(dumpMsg(m)));
    return m;
}

AirPlaySession::Msg AirPlaySession::http(
    const QString &method, const QString &uri,
    const QList<QPair<QString, QString>> &headers, const QByteArray &body,
    const QString &contentType)
{
    Msg m = exchange(method, uri, QStringLiteral("HTTP/1.1"), headers, body,
                     contentType, QStringLiteral("AirPlay/550.10"));
    castDebug(QStringLiteral("ap2< %1 %2 code=%3 %4%5")
                  .arg(method, uri).arg(m.code)
                  .arg(m.ok ? shortBody(m.body) : m.error)
                  .arg(dumpMsg(m)));
    return m;
}

bool AirPlaySession::verifyRaw(QByteArray &shared)
{
    // Pair-verify on the raw control socket (mirrors AirPlayPairing::verify
    // + pyatv hap.verify1): M1/M3 plain, keep the X25519 secret for the
    // channel keys.
    unsigned char seed[32];
    randombytes_buf(seed, sizeof(seed));
    const QByteArray xPriv(reinterpret_cast<char *>(seed), 32);
    unsigned char xpubRaw[32];
    crypto_scalarmult_base(xpubRaw, seed);
    const QByteArray xPub(reinterpret_cast<char *>(xpubRaw), 32);

    QList<QPair<int, QByteArray>> m1;
    m1 += {6, QByteArray(1, 1)};
    m1 += {3, xPub};
    Msg r = exchange(
        QStringLiteral("POST"), QStringLiteral("/pair-verify"),
        QStringLiteral("HTTP/1.1"),
        {{"X-Apple-HKP", "3"}, {"Connection", "keep-alive"}}, tlvWrite(m1),
        QStringLiteral("application/octet-stream"),
        QStringLiteral("AirPlay/320.20"));
    if (!r.ok || r.code != 200) {
        castDebug(QStringLiteral("ap2: verify M1 code=%1 err=%2")
                      .arg(r.code).arg(r.error));
        return false;
    }
    const QMap<int, QByteArray> tlv = tlvRead(r.body);
    {
        QString sum;
        for (auto it = tlv.constBegin(); it != tlv.constEnd(); ++it)
            sum += QStringLiteral("%1:%2B ").arg(it.key()).arg(it->size());
        castDebug(QStringLiteral("ap2: verify M1 resp code=%1 tlv={%2}")
                      .arg(r.code).arg(sum.trimmed()));
    }
    if (!tlv.contains(3) || !tlv.contains(5)) {
        castDebug(QStringLiteral("ap2: verify M1 missing key/salt"));
        return false;
    }
    const QByteArray serverXPub = tlv[3];
    unsigned char sh[32];
    if (crypto_scalarmult(
            sh, reinterpret_cast<const unsigned char *>(xPriv.constData()),
            reinterpret_cast<const unsigned char *>(serverXPub.constData()))
        != 0) {
        castDebug(QStringLiteral("ap2: verify ECDH failed"));
        return false;
    }
    shared = QByteArray(reinterpret_cast<char *>(sh), 32);
    const QByteArray encKey =
        hkdf("Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", shared);
    const QByteArray dec =
        aeadDecrypt(encKey, QByteArray("PV-Msg02", 8), tlv[5]);
    if (dec.isEmpty()) {
        castDebug(QStringLiteral("ap2: verify M2 decrypt failed"));
        return false;
    }
    const QMap<int, QByteArray> atv = tlvRead(dec);
    if (!atv.contains(1) || !atv.contains(0x0A)
        || atv[1] != m_creds.atvId) {
        castDebug(QStringLiteral("ap2: verify atv id mismatch"));
        return false;
    }
    const QByteArray info = serverXPub + m_creds.atvId + xPub;
    if (crypto_sign_verify_detached(
            reinterpret_cast<const unsigned char *>(atv[0x0A].constData()),
            reinterpret_cast<const unsigned char *>(info.constData()),
            info.size(),
            reinterpret_cast<const unsigned char *>(m_creds.ltpk.constData()))
        != 0) {
        castDebug(QStringLiteral("ap2: verify sig invalid"));
        return false;
    }
    unsigned char sig[64], sk[64], pk[32];
    crypto_sign_seed_keypair(
        pk, sk,
        reinterpret_cast<const unsigned char *>(m_creds.ltsk.constData()));
    const QByteArray info2 = xPub + m_creds.clientId + serverXPub;
    crypto_sign_detached(
        sig, nullptr,
        reinterpret_cast<const unsigned char *>(info2.constData()),
        info2.size(), sk);
    QList<QPair<int, QByteArray>> inner;
    inner += {1, m_creds.clientId};
    inner += {0x0A, QByteArray(reinterpret_cast<char *>(sig), 64)};
    const QByteArray enc =
        aeadEncrypt(encKey, QByteArray("PV-Msg03", 8), tlvWrite(inner));
    QList<QPair<int, QByteArray>> m3;
    m3 += {6, QByteArray(1, 3)};
    m3 += {5, enc};
    r = exchange(QStringLiteral("POST"), QStringLiteral("/pair-verify"),
                 QStringLiteral("HTTP/1.1"),
                 {{"X-Apple-HKP", "3"}, {"Connection", "keep-alive"}},
                 tlvWrite(m3), QStringLiteral("application/octet-stream"),
                 QStringLiteral("AirPlay/320.20"));
    if ((!r.ok && r.code != 200 && r.code != 204) || r.code == 0) {
        castDebug(QStringLiteral("ap2: verify M3 code=%1 err=%2")
                      .arg(r.code).arg(r.error));
        return false;
    }
    m_shared = shared;
    return true;
}

void AirPlaySession::enableControl(const QByteArray &shared)
{
    m_cc.on = true;
    m_cc.outKey = hkdf("Control-Salt", "Control-Write-Encryption-Key", shared);
    m_cc.inKey = hkdf("Control-Salt", "Control-Read-Encryption-Key", shared);
    m_cc.outCount = m_cc.inCount = 0;
    if (!m_leftover.isEmpty()) {
        bool broken = false;
        feedFrames(m_cc, m_leftover, broken);
        m_leftover.clear();
        if (broken)
            castDebug(QStringLiteral("ap2: leftover decrypt broken"));
    }
    castDebug(QStringLiteral("ap2: control channel encrypted"));
}

bool AirPlaySession::connectEvent(int eventPort, const QByteArray &shared)
{
    if (eventPort <= 0)
        return false;
    m_event = new QTcpSocket(this);
    connect(m_event, &QTcpSocket::readyRead, this,
            &AirPlaySession::onEventData);
    m_event->connectToHost(m_host, quint16(eventPort));
    if (!m_event->waitForConnected(6000)) {
        castDebug(QStringLiteral("ap2: event connect failed"));
        m_event->deleteLater();
        m_event = nullptr;
        return false;
    }
    // NOTE: read/write reversed — the connection originates from the
    // receiver (pyatv ap2_session comment).
    m_ec.on = true;
    m_ec.outKey = hkdf("Events-Salt", "Events-Read-Encryption-Key", shared);
    m_ec.inKey = hkdf("Events-Salt", "Events-Write-Encryption-Key", shared);
    m_ec.outCount = m_ec.inCount = 0;
    castDebug(QStringLiteral("ap2: event channel on %1").arg(eventPort));
    return true;
}

void AirPlaySession::onEventData()
{
    if (!m_event)
        return;
    const QByteArray chunk = m_event->readAll();
    if (chunk.isEmpty())
        return;
    bool broken = false;
    feedFrames(m_ec, chunk, broken);
    if (broken) {
        castDebug(QStringLiteral("ap2: event decrypt broken"));
        return;
    }
    Msg req;
    while (tryParseMsg(m_ec.plain, req, true)) {
        // Reply 200 OK with CSeq (/Server) echo, like pyatv EventChannel.
        QByteArray resp = "HTTP/1.1 200 OK\r\n";
        const QString cseq = req.headers.value(QStringLiteral("cseq"));
        if (!cseq.isEmpty())
            resp += "CSeq: " + cseq.toLatin1() + "\r\n";
        const QString srv = req.headers.value(QStringLiteral("server"));
        if (!srv.isEmpty())
            resp += "Server: " + srv.toLatin1() + "\r\n";
        resp += "Content-Length: 0\r\nAudio-Latency: 0\r\n\r\n";
        m_event->write(frameOut(m_ec, resp));
        m_event->flush();
        castDebug(QStringLiteral("ap2: event %1 -> 200").arg(req.message));
    }
}

bool AirPlaySession::bindTiming()
{
    m_timing = new QUdpSocket(this);
    // PROBE HACK: ufw DROPs inbound UDP by default; 53317/udp is one of the
    // few allowed inbound ports. If the TV's NTP queries reach us here,
    // phase-2 may stop stalling (production needs a proper ufw rule).
    if (!m_timing->bind(QHostAddress::Any, 53317)
        && !m_timing->bind(QHostAddress::Any, 0)) {
        castDebug(QStringLiteral("ap2: timing bind failed"));
        m_timing->deleteLater();
        m_timing = nullptr;
        return false;
    }
    castDebug(QStringLiteral("ap2: timing on %1").arg(m_timing->localPort()));
    connect(m_timing, &QUdpSocket::readyRead, this,
            &AirPlaySession::onTimingData);
    return true;
}

void AirPlaySession::onTimingData()
{
    // Minimal NTP server reply (pyatv TimingServer equivalent for the
    // timingPort we announce in SETUP).
    if (!m_timing)
        return;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const quint64 unixSec = quint64(nowMs / 1000);
    const quint64 frac =
        (quint64(nowMs % 1000) * 0x100000000ULL) / 1000ULL;
    const quint64 ntp = ((unixSec + 2208988800ULL) << 32) | frac;
    while (m_timing->hasPendingDatagrams()) {
        QByteArray req;
        req.resize(int(m_timing->pendingDatagramSize()));
        QHostAddress addr;
        quint16 port = 0;
        m_timing->readDatagram(req.data(), req.size(), &addr, &port);
        if (req.size() < 48)
            continue;
        static int ntpCount = 0;
        if (ntpCount < 5)
            castDebug(QStringLiteral("ap2: NTP query #%1 from %2:%3")
                          .arg(++ntpCount).arg(addr.toString()).arg(port));
        QByteArray rep(48, 0);
        rep[0] = char(0x24); // LI=0, VN=4, Mode=4 (server)
        rep[1] = char(1);    // stratum
        rep[2] = char(10);   // poll
        rep[3] = char(-20);  // precision
        rep[12] = 'L';
        rep[13] = 'O';
        rep[14] = 'C';
        rep[15] = 'L';
        auto put64 = [&](int off, quint64 v) {
            for (int i = 0; i < 8; ++i)
                rep[off + i] = char((v >> (56 - 8 * i)) & 0xFF);
        };
        put64(16, ntp); // reference
        // originate = client's transmit timestamp
        for (int i = 0; i < 8; ++i)
            rep[24 + i] = req[40 + i];
        put64(32, ntp); // receive
        put64(40, ntp); // transmit
        m_timing->writeDatagram(rep, addr, port);
    }
}

void AirPlaySession::postFeedback()
{
    if (!m_playing || !m_ctrl
        || m_ctrl->state() != QAbstractSocket::ConnectedState)
        return;
    Msg m = rtspRaw(QStringLiteral("POST"), QStringLiteral("/feedback"), {},
                    {});
    if (!m.ok || (m.code != 200 && m.code != 204))
        castDebug(QStringLiteral("ap2: feedback code=%1 err=%2")
                      .arg(m.code).arg(m.error));
}

bool AirPlaySession::play(const QString &url, double position)
{
    closeAll();
    if (m_host.isEmpty() || !m_creds.isValid()) {
        Q_EMIT notice(tr("Hiányzó AirPlay eszköz vagy párosítás"), "err");
        return false;
    }
    castDebug(QStringLiteral("ap2> play %1 pos=%2").arg(url).arg(position));

    m_ctrl = new QTcpSocket(this);
    m_ctrl->connectToHost(m_host, m_port);
    if (!m_ctrl->waitForConnected(6000)) {
        Q_EMIT notice(tr("AirPlay eszköz nem elérhető"), "err");
        closeAll();
        return false;
    }
    m_localIp = m_ctrl->localAddress().toString();
    m_sessionId = QRandomGenerator::global()->generate();
    m_activeRemote = QRandomGenerator::global()->generate();
    m_dacp = QString::number(
        (quint64(QRandomGenerator::global()->generate()) << 32)
            | QRandomGenerator::global()->generate(),
        16).toUpper();
    m_cseq = 0;
    m_playUuid =
        QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();

    QByteArray shared;
    if (!verifyRaw(shared)) {
        Q_EMIT notice(tr("AirPlay verify sikertelen"), "err");
        closeAll();
        return false;
    }
    enableControl(shared);
    if (!bindTiming()) {
        Q_EMIT notice(tr("AirPlay időzítő-hiba"), "err");
        closeAll();
        return false;
    }
    const quint16 timingPort = m_timing->localPort();

    // RTSP SETUP (pyatv airplayv2._setup_base body).
    QVariantMap setup;
    setup.insert(QStringLiteral("deviceID"), QStringLiteral("AA:BB:CC:DD:EE:FF"));
    setup.insert(QStringLiteral("sessionUUID"),
                 QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper());
    setup.insert(QStringLiteral("timingPort"), int(timingPort));
    setup.insert(QStringLiteral("timingProtocol"), QStringLiteral("NTP"));
    setup.insert(QStringLiteral("isMultiSelectAirPlay"), true);
    setup.insert(QStringLiteral("groupContainsGroupLeader"), false);
    setup.insert(QStringLiteral("macAddress"),
                 QStringLiteral("AA:BB:CC:DD:EE:FF"));
    setup.insert(QStringLiteral("model"), QStringLiteral("iPhone14,3"));
    setup.insert(QStringLiteral("name"), QStringLiteral("omaplayer"));
    setup.insert(QStringLiteral("osBuildVersion"), QStringLiteral("20F66"));
    setup.insert(QStringLiteral("osName"), QStringLiteral("iPhone OS"));
    setup.insert(QStringLiteral("osVersion"), QStringLiteral("16.5"));
    setup.insert(QStringLiteral("senderSupportsRelay"), false);
    setup.insert(QStringLiteral("sourceVersion"), QStringLiteral("690.7.1"));
    setup.insert(QStringLiteral("statsCollectionEnabled"), false);
    Msg s = rtsp(QStringLiteral("SETUP"), {}, setup);
    int eventPort = 0;
    if (s.ok && (s.code == 200 || s.code == 204)) {
        castDebug(QStringLiteral("ap2: setup body %1")
                      .arg(QString::fromLatin1(s.body.toHex(' ')).left(600)));
        eventPort = Bplist::decode(s.body)
                        .toMap()
                        .value(QStringLiteral("eventPort"))
                        .toInt();
        castDebug(QStringLiteral("ap2: eventPort=%1").arg(eventPort));
    } else {
        Q_EMIT notice(tr("AirPlay SETUP elutasítva (%1)").arg(s.code), "err");
        closeAll();
        return false;
    }
    if (eventPort > 0)
        connectEvent(eventPort, shared); // best-effort; play may work anyway

    Msg rec = rtspRaw(QStringLiteral("RECORD"), {}, {}, {});
    if (!rec.ok || (rec.code != 200 && rec.code != 204))
        castDebug(QStringLiteral("ap2: RECORD code=%1 err=%2")
                      .arg(rec.code).arg(rec.error));

    // pyatv starts feedback before RECORD; keep one manual beat here so the
    // receiver sees the same order (SETUP → event → /feedback → RECORD).
    m_feedback->start();
    postFeedback();

    // POST /play with the pyatv airplayv2 body.
    QVariantMap pb;
    pb.insert(QStringLiteral("Content-Location"), url);
    pb.insert(QStringLiteral("Start-Position-Seconds"), position);
    pb.insert(QStringLiteral("uuid"), m_playUuid);
    pb.insert(QStringLiteral("streamType"), 1);
    pb.insert(QStringLiteral("mediaType"), QStringLiteral("file"));
    pb.insert(QStringLiteral("mightSupportStorePastisKeyRequests"), true);
    pb.insert(QStringLiteral("playbackRestrictions"), 0);
    pb.insert(QStringLiteral("secureConnectionMs"), 22);
    pb.insert(QStringLiteral("volume"), 1.0);
    pb.insert(QStringLiteral("infoMs"), 122);
    pb.insert(QStringLiteral("connectMs"), 18);
    pb.insert(QStringLiteral("authMs"), 0);
    pb.insert(QStringLiteral("bonjourMs"), 0);
    pb.insert(QStringLiteral("referenceRestrictions"), 3);
    pb.insert(QStringLiteral("SenderMACAddress"),
              QStringLiteral("AA:BB:CC:DD:EE:FF"));
    pb.insert(QStringLiteral("model"), QStringLiteral("iPhone14,3"));
    pb.insert(QStringLiteral("postAuthMs"), 0);
    pb.insert(QStringLiteral("clientBundleID"),
              QStringLiteral("dev.pyatv.GPU"));
    pb.insert(QStringLiteral("clientProcName"),
              QStringLiteral("dev.pyatv.GPU"));
    pb.insert(QStringLiteral("osBuildVersion"), QStringLiteral("20G1116"));
    pb.insert(QStringLiteral("rate"), 1.0);
    const QList<QPair<QString, QString>> playHeaders = {
        {QStringLiteral("X-Apple-ProtocolVersion"), QStringLiteral("1")},
        {QStringLiteral("X-Apple-Session-ID"), m_playUuid},
        {QStringLiteral("X-Apple-Stream-ID"), QStringLiteral("1")},
    };
    Msg pr = http(QStringLiteral("POST"), QStringLiteral("/play"), playHeaders,
                  Bplist::encode(pb),
                  QStringLiteral("application/x-apple-binary-plist"));
    int tries = 0;
    while (pr.ok && pr.code == 500 && tries < 3) {
        // pyatv "let's try again" on Internal Server Error.
        ++tries;
        pump(1000);
        pr = http(QStringLiteral("POST"), QStringLiteral("/play"), playHeaders,
                  Bplist::encode(pb),
                  QStringLiteral("application/x-apple-binary-plist"));
    }
    if (!pr.ok || (pr.code < 200 || pr.code >= 300)) {
        // NOTE (LG webOS): /play 404s here — same as pyatv issue #1518
        // (open since 2021). Verified+encrypted channel, SETUP/RECORD and
        // /info all work; only the video HTTP paths are missing server-side
        // (/fp-setup absent, /auth-setup 403, /command exists). Probes used
        // during diagnosis were removed; AP2DUMP=1 re-enables wire detail.
        Q_EMIT notice(tr("AirPlay /play elutasítva (%1)").arg(pr.code), "err");
        closeAll();
        return false;
    }

    QVariantMap t;
    t.insert(QStringLiteral("value"), true);
    rtsp(QStringLiteral("PUT"),
         QStringLiteral("/setProperty?isInterestedInDateRange"), t);
    QVariantMap z;
    z.insert(QStringLiteral("value"), 0);
    rtsp(QStringLiteral("PUT"), QStringLiteral("/setProperty?actionAtItemEnd"),
         z);
    rtspRaw(QStringLiteral("POST"), QStringLiteral("/rate?value=1.000000"),
            {}, {});
    QVariantMap endInner;
    endInner.insert(QStringLiteral("flags"), 0);
    endInner.insert(QStringLiteral("value"), 0);
    endInner.insert(QStringLiteral("epoch"), 0);
    endInner.insert(QStringLiteral("timescale"), 0);
    QVariantMap endWrap;
    endWrap.insert(QStringLiteral("value"), endInner);
    rtsp(QStringLiteral("PUT"), QStringLiteral("/setProperty?forwardEndTime"),
         endWrap);
    rtsp(QStringLiteral("PUT"), QStringLiteral("/setProperty?reverseEndTime"),
         endWrap);

    // (feedback already started before RECORD, pyatv order)
    // Confirm via /playback-info: duration present, no error dict.
    bool started = false;
    for (int i = 0; i < 12 && !started; ++i) {
        Msg pi = http(QStringLiteral("GET"),
                      QStringLiteral("/playback-info"), {}, {},
                      {});
        if (pi.ok && (pi.code == 200 || pi.code == 204) && !pi.body.isEmpty()) {
            const QVariantMap m = Bplist::decode(pi.body).toMap();
            if (m.contains(QStringLiteral("error"))) {
                Q_EMIT notice(tr("AirPlay lejátszási hiba"), "err");
                closeAll();
                return false;
            }
            if (m.contains(QStringLiteral("duration"))) {
                castDebug(QStringLiteral("ap2: playing, duration=%1")
                              .arg(m.value(QStringLiteral("duration"))
                                       .toDouble()));
                started = true;
            }
        }
        if (!started)
            pump(800);
    }
    if (!started) {
        Q_EMIT notice(tr("AirPlay: nincs visszajelzés a TV-ről"), "err");
        closeAll();
        return false;
    }
    m_playing = true;
    return true;
}

void AirPlaySession::setRate(double rate)
{
    if (!m_playing || !m_ctrl
        || m_ctrl->state() != QAbstractSocket::ConnectedState)
        return;
    Msg m = rtspRaw(QStringLiteral("POST"),
                    QStringLiteral("/rate?value=%1")
                        .arg(rate, 0, 'f', 6),
                    {}, {});
    if (!m.ok || (m.code != 200 && m.code != 204))
        castDebug(QStringLiteral("ap2: rate code=%1 err=%2")
                      .arg(m.code).arg(m.error));
}

void AirPlaySession::stop()
{
    // Non-blocking by design: safe to call straight from QML handlers.
    // TEARDOWN is best-effort, then everything closes.
    if (m_ctrl && m_ctrl->state() == QAbstractSocket::ConnectedState
        && m_cc.on) {
        QByteArray req =
            (QStringLiteral("TEARDOWN ") + rtspUri()
             + QStringLiteral(" RTSP/1.0\r\nUser-Agent: AirPlay/550.10\r\n"
                              "CSeq: %1\r\nDACP-ID: %2\r\nActive-Remote: %3\r\n"
                              "Client-Instance: %2\r\nSession: %4\r\n\r\n")
                 .arg(m_cseq++)
                 .arg(m_dacp)
                 .arg(m_activeRemote)
                 .arg(m_sessionId))
                .toLatin1();
        m_ctrl->write(frameOut(m_cc, req));
        m_ctrl->flush();
        m_ctrl->waitForBytesWritten(1500);
    }
    castDebug(QStringLiteral("ap2: stopped"));
    closeAll();
}

bool AirPlaySession::mirrorProbe()
{
    // Mirror-handshake probe (diagnostic; grows into file-push playback).
    // Captured from a real iPhone 16 (iOS 26.6, AirPlay/960.13.1) mirroring
    // to UxPlay: GET /info → fp-setup ×2 → SETUP#1 (mirror keys) → /info →
    // RECORD → SETUP#2 (video stream type 110). No RTP yet.
    closeAll();
    m_uaRtsp = QStringLiteral("AirPlay/960.13.1");
    if (m_host.isEmpty() || !m_creds.isValid()) {
        Q_EMIT notice(tr("Hiányzó AirPlay eszköz vagy párosítás"), "err");
        return false;
    }
    castDebug(QStringLiteral("ap2> mirrorProbe %1:%2").arg(m_host).arg(m_port));
    m_ctrl = new QTcpSocket(this);
    m_ctrl->connectToHost(m_host, m_port);
    if (!m_ctrl->waitForConnected(6000)) {
        Q_EMIT notice(tr("AirPlay eszköz nem elérhető"), "err");
        closeAll();
        return false;
    }
    m_localIp = m_ctrl->localAddress().toString();
    m_sessionId = QRandomGenerator::global()->generate();
    m_activeRemote = QRandomGenerator::global()->generate();
    m_dacp = QString::number(
        (quint64(QRandomGenerator::global()->generate()) << 32)
            | QRandomGenerator::global()->generate(),
        16).toUpper();
    m_cseq = 0;

    QByteArray shared;
    if (!verifyRaw(shared)) {
        Q_EMIT notice(tr("AirPlay verify sikertelen"), "err");
        closeAll();
        return false;
    }
    enableControl(shared);
    if (!bindTiming()) {
        Q_EMIT notice(tr("AirPlay időzítő-hiba"), "err");
        closeAll();
        return false;
    }
    const quint16 timingPort = m_timing->localPort();

    // GET /info with qualifier (iPhone order).
    QVariantMap q;
    QVariantList ql;
    ql.append(QStringLiteral("txtAirPlay"));
    q.insert(QStringLiteral("qualifier"), ql);
    rtspRaw(QStringLiteral("GET"), QStringLiteral("/info"), Bplist::encode(q),
            QStringLiteral("application/x-apple-binary-plist"),
            {{QStringLiteral("X-Apple-ProtocolVersion"),
              QStringLiteral("1")}});

    // fp-setup M1/M3 with captured iPhone bytes (M3 replay likely fails if
    // bound to the original session — the code tells us).
    const QByteArray fpM1 = QByteArray::fromHex(
        "46504c590301010000000004020002bb");
    const QByteArray fpM3 = QByteArray::fromHex(
        "46504c590301030000000098028f1a9c496f3a0a61826e27471506e07eb1779e56b47"
        "467752cce3f2432d20b0d437898bf05f6192e62bc1071fd7ea7006285fd18071b6c87"
        "bc4d35bb55f0cb9e444bf2468824f5c688054de202f0746ef9ae664a365cd1256ce1"
        "c76bb4a96e8c57a59247a8d43a906113cf09e51fa4e3e1aa5f56f9d69475e7b938e4"
        "a90847e76faa33463881006169023cfb2355487fc1362758dca56e");
    const QList<QPair<QString, QString>> fpH = {
        {QStringLiteral("X-Apple-ET"), QStringLiteral("32")}};
    Msg f1 = rtspRaw(QStringLiteral("POST"), QStringLiteral("/fp-setup"),
                     fpM1, QStringLiteral("application/octet-stream"), fpH);
    Msg f3 = rtspRaw(QStringLiteral("POST"), QStringLiteral("/fp-setup"),
                     fpM3, QStringLiteral("application/octet-stream"), fpH);
    castDebug(QStringLiteral("ap2: fp-setup M1->%1 M3->%2")
                  .arg(f1.code).arg(f3.code));

    // Mirror SETUP#1 WITHOUT ekey/eiv first (tests whether LG demands FP).
    const QString sessUuid =
        QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
    const QString corrUuid =
        QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
    QVariantMap setup;
    setup.insert(QStringLiteral("osVersion"), QStringLiteral("26.6"));
    setup.insert(QStringLiteral("internalBuild"), false);
    // Replayed iPhone ekey/eiv (valid FP envelope; decrypts to audio key
    // ff87de... on ANY Apple-FP stack — including LG's licensed one).
    // If LG needs keys, this unblocks phase-2 without any FP crypto.
    setup.insert(QStringLiteral("et"), 32);
    setup.insert(QStringLiteral("ekey"),
                 QByteArray::fromHex(
                     "46504c59010201000000003c000000001cbcb66249ddd66f69fcef85"
                     "b504693f000000106cfea909aec2bb76ffc5d595c4b3a95322bc8665"
                     "de4bec09ad96e01f0513583054cc102e"));
    setup.insert(QStringLiteral("eiv"),
                 QByteArray::fromHex("5c764c819eda4028afc1af38c480a632"));
    setup.insert(QStringLiteral("name"), QStringLiteral("omaplayer"));
    setup.insert(QStringLiteral("sessionCorrelationUUID"), corrUuid);
    setup.insert(QStringLiteral("diagnosticsAndUsage"), true);
    setup.insert(QStringLiteral("timingProtocol"), QStringLiteral("NTP"));
    setup.insert(QStringLiteral("deviceID"),
                 QStringLiteral("AA:BB:CC:DD:EE:FF"));
    setup.insert(QStringLiteral("timingPort"), int(timingPort));
    setup.insert(QStringLiteral("sourceVersion"), QStringLiteral("960.13.1"));
    setup.insert(QStringLiteral("isScreenMirroringSession"), true);
    setup.insert(QStringLiteral("model"), QStringLiteral("iPhone16,2"));
    setup.insert(QStringLiteral("statsCollectionEnabled"), false);
    setup.insert(QStringLiteral("macAddress"),
                 QStringLiteral("AA:BB:CC:DD:EE:FF"));
    setup.insert(QStringLiteral("sessionUUID"), sessUuid);
    setup.insert(QStringLiteral("osBuildVersion"), QStringLiteral("23G71"));
    setup.insert(QStringLiteral("updateSessionRequest"), false);
    setup.insert(QStringLiteral("osName"), QStringLiteral("iPhone OS"));
    Msg s1 = rtsp(QStringLiteral("SETUP"), {}, setup);
    int eventPort = 0, tvTimingPort = 0;
    if (s1.ok && (s1.code == 200 || s1.code == 204)) {
        const QVariantMap m = Bplist::decode(s1.body).toMap();
        eventPort = m.value(QStringLiteral("eventPort")).toInt();
        tvTimingPort = m.value(QStringLiteral("timingPort")).toInt();
    }
    castDebug(QStringLiteral("ap2: mirror SETUP#1 code=%1 eventPort=%2 tvTiming=%3")
                  .arg(s1.code).arg(eventPort).arg(tvTimingPort));
    // iPhone sends a bare GET /info here (CSeq 4) before RECORD — the TV's
    // state machine seems to expect it.
    rtspRaw(QStringLiteral("GET"), QStringLiteral("/info"), {}, {}, {});
    // iPhone→UxPlay got eventPort=0 (no event channel for mirror); LG
    // returns nonzero and RECORD needs it here — keep connected.
    if (eventPort > 0)
        connectEvent(eventPort, shared); // best-effort; RECORD needs it

    Msg rec = rtspRaw(QStringLiteral("RECORD"), {}, {}, {});
    if ((!rec.ok || rec.code == 0) && m_ctrl
        && m_ctrl->state() == QAbstractSocket::ConnectedState) {
        // One retry: mirror init can be slow on the TV side.
        pump(1500);
        rec = rtspRaw(QStringLiteral("RECORD"), {}, {}, {});
    }
    castDebug(QStringLiteral("ap2: mirror RECORD code=%1").arg(rec.code));

    // SETUP#2: video stream type 110. Bisect: minimal first (type only),
    // then full — the TV hangs on the full body, find what it wants.
    auto phase2 = [this](const QVariantMap &streamDict, int timeoutMs) {
        QVariantMap b;
        QVariantList sl;
        sl.append(streamDict);
        b.insert(QStringLiteral("streams"), sl);
        return rtsp(QStringLiteral("SETUP"), {}, b, {}, timeoutMs);
    };
    QVariantMap minStream;
    minStream.insert(QStringLiteral("type"), 110);
    Msg s2a = phase2(minStream, 10000);
    castDebug(QStringLiteral("ap2: mirror SETUP#2a(min) code=%1 err=%2")
                  .arg(s2a.code).arg(s2a.error));
    // Exact captured iPhone value (rules out ID-shape issues).
    const qlonglong streamConnId = qlonglong(-2461909249595846697LL);
    QVariantMap stream;
    QVariantList tsNames;
    for (const char *n : {"SubSu", "BePxT", "AfPxT", "BefEn", "EmEnc"}) {
        QVariantMap e;
        e.insert(QStringLiteral("name"), QString::fromLatin1(n));
        tsNames.append(e);
    }
    stream.insert(QStringLiteral("timestampInfo"), tsNames);
    stream.insert(QStringLiteral("latencyMs"), 100);
    stream.insert(QStringLiteral("type"), 110);
    stream.insert(QStringLiteral("streamConnectionID"), streamConnId);
    Msg s2 = phase2(stream, 10000);
    if (!s2.ok && s2.code == 0) {
        // Diagnostic: did ANYTHING arrive (partial response vs silence)?
        castDebug(QStringLiteral("ap2: phase2 stall, plainbuf=%1 feedbuf=%2")
                      .arg(QString::fromLatin1(m_cc.plain.left(200).toHex(' ')))
                      .arg(m_cc.feed.size()));
        // The TV may need a while (video pipeline, HDCP, mode switch):
        // one patient retry with a long timeout.
        castDebug(QStringLiteral("ap2: mirror SETUP#2 retry, 40s budget"));
        s2 = phase2(stream, 40000);
    }
    int dataPort = 0;
    if (s2.ok && (s2.code == 200 || s2.code == 204)) {
        const QVariantMap m = Bplist::decode(s2.body).toMap();
        const QVariantList sl = m.value(QStringLiteral("streams")).toList();
        if (!sl.isEmpty())
            dataPort = sl.first().toMap().value(QStringLiteral("dataPort"))
                           .toInt();
    }
    castDebug(QStringLiteral("ap2: mirror SETUP#2 code=%1 dataPort=%2")
                  .arg(s2.code).arg(dataPort));

    // Audio phase-2 on the SAME session (ALAC): if the TV's video pipeline
    // waits for an audio clock, this may unblock video afterwards.
    QUdpSocket *rtcp = new QUdpSocket(this);
    rtcp->bind(QHostAddress::Any, 0);
    QVariantMap astream;
    astream.insert(QStringLiteral("type"), 96);
    astream.insert(QStringLiteral("ct"), 2);
    astream.insert(QStringLiteral("spf"), 352);
    astream.insert(QStringLiteral("sr"), 44100);
    astream.insert(QStringLiteral("audioFormat"), 0x40000);
    astream.insert(QStringLiteral("audioMode"), QStringLiteral("default"));
    astream.insert(QStringLiteral("latencyMin"), 11025);
    astream.insert(QStringLiteral("latencyMax"), 88200);
    astream.insert(QStringLiteral("isMedia"), true);
    astream.insert(QStringLiteral("supportsDynamicStreamID"), false);
    astream.insert(QStringLiteral("streamConnectionID"),
                   qlonglong(m_sessionId));
    astream.insert(QStringLiteral("controlPort"), int(rtcp->localPort()));
    QVariantMap abody;
    QVariantList asl;
    asl.append(astream);
    abody.insert(QStringLiteral("streams"), asl);
    Msg sa = rtsp(QStringLiteral("SETUP"), {}, abody, {}, 20000);
    castDebug(QStringLiteral("ap2: mirror audio SETUP code=%1 %2")
                  .arg(sa.code)
                  .arg(sa.ok ? shortBody(sa.body) : sa.error));
    // And video phase-2 once more after audio.
    Msg s3 = phase2(stream, 20000);
    int dataPort2 = 0;
    if (s3.ok && (s3.code == 200 || s3.code == 204)) {
        const QVariantMap m = Bplist::decode(s3.body).toMap();
        const QVariantList sl = m.value(QStringLiteral("streams")).toList();
        if (!sl.isEmpty())
            dataPort2 = sl.first().toMap().value(QStringLiteral("dataPort"))
                            .toInt();
    }
    castDebug(QStringLiteral("ap2: mirror SETUP#3(video) code=%1 dataPort=%2")
                  .arg(s3.code).arg(dataPort2));
    rtcp->deleteLater();

    Q_EMIT notice(tr("Mirror-próba: fp %1/%2, setup %3, record %4, video %5")
                      .arg(f1.code)
                      .arg(f3.code)
                      .arg(s1.code)
                      .arg(rec.code)
                      .arg(s2.code),
                  "info");
    // Always release the session: abandoned pipelines (black screen!) may
    // block later phase-2 allocations on single-pipeline TVs.
    stop();
    return true;
}

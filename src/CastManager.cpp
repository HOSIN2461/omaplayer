#include "CastManager.h"

#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>
#include <QXmlStreamReader>

#include <memory>

namespace {

constexpr int kSsdpPort = 1900;
constexpr int kDiscoveryMs = 2500;
const QByteArray kSearchTarget = "urn:schemas-upnp-org:device:MediaRenderer:1";
const QByteArray kAvNs = "urn:schemas-upnp-org:service:AVTransport:1";

constexpr qint64 kChunkSize = 1 << 20; // 1 MiB per write while streaming

QString utf8(const QByteArray &b)
{
    return QString::fromUtf8(b);
}

QString mimeFor(const QString &path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("mp4") || ext == QLatin1String("m4v"))
        return QStringLiteral("video/mp4");
    if (ext == QLatin1String("mkv"))
        return QStringLiteral("video/x-matroska");
    if (ext == QLatin1String("webm"))
        return QStringLiteral("video/webm");
    if (ext == QLatin1String("avi"))
        return QStringLiteral("video/x-msvideo");
    if (ext == QLatin1String("mpg") || ext == QLatin1String("mpeg"))
        return QStringLiteral("video/mpeg");
    if (ext == QLatin1String("mov"))
        return QStringLiteral("video/quicktime");
    if (ext == QLatin1String("ts") || ext == QLatin1String("mts")
        || ext == QLatin1String("m2ts"))
        return QStringLiteral("video/mp2t");
    return QStringLiteral("application/octet-stream");
}

QString durationText(double seconds)
{
    const int total = qMax(0, int(seconds));
    return QStringLiteral("%1:%2:%3")
        .arg(total / 3600, 2, 10, QLatin1Char('0'))
        .arg((total % 3600) / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

QString didlFor(const QString &title)
{
    // Minimal DIDL-Lite so the renderer can show the file name.
    return QStringLiteral(
        "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<item><dc:title>%1</dc:title></item></DIDL-Lite>").arg(
        title.toHtmlEscaped().toUtf8().toPercentEncoding());
}

} // namespace

namespace {

// --- mDNS wire helpers ----------------------------------------------------

quint16 dnsU16(const QByteArray &pkt, int off)
{
    return (quint16(quint8(pkt.at(off))) << 8)
        | quint16(quint8(pkt.at(off + 1)));
}

void dnsPutU16(QByteArray &pkt, quint16 v)
{
    pkt.append(char(v >> 8));
    pkt.append(char(v & 0xFF));
}

// Decodes a (possibly compressed) DNS name; returns the byte offset right
// after it, or -1 on error. Compression pointers are followed with the
// proviso that the outer offset is the first pointer's end.
int dnsDecodeName(const QByteArray &pkt, int off, QString *name)
{
    name->clear();
    QByteArray raw;
    int pos = off;
    int outerEnd = -1;
    for (int guard = 0; guard < 128 && pos < pkt.size(); ++guard) {
        const quint8 c = quint8(pkt.at(pos));
        if (c == 0) {
            if (outerEnd < 0)
                outerEnd = pos + 1;
            break;
        }
        if ((c & 0xC0) == 0xC0) {
            if (pos + 1 >= pkt.size())
                return -1;
            const int target = ((int(c) & 0x3F) << 8) | quint8(pkt.at(pos + 1));
            if (outerEnd < 0)
                outerEnd = pos + 2;
            pos = target;
            continue;
        }
        if (pos + 1 + c > pkt.size())
            return -1;
        if (!raw.isEmpty())
            raw.append('.');
        raw.append(pkt.mid(pos + 1, c));
        pos += 1 + c;
    }
    *name = QString::fromLatin1(raw);
    return outerEnd < 0 ? pos : outerEnd;
}

QByteArray dnsEncodeName(const QString &name)
{
    QByteArray out;
    const QList<QByteArray> labels = name.toLatin1().split('.');
    for (const QByteArray &label : labels) {
        if (label.isEmpty())
            continue;
        out.append(char(label.size()));
        out.append(label);
    }
    out.append(char(0));
    return out;
}

// The network interface that owns `ip` (used with QUdpSocket multicast fns).
QNetworkInterface ifaceForIp(const QHostAddress &ip)
{
    const QList<QNetworkInterface> ifaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : ifaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp)
            || (iface.flags() & QNetworkInterface::IsLoopBack))
            continue;
        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            if (entry.ip() == ip)
                return iface;
        }
    }
    return QNetworkInterface();
}

} // namespace

CastManager::CastManager(QObject *parent)
    : QObject(parent)
    , m_ssdp(new QUdpSocket(this))
    , m_mdns(new QUdpSocket(this))
{
    m_discoverTimer = new QTimer(this);
    m_discoverTimer->setSingleShot(true);
    m_discoverTimer->setInterval(kDiscoveryMs);
    connect(m_discoverTimer, &QTimer::timeout, this, [this] {
        m_discovering = false;
        flushPending();
        // Late mDNS SRV/TXT/A answers may still trickle in after the burst;
        // drain once more shortly so those reach fetchDescription() too.
        m_drainTimer->start();
    });

    m_drainTimer = new QTimer(this);
    m_drainTimer->setSingleShot(true);
    m_drainTimer->setInterval(1500);
    connect(m_drainTimer, &QTimer::timeout, this, [this] {
        flushPending();
    });

    connect(m_ssdp, &QUdpSocket::readyRead,
            this, &CastManager::handleSsdp);
    connect(m_mdns, &QUdpSocket::readyRead,
            this, &CastManager::handleMdns);
}

QString CastManager::activeDeviceName() const
{
    if (m_activeDevice < 0 || m_activeDevice >= m_devices.size())
        return {};
    return m_devices.at(m_activeDevice).toMap()
        .value(QStringLiteral("name")).toString();
}

void CastManager::startDiscovery()
{
    m_devices.clear();
    m_pending.clear();
    m_pendingDesc.clear();
    m_mdnsInstances.clear();
    m_mdnsInfo.clear();
    m_mdnsHosts.clear();
    m_mdnsProbed.clear();
    m_discovering = true;
    if (m_ssdp->state() != QAbstractSocket::BoundState) {
        m_ssdp->bind(QHostAddress::AnyIPv4, 0);
        const QNetworkInterface iface = ifaceForIp(QHostAddress(localIp()));
        if (iface.isValid())
            m_ssdp->setMulticastInterface(iface);
        m_ssdp->joinMulticastGroup(QHostAddress(QStringLiteral("239.255.255.250")));
        m_ssdp->joinMulticastGroup(QHostAddress(QStringLiteral("239.255.255.251")));
    }
    Q_EMIT devicesChanged();

    const QByteArray msearch =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: " + kSearchTarget + "\r\n"
        "\r\n";
    m_ssdp->writeDatagram(msearch,
                          QHostAddress(QStringLiteral("239.255.255.250")),
                          kSsdpPort);
    m_ssdp->writeDatagram(msearch,
                          QHostAddress(QStringLiteral("239.255.255.251")),
                          kSsdpPort);
    startMdns();
    m_discoverTimer->start();
}

void CastManager::stopDiscovery()
{
    m_discoverTimer->stop();
    m_drainTimer->stop();
}

void CastManager::handleSsdp()
{
    while (m_ssdp->hasPendingDatagrams()) {
        QByteArray buf;
        buf.resize(int(m_ssdp->pendingDatagramSize()));
        m_ssdp->readDatagram(buf.data(), buf.size());
        // Parse the HTTP/1.1 reply: extract LOCATION (and, for good measure,
        // remember ST so rootdevice answers can seed mDNS fallback).
        QString location, st;
        const QList<QByteArray> lines = buf.split('\n');
        for (const QByteArray &line : lines) {
            const int colon = line.indexOf(':');
            if (colon <= 0)
                continue;
            const QByteArray key = line.left(colon).trimmed().toLower();
            const QString value = utf8(line.mid(colon + 1).trimmed());
            if (key == "location" && location.isEmpty())
                location = value;
            else if (key == "st" && st.isEmpty())
                st = value;
        }
        if (!location.isEmpty())
            appendLocation(location);
    }
}

void CastManager::startMdns()
{
    // Ask DNS-SD for UPnP media renderers. Devices that never answer SSDP
    // (many TVs) still publish `_mediarender._tcp` via multicast DNS.
    const auto bindMdns = [this] {
        // Port 5353 is shared (avahi, browsers...): SO_REUSEADDR/SO_REUSEPORT
        // lets us receive the multicast answers without stealing the socket.
        if (!m_mdns->bind(QHostAddress::AnyIPv4, 5353,
                          QAbstractSocket::ShareAddress
                              | QAbstractSocket::ReuseAddressHint)) {
            qWarning() << "cast: mDNS bind failed"
                       << m_mdns->errorString();
            return false;
        }
        const QNetworkInterface iface = ifaceForIp(QHostAddress(localIp()));
        if (iface.isValid())
            m_mdns->setMulticastInterface(iface);
        const bool joined = m_mdns->joinMulticastGroup(
            QHostAddress(QStringLiteral("224.0.0.251")));
        if (!joined)
            qWarning() << "cast: mDNS join failed" << m_mdns->errorString();
        return true;
    };
    if (m_mdns->state() != QAbstractSocket::BoundState && !bindMdns())
        return;
    sendMdnsQuery(12, QStringLiteral("_mediarender._tcp.local"));  // PTR
    sendMdnsQuery(33, QStringLiteral("_mediarender._tcp.local"));  // SRV
    sendMdnsQuery(16, QStringLiteral("_mediarender._tcp.local"));  // TXT
}

void CastManager::sendMdnsQuery(int type, const QString &name)
{
    QByteArray pkt;
    dnsPutU16(pkt, m_mdnsQueryId++);
    dnsPutU16(pkt, 0x0000);            // standard query (QR=0)
    dnsPutU16(pkt, 1);                 // QDCOUNT
    dnsPutU16(pkt, 0);                 // ANCOUNT
    dnsPutU16(pkt, 0);                 // NSCOUNT
    dnsPutU16(pkt, 0);                 // ARCOUNT
    pkt += dnsEncodeName(name);
    dnsPutU16(pkt, quint16(type));     // PTR/SRV/TXT
    dnsPutU16(pkt, 0x8001);            // QU bit + IN class: ask for a unicast
                                       // reply straight back to our socket
    m_mdns->writeDatagram(pkt,
                          QHostAddress(QStringLiteral("224.0.0.251")), 5353);
}

void CastManager::handleMdns()
{
    while (m_mdns->hasPendingDatagrams()) {
        QByteArray buf;
        buf.resize(int(m_mdns->pendingDatagramSize()));
        m_mdns->readDatagram(buf.data(), buf.size());
        if (buf.size() < 12)
            continue;
        const quint16 qd = dnsU16(buf, 4);
        const quint16 an = dnsU16(buf, 6);
        const int total = int(an) + int(dnsU16(buf, 10)); // answers + additional
        // Skip the question section.
        int off = 12;
        for (int i = 0; i < qd; ++i) {
            QString ignored;
            const int next = dnsDecodeName(buf, off, &ignored);
            if (next < 0)
                break;
            off = next + 4; // QTYPE + QCLASS
        }
        // Walk answer/additional records (A records for SRV targets often
        // live in the additional section).
        for (int i = 0; i < total; ++i) {
            QString owner;
            int next = dnsDecodeName(buf, off, &owner);
            if (next < 0)
                break;
            const int rtype = dnsU16(buf, next);
            const int rclass = dnsU16(buf, next + 2);
            const int ttl = int(dnsU16(buf, next + 4) << 16)
                          | int(dnsU16(buf, next + 6));
            (void)rclass; (void)ttl;
            const int rdlen = dnsU16(buf, next + 8);
            int rdata = next + 10;
            if (rdata + rdlen > buf.size())
                break;
            if (rtype == 12 && owner.endsWith("_mediarender._tcp.local")) {
                // PTR answer: owner=service, rdata=instance name.
                QString instance;
                if (dnsDecodeName(buf, rdata, &instance) >= 0
                    && !m_mdnsInstances.contains(instance)) {
                    m_mdnsInstances.append(instance);
                    sendMdnsQuery(33, instance); // SRV
                    sendMdnsQuery(16, instance); // TXT
                }
            } else if (rtype == 33) {
                // SRV: priority(2) weight(2) port(2) target(name).
                const int port = dnsU16(buf, rdata + 4);
                QString target;
                dnsDecodeName(buf, rdata + 6, &target);
                QVariantMap info = m_mdnsInfo.value(owner);
                info[QStringLiteral("port")] = port;
                info[QStringLiteral("target")] = target;
                m_mdnsInfo.insert(owner, info);
                // The SRV target still needs an A/AAAA record to be usable.
                const QHostAddress addr(target);
                if (target.isEmpty() || addr.isNull())
                    sendMdnsQuery(1, target);   // A
            } else if (rtype == 1 && rdlen == 4) {
                // A record: host -> ip.
                const QString ip = QStringLiteral("%1.%2.%3.%4")
                    .arg(quint8(buf.at(rdata)))
                    .arg(quint8(buf.at(rdata + 1)))
                    .arg(quint8(buf.at(rdata + 2)))
                    .arg(quint8(buf.at(rdata + 3)));
                m_mdnsHosts.insert(owner, ip);
            } else if (rtype == 16) {
                // TXT: path=<...> often carries the device description URL.
                int p = rdata;
                const int end = rdata + rdlen;
                while (p < end) {
                    const int len = quint8(buf.at(p));
                    if (p + 1 + len > end)
                        break;
                    const QByteArray kv = buf.mid(p + 1, len);
                    p += 1 + len;
                    const int eq = kv.indexOf('=');
                    if (eq > 0 && kv.left(eq).toLower() == "path") {
                        QVariantMap info = m_mdnsInfo.value(owner);
                        info[QStringLiteral("path")] = utf8(kv.mid(eq + 1));
                        m_mdnsInfo.insert(owner, info);
                    }
                }
            }
            off = rdata + rdlen;
        }
        // Resolve any instances whose SRV/TXT just arrived.
        for (const QString &instance : std::as_const(m_mdnsInstances))
            mdnsTryResolve(instance);
    }
}

void CastManager::mdnsTryResolve(const QString &instance)
{
    if (m_mdnsProbed.contains(instance))
        return;
    const QVariantMap info = m_mdnsInfo.value(instance);
    const int port = info.value(QStringLiteral("port")).toInt();
    const QString target = info.value(QStringLiteral("target")).toString();
    if (port <= 0 || target.isEmpty())
        return;
    const QString ip = m_mdnsHosts.value(target);
    if (ip.isEmpty())
        return;
    m_mdnsProbed.append(instance);
    const QString path = info.value(QStringLiteral("path")).toString();
    if (!path.isEmpty()) {
        QString loc = path;
        if (!loc.startsWith(QLatin1Char('/')))
            loc.prepend(QLatin1Char('/'));
        appendLocation(QStringLiteral("http://%1:%2%3")
                           .arg(ip).arg(port).arg(loc));
    } else {
        // No TXT path: ask the box directly for its description via unicast
        // SSDP; handleSsdp picks the LOCATION up the same way as multicast.
        const QByteArray msearch =
            "M-SEARCH * HTTP/1.1\r\n"
            "HOST: " + ip.toUtf8() + ":1900\r\n"
            "MAN: \"ssdp:discover\"\r\n"
            "MX: 2\r\n"
            "ST: " + kSearchTarget + "\r\n"
            "\r\n";
        m_ssdp->writeDatagram(msearch, QHostAddress(ip), kSsdpPort);
    }
}

QString CastManager::mdnsLocation(const QString &instance) const
{
    const QVariantMap info = m_mdnsInfo.value(instance);
    const int port = info.value(QStringLiteral("port")).toInt();
    const QString target = info.value(QStringLiteral("target")).toString();
    const QString ip = m_mdnsHosts.value(target);
    if (port <= 0 || ip.isEmpty())
        return {};
    QString path = info.value(QStringLiteral("path")).toString();
    if (path.isEmpty())
        path = QStringLiteral("/");
    if (!path.startsWith(QLatin1Char('/')))
        path.prepend(QLatin1Char('/'));
    return QStringLiteral("http://%1:%2%3").arg(ip).arg(port).arg(path);
}

void CastManager::appendLocation(const QString &location)
{
    if (location.isEmpty() || m_pending.contains(location)
        || m_pendingDesc.contains(location))
        return;
    m_pending.append(location);
}

void CastManager::flushPending()
{
    const QStringList pending = m_pending;
    m_pending.clear();
    for (const QString &location : pending)
        fetchDescription(location);
    if (m_pendingDesc.isEmpty())
        Q_EMIT devicesChanged();
}

void CastManager::fetchDescription(const QString &location)
{
    // A MediaRenderer may answer with a bare "upnp:rootdevice" on the first
    // probe; the LOCATION we grabbed is already filtered to AV-capable ones
    // by onSsdpTimer logic below. Fetch the SCDP to find AVTransport.
    auto *nam = new QNetworkAccessManager(this);
    QNetworkRequest req{QUrl(location)};
    req.setTransferTimeout(6000);
    QNetworkReply *reply = nam->get(req);
    m_pendingDesc.append(location);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, nam, location] {
        reply->deleteLater();
        nam->deleteLater();
        QString controlUrl, name;
        if (reply->error() == QNetworkReply::NoError) {
            QXmlStreamReader xml(reply->readAll());
            while (!xml.atEnd() && !xml.hasError()) {
                xml.readNext();
                if (xml.isStartElement()
                    && xml.name() == QLatin1String("controlURL")
                    && controlUrl.isEmpty())
                    controlUrl = xml.readElementText().simplified();
                else if (xml.isStartElement()
                         && xml.name() == QLatin1String("friendlyName")
                         && name.isEmpty())
                    name = xml.readElementText().simplified();
            }
        }
        const QUrl base(location);
        if (!controlUrl.isEmpty()) {
            if (!controlUrl.startsWith(QLatin1String("http"))) {
                QUrl rel(base);
                rel.setPath(controlUrl.startsWith(QLatin1Char('/'))
                                ? controlUrl
                                : QLatin1Char('/') + controlUrl);
                controlUrl = rel.toString();
            }
            QVariantMap dev;
            dev[QStringLiteral("name")] = name.isEmpty() ? base.host() : name;
            dev[QStringLiteral("host")] = base.host();
            dev[QStringLiteral("port")] = base.port(80);
            dev[QStringLiteral("controlUrl")] = controlUrl;
            // SSDP and mDNS may both report the same renderer; keep one.
            bool dupe = false;
            for (const QVariant &existing : m_devices) {
                if (existing.toMap()
                        .value(QStringLiteral("controlUrl")).toString()
                    == controlUrl) {
                    dupe = true;
                    break;
                }
            }
            if (!dupe)
                m_devices.append(dev);
        }
        m_pendingDesc.removeAll(location);
        if (m_pendingDesc.isEmpty())
            Q_EMIT devicesChanged();
    });
}

void CastManager::soap(int deviceIndex, const QString &action,
                       double seekTarget,
                       const std::function<void(bool)> &done)
{
    if (deviceIndex < 0 || deviceIndex >= m_devices.size()) {
        if (done)
            done(false);
        return;
    }
    const QString controlUrl = m_devices.at(deviceIndex).toMap()
                                   .value(QStringLiteral("controlUrl")).toString();
    if (controlUrl.isEmpty()) {
        if (done)
            done(false);
        return;
    }

    QString body;
    if (action == QLatin1String("Seek")) {
        body = QStringLiteral(
            "<?xml version=\"1.0\"?>\n"
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
            "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
            "<s:Body><u:Seek xmlns:u=\"%1\"><InstanceID>0</InstanceID>"
            "<Unit>REL_TIME</Unit><Target>%2</Target></u:Seek></s:Body></s:Envelope>")
            .arg(utf8(kAvNs), durationText(seekTarget));
    } else {
        QString extra;
        if (action == QLatin1String("SetAVTransportURI")) {
            extra = QStringLiteral("<CurrentURI>%1</CurrentURI>")
                        .arg(m_castUrl.toHtmlEscaped())
                + QStringLiteral("<CurrentURIMetaData>%1</CurrentURIMetaData>")
                      .arg(didlFor(QFileInfo(m_castPath).fileName()));
            // CastURLs with '&' inside .toHtmlEscaped() may confuse picky
            // renderers; percent-encode instead.
            extra = QStringLiteral("<CurrentURI>%1</CurrentURI>")
                        .arg(QString::fromUtf8(
                            m_castUrl.toUtf8().toPercentEncoding("/;?:@&=+$,")))
                + QStringLiteral("<CurrentURIMetaData>%1</CurrentURIMetaData>")
                      .arg(didlFor(QFileInfo(m_castPath).fileName()));
        }
        body = QStringLiteral(
            "<?xml version=\"1.0\"?>\n"
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
            "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
            "<s:Body><u:%1 xmlns:u=\"%2\"><InstanceID>0</InstanceID>%3</u:%1>"
            "</s:Body></s:Envelope>").arg(action, utf8(kAvNs), extra);
    }

    auto *nam = new QNetworkAccessManager(this);
    QNetworkRequest req{QUrl(controlUrl)};
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("text/xml; charset=\"utf-8\""));
    req.setRawHeader("SOAPACTION",
                     QByteArray("\"") + kAvNs + "#" + action.toUtf8() + "\"");
    req.setTransferTimeout(6000);
    QNetworkReply *reply = nam->post(req, body.toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam, done] {
        reply->deleteLater();
        nam->deleteLater();
        const bool ok = reply->error() == QNetworkReply::NoError
            || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
        if (!ok)
            qWarning() << "cast: SOAP failed" << reply->errorString();
        if (done)
            done(ok);
    });
}

bool CastManager::cast(int deviceIndex, const QString &filePath, double position)
{
    if (deviceIndex < 0 || deviceIndex >= m_devices.size()) {
        Q_EMIT notice(tr("Először keress DLNA renderert"), "err");
        return false;
    }
    if (filePath.isEmpty() || !QFileInfo(filePath).isFile()) {
        Q_EMIT notice(tr("Csak helyi fájlt lehet kivetíteni"), "err");
        return false;
    }
    if (!m_server && !(m_server = new QTcpServer(this)))
        return false;
    if (!m_server->isListening() && !m_server->listen(QHostAddress::AnyIPv4, 0)) {
        Q_EMIT notice(tr("Cast kiszolgáló indítása nem sikerült"), "err");
        return false;
    }
    if (m_server->isListening() && !m_serverRunning) {
        m_serverRunning = true;
        Q_EMIT serverStateChanged();
    }
    connect(m_server, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket *client = m_server->nextPendingConnection()) {
            connect(client, &QTcpSocket::readyRead, this, [this, client] {
                if (client->bytesAvailable() < 16)
                    return;
                const QByteArray req = client->readAll();
                if (!req.startsWith("GET"))
                    return;
                // "GET /<enc> HTTP/1.1" + optional Range: bytes=start-
                const int sp = req.indexOf(' ');
                const int sp2 = req.indexOf(' ', sp + 1);
                if (sp < 0 || sp2 < 0)
                    return;
                const QString path = QUrl::fromPercentEncoding(
                    req.mid(sp + 1, sp2 - sp - 1));
                qint64 start = 0;
                bool hasRange = false;
                const QByteArray lower = req.toLower();
                const int ri = lower.indexOf("range: bytes=");
                if (ri >= 0) {
                    const int e = lower.indexOf("\r\n", ri);
                    const QByteArray spec = req.mid(ri + 13, e - (ri + 13)).trimmed();
                    const int dash = spec.indexOf('-');
                    if (dash > 0) {
                        start = spec.left(dash).trimmed().toLongLong(&hasRange);
                        hasRange = hasRange && start >= 0;
                    }
                }
                serveFile(client, path, start, hasRange);
            });
            connect(client, &QTcpSocket::disconnected, this,
                    [this, client] { m_connections.removeAll(client); });
        }
    });
    m_castPath = QFileInfo(filePath).canonicalFilePath();
    m_castUrl = QStringLiteral("http://%1:%2/%3")
        .arg(localIp())
        .arg(m_server->serverPort())
        .arg(QString::fromLatin1(
            QUrl::toPercentEncoding(m_castPath, "/", "")));
    m_activeDevice = deviceIndex;
    Q_EMIT activeChanged();

    const auto sendPlay = [this] {
        soap(m_activeDevice, QStringLiteral("Play"), -1.0, [this](bool ok) {
            Q_EMIT notice(ok ? tr("Kivetítve: %1").arg(activeDeviceName())
                             : tr("Lejátszás indítása nem sikerült"),
                          ok ? QStringLiteral("ok") : QStringLiteral("err"));
        });
    };
    soap(m_activeDevice, QStringLiteral("SetAVTransportURI"), -1.0,
         [sendPlay, position, this](bool ok) {
             if (ok) {
                 sendPlay();
                 if (position > 2.0)
                     castSeek(position);
             } else {
                 Q_EMIT notice(tr("Kivetítés indítása nem sikerült"), "err");
             }
         });
    return true;
}

void CastManager::castSeek(double position)
{
    m_lastSeekTarget = position;
    if (m_activeDevice >= 0)
        soap(m_activeDevice, QStringLiteral("Seek"), position, nullptr);
}

void CastManager::stopCast()
{
    if (m_castUrl.isEmpty())
        return;
    if (m_activeDevice >= 0) {
        soap(m_activeDevice, QStringLiteral("Stop"), -1.0, nullptr);
        Q_EMIT notice(tr("Kivetítés leállítva"), "info");
    }
    m_activeDevice = -1;
    Q_EMIT activeChanged();
}

void CastManager::resumeCast()
{
    if (m_activeDevice < 0 || m_castUrl.isEmpty())
        return;
    soap(m_activeDevice, QStringLiteral("SetAVTransportURI"), -1.0,
         [this](bool ok) {
             if (ok)
                 soap(m_activeDevice, QStringLiteral("Play"), -1.0, nullptr);
         });
}

bool CastManager::isCastingCapable(const QString &filePath)
{
    return !filePath.isEmpty() && QFileInfo(filePath).isFile()
        && !filePath.startsWith(QLatin1String("http"))
        && !filePath.startsWith(QLatin1String("https"))
        && !filePath.startsWith(QLatin1String("ytdl"));
}

// --- local HTTP server that streams the cast file --------------------------

void CastManager::serveFile(QTcpSocket *client, const QString &path,
                            qint64 rangeStart, bool hasRange)
{
    // Only ever serve the file we announced.
    if (!m_castPath.isEmpty()
        && QFileInfo(path).canonicalFilePath() != QFileInfo(m_castPath).canonicalFilePath()) {
        client->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
        client->disconnectFromHost();
        return;
    }
    auto file = std::make_shared<QFile>(m_castPath);
    if (!file->open(QIODevice::ReadOnly)) {
        client->write("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n");
        client->disconnectFromHost();
        return;
    }
    const qint64 total = file->size();
    const qint64 start = hasRange
        ? qBound<qint64>(0, rangeStart, qMax<qint64>(0, total - 1))
        : 0;
    const qint64 length = total - start;
    file->seek(start);

    QByteArray head;
    if (hasRange) {
        head = QStringLiteral("HTTP/1.1 206 Partial Content\r\n"
                              "Content-Range: bytes %1-%2/%3\r\n")
                   .arg(start).arg(total - 1).arg(total).toUtf8();
    } else {
        head = "HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\n";
        if (total <= 0)
            head = "HTTP/1.1 200 OK\r\n";
    }
    if (total > 0) {
        head += "Content-Length: " + QByteArray::number(length) + "\r\n";
    }
    head += "Content-Type: " + mimeFor(m_castPath).toUtf8()
        + "\r\nConnection: close\r\n\r\n";
    client->write(head);

    // Stream the rest in chunks on bytesWritten so the renderer's read pace
    // throttles us; lambdas own the file + the remaining counter.
    auto remaining = std::make_shared<qint64>(length);
    connect(client, &QTcpSocket::bytesWritten, this, [this, client, file, remaining] {
        while (*remaining > 0) {
            const QByteArray chunk = file->read(qMin<qint64>(kChunkSize, *remaining));
            if (chunk.isEmpty())
                break;
            *remaining -= chunk.size();
            client->write(chunk);
        }
        if (*remaining <= 0)
            client->disconnectFromHost();
    });
    // Kick off the first chunk right away.
    const QByteArray first = file->read(qMin<qint64>(kChunkSize, *remaining));
    *remaining -= first.size();
    if (first.size())
        client->write(first);
    if (*remaining <= 0)
        client->disconnectFromHost();
}

QString CastManager::localIp() const
{
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        if (!(iface.flags() & QNetworkInterface::IsUp)
            || (iface.flags() & QNetworkInterface::IsLoopBack))
            continue;
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress &addr = entry.ip();
            if (addr.protocol() == QAbstractSocket::IPv4Protocol
                && !addr.isLoopback() && !addr.isLinkLocal())
                return addr.toString();
        }
    }
    return QStringLiteral("127.0.0.1");
}
#include "CastManager.h"
#include "GoogleCastClient.h"
#include "AirPlayPairing.h"
#include "AirPlaySession.h"
#include "HlsSession.h"
#include "CastDebug.h"

#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>
#include <QXmlStreamReader>

#include <algorithm>
#include <memory>

namespace {

constexpr int kSsdpPort = 1900;
constexpr int kDiscoveryMs = 4000;   // one round; empty rounds auto-retry
constexpr int kMaxEmptyRounds = 2;   // …this many extra rounds, then give up
constexpr int kProbeMs = 1000;       // re-broadcast M-SEARCH/PTR while open
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
    , m_net(new QNetworkAccessManager(this))
    , m_gcast(new GoogleCastClient(this))
    , m_pair(new AirPlayPairing(this))
    , m_air(new AirPlaySession(this))
    , m_hls(new HlsSession(this))
{
    connect(m_pair, &AirPlayPairing::notice, this, &CastManager::notice);
    connect(m_air, &AirPlaySession::notice, this, &CastManager::notice);
    connect(m_gcast, &GoogleCastClient::notice, this,
            &CastManager::notice);
    connect(m_gcast, &GoogleCastClient::loaded, this,
            &CastManager::onGcastLoaded);
    m_discoverTimer = new QTimer(this);
    m_discoverTimer->setSingleShot(true);
    m_discoverTimer->setInterval(kDiscoveryMs);
    connect(m_discoverTimer, &QTimer::timeout, this, [this] {
        m_queryTimer->stop();
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
        // Nothing answered this round: re-click the search automatically a
        // couple of times (slow/multicast-shy boxes), then give up so we do
        // not probe the LAN forever.
        if (m_devices.isEmpty() && m_discovering
            && m_emptyRounds < kMaxEmptyRounds) {
            ++m_emptyRounds;
            sendDiscoveryProbes();
            m_queryTimer->start();
            m_discoverTimer->start();
        } else {
            m_discovering = false;
            Q_EMIT devicesChanged();
        }
    });

    m_queryTimer = new QTimer(this);
    m_queryTimer->setSingleShot(false);
    m_queryTimer->setInterval(kProbeMs);
    connect(m_queryTimer, &QTimer::timeout, this, [this] {
        if (m_discovering)
            sendDiscoveryProbes();
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
    m_gcastInstances.clear();
    m_airplayInstances.clear();
    m_mdnsInfo.clear();
    m_mdnsHosts.clear();
    m_mdnsProbed.clear();
    m_emptyRounds = 0;
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

    startMdns(); // binds the mDNS socket on first use
    sendDiscoveryProbes();
    m_queryTimer->start();
    m_discoverTimer->start();
}

void CastManager::sendDiscoveryProbes()
{
    // One broadcast round: SSDP M-SEARCH (DLNA) + DNS-SD PTR queries (DLNA /
    // Google Cast / AirPlay). Called on start, every kProbeMs while the
    // window is open, and on empty-round auto-retry.
    const QByteArray msearch =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: " + kSearchTarget + "\r\n"
        "\r\n";
    if (m_ssdp->state() == QAbstractSocket::BoundState) {
        m_ssdp->writeDatagram(msearch,
                              QHostAddress(QStringLiteral("239.255.255.250")),
                              kSsdpPort);
        m_ssdp->writeDatagram(msearch,
                              QHostAddress(QStringLiteral("239.255.255.251")),
                              kSsdpPort);
    }
    if (m_mdns->state() == QAbstractSocket::BoundState) {
        sendMdnsQuery(12, QStringLiteral("_mediarender._tcp.local"));  // PTR
        sendMdnsQuery(12, QStringLiteral("_googlecast._tcp.local"));   // PTR
        sendMdnsQuery(12, QStringLiteral("_airplay._tcp.local"));      // PTR
        // Re-ask SRV/TXT(/A) for instances seen but not yet resolved: their
        // answers may have been lost (multicast), and without this only a
        // full manual rescan (which clears the instance lists) would retry.
        const auto reask = [this](const QStringList &instances) {
            for (const QString &instance : instances) {
                if (m_mdnsProbed.contains(instance))
                    continue;
                sendMdnsQuery(33, instance); // SRV
                sendMdnsQuery(16, instance); // TXT
                const QString target = m_mdnsInfo.value(instance)
                                           .value(QStringLiteral("target"))
                                           .toString();
                if (!target.isEmpty()
                    && !m_mdnsHosts.contains(target))
                    sendMdnsQuery(1, target); // A
            }
        };
        reask(m_mdnsInstances);
        reask(m_gcastInstances);
        reask(m_airplayInstances);
    }
}

void CastManager::stopDiscovery()
{
    m_discoverTimer->stop();
    m_drainTimer->stop();
    m_queryTimer->stop();
    if (m_discovering) {
        m_discovering = false;
        Q_EMIT devicesChanged();
    }
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
    // Google Cast + AirPlay service enumeration shares this socket.
    sendMdnsQuery(12, QStringLiteral("_googlecast._tcp.local"));   // PTR
    sendMdnsQuery(12, QStringLiteral("_airplay._tcp.local"));      // PTR
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
    dnsPutU16(pkt, 0x0001);            // IN class, QM (multicast reply): QU
                                       // unicast answers would often land on
                                       // avahi's 5353 socket instead of ours
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
            if (rtype == 12) {
                // PTR answer: owner=service, rdata=instance name.
                QString instance;
                if (dnsDecodeName(buf, rdata, &instance) < 0)
                    ; // fall through to off update
                else if (owner.endsWith("_mediarender._tcp.local")) {
                    if (!m_mdnsInstances.contains(instance)) {
                        m_mdnsInstances.append(instance);
                        sendMdnsQuery(33, instance); // SRV
                        sendMdnsQuery(16, instance); // TXT
                    }
                } else if (owner.endsWith("_googlecast._tcp.local")) {
                    if (!m_gcastInstances.contains(instance)) {
                        m_gcastInstances.append(instance);
                        sendMdnsQuery(33, instance); // SRV
                        sendMdnsQuery(16, instance); // TXT
                        sendMdnsQuery(1, instance);  // A (some sticks answer here)
                    }
                } else if (owner.endsWith("_airplay._tcp.local")) {
                    if (!m_airplayInstances.contains(instance)) {
                        m_airplayInstances.append(instance);
                        sendMdnsQuery(33, instance); // SRV
                        sendMdnsQuery(16, instance); // TXT
                        sendMdnsQuery(1, instance);  // A
                    }
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
                // TXT: DLNA carries path=<device-desc URL>; Cast/AirPlay
                // carry fn=<friendly name> (+ model/feature keys we ignore).
                int p = rdata;
                const int end = rdata + rdlen;
                while (p < end) {
                    const int len = quint8(buf.at(p));
                    if (p + 1 + len > end)
                        break;
                    const QByteArray kv = buf.mid(p + 1, len);
                    p += 1 + len;
                    const int eq = kv.indexOf('=');
                    if (eq <= 0)
                        continue;
                    const QByteArray key = kv.left(eq).toLower();
                    if (key == "path") {
                        QVariantMap info = m_mdnsInfo.value(owner);
                        info[QStringLiteral("path")] = utf8(kv.mid(eq + 1));
                        m_mdnsInfo.insert(owner, info);
                    } else if (key == "fn") {
                        QVariantMap info = m_mdnsInfo.value(owner);
                        info[QStringLiteral("fn")] = utf8(kv.mid(eq + 1));
                        m_mdnsInfo.insert(owner, info);
                    } else if (key == "deviceid") {
                        QVariantMap info = m_mdnsInfo.value(owner);
                        info[QStringLiteral("deviceid")] = utf8(kv.mid(eq + 1));
                        m_mdnsInfo.insert(owner, info);
                    }
                }
            }
            off = rdata + rdlen;
        }
        // Resolve any instances whose SRV/TXT just arrived.
        for (const QString &instance : std::as_const(m_mdnsInstances))
            mdnsTryResolve(instance);
        for (const QString &instance : std::as_const(m_gcastInstances))
            mdnsTryResolveCast(instance, QStringLiteral("googlecast"));
        for (const QString &instance : std::as_const(m_airplayInstances))
            mdnsTryResolveCast(instance, QStringLiteral("airplay"));
    }
}

// Shared helper: turn a resolved _googlecast/_airplay instance into a device
// entry (no SCDP fetch — the mDNS SRV port + A ip are the control endpoint).
void CastManager::mdnsTryResolveCast(const QString &instance,
                                     const QString &type)
{
    if (m_mdnsProbed.contains(instance))
        return;
    const QVariantMap info = m_mdnsInfo.value(instance);
    int port = info.value(QStringLiteral("port")).toInt();
    const QString target = info.value(QStringLiteral("target")).toString();
    QString ip = m_mdnsHosts.value(target);
    if (ip.isEmpty())
        ip = m_mdnsHosts.value(instance);
    if (port <= 0 || ip.isEmpty())
        return;
    if (type == QLatin1String("airplay") && port <= 0)
        port = 7000;
    m_mdnsProbed.append(instance);
    QString name = info.value(QStringLiteral("fn")).toString();
    if (name.isEmpty()) {
        // Instance is "<name>._googlecast._tcp.local" — prettify the head.
        name = instance.section(QLatin1Char('.'), 0, 0);
        name.replace(QLatin1Char('-'), QLatin1Char(' '));
        name.replace(QLatin1Char('_'), QLatin1Char(' '));
    }
    for (const QVariant &existing : std::as_const(m_devices)) {
        const QVariantMap m = existing.toMap();
        if (m.value(QStringLiteral("host")).toString() == ip
            && m.value(QStringLiteral("port")).toInt() == port
            && m.value(QStringLiteral("type")).toString() == type)
            return;
    }
    QVariantMap dev;
    dev[QStringLiteral("name")] = name;
    dev[QStringLiteral("host")] = ip;
    dev[QStringLiteral("port")] = port;
    dev[QStringLiteral("controlUrl")] = QString();
    dev[QStringLiteral("type")] = type;
    const QString did =
        info.value(QStringLiteral("deviceid")).toString();
    if (!did.isEmpty())
        dev[QStringLiteral("id")] = did;
    m_devices.append(dev);
    sortDevices();
    Q_EMIT devicesChanged();
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
            dev[QStringLiteral("type")] = QStringLiteral("dlna");
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
            if (!dupe) {
                m_devices.append(dev);
                sortDevices();
            }
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
            // Percent-encoding (not HTML-escaping): picky renderers choke
            // on '&' inside the URI element.
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

bool CastManager::ensureServer()
{
    if (!m_server && !(m_server = new QTcpServer(this)))
        return false;
    if (!m_server->isListening()) {
        // Stable port so a LAN firewall rule (ufw) can allow it once;
        // ephemeral fallback when occupied.
        static constexpr quint16 kCastPort = 8099;
        if (!m_server->listen(QHostAddress::AnyIPv4, kCastPort)
            && !m_server->listen(QHostAddress::AnyIPv4, 0)) {
            Q_EMIT notice(tr("Cast kiszolgáló indítása nem sikerült"), "err");
            return false;
        }
    }
    if (m_server->isListening() && !m_serverRunning) {
        m_serverRunning = true;
        Q_EMIT serverStateChanged();
        connect(m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *client = m_server->nextPendingConnection()) {
                castDebug(QStringLiteral("http accept peer=%1")
                              .arg(client->peerAddress().toString()));
                m_connections.append(client);
                connect(client, &QTcpSocket::readyRead, this, [this, client] {
                    if (client->bytesAvailable() < 16)
                        return;
                    const QByteArray req = client->readAll();
                    if (!req.startsWith("GET") && !req.startsWith("HEAD"))
                        return;
                    // "GET /<enc> HTTP/1.1" + optional Range: bytes=start-
                    const int sp = req.indexOf(' ');
                    const int sp2 = req.indexOf(' ', sp + 1);
                    if (sp < 0 || sp2 < 0)
                        return;
                    const QString rawPath = QUrl::fromPercentEncoding(
                        req.mid(sp + 1, sp2 - sp - 1));
                    // Short stable alias for the announced file (typable on a
                    // phone for network tests, and shorter contentIds).
                    const QString path = (rawPath == QLatin1String("/cast.mp4")
                                          || rawPath == QLatin1String("/cast"))
                        ? m_castPath
                        : rawPath;
                    qint64 start = 0;
                    qint64 end = -1; // inclusive; -1 = to EOF
                    bool hasRange = false;
                    const QByteArray lower = req.toLower();
                    const int ri = lower.indexOf("range: bytes=");
                    if (ri >= 0) {
                        const int e = lower.indexOf("\r\n", ri);
                        const QByteArray spec = req.mid(ri + 13, e - (ri + 13))
                                                    .trimmed();
                        const int dash = spec.indexOf('-');
                        if (dash > 0) {
                            start = spec.left(dash).trimmed().toLongLong(
                                &hasRange);
                            hasRange = hasRange && start >= 0;
                            if (hasRange && dash + 1 < spec.size()) {
                                bool okEnd = false;
                                const qint64 endVal = spec.mid(dash + 1).trimmed()
                                                          .toLongLong(&okEnd);
                                if (okEnd && endVal >= start)
                                    end = endVal;
                            }
                        }
                    }
                    serveFile(client, path, start, hasRange, end);
                });
                connect(client, &QTcpSocket::disconnected, this,
                        [this, client] {
                            m_connections.removeAll(client);
                            client->deleteLater();
                        });
            }
        });
    }
    return true;
}

QString CastManager::deviceType(int deviceIndex) const
{
    if (deviceIndex < 0 || deviceIndex >= m_devices.size())
        return {};
    const QString t = m_devices.at(deviceIndex).toMap()
                          .value(QStringLiteral("type")).toString();
    return t.isEmpty() ? QStringLiteral("dlna") : t; // legacy entries
}

void CastManager::requestCast(int deviceIndex, const QString &filePath,
                               double position)
{
    const QString file = filePath;
    QTimer::singleShot(0, this, [this, deviceIndex, file, position] {
        cast(deviceIndex, file, position);
    });
}

QString CastManager::deviceKey(const QVariantMap &dev)
{    return dev.value(QStringLiteral("type")).toString()
        + QLatin1Char('\x1f') + dev.value(QStringLiteral("host")).toString()
        + QLatin1Char(':') + dev.value(QStringLiteral("port")).toString();
}

int CastManager::findDevice(const QString &key) const
{
    if (key.isEmpty())
        return -1;
    for (int i = 0; i < m_devices.size(); ++i) {
        if (deviceKey(m_devices.at(i).toMap()) == key)
            return i;
    }
    return -1;
}

void CastManager::setActive(int deviceIndex)
{
    m_activeDevice = deviceIndex;
    m_activeKey = (deviceIndex >= 0 && deviceIndex < m_devices.size())
        ? deviceKey(m_devices.at(deviceIndex).toMap())
        : QString();
    m_activeType = deviceType(deviceIndex);
    Q_EMIT activeChanged();
}

// Stable row order (type, then name, then host) so a row keeps meaning
// across scans; the active target is re-resolved by key after sorting.
void CastManager::sortDevices()
{
    const auto rank = [](const QVariantMap &m) {
        const QString t = m.value(QStringLiteral("type")).toString();
        if (t == QLatin1String("googlecast"))
            return 0;
        if (t == QLatin1String("airplay"))
            return 1;
        return 2;
    };
    std::sort(m_devices.begin(), m_devices.end(),
              [&](const QVariant &a, const QVariant &b) {
                  const QVariantMap ma = a.toMap(), mb = b.toMap();
                  if (rank(ma) != rank(mb))
                      return rank(ma) < rank(mb);
                  const int n = QString::localeAwareCompare(
                      ma.value(QStringLiteral("name")).toString(),
                      mb.value(QStringLiteral("name")).toString());
                  if (n != 0)
                      return n < 0;
                  return ma.value(QStringLiteral("host")).toString()
                      < mb.value(QStringLiteral("host")).toString();
              });
    if (!m_activeKey.isEmpty())
        m_activeDevice = findDevice(m_activeKey);
}

bool CastManager::cast(int deviceIndex, const QString &filePath, double position)
{
    if (deviceIndex < 0 || deviceIndex >= m_devices.size()) {
        Q_EMIT notice(tr("Először keress eszközt a hálózaton"), "err");
        return false;
    }
    if (filePath.isEmpty() || !QFileInfo(filePath).isFile()) {
        Q_EMIT notice(tr("Csak helyi fájlt lehet kivetíteni"), "err");
        return false;
    }
    const QString type = deviceType(deviceIndex);
    const QString local = QFileInfo(filePath).canonicalFilePath();
    // Mark the target active right away (even while converting) so the
    // panel never shows a stale device as connected.
    setActive(deviceIndex);
    // Chromecast/AirPlay only speak a narrow set of streams (MP4/H.264 +
    // stereo AAC). Anything else goes live-HLS (no full pre-transcode);
    // DLNA TVs play MKV/E-AC-3 natively, so they always go direct.
    if ((type == QLatin1String("googlecast")
         || type == QLatin1String("airplay"))
        && needsConversion(local)) {
        startHls(deviceIndex, local, position);
        return true;
    }
    continueCast(deviceIndex, local, position);
    return true;
}

void CastManager::continueCast(int deviceIndex, const QString &localPath,
                               double position)
{
    if (!ensureServer())
        return;
    m_castPath = localPath;
    // Encoded path already starts with '/' — no extra separator, otherwise
    // the URL gets a '//' after the port.
    m_castUrl = QStringLiteral("http://%1:%2%3")
        .arg(localIp())
        .arg(m_server->serverPort())
        .arg(QString::fromLatin1(
            QUrl::toPercentEncoding(m_castPath, "/", "")));
    castDebug(QStringLiteral("cast idx=%1 type=%2 url=%3")
                  .arg(deviceIndex)
                  .arg(m_activeType, m_castUrl));
    setActive(deviceIndex); // panel shows the target right away
    Q_EMIT serverStateChanged();

    if (m_activeType == QLatin1String("googlecast")) {
        castGoogle(deviceIndex, position);
        return;
    }
    if (m_activeType == QLatin1String("airplay")) {
        castAirPlay(deviceIndex, position);
        return;
    }
    const auto sendPlay = [this] {
        soap(m_activeDevice, QStringLiteral("Play"), -1.0, [this](bool ok) {
            if (!ok)
                setActive(-1); // never connected
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
                 setActive(-1); // never connected
                 Q_EMIT notice(tr("Kivetítés indítása nem sikerült"), "err");
             }
         });
}

namespace {

// ffprobe view of a file for the cast decision: first video codec plus
// whether the whole file is directly Cast-playable.
struct CastProbe {
    QString vcodec;
    double duration = 0.0; // seconds (0 = unknown)
    bool direct = false; // no conversion needed
};

CastProbe probeForCast(const QString &path)
{
    CastProbe out;
    QProcess probe;
    probe.start(QStandardPaths::findExecutable(QStringLiteral("ffprobe")),
                {QStringLiteral("-v"), QStringLiteral("error"),
                 QStringLiteral("-show_entries"),
                 QStringLiteral("format=duration:stream=codec_name,channels"),
                 QStringLiteral("-of"), QStringLiteral("json"), path});
    if (!probe.waitForFinished(10000) || probe.exitCode() != 0)
        return out;
    const QJsonDocument doc = QJsonDocument::fromJson(probe.readAllStandardOutput());
    const QJsonObject root = doc.object();
    out.duration = root.value(QStringLiteral("format")).toObject()
                       .value(QStringLiteral("duration")).toString().toDouble();
    const QJsonArray streams = root.value(QStringLiteral("streams")).toArray();
    bool haveVideo = false, videoOk = false, audioOk = true;
    for (const QJsonValue &v : streams) {
        const QJsonObject s = v.toObject();
        const QString codec = s.value(QStringLiteral("codec_name")).toString();
        if (!haveVideo && !codec.isEmpty() && codec != QLatin1String("mjpeg")
            && codec != QLatin1String("png")) {
            haveVideo = true;
            out.vcodec = codec;
            videoOk = (codec == QLatin1String("h264"));
        } else if (haveVideo && !codec.isEmpty()
                   && codec != QLatin1String("mjpeg")
                   && codec != QLatin1String("png")
                   && codec != QLatin1String("subrip")
                   && codec != QLatin1String("ass")
                   && codec != QLatin1String("mov_text")) {
            // Audio (or other playable) stream: only stereo AAC passes.
            const int ch = s.value(QStringLiteral("channels")).toInt(2);
            if (codec != QLatin1String("aac") || ch > 2)
                audioOk = false;
        }
    }
    out.direct = haveVideo && videoOk && audioOk;
    // Direct play additionally needs an MP4-family container.
    if (out.direct) {
        const QString ext = QFileInfo(path).suffix().toLower();
        out.direct = (ext == QLatin1String("mp4")
                      || ext == QLatin1String("m4v")
                      || ext == QLatin1String("mov"));
    }
    return out;
}

} // namespace

bool CastManager::needsConversion(const QString &path) const
{
    if (QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty())
        return false; // cannot judge — try direct, receiver will complain
    return !probeForCast(path).direct;
}

QString CastManager::conversionCachePath(const QString &path) const
{
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
             + QStringLiteral("/cast"));
    dir.mkpath(QStringLiteral("."));
    const QFileInfo fi(path);
    const QString base = fi.completeBaseName().left(60).replace(
        QLatin1Char('/'), QLatin1Char('_'));
    return dir.filePath(QStringLiteral("%1-%2-%3.mp4")
                            .arg(base)
                            .arg(fi.size())
                            .arg(fi.lastModified().toSecsSinceEpoch()));
}

void CastManager::startConversion(int deviceIndex, const QString &path,
                                  double position)
{
    cancelConversion();
    const QString dst = conversionCachePath(path);
    // Atomic cache writes: ffmpeg goes to <dst>.part, renamed only on
    // success — a killed conversion must never poison the cache (a
    // moov-less partial plays nowhere and fails LOAD on the TV).
    const QString part = dst + QStringLiteral(".part");
    QFile::remove(part);
    m_convertKey = (deviceIndex >= 0 && deviceIndex < m_devices.size())
        ? deviceKey(m_devices.at(deviceIndex).toMap())
        : QString();
    m_convertSrc = path;
    m_convertPos = position;
    const QFileInfo dstInfo(dst);
    if (dstInfo.exists() && dstInfo.size() > (1 << 20)
        && dstInfo.lastModified() >= QFileInfo(path).lastModified()) {
        castDebug(QStringLiteral("convert: cache hit %1").arg(dst));
        const int dev = findDevice(m_convertKey);
        m_convertKey.clear();
        if (dev >= 0)
            continueCast(dev, dst, position);
        return;
    }
    if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
        Q_EMIT notice(tr("Vetítéshez ffmpeg kell (nincs telepítve)"), "err");
        setActive(-1); // drop the optimistic target
        m_convertKey.clear();
        return;
    }
    // Video passes through when already H.264; anything else is re-encoded
    // (slow but plays). Audio always becomes stereo AAC; subs are dropped
    // (TV-side subtitle track would need a second feature).
    const CastProbe probeInfo = probeForCast(path);
    const bool vcopy = (probeInfo.vcodec == QLatin1String("h264"));
    m_convertDuration = probeInfo.duration;
    m_convertProgress = 0.0;
    Q_EMIT convertProgressChanged();
    QStringList args = {QStringLiteral("-y"), QStringLiteral("-nostdin"),
                        QStringLiteral("-v"),
                        QStringLiteral("error"), QStringLiteral("-i"), path,
                        QStringLiteral("-map"), QStringLiteral("0:v:0"),
                        QStringLiteral("-map"), QStringLiteral("0:a?")};
    if (vcopy)
        args += {QStringLiteral("-c:v"), QStringLiteral("copy")};
    else
        args += {QStringLiteral("-c:v"), QStringLiteral("libx264"),
                 QStringLiteral("-preset"), QStringLiteral("veryfast"),
                 QStringLiteral("-crf"), QStringLiteral("21")};
    args += {QStringLiteral("-c:a"), QStringLiteral("aac"),
             QStringLiteral("-ac"), QStringLiteral("2"),
             QStringLiteral("-b:a"), QStringLiteral("160k"),
             QStringLiteral("-movflags"), QStringLiteral("+faststart"),
             QStringLiteral("-progress"), QStringLiteral("pipe:1"),
             QStringLiteral("-nostats"), part};
    castDebug(QStringLiteral("convert: ffmpeg %1").arg(args.join(' ')));
    m_convertProc = new QProcess(this);
    connect(m_convertProc, &QProcess::readyReadStandardOutput, this, [this] {
        // ffmpeg -progress lines: out_time_ms=12345 … progress=end.
        const QByteArray out =
            m_convertProc ? m_convertProc->readAllStandardOutput() : QByteArray();
        for (const QByteArray &line : out.split('\n')) {
            if (!line.startsWith("out_time_ms="))
                continue;
            const double secs = line.mid(12).trimmed().toLongLong() / 1000000.0;
            if (m_convertDuration > 1.0) {
                const double p = qBound(0.0, secs / m_convertDuration, 1.0);
                if (qAbs(p - m_convertProgress) > 0.005) {
                    m_convertProgress = p;
                    Q_EMIT convertProgressChanged();
                }
            }
        }
    });
    connect(m_convertProc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, dst, part](int code, QProcess::ExitStatus status) {
                QProcess *proc = m_convertProc;
                m_convertProc = nullptr;
                if (proc)
                    proc->deleteLater();
                Q_EMIT convertingChanged();
                const int dev = findDevice(m_convertKey);
                const double pos = m_convertPos;
                m_convertKey.clear();
                bool renamed = false;
                if (status == QProcess::NormalExit && code == 0
                    && QFileInfo(part).size() > (1 << 20)) {
                    QFile::remove(dst);
                    renamed = QFile::rename(part, dst);
                } else {
                    QFile::remove(part);
                }
                m_convertProgress = renamed ? 1.0 : 0.0;
                Q_EMIT convertProgressChanged();
                if (renamed) {
                    castDebug(QStringLiteral("convert: done %1").arg(dst));
                    Q_EMIT notice(tr("Konvertálva, vetítés indul"), "ok");
                    if (dev >= 0)
                        continueCast(dev, dst, pos);
                    else
                        setActive(-1);
                } else {
                    castDebug(QStringLiteral("convert: FAILED code=%1").arg(code));
                    Q_EMIT notice(tr("Konvertálás nem sikerült"), "err");
                    setActive(-1);
                }
            });
    Q_EMIT notice(tr("Konvertálás vetítéshez (sztereó MP4)…"), "info");
    Q_EMIT convertingChanged();
    m_convertProc->start(QStandardPaths::findExecutable(
                             QStringLiteral("ffmpeg")),
                         args);
}

void CastManager::cancelConversion()
{
    if (!m_convertProc)
        return;
    QProcess *proc = m_convertProc;
    m_convertProc = nullptr;
    proc->disconnect(this);
    proc->kill();
    proc->deleteLater();
    m_convertKey.clear();
    m_convertProgress = 0.0;
    Q_EMIT convertProgressChanged();
    Q_EMIT convertingChanged();
}

void CastManager::startHls(int deviceIndex, const QString &path,
                           double position)
{
    stopHls();
    cancelConversion();
    // Drop stale single-shot session handlers from a previous attempt.
    disconnect(m_hls, nullptr, this, nullptr);
    m_hlsKey = (deviceIndex >= 0 && deviceIndex < m_devices.size())
        ? deviceKey(m_devices.at(deviceIndex).toMap())
        : QString();
    m_hlsSrc = path;
    m_hlsPos = position;
    if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()) {
        Q_EMIT notice(tr("Vetítéshez ffmpeg kell (nincs telepítve)"), "err");
        setActive(-1);
        m_hlsKey.clear();
        return;
    }
    const QString vcodec = probeForCast(path).vcodec;
    connect(m_hls, &HlsSession::ready, this,
            [this](const QString &rel) {
                const int dev = findDevice(m_hlsKey);
                if (dev < 0 || !m_hls || !m_hls->running()) {
                    stopHls();
                    setActive(-1);
                    return;
                }
                if (!ensureServer()) {
                    stopHls();
                    setActive(-1);
                    return;
                }
                m_hlsBusy = false;
                Q_EMIT hlsBusyChanged();
                m_hlsMode = true;
                m_castPath.clear(); // HLS marker: serveFile uses session dir
                m_castUrl = QStringLiteral("http://%1:%2/hls/%3/%4")
                                .arg(localIp())
                                .arg(m_server->serverPort())
                                .arg(m_hls->id(), rel);
                castDebug(QStringLiteral("hls ready %1").arg(m_castUrl));
                Q_EMIT serverStateChanged();
                // The playlist starts AT the requested position.
                if (m_activeType == QLatin1String("googlecast"))
                    castGoogle(dev, 0.0);
                else if (m_activeType == QLatin1String("airplay"))
                    castAirPlay(dev, 0.0);
                else
                    stopHls();
            },
            Qt::SingleShotConnection);
    connect(m_hls, &HlsSession::failed, this,
            [this](const QString &why) {
                castDebug(QStringLiteral("hls failed: %1").arg(why));
                const int dev = findDevice(m_hlsKey);
                const QString src = m_hlsSrc;
                const double pos = m_hlsPos;
                stopHls();
                if (dev < 0) {
                    setActive(-1);
                    return;
                }
                // Fall back to a full cached transcode.
                Q_EMIT notice(tr("Élő nem indult, teljes konvertálás…"),
                              "info");
                startConversion(dev, src, pos);
            },
            Qt::SingleShotConnection);
    m_hlsBusy = true;
    Q_EMIT hlsBusyChanged();
    Q_EMIT notice(tr("Élő indítása (HLS)…"), "info");
    m_hls->start(path, position, vcodec);
}

void CastManager::stopHls()
{
    if (m_hls)
        m_hls->stop();
    m_hlsMode = false;
    if (m_hlsBusy) {
        m_hlsBusy = false;
        Q_EMIT hlsBusyChanged();
    }
    m_hlsKey.clear();
}

void CastManager::castGoogle(int deviceIndex, double position)
{
    const QVariantMap dev = m_devices.at(deviceIndex).toMap();
    const QString host = dev.value(QStringLiteral("host")).toString();
    const quint16 port = quint16(
        dev.value(QStringLiteral("port")).toInt() ?: 8009);
    if (host.isEmpty()) {
        Q_EMIT notice(tr("Google Cast eszköz címe hiányzik"), "err");
        setActive(-1); // never connected — must not offer "lekapcsolódás"
        return;
    }
    Q_EMIT notice(tr("Csatlakozás: %1…").arg(dev.value(QStringLiteral("name"))
                                                 .toString()),
                  "info");
    // load() queues the payload: if TLS is already up it triggers
    // GET_STATUS→LOAD at once, otherwise it flushes from onEncrypted().
    // connectTo() is a no-op when already on this host. The session is
    // always reset first: a previous STOP/CLOSE leaves a dead transport
    // behind, and LOADing on it goes nowhere.
    m_gcast->resetSession();
    m_gcast->connectTo(host, port);
    m_gcast->load(m_castUrl,
                  m_hlsMode ? QStringLiteral("application/x-mpegURL")
                            : mimeFor(m_castPath),
                  QFileInfo(m_castPath.isEmpty() ? m_hlsSrc : m_castPath)
                      .fileName(),
                  position);
}

void CastManager::castAirPlay(int deviceIndex, double position)
{
    QVariantMap dev =
        (deviceIndex >= 0 && deviceIndex < m_devices.size())
        ? m_devices.at(deviceIndex).toMap()
        : QVariantMap();
    const QString host = dev.value(QStringLiteral("host")).toString();
    const int port = dev.value(QStringLiteral("port")).toInt() ?: 7000;
    if (host.isEmpty()) {
        Q_EMIT notice(tr("AirPlay eszköz címe hiányzik"), "err");
        return;
    }
    m_pair->setDevice(host, quint16(port),
                      dev.value(QStringLiteral("id")).toString());
    if (!m_pair->hasPairing()) {
        // First contact: TV shows a PIN, user types it into CastPanel.
        m_pendingAirKey = deviceKey(dev);
        m_pendingAirPos = position;
        if (m_pair->begin()) {
            m_airplayPairing = true;
            Q_EMIT airplayPairingChanged();
        } else {
            m_pendingAirKey.clear();
        }
        return;
    }
    if (!m_pair->verify()) {
        // Stale pairing — redo the PIN flow.
        m_pendingAirKey = deviceKey(dev);
        m_pendingAirPos = position;
        if (m_pair->begin()) {
            m_airplayPairing = true;
            Q_EMIT airplayPairingChanged();
        } else {
            m_pendingAirKey.clear();
        }
        return;
    }
    postAirPlayPlay(dev, position);
}

void CastManager::finishAirPlayPair(const QString &pin)
{
    if (!m_airplayPairing)
        return;
    // Same reentrancy rule as requestCast: the blocking pairing HTTP must
    // not run inside this QML handler's JS evaluation.
    const QString code = pin;
    QTimer::singleShot(0, this, [this, code] { finishAirPlayPairNow(code); });
}

void CastManager::finishAirPlayPairNow(const QString &pin)
{
    if (!m_airplayPairing)
        return;
    m_airplayPairing = false;
    Q_EMIT airplayPairingChanged();
    if (!m_pair->finish(pin, QStringLiteral("omaplayer"))) {
        m_pendingAirKey.clear();
        return; // notice already emitted
    }
    if (!m_pair->verify()) {
        m_pendingAirKey.clear();
        return;
    }
    const int dev = findDevice(m_pendingAirKey);
    m_pendingAirKey.clear();
    if (dev < 0) {
        Q_EMIT notice(tr("AirPlay eszköz eltűnt"), "err");
        return;
    }
    postAirPlayPlay(m_devices.at(dev).toMap(), m_pendingAirPos);
}

void CastManager::cancelAirPlayPair()
{
    m_airplayPairing = false;
    m_pendingAirKey.clear();
    Q_EMIT airplayPairingChanged();
    setActive(-1);
}

void CastManager::postAirPlayPlay(const QVariantMap &dev, double position)
{
    // AirPlay 2 (pyatv AP2 flow): the legacy open /play is refused by
    // AirPlay 2 receivers (403 / dropped connection), so playback goes
    // through AirPlaySession — verify on a raw socket, HAP-encrypted
    // control channel, RTSP SETUP/RECORD, binary-plist /play, /rate=1.
    const QString host = dev.value(QStringLiteral("host")).toString();
    const int port = dev.value(QStringLiteral("port")).toInt() ?: 7000;
    if (host.isEmpty()) {
        setActive(-1);
        return;
    }
    Q_EMIT notice(tr("AirPlay indítás: %1…").arg(
                      dev.value(QStringLiteral("name")).toString()),
                  "info");
    m_air->setDevice(host, quint16(port), m_pair->credentials());
    const bool ok = m_air->play(m_castUrl, position);
    if (!ok) {
        setActive(-1); // never connected — must not offer "lekapcsolódás"
        return;
    }
    Q_EMIT notice(tr("Kivetítve: %1").arg(
                      dev.value(QStringLiteral("name")).toString()),
                  "ok");
}

void CastManager::onGcastLoaded(bool ok)
{
    if (!ok && m_activeDevice >= 0
        && m_activeType == QLatin1String("googlecast")) {
        Q_EMIT notice(tr("Google Cast betöltés sikertelen"), "err");
        setActive(-1); // never connected — must not offer "lekapcsolódás"
    }
}

void CastManager::castSeek(double position)
{
    const int dev = findDevice(m_activeKey);
    if (dev < 0)
        return;
    m_activeDevice = dev;
    if (m_activeType == QLatin1String("googlecast")) {
        m_gcast->seek(position);
        return;
    }
    if (m_activeType == QLatin1String("airplay")) {
        const QVariantMap map = m_devices.at(dev).toMap();
        const QString host = map.value(QStringLiteral("host")).toString();
        const int port = map.value(QStringLiteral("port")).toInt() ?: 7000;
        if (host.isEmpty())
            return;
        QNetworkRequest req{QUrl(QStringLiteral("http://%1:%2/scrub?position=%3")
                                     .arg(host)
                                     .arg(port)
                                     .arg(position, 0, 'f', 1))};
        req.setTransferTimeout(6000);
        QNetworkReply *reply = m_net->post(req, QByteArray());
        connect(reply, &QNetworkReply::finished, reply,
                &QNetworkReply::deleteLater);
        return;
    }
    soap(m_activeDevice, QStringLiteral("Seek"), position, nullptr);
}

void CastManager::stopCast()
{
    cancelConversion(); // a pending transcode is pointless once stopped
    stopHls();          // live session teardown (ffmpeg killed, dir gone)
    m_hlsMode = false;
    if (m_airplayPairing)
        cancelAirPlayPair(); // clears target; per-type stop below is skipped
    if (m_castUrl.isEmpty()) {
        setActive(-1);
        return;
    }
    const int dev = findDevice(m_activeKey);
    if (dev >= 0) {
        m_activeDevice = dev;
        if (m_activeType == QLatin1String("googlecast")) {
            m_gcast->stop();
            Q_EMIT notice(tr("Kivetítés leállítva"), "info");
        } else if (m_activeType == QLatin1String("airplay")) {
            // TEARDOWN on the AP2 session (non-blocking by design, safe
            // straight from the QML handler).
            m_air->stop();
            Q_EMIT notice(tr("Kivetítés leállítva"), "info");
        } else {
            soap(dev, QStringLiteral("Stop"), -1.0, nullptr);
            Q_EMIT notice(tr("Kivetítés leállítva"), "info");
        }
    }
    m_activeType.clear();
    setActive(-1);
}

void CastManager::resumeCast()
{
    const int dev = findDevice(m_activeKey);
    if (dev < 0 || m_castUrl.isEmpty())
        return;
    m_activeDevice = dev;
    if (m_activeType == QLatin1String("googlecast")) {
        m_gcast->play();
        return;
    }
    if (m_activeType == QLatin1String("airplay")) {
        // Blocking RTSP exchange — must not run inside the QML handler.
        QTimer::singleShot(0, this, [this] {
            if (m_air->isPlaying())
                m_air->setRate(1.0);
        });
        return;
    }
    soap(dev, QStringLiteral("SetAVTransportURI"), -1.0,
         [this](bool ok) {
             if (ok) {
                 const int d = findDevice(m_activeKey);
                 if (d >= 0)
                     soap(d, QStringLiteral("Play"), -1.0, nullptr);
             }
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
                            qint64 rangeStart, bool hasRange, qint64 rangeEnd)
{
    const QString peer = client->peerAddress().toString();
    // Live HLS session files (playlist + segments) bypass the single-file
    // check below; served straight from the session dir.
    if (m_hls && m_hls->available() && path.startsWith(QLatin1String("/hls/"))) {
        const QString rest = path.mid(5);
        const int slash = rest.indexOf(QLatin1Char('/'));
        QByteArray data, ctype;
        const bool ok = slash > 0 && rest.left(slash) == m_hls->id()
            && m_hls->serve(rest.mid(slash + 1), &ctype, &data);
        if (!ok) {
            client->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
            client->disconnectFromHost();
            return;
        }
        qint64 start = 0;
        if (hasRange)
            start = qBound<qint64>(0, rangeStart, qMax<qint64>(0, data.size() - 1));
        const qint64 length = data.size() - start;
        QByteArray head;
        if (hasRange) {
            head = QStringLiteral("HTTP/1.1 206 Partial Content\r\n"
                                  "Content-Range: bytes %1-%2/%3\r\n")
                       .arg(start).arg(data.size() - 1).arg(data.size()).toUtf8();
        } else {
            head = "HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\n";
        }
        head += "Content-Length: " + QByteArray::number(length) + "\r\n";
        head += "Content-Type: " + ctype + "\r\n";
        if (ctype.contains("mpegURL"))
            head += "Cache-Control: no-cache\r\n";
        head += "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
        client->write(head);
        client->write(data.mid(int(start)));
        client->disconnectFromHost();
        castDebug(QStringLiteral("http hls peer=%1 %2 bytes=%3")
                      .arg(peer, rest.mid(slash + 1)).arg(length));
        return;
    }
    // Only ever serve the file we announced.
    if (!m_castPath.isEmpty()
        && QFileInfo(path).canonicalFilePath() != QFileInfo(m_castPath).canonicalFilePath()) {
        castDebug(QStringLiteral("http 404 peer=%1 path=%2").arg(peer, path));
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
    const qint64 last = (hasRange && rangeEnd >= start)
        ? qMin(rangeEnd, total - 1)
        : total - 1;
    const qint64 length = last - start + 1;
    file->seek(start);

    // Cast receivers (like browsers) require CORS headers on media responses.
    constexpr const char *kCors =
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n"
        "Access-Control-Expose-Headers: Content-Length, Content-Range\r\n";
    QByteArray head;
    if (hasRange) {
        head = QStringLiteral("HTTP/1.1 206 Partial Content\r\n"
                              "Content-Range: bytes %1-%2/%3\r\n")
                   .arg(start).arg(last).arg(total).toUtf8();
    } else {
        head = "HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\n";
        if (total <= 0)
            head = "HTTP/1.1 200 OK\r\n";
    }
    if (total > 0) {
        head += "Content-Length: " + QByteArray::number(length) + "\r\n";
    }
    head += "Content-Type: " + mimeFor(m_castPath).toUtf8()
        + "\r\n" + kCors + "Connection: close\r\n\r\n";
    castDebug(QStringLiteral("http %1 peer=%2 range=%3-%4 total=%5")
                  .arg(hasRange ? QStringLiteral("206") : QStringLiteral("200"))
                  .arg(peer)
                  .arg(start)
                  .arg(last)
                  .arg(total));
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
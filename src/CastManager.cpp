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

CastManager::CastManager(QObject *parent)
    : QObject(parent)
    , m_ssdp(new QUdpSocket(this))
{
    m_discoverTimer = new QTimer(this);
    m_discoverTimer->setSingleShot(true);
    m_discoverTimer->setInterval(kDiscoveryMs);
    connect(m_discoverTimer, &QTimer::timeout, this, [this] {
        m_discovering = false;
        const QStringList pending = m_pending;
        m_pending.clear();
        for (const QString &location : pending)
            fetchDescription(location);
        if (m_pendingDesc.isEmpty())
            Q_EMIT devicesChanged();
    });
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
    m_discovering = true;
    if (m_ssdp->state() != QAbstractSocket::BoundState)
        m_ssdp->bind(QHostAddress::AnyIPv4, 0);
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
    m_discoverTimer->start();
}

void CastManager::stopDiscovery()
{
    m_discoverTimer->stop();
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
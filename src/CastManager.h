#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration>
#include <functional>

class QTcpServer;
class QTcpSocket;
class QUdpSocket;
class QTimer;

// DLNA / UPnP AV casting — stream the currently playing local file to any
// MediaRenderer on the LAN (smart TVs, VLC, Kodi, game consoles…).
//
// Three cooperating pieces:
//  * discovery — SSDP M-SEARCH for urn:schemas-upnp-org:device:MediaRenderer:1;
//    each reply's device description (SCDP) is fetched to locate the
//    AVTransport control URL;
//  * a tiny HTTP server that streams the local file (Range supported) so the
//    renderer can DirectPlay it;
//  * SOAP commands (SetAVTransportURI + Play / Stop / Seek) sent to the
//    renderer over the control URL.
//
// Only local files can be cast — remote streams (Jellyfin/YouTube) already
// live on a network another device may not reach, or are transient/DRM'd.
class CastManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(bool discovering READ discovering NOTIFY devicesChanged)
    Q_PROPERTY(bool serverRunning READ serverRunning NOTIFY serverStateChanged)
    Q_PROPERTY(QString castUrl READ castUrl NOTIFY serverStateChanged)
    Q_PROPERTY(int activeDevice READ activeDevice NOTIFY activeChanged)
    Q_PROPERTY(QString activeDeviceName READ activeDeviceName NOTIFY activeChanged)

public:
    explicit CastManager(QObject *parent = nullptr);

    QVariantList devices() const { return m_devices; }
    bool discovering() const { return m_discovering; }
    bool serverRunning() const { return m_serverRunning; }
    QString castUrl() const { return m_castUrl; }
    int activeDevice() const { return m_activeDevice; }
    QString activeDeviceName() const;

    // Fire an M-SEARCH and collect MediaRenderer responses for ~2.5 s, then
    // resolve their AVTransport control URLs from the device descriptions.
    Q_INVOKABLE void startDiscovery();
    Q_INVOKABLE void stopDiscovery();

    // Cast `filePath` to the renderer at `deviceIndex`, optionally seeking to
    // `position` seconds right after Play. Emits `notice` on failure.
    Q_INVOKABLE bool cast(int deviceIndex, const QString &filePath,
                          double position = 0.0);
    Q_INVOKABLE void castSeek(double position);
    Q_INVOKABLE void stopCast();
    Q_INVOKABLE void resumeCast();

    // True when the given media path can be served to a LAN renderer.
    Q_INVOKABLE static bool isCastingCapable(const QString &filePath);

signals:
    void devicesChanged();
    void serverStateChanged();
    void activeChanged();
    // Transient user feedback (toasts): text + "info"/"ok"/"err".
    void notice(const QString &text, const QString &kind);

private:
    void fetchDescription(const QString &location);
    void soap(int deviceIndex, const QString &action, double seekTarget = -1.0,
              const std::function<void(bool)> &done = {});
    void serveFile(QTcpSocket *client, const QString &path, qint64 rangeStart,
                   bool hasRange);
    QString localIp() const;

    // SSDP: the M-SEARCH replies are read here (unicast back to our socket
    // and multicast announcements) and their LOCATION headers collected.
    void handleSsdp();
    // mDNS resolvers: on top of SSDP we also ask DNS-SD `_mediarender._tcp`
    // because many TVs only advertise DLNA there and never answer M-SEARCH.
    void startMdns();
    void handleMdns();
    void sendMdnsQuery(int type, const QString &name);
    void mdnsTryResolve(const QString &instance);
    QString mdnsLocation(const QString &instance) const;
    // Feeds a device-description URL into the fetch queue (deduplicated).
    void appendLocation(const QString &location);
    // Sends one unicast M-SEARCH and (maloptional) schedules a drain.
    void flushPending();

    QVariantList m_devices;      // [{name, host, port, controlUrl, baseUrl}]
    QStringList m_pending;       // SCDP locations seen in the current burst
    QStringList m_pendingDesc;   // locations still awaiting their description
    QUdpSocket *m_ssdp = nullptr;
    QUdpSocket *m_mdns = nullptr;
    QTimer *m_discoverTimer = nullptr;
    bool m_discovering = false;

    // mDNS instances collected from `_mediarender._tcp` PTR answers, mapped
    // to their resolved host/port/description path once SRV/TXT arrive.
    QStringList m_mdnsInstances;
    QHash<QString, QVariantMap> m_mdnsInfo;
    QHash<QString, QString> m_mdnsHosts; // mdns target host -> ip we learned
    QStringList m_mdnsProbed;            // instances that already got a unicast M-SEARCH
    QTimer *m_drainTimer = nullptr;      // late-arrival drain after the burst
    quint16 m_mdnsQueryId = 0;

    QTcpServer *m_server = nullptr;
    bool m_serverRunning = false;
    QString m_castUrl;           // URL announced to the renderer
    QString m_castPath;          // canonical local path currently served
    QList<QTcpSocket *> m_connections;
    double m_lastSeekTarget = 0.0;
    int m_activeDevice = -1;
};
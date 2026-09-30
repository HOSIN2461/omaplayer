#pragma once

#include <QObject>
#include <QJsonObject>
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
class QNetworkAccessManager;
class QProcess;
class GoogleCastClient;
class AirPlayPairing;
class AirPlaySession;
class HlsSession;

// DLNA / UPnP AV + Google Cast + AirPlay casting — stream the currently
// playing local file to LAN renderers.
//
// Three protocols share one tiny HTTP file server (Range supported):
//  * DLNA/UPnP AV — SSDP M-SEARCH + SOAP SetAVTransportURI/Play/Stop/Seek;
//  * Google Cast — mDNS `_googlecast._tcp`, TLS CastV2 to port 8009,
//    DefaultMediaReceiver LOAD with the local URL (see GoogleCastClient);
//  * AirPlay 2 — mDNS `_airplay._tcp`, HAP PIN pairing (see
//    AirPlayPairing), then verify + HAP-encrypted control channel with
//    RTSP SETUP/RECORD and binary-plist /play (see AirPlaySession).
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

    // Fire an M-SEARCH + DNS-SD probes and collect renderers/Cast/AirPlay
    // answers for one round, then resolve their control endpoints.
    // Empty rounds auto-retry a couple of times (multicast is lossy).
    Q_INVOKABLE void startDiscovery();
    Q_INVOKABLE void stopDiscovery();

    // Async entry for QML clicks: defers to cast() on the next event-loop
    // turn. cast() performs blocking waits (ffprobe, AirPlay pairing HTTP);
    // running those inside the QML click handler's JS evaluation lets GUI
    // timers (e.g. toast expiry → card.destroy()) run reentrantly, which
    // is fatal to the QML engine (SIGABRT in QQmlData/~QObject).
    Q_INVOKABLE void requestCast(int deviceIndex, const QString &filePath,
                                 double position = 0.0);
    // Mirror-probe entry for QML (AirPlay rows): deferred like requestCast,
    // then runs the DoubleTake-style handshake probe on the next event-loop
    // turn. No file needed — diagnostics only (verify → SETUP → dataPort).
    Q_INVOKABLE void requestMirrorProbe(int deviceIndex);

    // Cast `filePath` to the renderer at `deviceIndex`, optionally seeking to
    // `position` seconds right after Play. Emits `notice` on failure.
    Q_INVOKABLE bool cast(int deviceIndex, const QString &filePath,
                          double position = 0.0);
    Q_INVOKABLE void castSeek(double position);
    Q_INVOKABLE void stopCast();
    Q_INVOKABLE void resumeCast();

    // True when the given media path can be served to a LAN renderer.
    Q_INVOKABLE static bool isCastingCapable(const QString &filePath);
    // Transport of a listed device: "dlna" | "googlecast" | "airplay".
    Q_INVOKABLE QString deviceType(int deviceIndex) const;
    // Stable device identity ("type/host/port") — the list is re-sorted on
    // every discovery hit, so row indices shift; the active/converting
    // target is tracked by key, resolved to a fresh index on use.
    static QString deviceKey(const QVariantMap &dev);
    int findDevice(const QString &key) const;
    void setActive(int deviceIndex);
    // True while a cast conversion (ffmpeg) is running.
    Q_PROPERTY(bool converting READ converting NOTIFY convertingChanged)
    bool converting() const { return m_convertProc != nullptr; }
    // 0..1 conversion progress (ffmpeg out_time vs duration).
    Q_PROPERTY(double convertProgress READ convertProgress NOTIFY convertProgressChanged)
    double convertProgress() const { return m_convertProgress; }
    // True while a live HLS session is starting (playlist not ready yet).
    Q_PROPERTY(bool hlsBusy READ hlsBusy NOTIFY hlsBusyChanged)
    bool hlsBusy() const { return m_hlsBusy; }
    // True while an AirPlay PIN is awaited from the user (see CastPanel).
    Q_PROPERTY(bool airplayPairing READ airplayPairing NOTIFY airplayPairingChanged)
    bool airplayPairing() const { return m_airplayPairing; }
    // Complete AirPlay pairing with the on-TV PIN, then play pending cast.
    Q_INVOKABLE void finishAirPlayPair(const QString &pin);
    Q_INVOKABLE void cancelAirPlayPair();
    void finishAirPlayPairNow(const QString &pin);
    // Screen mirroring via the DoubleTake backend (external `doubletake`
    // sender, AUR `doubletake-bin`). Proven against LG webOS AirPlay where
    // the native URL-flow (/play → 404) and the native mirror handshake
    // (stream SETUP stall) both fail. The daemon pairs once (PIN row is
    // shared), then streams the desktop; audio starts MUTED (09-22 lesson).
    Q_PROPERTY(bool dtAvailable READ dtAvailable NOTIFY dtStateChanged)
    Q_PROPERTY(QString dtState READ dtState NOTIFY dtStateChanged)
    Q_PROPERTY(bool dtPairing READ dtPairing NOTIFY dtStateChanged)
    Q_PROPERTY(bool dtStreaming READ dtStreaming NOTIFY dtStateChanged)
    Q_PROPERTY(bool dtMuted READ dtMuted NOTIFY dtStateChanged)
    Q_PROPERTY(QString dtHost READ dtHost NOTIFY dtStateChanged)
    bool dtAvailable() const { return m_dtAvailable; }
    QString dtState() const { return m_dtState; }
    bool dtPairing() const { return m_dtState == QLatin1String("pin_required"); }
    bool dtStreaming() const { return m_dtState == QLatin1String("streaming"); }
    bool dtMuted() const { return m_dtMuted; }
    QString dtHost() const { return m_dtHost; }
    Q_INVOKABLE void requestDtMirror(int deviceIndex);
    Q_INVOKABLE void submitDtPin(const QString &pin);
    Q_INVOKABLE void stopDtMirror();
    Q_INVOKABLE void toggleDtMute();
signals:
    void dtStateChanged();
signals:
    void convertingChanged();
    void convertProgressChanged();
    void hlsBusyChanged();
    void airplayPairingChanged();

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
                   bool hasRange, qint64 rangeEnd = -1);
    QString localIp() const;
    bool ensureServer();
    // Protocol dispatch for the unified device list.
    void castGoogle(int deviceIndex, double position);
    void castAirPlay(int deviceIndex, double position);
    void onGcastLoaded(bool ok);
    // Chromecast-safe conversion: probe + ffmpeg to a cached stereo MP4.
    // DLNA needs none of this (TVs play MKV/E-AC-3 fine) — only Cast/AirPlay.
    void continueCast(int deviceIndex, const QString &localPath,
                      double position);
    bool needsConversion(const QString &path) const;
    QString conversionCachePath(const QString &path) const;
    void startConversion(int deviceIndex, const QString &path,
                         double position);
    void cancelConversion();
    // Live HLS (no full pre-transcode): start session, serve playlist.
    void startHls(int deviceIndex, const QString &path, double position);
    void stopHls();
    // AirPlay 2 playback: verify pairing, then SETUP/RECORD//play on
    // the HAP-encrypted control channel (see AirPlaySession).
    void postAirPlayPlay(const QVariantMap &dev, double position);
    // Mirror handshake probe (DoubleTake-style negotiation, diagnostics
    // only — needs an existing pairing, see requestMirrorProbe).
    void probeMirror(int deviceIndex);
    // DoubleTake backend internals (see requestDtMirror). All ctl calls are
    // short blocking QProcess runs; entries are deferred past QML handlers.
    QJsonObject dtCtl(const QStringList &args, int timeoutMs = 15000);
    bool ensureDtDaemon();
    void dtStart(const QString &host);
    void pollDt();
    void dtSetState(const QString &state);

    // SSDP: the M-SEARCH replies are read here (unicast back to our socket
    // and multicast announcements) and their LOCATION headers collected.
    void handleSsdp();
    // mDNS resolvers: on top of SSDP we also ask DNS-SD `_mediarender._tcp`
    // because many TVs only advertise DLNA there and never answer M-SEARCH.
    // Google Cast (`_googlecast._tcp`) and AirPlay (`_airplay._tcp`) share
    // the same socket; their SRV/TXT/A answers resolve straight into the
    // device list (no SCDP fetch needed).
    void startMdns();
    void handleMdns();
    void sendDiscoveryProbes();
    void sendMdnsQuery(int type, const QString &name);
    void mdnsTryResolve(const QString &instance);
    void mdnsTryResolveCast(const QString &instance, const QString &type);
    void sortDevices();
    // Feeds a device-description URL into the fetch queue (deduplicated).
    void appendLocation(const QString &location);
    // Sends one unicast M-SEARCH and (maloptional) schedules a drain.
    void flushPending();

    QVariantList m_devices;      // [{name, host, port, controlUrl, baseUrl, type}]
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
    // Same SRV/TXT/A machinery for Cast + AirPlay service instances.
    QStringList m_gcastInstances;
    QStringList m_airplayInstances;
    QTimer *m_drainTimer = nullptr;      // late-arrival drain after the burst
    QTimer *m_queryTimer = nullptr;      // re-broadcast probes while open
    int m_emptyRounds = 0;               // auto-retried empty rounds so far
    quint16 m_mdnsQueryId = 0;

    QTcpServer *m_server = nullptr;
    bool m_serverRunning = false;
    QString m_castUrl;           // URL announced to the renderer
    QString m_castPath;          // canonical local path currently served
    QList<QTcpSocket *> m_connections;
    int m_activeDevice = -1;
    QString m_activeKey;         // stable id of the active target
    QString m_activeType;        // transport of the active cast session
    GoogleCastClient *m_gcast = nullptr;
    QNetworkAccessManager *m_net = nullptr;
    // Pending cast conversion (ffmpeg → cached MP4, then continueCast).
    QProcess *m_convertProc = nullptr;
    QString m_convertKey;        // stable id of the conversion target
    QString m_convertSrc;
    double m_convertPos = 0.0;
    double m_convertProgress = 0.0;
    double m_convertDuration = 0.0; // seconds, progress base
    // AirPlay 2 session (verify + encrypted control channel + RTSP /play).
    // Replaces the legacy open-/play path LG-class TVs reject with 403.
    class AirPlaySession *m_air = nullptr;
    // AirPlay pairing assistant + pending gated cast.
    class AirPlayPairing *m_pair = nullptr;
    bool m_airplayPairing = false;
    QString m_pendingAirKey;
    double m_pendingAirPos = 0.0;
    // Live HLS session (replaces file conversion when starting).
    HlsSession *m_hls = nullptr;
    bool m_hlsBusy = false;  // starting (playlist not ready)
    bool m_hlsMode = false;  // current cast streams via HLS playlist
    QString m_hlsKey;        // stable id of the HLS target
    QString m_hlsSrc;
    double m_hlsPos = 0.0;
    // DoubleTake mirror backend state (external sender process).
    bool m_dtAvailable = false; // doubletake + doubletake-ctl on PATH
    QString m_dtState = QStringLiteral("idle"); // idle|connecting|pin_required|streaming
    QString m_dtHost;                           // mirror target in flight
    bool m_dtMuted = false;
    bool m_dtMuteSent = false; // mute pushed right after streaming starts
    QTimer *m_dtTimer = nullptr; // 2 s status poll while busy/streaming
};
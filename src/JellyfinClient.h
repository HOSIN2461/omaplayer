#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QJsonObject>
#include <QNetworkRequest>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// Jellyfin media-server client (REST, direct play — no transcoding).
//
// Talks to any self-hosted Jellyfin server: credentials for multiple servers
// are persisted in QSettings, an access token authenticates the user (password
// flow), library browsing runs against the standard /Items endpoints, and
// playback uses the Direct Play stream URL handed straight to libmpv while
// resume/progress is reported back to the server. UI-facing items are flat
// QVariantMaps so QML can bind fields with dot access, mirroring the
// iina-jellyfin plugin's API usage.
class JellyfinClient : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QVariantList servers READ servers NOTIFY serversChanged)
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString activeServerName READ activeServerName NOTIFY connectionChanged)
    Q_PROPERTY(QString deviceId READ deviceId CONSTANT)
    // The item currently reported as playing (for the info overlay).
    Q_PROPERTY(QVariantMap playingItem READ playingItem NOTIFY playingItemChanged)

public:
    explicit JellyfinClient(QObject *parent = nullptr);

    QVariantList servers() const { return m_servers; }
    QVariantList items() const { return m_items; }
    QString status() const { return m_status; }
    bool busy() const { return m_busy; }
    QString activeServerName() const { return m_serverName; }
    QString deviceId() const { return m_deviceId; }
    QVariantMap playingItem() const { return m_trackItem; }

    // --- server management ---------------------------------------------
    Q_INVOKABLE void addServer(const QString &serverUrl);
    Q_INVOKABLE void removeServer(int index);
    Q_INVOKABLE void login(int serverIndex, const QString &username,
                           const QString &password);
    Q_INVOKABLE void logout();
    // Re-activate a previously logged-in server (restored token) and load its views.
    Q_INVOKABLE void selectServer(int index);

    // --- library browsing ----------------------------------------------
    Q_INVOKABLE void fetchViews();      // user's libraries (Movies, Series…)
    Q_INVOKABLE void fetchItems(const QString &parentId, int limit = 120);
    Q_INVOKABLE void fetchResume();     // Continue Watching
    Q_INVOKABLE void fetchNextUp();     // Up Next for series
    Q_INVOKABLE void fetchSeasons(const QString &seriesId);
    Q_INVOKABLE void fetchEpisodes(const QString &seriesId,
                                   const QString &seasonId);
    Q_INVOKABLE void fetchSearch(const QString &query);

    // --- playback ------------------------------------------------------
    Q_INVOKABLE void playItem(const QVariantMap &item);
    Q_INVOKABLE void stopPlayback();
    Q_INVOKABLE QUrl imageUrl(const QString &itemId, const QString &imageType,
                              int maxWidth = 300) const;
    Q_INVOKABLE QString streamUrl(const QString &itemId,
                                  const QString &type = QStringLiteral("Videos")) const;

signals:
    void serversChanged();
    void itemsChanged();
    void statusChanged();
    void busyChanged();
    void connectionChanged();
    void playingItemChanged();

private:
    void persistServers();
    void setStatus(const QString &s);
    void setBusy(bool b);

    void get(const QUrl &url, const std::function<void(const QJsonObject &)> &cb,
         const std::function<void()> &onError = {});
    void post(const QUrl &url, const QJsonObject &body,
              const std::function<void(const QJsonObject &)> &cb);
    QNetworkRequest request(const QUrl &url) const;
    void finishRequest(QNetworkReply *reply);

    void fetchAsItems(const QString &url);
    QVariantMap normalizeItem(const QJsonObject &o) const;
    QString displayTitle(const QVariantMap &item) const;
    void advanceToNextEpisode();
    void startPlayback(const QVariantMap &item);

    QString baseUrl() const;
    QString token() const;
    QString unitId() const { return m_active.value(QStringLiteral("userId")).toString(); }

    // playback progress tracking
    void reportTick();
    void reportStop();

    QNetworkAccessManager *m_nam;
    QVariantList m_servers;
    QVariantList m_items;
    QString m_status;
    QString m_serverName;
    QVariantMap m_active;      // selected {url, name, token, userId, username}
    QString m_deviceId;
    bool m_busy = false;

    QTimer *m_progressTimer = nullptr;
    QTimer *m_statusTimer = nullptr;
    int m_netInFlight = 0;
    bool m_tracking = false;
    bool m_advancing = false;
    QString m_trackItemId;
    QString m_trackSeriesId;
    QString m_trackSession;
    QString m_trackMediaSource;
    double m_lastPositionSeconds = 0.0;
    QVariantMap m_trackItem;      // last started item (info overlay)
};
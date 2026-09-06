#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QJsonObject>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

// Unified metadata for the pause-overlay info card.
//
// Two feeds power it:
//  * Jellyfin items — the server already holds the TMDb/IMDb enriched data
//    (overview, genres, rating, poster/backdrop, TMDb/IMDb ids), so nothing
//    is looked up again and no API key is needed. The overlay just renders
//    the normalized item (forJellyfin).
//  * Local files — provider chain. Each provider is tried in order and the
//    first hit wins: TMDb (needs the user's free API key, best + Hungarian),
//    then TVMaze (keyless, TV shows/episodes), then iTunes (keyless, movies).
//    The order and which providers are used are user-configurable (QSettings
//    "metadata/primary" + "metadata/use_<name>"). A picked TMDb item is
//    remembered per file path so reopening a file skips the search.
//
// The exposed QVariantMap uses stable keys so qml can bind dot paths:
//   title, subtitle, type("movie"|"series"|"episode"), year, rating,
//   genres, overview, airDate, posterUrl, backdropUrl, linkUrl, state.
// state is "" while idle, "ok" with data, "needkey" (TMDb is the only
// enabled provider but its key is missing), "notfound" (no provider had a
// hit) or "error".
class MetadataInfo : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QVariantMap info READ info NOTIFY infoChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString tmdbKey READ tmdbKey WRITE setTmdbKey NOTIFY tmdbKeyChanged)
    Q_PROPERTY(bool overlayEnabled READ overlayEnabled WRITE setOverlayEnabled NOTIFY overlayEnabledChanged)
    Q_PROPERTY(QVariantMap providers READ providers NOTIFY providersChanged)

public:
    explicit MetadataInfo(QObject *parent = nullptr);

    QVariantMap info() const { return m_info; }
    bool busy() const { return m_busy; }
    bool overlayEnabled() const;
    void setOverlayEnabled(bool on);
    QString tmdbKey() const;
    void setTmdbKey(const QString &key);

    // Providers config: map with "primary" (shared string) and a bool per
    // provider name ("tmdb", "tvmaze", "itunes"). persist in QSettings.
    QVariantMap providers() const;
    Q_INVOKABLE void setPrimaryProvider(const QString &name);
    Q_INVOKABLE void setProviderEnabled(const QString &name, bool on);

    // Builds the info map straight from a Jellyfin item normalized by
    // JellyfinClient (it already carries overview/genres/rating/provider ids).
    // image URLs come from the client (it knows the server baseUrl + token).
    Q_INVOKABLE void forJellyfin(const QVariantMap &item,
                                 const QString &posterUrl,
                                 const QString &backdropUrl);
    // Provider-chain lookup for a plain local file.
    Q_INVOKABLE void forLocalFile(const QString &filePath);
    Q_INVOKABLE void clear();

signals:
    void infoChanged();
    void busyChanged();
    void tmdbKeyChanged();
    void overlayEnabledChanged();
    void providersChanged();

private:
    // Chain bookkeeping for one local-file lookup. Lives on the heap (shared)
    // so async provider callbacks can chase the "try next" chain with it.
    struct LocalCtx
    {
        QString filePath, title, apiKey, rememberedType, memoryKey;
        int season = 0, ep = 0, movieYear = 0, rememberedId = 0;
        QStringList providers;
    };

    void setBusy(bool b);
    void setInfo(QVariantMap info);

    void get(const QString &url, const std::function<void(const QJsonObject &)> &cb);
    void finish(QNetworkReply *reply);
    QStringList orderedProviders() const;

    void advance(const std::shared_ptr<LocalCtx> &ctx, int idx);
    void tmdbTry(const std::shared_ptr<LocalCtx> &ctx, int idx);
    void tvmazeTry(const std::shared_ptr<LocalCtx> &ctx, int idx);
    void itunesTry(const std::shared_ptr<LocalCtx> &ctx, int idx);

    QVariantMap buildFromTmdb(const QJsonObject &media,
                              const QJsonObject *episode = nullptr,
                              const QJsonObject *series = nullptr);
    QVariantMap buildFromTvmaze(const QJsonObject &show, const QJsonObject &episode);
    QVariantMap buildFromItunes(const QJsonObject &r, bool episode,
                                int sNum = 0, int eNum = 0);

    QNetworkAccessManager *m_nam;
    QVariantMap m_info;
    bool m_busy = false;
};
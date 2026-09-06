#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QJsonObject>

class QNetworkAccessManager;
class QNetworkReply;

// Unified metadata for the pause-overlay info card.
//
// Two sources feed it:
//  * Jellyfin items — the server already holds the TMDb/IMDb enriched data
//    (overview, genres, rating, poster/backdrop, TMDb/IMDb ids), so nothing
//    is looked up again and no API key is needed. The overlay just renders
//    the normalized item (forJellyfin).
//  * Local files — a best-guess TMDb lookup (search/movie or search/tv plus
//    season/episode when the filename carries an SxxEyy marker). Needs the
//    user's free TMDb API key (stored in QSettings). The chosen TMDb item is
//    remembered per file path so reopening a file skips the search.
//
// The exposed QVariantMap uses stable keys so qml can bind dot paths:
//   title, subtitle, type("movie"|"series"|"episode"), year, rating,
//   genres, overview, airDate, posterUrl, backdropUrl, linkUrl, state.
// state is "" while idle, "ok" with data, "needkey" (TMDb key missing),
// "notfound" (TMDb had no good hit) or "error".
class MetadataInfo : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QVariantMap info READ info NOTIFY infoChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString tmdbKey READ tmdbKey WRITE setTmdbKey NOTIFY tmdbKeyChanged)
    Q_PROPERTY(bool overlayEnabled READ overlayEnabled WRITE setOverlayEnabled NOTIFY overlayEnabledChanged)

public:
    explicit MetadataInfo(QObject *parent = nullptr);

    QVariantMap info() const { return m_info; }
    bool busy() const { return m_busy; }
    bool overlayEnabled() const;
    void setOverlayEnabled(bool on);
    QString tmdbKey() const;
    void setTmdbKey(const QString &key);

    // Builds the info map straight from a Jellyfin item normalized by
    // JellyfinClient (it already carries overview/genres/rating/provider ids).
    // image URLs come from the client (it knows the server baseUrl + token).
    Q_INVOKABLE void forJellyfin(const QVariantMap &item,
                                 const QString &posterUrl,
                                 const QString &backdropUrl);
    // TMDb path for a plain local file.
    Q_INVOKABLE void forLocalFile(const QString &filePath);
    Q_INVOKABLE void clear();

signals:
    void infoChanged();
    void busyChanged();
    void tmdbKeyChanged();
    void overlayEnabledChanged();

private:
    void setBusy(bool b);
    void setInfo(QVariantMap info);

    void get(const QString &url, const std::function<void(const QJsonObject &)> &cb);
    void finish(QNetworkReply *reply);

    void buildFromTmdb(const QJsonObject &media,
                       const QJsonObject *episode = nullptr,
                       const QJsonObject *series = nullptr);
    void lookupLocal(const QString &filePath, const QString &title,
                     int season, int ep, const QString &apiKey,
                     const QString &mediaType, int mediaId,
                     int movieYear, const QString &key);

    QNetworkAccessManager *m_nam;
    QVariantMap m_info;
    bool m_busy = false;
};
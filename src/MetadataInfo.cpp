#include "MetadataInfo.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QUrl>
#include <QUrlQuery>
#include <QSettings>
#include <QFileInfo>
#include <QRegularExpression>
#include <QDebug>

#include <functional>

namespace {

const QStringList kGenresFrom(const QJsonObject &o)
{
    QStringList genres;
    const QJsonArray a = o.value(QLatin1String("genres")).toArray();
    for (const QJsonValue &v : a)
        genres.append(v.toObject().value(QLatin1String("name")).toString());
    return genres;
}

// TMDB v3 api. language=hu-HU where the user's locale is Hungarian.
QString apiUrl(const QString &pathWithQuery, const QString &key)
{
    return QStringLiteral("https://api.themoviedb.org/3/") + pathWithQuery
        + (pathWithQuery.contains(QLatin1Char('?')) ? QLatin1Char('&') : QLatin1Char('?'))
        + QStringLiteral("api_key=") + key
        + QStringLiteral("&language=hu-HU");
}

QString imageUrl(const QString &path, const QString &size)
{
    return path.isEmpty() ? QString() : QStringLiteral("https://image.tmdb.org/t/p/")
        + size + path;
}

} // namespace

MetadataInfo::MetadataInfo(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

QString MetadataInfo::tmdbKey() const
{
    // Test hook so a key can be injected without touching QSettings.
    const QString env = qEnvironmentVariable("OMAPLAYER_TMDB_KEY");
    if (!env.isEmpty())
        return env;
    return QSettings().value(QStringLiteral("metadata/tmdbKey")).toString();
}

void MetadataInfo::setTmdbKey(const QString &key)
{
    if (tmdbKey() == key)
        return;
    QSettings().setValue(QStringLiteral("metadata/tmdbKey"), key.trimmed());
    Q_EMIT tmdbKeyChanged();
}

bool MetadataInfo::overlayEnabled() const
{
    return QSettings().value(QStringLiteral("metadata/overlayEnabled"), true).toBool();
}

void MetadataInfo::setOverlayEnabled(bool on)
{
    if (on == overlayEnabled())
        return;
    QSettings().setValue(QStringLiteral("metadata/overlayEnabled"), on);
    Q_EMIT overlayEnabledChanged();
}

void MetadataInfo::setBusy(bool b)
{
    if (m_busy == b)
        return;
    m_busy = b;
    Q_EMIT busyChanged();
}

void MetadataInfo::setInfo(QVariantMap info)
{
    m_info = std::move(info);
    Q_EMIT infoChanged();
}

void MetadataInfo::clear()
{
    setInfo({});
}

void MetadataInfo::get(const QString &url,
                       const std::function<void(const QJsonObject &)> &cb)
{
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("omaplayer/" OMAPLAYER_VERSION));
    req.setTransferTimeout(15000);
    setBusy(true);
    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, cb] {
        const bool ok = reply->error() == QNetworkReply::NoError;
        finish(reply);
        const QJsonObject obj = ok
            ? QJsonDocument::fromJson(reply->readAll()).object()
            : QJsonObject();
        cb(obj);
    });
}

void MetadataInfo::finish(QNetworkReply *reply)
{
    reply->deleteLater();
    setBusy(false);
}

void MetadataInfo::forJellyfin(const QVariantMap &item,
                               const QString &posterUrl,
                               const QString &backdropUrl)
{
    const QString type = item.value(QStringLiteral("type")).toString();
    const bool episode = type == QLatin1String("Episode");
    const int season = item.value(QStringLiteral("parentIndexNumber")).toInt();
    const int ep = item.value(QStringLiteral("indexNumber")).toInt();

    QVariantMap out;
    out.insert(QStringLiteral("type"),
               episode ? QStringLiteral("episode")
                       : (type == QLatin1String("Series") ? QStringLiteral("series")
                                                          : QStringLiteral("movie")));
    out.insert(QStringLiteral("title"),
               episode ? item.value(QStringLiteral("seriesName")).toString()
                       : item.value(QStringLiteral("name")).toString());
    QString subtitle;
    if (episode) {
        if (season > 0 && ep > 0)
            subtitle = QStringLiteral("S%1E%2").arg(season, 2, 10, QLatin1Char('0'))
                                                 .arg(ep, 2, 10, QLatin1Char('0'));
        const QString epName = item.value(QStringLiteral("name")).toString();
        if (!epName.isEmpty()) {
            if (!subtitle.isEmpty())
                subtitle += QLatin1String("  •  ");
            subtitle += epName;
        }
    }
    if (subtitle.isEmpty())
        subtitle = item.value(QStringLiteral("name")).toString();
    out.insert(QStringLiteral("subtitle"), subtitle);
    out.insert(QStringLiteral("year"),
               item.value(QStringLiteral("productionYear")).toInt());

    QStringList genres;
    const QVariantList gl = item.value(QStringLiteral("genres")).toList();
    for (const QVariant &g : gl)
        genres.append(g.toString());
    out.insert(QStringLiteral("genres"), genres.join(QStringLiteral(", ")));

    const double rating = item.value(QStringLiteral("communityRating")).toDouble();
    if (rating > 0.0)
        out.insert(QStringLiteral("rating"), rating);
    const QString overview = item.value(QStringLiteral("overview")).toString();
    if (!overview.isEmpty())
        out.insert(QStringLiteral("overview"), overview);

    out.insert(QStringLiteral("posterUrl"), posterUrl);
    out.insert(QStringLiteral("backdropUrl"), backdropUrl);

    QString link;
    const QString tmdb = item.value(QStringLiteral("tmdbId")).toString();
    const QString imdb = item.value(QStringLiteral("imdbId")).toString();
    if (!tmdb.isEmpty()) {
        link = QStringLiteral("https://www.themoviedb.org/")
            + (episode || type == QLatin1String("Series") ? QStringLiteral("tv/")
                                                          : QStringLiteral("movie/"))
            + tmdb;
    } else if (!imdb.isEmpty()) {
        link = QStringLiteral("https://www.imdb.com/title/") + imdb;
    }
    if (!link.isEmpty())
        out.insert(QStringLiteral("linkUrl"), link);

    out.insert(QStringLiteral("state"), QStringLiteral("ok"));
    out.insert(QStringLiteral("source"), QStringLiteral("jellyfin"));
    setInfo(std::move(out));
    qInfo() << "metadata: jellyfin" << out.value("title").toString()
            << out.value("year").toInt()
            << "rating" << out.value("rating").toDouble()
            << "genres" << out.value("genres").toString()
            << "link" << out.value("linkUrl").toString();
}

void MetadataInfo::forLocalFile(const QString &filePath)
{
    if (filePath.isEmpty()) {
        clear();
        return;
    }
    const QFileInfo fi(filePath);
    const QString base = fi.completeBaseName();

    // --- parse the filename ---------------------------------------------
    int season = 0, ep = 0;
    int movieYear = 0;
    QString title = base;
    const QRegularExpression rxSxxEyy(QStringLiteral("S(\\d{1,2})E(\\d{1,3})"),
                                      QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch sm = rxSxxEyy.match(base);
    if (sm.hasMatch()) {
        season = sm.captured(1).toInt();
        ep = sm.captured(2).toInt();
        title = base.left(sm.capturedStart());
    } else {
        // Movie: pull "Title (Year)" / "Title [Year]" / "Title.2021." off
        // the filename and drop the usual release-group tags.
        QRegularExpression rxYear(QStringLiteral("\\(?(\\d{4})\\)?\\s*$"));
        QRegularExpressionMatch ym = rxYear.match(title);
        if (ym.hasMatch()) {
            movieYear = ym.captured(1).toInt();
            title = title.left(ym.capturedStart());
        }
        QRegularExpression rxTags(
            QStringLiteral("[.\\[ ](?:(?:19|20)\\d{2}|1080p|720p|2160p|4k|web[ -]?rip|bluray|brrip|hdtv|hdrip|x265|x264|h\\d{3})[.\\[ ]"),
            QRegularExpression::CaseInsensitiveOption);
        int cut = title.size();
        for (const QRegularExpressionMatch &m : rxTags.globalMatch(title)) {
            if (m.capturedStart() < cut)
                cut = m.capturedStart();
        }
        if (cut < title.size())
            title = title.left(cut).trimmed();
    }
    title = title.replace(QRegularExpression(QStringLiteral("[\\._]")),
                          QStringLiteral(" ")).trimmed();
    if (title.length() < 2) {
        QVariantMap e;
        e.insert(QStringLiteral("state"), QStringLiteral("notfound"));
        setInfo(std::move(e));
        return;
    }

    // --- per-path memory + tmdb flow ------------------------------------
    const QString key = QStringLiteral("metadata/paths/")
        + QString::number(qHash(filePath));
    QSettings settings;
    const QVariantMap remembered = settings.value(key).toMap();
    const QString mediaType =
        remembered.value(QStringLiteral("mediaType")).toString();
    const int mediaId = remembered.value(QStringLiteral("mediaId")).toInt();

    const QString apiKey = tmdbKey();
    if (apiKey.isEmpty()) {
        QVariantMap needKey;
        needKey.insert(QStringLiteral("title"), title);
        needKey.insert(QStringLiteral("state"), QStringLiteral("needkey"));
        setInfo(std::move(needKey));
        qInfo() << "metadata: needkey for" << title << season << ep;
        return;
    }

    lookupLocal(filePath, title, season, ep, apiKey,
                mediaType, mediaId, movieYear, key);
}

void MetadataInfo::buildFromTmdb(const QJsonObject &media,
                                 const QJsonObject *episode,
                                 const QJsonObject *series)
{
    const bool isTv = media.value(QLatin1String("first_air_date")).isString()
        || episode || series;
    const int tmdbId = isTv ? series ? series->value(QLatin1String("id")).toInt()
                                     : media.value(QLatin1String("id")).toInt()
                            : media.value(QLatin1String("id")).toInt();

    QVariantMap out;
    out.insert(QStringLiteral("type"),
               episode ? QStringLiteral("episode")
                       : (isTv ? QStringLiteral("series") : QStringLiteral("movie")));
    out.insert(QStringLiteral("title"),
               episode ? (series ? series->value(QLatin1String("name")).toString()
                                 : QString())
                       : (isTv ? media.value(QLatin1String("name")).toString()
                               : media.value(QLatin1String("title")).toString()));

    QString subtitle;
    if (episode) {
        const int s = episode->value(QLatin1String("season_number")).toInt();
        const int e = episode->value(QLatin1String("episode_number")).toInt();
        if (s > 0 && e > 0)
            subtitle = QStringLiteral("S%1E%2").arg(s, 2, 10, QLatin1Char('0'))
                                               .arg(e, 2, 10, QLatin1Char('0'));
        const QString epName = episode->value(QLatin1String("name")).toString();
        if (!epName.isEmpty()) {
            if (!subtitle.isEmpty())
                subtitle += QLatin1String("  •  ");
            subtitle += epName;
        }
    }
    out.insert(QStringLiteral("subtitle"), subtitle);

    QString date = isTv ? media.value(QLatin1String("first_air_date")).toString()
                        : media.value(QLatin1String("release_date")).toString();
    if (episode && !episode->value(QLatin1String("air_date")).toString().isEmpty())
        date = episode->value(QLatin1String("air_date")).toString();
    const int year = date.left(4).toInt();
    if (year > 0)
        out.insert(QStringLiteral("year"), year);

    const QStringList genres = kGenresFrom(media);
    if (!genres.isEmpty())
        out.insert(QStringLiteral("genres"), genres.join(QStringLiteral(", ")));

    const double rating = media.value(QLatin1String("vote_average")).toDouble();
    if (rating > 0.0)
        out.insert(QStringLiteral("rating"), rating);

    QString overview = episode && !episode->value(QLatin1String("overview")).toString().isEmpty()
        ? episode->value(QLatin1String("overview")).toString()
        : media.value(QLatin1String("overview")).toString();
    if (!overview.isEmpty())
        out.insert(QStringLiteral("overview"), overview);

    out.insert(QStringLiteral("posterUrl"),
               imageUrl(media.value(QLatin1String("poster_path")).toString(),
                        QStringLiteral("w500")));
    out.insert(QStringLiteral("backdropUrl"),
               imageUrl((series ? series->value(QLatin1String("backdrop_path"))
                                : media.value(QLatin1String("backdrop_path"))).toString(),
                        QStringLiteral("w1280")));

    const QString kind = episode ? QStringLiteral("tv") : (isTv ? QStringLiteral("tv") : QStringLiteral("movie"));
    out.insert(QStringLiteral("linkUrl"),
               QStringLiteral("https://www.themoviedb.org/") + kind + QLatin1Char('/')
                   + QString::number(tmdbId));

    out.insert(QStringLiteral("state"), QStringLiteral("ok"));
    out.insert(QStringLiteral("source"), QStringLiteral("tmdb"));
    qInfo() << "metadata: tmdb built" << out.value("title").toString()
            << out.value("subtitle").toString()
            << "year" << out.value("year").toInt()
            << "rating" << out.value("rating").toDouble()
            << "poster" << out.value("posterUrl").toString()
            << "link" << out.value("linkUrl").toString();
    setInfo(std::move(out));
}

void MetadataInfo::lookupLocal(const QString &filePath,
                               const QString &title,
                               int season, int ep,
                               const QString &apiKey,
                               const QString &rememberedType,
                               int rememberedId,
                               int movieYear,
                               const QString &memoryKey)
{
    const auto remember = [this, memoryKey](const QString &type, int id) {
        QVariantMap m;
        m.insert(QStringLiteral("mediaType"), type);
        m.insert(QStringLiteral("mediaId"), id);
        QSettings().setValue(memoryKey, m);
    };

    const auto finishNotFound = [this](QVariantMap out) {
        if (!out.contains(QStringLiteral("state")))
            out.insert(QStringLiteral("state"), QStringLiteral("notfound"));
        setInfo(std::move(out));
    };

    if (!rememberedType.isEmpty() && rememberedId > 0) {
        // Known id — go straight to the detail/episode endpoints.
        if (rememberedType == QLatin1String("movie")) {
            get(apiUrl(QStringLiteral("movie/") + QString::number(rememberedId), apiKey),
                [this](const QJsonObject &o) { buildFromTmdb(o); });
            return;
        }
        if (season > 0 && ep > 0) {
            const QString series = QString::number(rememberedId);
            get(apiUrl(QStringLiteral("tv/") + series + QStringLiteral("/season/")
                       + QString::number(season) + QStringLiteral("/episode/")
                       + QString::number(ep), apiKey),
                [this, series, apiKey](const QJsonObject &epJson) {
                    get(apiUrl(QStringLiteral("tv/") + series, apiKey),
                        [this, epJson](const QJsonObject &seriesJson) {
                            QJsonObject e = epJson;
                            buildFromTmdb(seriesJson, &e, &seriesJson);
                        });
                });
            return;
        }
        get(apiUrl(QStringLiteral("tv/") + QString::number(rememberedId), apiKey),
            [this](const QJsonObject &o) { buildFromTmdb(o); });
        return;
    }

    // --- search ---------------------------------------------------------
    if (season > 0 && ep > 0) {
        get(apiUrl(QStringLiteral("search/tv?query=")
                   + QUrl::toPercentEncoding(title) + QStringLiteral("&include_adult=false"), apiKey),
            [this, season, ep, apiKey, finishNotFound, remember](const QJsonObject &res) {
                const QJsonArray results = res.value(QLatin1String("results")).toArray();
                if (results.isEmpty()) {
                    finishNotFound({});
                    return;
                }
                const QJsonObject show = results.first().toObject();
                const int sid = show.value(QLatin1String("id")).toInt();
                remember(QStringLiteral("tv"), sid);
                const QString series = QString::number(sid);
                get(apiUrl(QStringLiteral("tv/") + series + QStringLiteral("/season/")
                           + QString::number(season) + QStringLiteral("/episode/")
                           + QString::number(ep), apiKey),
                    [this, series, apiKey](const QJsonObject &epJson) {
                        get(apiUrl(QStringLiteral("tv/") + series, apiKey),
                            [this, epJson](const QJsonObject &seriesJson) {
                                QJsonObject e = epJson;
                                buildFromTmdb(seriesJson, &e, &seriesJson);
                            });
                    });
            });
        return;
    }

    // Movie search.
    QVariantMap out;
    out.insert(QStringLiteral("title"), title);
    QString movieQuery = QStringLiteral("search/movie?query=")
        + QUrl::toPercentEncoding(title) + QStringLiteral("&include_adult=false");
    if (movieYear > 0)
        movieQuery += QStringLiteral("&year=") + QString::number(movieYear);
    get(apiUrl(movieQuery, apiKey),
        [this, out, finishNotFound, remember, apiKey](const QJsonObject &res) {
            const QJsonArray results = res.value(QLatin1String("results")).toArray();
            if (results.isEmpty()) {
                finishNotFound(out);
                return;
            }
            const QJsonObject first = results.first().toObject();
            remember(QStringLiteral("movie"), first.value(QLatin1String("id")).toInt());
            get(apiUrl(QStringLiteral("movie/")
                       + QString::number(first.value(QLatin1String("id")).toInt()), apiKey),
                [this](const QJsonObject &o) { buildFromTmdb(o); });
        });
}
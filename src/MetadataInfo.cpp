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

// TVMaze summaries are HTML — flatten to plain text.
QString htmlToText(const QString &src)
{
    QString t = src;
    t.replace(QRegularExpression(QStringLiteral("<br\\s*/?>")), QStringLiteral("\n"));
    t.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    t.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    t.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    t.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    t.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    t.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    return t.trimmed();
}

// iTunes artwork URLs are square blobs ("100x100bb") — scale the poster up.
QString bigItunesImage(QString url)
{
    return url.contains(QLatin1String("100x100"))
        ? url.replace(QLatin1String("100x100"), QLatin1String("600x600"))
        : url;
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

QVariantMap MetadataInfo::providers() const
{
    QSettings s;
    QVariantMap m;
    m.insert(QStringLiteral("primary"),
             s.value(QStringLiteral("metadata/primary"), QStringLiteral("tmdb")).toString());
    for (const QString &p : QStringList{QStringLiteral("tmdb"),
                                        QStringLiteral("tvmaze"),
                                        QStringLiteral("itunes")})
        m.insert(p, s.value(QLatin1String("metadata/use_") + p, true).toBool());
    return m;
}

void MetadataInfo::setPrimaryProvider(const QString &name)
{
    if (name != QLatin1String("tmdb") && name != QLatin1String("tvmaze")
        && name != QLatin1String("itunes"))
        return;
    if (providers().value(QLatin1String("primary")).toString() == name)
        return;
    QSettings().setValue(QStringLiteral("metadata/primary"), name);
    Q_EMIT providersChanged();
}

void MetadataInfo::setProviderEnabled(const QString &name, bool on)
{
    if (name != QLatin1String("tmdb") && name != QLatin1String("tvmaze")
        && name != QLatin1String("itunes"))
        return;
    if (providers().value(name).toBool() == on)
        return;
    QSettings().setValue(QLatin1String("metadata/use_") + name, on);
    Q_EMIT providersChanged();
}

QVariantList MetadataInfo::providerOrder() const
{
    QVariantList l;
    for (const QString &p : orderedProviders())
        l.append(p);
    return l;
}

void MetadataInfo::moveProvider(const QString &name, int dir)
{
    QStringList o = orderedProviders();
    const int i = o.indexOf(name);
    if (i < 0)
        return;
    const int j = i + dir;
    if (j < 0 || j >= o.size())
        return;
    o.swapItemsAt(i, j);
    QSettings s;
    s.setValue(QStringLiteral("metadata/primary"), o.first());
    Q_EMIT providersChanged();
}

QStringList MetadataInfo::orderedProviders() const
{
    const QString primary = providers().value(QLatin1String("primary")).toString();
    QStringList order{QLatin1String("tmdb"), QLatin1String("tvmaze"),
                      QLatin1String("itunes")};
    order.removeAll(primary);
    order.prepend(primary);
    QStringList out;
    for (const QString &p : std::as_const(order))
        if (providers().value(p).toBool())
            out.append(p);
    return out;
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
                subtitle += QStringLiteral("  •  ");
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

    // --- provider chain ---------------------------------------------------
    auto ctx = std::make_shared<LocalCtx>();
    ctx->filePath = filePath;
    ctx->title = title;
    ctx->season = season;
    ctx->ep = ep;
    ctx->movieYear = movieYear;
    ctx->apiKey = tmdbKey();
    ctx->memoryKey = QStringLiteral("metadata/paths/")
        + QString::number(qHash(filePath));
    const QVariantMap remembered = QSettings().value(ctx->memoryKey).toMap();
    ctx->rememberedType = remembered.value(QStringLiteral("mediaType")).toString();
    ctx->rememberedId = remembered.value(QStringLiteral("mediaId")).toInt();
    ctx->providers = orderedProviders();

    if (ctx->providers.isEmpty()) {
        QVariantMap e;
        e.insert(QStringLiteral("title"), title);
        e.insert(QStringLiteral("state"), QStringLiteral("notfound"));
        setInfo(std::move(e));
        return;
    }
    advance(ctx, 0);
}

QVariantMap MetadataInfo::buildFromTmdb(const QJsonObject &media,
                                        const QJsonObject *episode,
                                        const QJsonObject *series)
{
    const bool isTv = media.value(QLatin1String("first_air_date")).isString()
        || episode || series;
    const int tmdbId = isTv ? (series ? series->value(QLatin1String("id")).toInt()
                                      : media.value(QLatin1String("id")).toInt())
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
    if (out.value(QStringLiteral("title")).toString().isEmpty())
        return {};

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
                subtitle += QStringLiteral("  •  ");
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
    return out;
}

void MetadataInfo::advance(const std::shared_ptr<LocalCtx> &ctx, int idx)
{
    if (idx >= ctx->providers.size()) {
        QVariantMap e;
        e.insert(QStringLiteral("title"), ctx->title);
        e.insert(QStringLiteral("state"), QStringLiteral("notfound"));
        e.insert(QStringLiteral("source"), QStringLiteral("none"));
        setInfo(std::move(e));
        return;
    }
    const QString p = ctx->providers[idx];
    if (p == QLatin1String("tmdb"))
        tmdbTry(ctx, idx);
    else if (p == QLatin1String("tvmaze"))
        tvmazeTry(ctx, idx);
    else if (p == QLatin1String("itunes"))
        itunesTry(ctx, idx);
    else
        advance(ctx, idx + 1);
}

void MetadataInfo::tmdbTry(const std::shared_ptr<LocalCtx> &ctx, int idx)
{
    const auto miss = [this, ctx, idx] { advance(ctx, idx + 1); };
    const QString &apiKey = ctx->apiKey;
    if (apiKey.isEmpty()) {
        if (ctx->providers.size() == 1) {
            QVariantMap e;
            e.insert(QStringLiteral("title"), ctx->title);
            e.insert(QStringLiteral("state"), QStringLiteral("needkey"));
            setInfo(std::move(e));
        } else {
            qInfo() << "metadata: skip tmdb (no key), trying next";
            miss();
        }
        return;
    }

    const auto remember = [ctx](const QString &type, int id) {
        QVariantMap m;
        m.insert(QStringLiteral("mediaType"), type);
        m.insert(QStringLiteral("mediaId"), id);
        QSettings().setValue(ctx->memoryKey, m);
    };
    const auto finishOk = [this, miss](const QJsonObject &media,
                                       const QJsonObject *episode = nullptr,
                                       const QJsonObject *series = nullptr) {
        const QVariantMap out = buildFromTmdb(media, episode, series);
        if (out.isEmpty())
            miss();
        else
            setInfo(out);
    };

    if (!ctx->rememberedType.isEmpty() && ctx->rememberedId > 0) {
        // Known id — go straight to the detail/episode endpoints.
        if (ctx->rememberedType == QLatin1String("movie")) {
            get(apiUrl(QStringLiteral("movie/") + QString::number(ctx->rememberedId), apiKey),
                [this, miss, finishOk](const QJsonObject &o) {
                    if (o.isEmpty())
                        miss();
                    else
                        finishOk(o);
                });
            return;
        }
        if (ctx->season > 0 && ctx->ep > 0) {
            const QString series = QString::number(ctx->rememberedId);
            get(apiUrl(QStringLiteral("tv/") + series + QStringLiteral("/season/")
                       + QString::number(ctx->season) + QStringLiteral("/episode/")
                       + QString::number(ctx->ep), apiKey),
                [this, series, apiKey, miss, finishOk](const QJsonObject &epJson) {
                    if (epJson.isEmpty()) {
                        miss();
                        return;
                    }
                    get(apiUrl(QStringLiteral("tv/") + series, apiKey),
                        [this, epJson, miss, finishOk](const QJsonObject &seriesJson) {
                            if (seriesJson.isEmpty()) {
                                miss();
                                return;
                            }
                            QJsonObject e = epJson;
                            finishOk(seriesJson, &e, &seriesJson);
                        });
                });
            return;
        }
        get(apiUrl(QStringLiteral("tv/") + QString::number(ctx->rememberedId), apiKey),
            [this, miss, finishOk](const QJsonObject &o) {
                if (o.isEmpty())
                    miss();
                else
                    finishOk(o);
            });
        return;
    }

    // --- search ---------------------------------------------------------
    if (ctx->season > 0 && ctx->ep > 0) {
        get(apiUrl(QStringLiteral("search/tv?query=")
                   + QUrl::toPercentEncoding(ctx->title)
                   + QStringLiteral("&include_adult=false"), apiKey),
            [this, ctx, apiKey, miss, finishOk, remember](const QJsonObject &res) {
                const QJsonArray results = res.value(QLatin1String("results")).toArray();
                if (results.isEmpty()) {
                    miss();
                    return;
                }
                const QJsonObject show = results.first().toObject();
                const int sid = show.value(QLatin1String("id")).toInt();
                remember(QStringLiteral("tv"), sid);
                const QString series = QString::number(sid);
                get(apiUrl(QStringLiteral("tv/") + series + QStringLiteral("/season/")
                           + QString::number(ctx->season) + QStringLiteral("/episode/")
                           + QString::number(ctx->ep), apiKey),
                    [this, series, apiKey, miss, finishOk](const QJsonObject &epJson) {
                        if (epJson.isEmpty()) {
                            miss();
                            return;
                        }
                        get(apiUrl(QStringLiteral("tv/") + series, apiKey),
                            [this, epJson, miss, finishOk](const QJsonObject &seriesJson) {
                                if (seriesJson.isEmpty()) {
                                    miss();
                                    return;
                                }
                                QJsonObject e = epJson;
                                finishOk(seriesJson, &e, &seriesJson);
                            });
                    });
            });
        return;
    }

    // Movie search.
    QString movieQuery = QStringLiteral("search/movie?query=")
        + QUrl::toPercentEncoding(ctx->title) + QStringLiteral("&include_adult=false");
    if (ctx->movieYear > 0)
        movieQuery += QStringLiteral("&year=") + QString::number(ctx->movieYear);
    get(apiUrl(movieQuery, apiKey),
        [this, miss, finishOk, remember, apiKey](const QJsonObject &res) {
            const QJsonArray results = res.value(QLatin1String("results")).toArray();
            if (results.isEmpty()) {
                miss();
                return;
            }
            const QJsonObject first = results.first().toObject();
            remember(QStringLiteral("movie"), first.value(QLatin1String("id")).toInt());
            get(apiUrl(QStringLiteral("movie/")
                       + QString::number(first.value(QLatin1String("id")).toInt()), apiKey),
                [this, miss, finishOk](const QJsonObject &o) {
                    if (o.isEmpty())
                        miss();
                    else
                        finishOk(o);
                });
        });
}

void MetadataInfo::tvmazeTry(const std::shared_ptr<LocalCtx> &ctx, int idx)
{
    const auto miss = [this, ctx, idx] { advance(ctx, idx + 1); };
    // TVMaze only knows TV — films fall through to the next provider.
    if (ctx->season <= 0 || ctx->ep <= 0) {
        qInfo() << "metadata: skip tvmaze (not a TV title), trying next";
        miss();
        return;
    }
    const QString url = QStringLiteral("https://api.tvmaze.com/singlesearch/shows?q=")
        + QUrl::toPercentEncoding(ctx->title);
    get(url, [this, ctx, miss](const QJsonObject &show) {
        if (show.isEmpty() || !show.contains(QLatin1String("id"))) {
            qInfo() << "metadata: tvmaze no show for" << ctx->title << ", trying next";
            miss();
            return;
        }
        const int sid = show.value(QLatin1String("id")).toInt();
        const QString url2 = QStringLiteral("https://api.tvmaze.com/shows/%1/episodebynumber"
                                            "?season=%2&number=%3")
            .arg(sid).arg(ctx->season).arg(ctx->ep);
        get(url2, [this, miss, show](const QJsonObject &ep) {
            if (ep.isEmpty()) {
                qInfo() << "metadata: tvmaze no episode, trying next";
                miss();
                return;
            }
            QVariantMap out = buildFromTvmaze(show, ep);
            if (out.isEmpty())
                miss();
            else
                setInfo(std::move(out));
        });
    });
}

void MetadataInfo::itunesTry(const std::shared_ptr<LocalCtx> &ctx, int idx)
{
    const auto miss = [this, ctx, idx] { advance(ctx, idx + 1); };
    const bool tv = ctx->season > 0 && ctx->ep > 0;
    QString url = QStringLiteral("https://itunes.apple.com/search?term=")
        + QUrl::toPercentEncoding(ctx->title);
    // Note: this API's media=movie/entity=movie filter returns 0 results these
    // days; movies come back as kind="feature-movie" on a bare search.
    url += tv ? QStringLiteral("&media=tvShow&entity=tvEpisode&limit=25")
              : QStringLiteral("&country=US&limit=50");
    get(url, [this, ctx, miss, tv](const QJsonObject &res) {
        const QJsonArray results = res.value(QLatin1String("results")).toArray();
        if (results.isEmpty()) {
            qInfo() << "metadata: itunes no hit for" << ctx->title << ", trying next";
            miss();
            return;
        }
        QJsonObject best;
        if (tv) {
            best = results.first().toObject();
        } else {
            // Feature-movie only; first hit is the relevance pick, an exact
            // release-year match (from the filename) wins if present.
            const auto isMovie = [](const QJsonObject &r) {
                const QString k = r.value(QLatin1String("kind")).toString();
                return k == QLatin1String("feature-movie") || k == QLatin1String("movie");
            };
            QJsonObject firstMovie, yearHit;
            for (const QJsonValue &v : results) {
                const QJsonObject r = v.toObject();
                if (!isMovie(r))
                    continue;
                if (firstMovie.isEmpty())
                    firstMovie = r;
                if (ctx->movieYear > 0 && yearHit.isEmpty()
                    && r.value(QLatin1String("releaseDate")).toString()
                           .left(4).toInt() == ctx->movieYear)
                    yearHit = r;
            }
            best = yearHit.isEmpty() ? firstMovie : yearHit;
        }
        if (best.isEmpty()) {
            qInfo() << "metadata: itunes no movie/episode entry for" << ctx->title
                    << ", trying next";
            miss();
            return;
        }
        const QVariantMap out = buildFromItunes(best, tv, ctx->season, ctx->ep);
        if (out.isEmpty())
            miss();
        else
            setInfo(out);
    });
}

QVariantMap MetadataInfo::buildFromTvmaze(const QJsonObject &show,
                                          const QJsonObject &episode)
{
    QVariantMap out;
    out.insert(QStringLiteral("type"), QStringLiteral("episode"));
    out.insert(QStringLiteral("title"), show.value(QLatin1String("name")).toString());
    if (out.value(QStringLiteral("title")).toString().isEmpty())
        return {};

    QString subtitle;
    const int s = episode.value(QLatin1String("season")).toInt();
    const int e = episode.value(QLatin1String("number")).toInt();
    if (s > 0 && e > 0)
        subtitle = QStringLiteral("S%1E%2").arg(s, 2, 10, QLatin1Char('0'))
                                           .arg(e, 2, 10, QLatin1Char('0'));
    const QString epName = episode.value(QLatin1String("name")).toString();
    if (!epName.isEmpty()) {
        if (!subtitle.isEmpty())
            subtitle += QStringLiteral("  •  ");
        subtitle += epName;
    }
    out.insert(QStringLiteral("subtitle"), subtitle);

    const QString prem = show.value(QLatin1String("premiered")).toString();
    if (!prem.isEmpty())
        out.insert(QStringLiteral("year"), prem.left(4).toInt());

    QStringList genres;
    const QJsonArray gs = show.value(QLatin1String("genres")).toArray();
    for (const QJsonValue &g : gs)
        genres.append(g.toString());
    if (!genres.isEmpty())
        out.insert(QStringLiteral("genres"), genres.join(QStringLiteral(", ")));

    const double rating = show.value(QLatin1String("rating")).toObject()
                              .value(QLatin1String("average")).toDouble();
    if (rating > 0.0)
        out.insert(QStringLiteral("rating"), rating);

    QString overview = episode.value(QLatin1String("summary")).toString();
    if (overview.isEmpty())
        overview = show.value(QLatin1String("summary")).toString();
    if (!overview.isEmpty())
        out.insert(QStringLiteral("overview"), htmlToText(overview));

    out.insert(QStringLiteral("posterUrl"),
               show.value(QLatin1String("image")).toObject()
                   .value(QLatin1String("medium")).toString());
    const QString imdb = show.value(QLatin1String("externals")).toObject()
                             .value(QLatin1String("imdb")).toString();
    out.insert(QStringLiteral("linkUrl"),
               imdb.isEmpty() ? show.value(QLatin1String("url")).toString()
                              : QStringLiteral("https://www.imdb.com/title/") + imdb);

    out.insert(QStringLiteral("state"), QStringLiteral("ok"));
    out.insert(QStringLiteral("source"), QStringLiteral("tvmaze"));
    qInfo() << "metadata: tvmaze built" << out.value("title").toString()
            << out.value("subtitle").toString()
            << "year" << out.value("year").toInt()
            << "link" << out.value("linkUrl").toString();
    return out;
}

QVariantMap MetadataInfo::buildFromItunes(const QJsonObject &r, bool episode,
                                          int sNum, int eNum)
{
    QVariantMap out;
    out.insert(QStringLiteral("type"),
               episode ? QStringLiteral("episode") : QStringLiteral("movie"));
    out.insert(QStringLiteral("title"),
               episode ? r.value(QLatin1String("collectionName")).toString()
                       : r.value(QLatin1String("trackName")).toString());
    if (out.value(QStringLiteral("title")).toString().isEmpty())
        return {};

    QString subtitle;
    if (episode) {
        // iTunes tvEpisode records usually lack season/episode numbers — the
        // filename's SxxEyy is the authoritative badge here.
        if (sNum > 0 && eNum > 0)
            subtitle = QStringLiteral("S%1E%2").arg(sNum, 2, 10, QLatin1Char('0'))
                                               .arg(eNum, 2, 10, QLatin1Char('0'));
        const QString epName = r.value(QLatin1String("trackName")).toString();
        if (!epName.isEmpty()) {
            if (!subtitle.isEmpty())
                subtitle += QStringLiteral("  •  ");
            subtitle += epName;
        }
    }
    out.insert(QStringLiteral("subtitle"), subtitle);

    const QString rd = r.value(QLatin1String("releaseDate")).toString();
    if (!rd.isEmpty())
        out.insert(QStringLiteral("year"), rd.left(4).toInt());
    const QString genre = r.value(QLatin1String("primaryGenreName")).toString();
    if (!genre.isEmpty())
        out.insert(QStringLiteral("genres"), genre);
    QString overview = r.value(QLatin1String("longDescription")).toString();
    if (overview.isEmpty())
        overview = r.value(QLatin1String("shortDescription")).toString();
    if (!overview.isEmpty())
        out.insert(QStringLiteral("overview"), overview);

    out.insert(QStringLiteral("posterUrl"),
               bigItunesImage(r.value(QLatin1String("artworkUrl100")).toString()));
    const QString link = r.value(QLatin1String("trackViewUrl")).toString();
    if (!link.isEmpty())
        out.insert(QStringLiteral("linkUrl"), link);

    out.insert(QStringLiteral("state"), QStringLiteral("ok"));
    out.insert(QStringLiteral("source"), QStringLiteral("itunes"));
    qInfo() << "metadata: itunes built" << out.value("title").toString()
            << out.value("subtitle").toString()
            << "year" << out.value("year").toInt()
            << "link" << out.value("linkUrl").toString();
    return out;
}
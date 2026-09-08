#include "JellyfinClient.h"
#include "MpvCore.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QUrl>
#include <QUrlQuery>
#include <QSettings>
#include <QTimer>
#include <QCoreApplication>
#include <QRandomGenerator>

namespace {

constexpr qint64 kTicksPerSecond = 10000000;

// Jellyfin accepts the token equally via the X-Emby-Token header or the
// api_key query parameter; the stream/image URLs need the query form.
QString apiKeyQuery(const QString &token)
{
    return QStringLiteral("api_key=") + token;
}

// Item fields the info overlay renders (plus the data playback needs).
constexpr char kItemFields[] =
    "Overview,Genres,CommunityRating,ProviderIds,ProductionYear,MediaSources";

} // namespace

JellyfinClient::JellyfinClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    // A stable device id keeps playback sessions attached to the same device
    // across restarts (Jellyfin keys sessions by DeviceId).
    QSettings settings;
    m_deviceId = settings.value(QStringLiteral("jellyfin/deviceId")).toString();
    if (m_deviceId.isEmpty()) {
        m_deviceId = QStringLiteral("omaplayer-")
            + QString::number(QRandomGenerator::global()->generate64(), 16);
        settings.setValue(QStringLiteral("jellyfin/deviceId"), m_deviceId);
    }

    const QVariantList stored = settings.value(QStringLiteral("jellyfin/servers")).toList();
    for (const QVariant &v : stored) {
        if (v.canConvert<QVariantMap>())
            m_servers.append(v.toMap());
    }

    m_progressTimer = new QTimer(this);
    m_progressTimer->setInterval(10000);
    connect(m_progressTimer, &QTimer::timeout, this, &JellyfinClient::reportTick);

    m_statusTimer = new QTimer(this);
    m_statusTimer->setSingleShot(true);
    m_statusTimer->setInterval(7000);
    // Transient feedback (e.g. "Szerver hozzáadva") fades away on its own.
    connect(m_statusTimer, &QTimer::timeout, this, [this] {
        if (m_status.isEmpty())
            return;
        m_status.clear();
        Q_EMIT statusChanged();
    });

    // Make sure an outstanding session is closed when the app exits.
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        if (m_tracking)
            reportStop();
    });
}

void JellyfinClient::persistServers()
{
    QSettings settings;
    settings.setValue(QStringLiteral("jellyfin/servers"), m_servers);
}

// ---------------------------------------------------------------- network --

QUrl JellyfinClient::imageUrl(const QString &itemId, const QString &imageType,
                              int maxWidth) const
{
    if (itemId.isEmpty() || baseUrl().isEmpty())
        return {};
    QUrl url(baseUrl() + QStringLiteral("/Items/") + itemId
             + QStringLiteral("/Images/") + imageType);
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("maxWidth"), QString::number(maxWidth));
    q.addQueryItem(QStringLiteral("quality"), QStringLiteral("90"));
    if (!token().isEmpty())
        q.addQueryItem(QStringLiteral("api_key"), token());
    url.setQuery(q);
    return url;
}

QString JellyfinClient::streamUrl(const QString &itemId, const QString &type) const
{
    if (itemId.isEmpty() || baseUrl().isEmpty())
        return {};
    const QString kind = type == QLatin1String("Audio")
                             ? QStringLiteral("Audio")
                             : QStringLiteral("Videos");
    QString url = baseUrl() + QStringLiteral("/") + kind
        + QStringLiteral("/") + itemId + QStringLiteral("/stream?static=true");
    if (!token().isEmpty())
        url += QLatin1Char('&') + apiKeyQuery(token());
    return url;
}

QNetworkRequest JellyfinClient::request(const QUrl &url) const
{
    QNetworkRequest req{url};
    req.setRawHeader("User-Agent",
                     "omaplayer/" OMAPLAYER_VERSION);
    req.setTransferTimeout(15000);
    if (!token().isEmpty())
        req.setRawHeader("X-Emby-Token", token().toUtf8());
    return req;
}

void JellyfinClient::finishRequest(QNetworkReply *reply)
{
    reply->deleteLater();
    if (m_netInFlight > 0)
        --m_netInFlight;
    setBusy(m_netInFlight > 0);
}

void JellyfinClient::get(const QUrl &url,
                         const std::function<void(const QJsonObject &)> &cb,
                         const std::function<void()> &onError)
{
    setBusy(++m_netInFlight > 0);
    QNetworkReply *reply = m_nam->get(request(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, cb, onError] {
        finishRequest(reply);
        if (reply->error() != QNetworkReply::NoError) {
            setStatus(tr("Jellyfin hiba: %1").arg(reply->errorString()));
            if (onError)
                onError();
            return;
        }
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        cb(obj);
    });
}

void JellyfinClient::post(const QUrl &url, const QJsonObject &body,
                          const std::function<void(const QJsonObject &)> &cb)
{
    QNetworkRequest req = request(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json"));
    setBusy(++m_netInFlight > 0);
    QNetworkReply *reply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, cb] {
        finishRequest(reply);
        if (reply->error() != QNetworkReply::NoError) {
            setStatus(tr("Jellyfin hiba: %1").arg(reply->errorString()));
            return;
        }
        cb(QJsonDocument::fromJson(reply->readAll()).object());
    });
}

// ------------------------------------------------------------ servers -----

void JellyfinClient::addServer(const QString &serverUrlIn)
{
    QString serverUrl = serverUrlIn.trimmed();
    while (serverUrl.endsWith(QLatin1Char('/')))
        serverUrl.chop(1);
    if (serverUrl.isEmpty()) {
        setStatus(tr("Add meg a szerver URL-jét"));
        return;
    }
    if (!serverUrl.contains(QLatin1String("://")))
        serverUrl.prepend(QStringLiteral("http://"));

    setStatus(tr("Szerver ellenőrzése…"));
    get(QUrl(serverUrl + QStringLiteral("/System/Info/Public")),
        [this, serverUrl](const QJsonObject &info) {
            QVariantMap rec;
            rec.insert(QStringLiteral("url"), serverUrl);
            rec.insert(QStringLiteral("name"),
                       info.value(QLatin1String("ServerName")).toString(serverUrl));
            rec.insert(QStringLiteral("token"), QString());
            rec.insert(QStringLiteral("userId"), QString());
            rec.insert(QStringLiteral("username"), QString());
            m_servers.append(rec);
            persistServers();
            setStatus(tr("Szerver hozzáadva: %1").arg(rec.value(QStringLiteral("name")).toString()));
            Q_EMIT serversChanged();
        });
}

void JellyfinClient::removeServer(int index)
{
    if (index < 0 || index >= m_servers.size())
        return;
    const QVariantMap rec = m_servers.at(index).toMap();
    const bool wasActive = rec.value(QStringLiteral("url")) == baseUrl();
    m_servers.removeAt(index);
    if (wasActive) {
        m_active.clear();
        m_serverName.clear();
        m_items.clear();
        Q_EMIT itemsChanged();
        Q_EMIT connectionChanged();
    }
    persistServers();
    Q_EMIT serversChanged();
}

void JellyfinClient::login(int serverIndex, const QString &username,
                           const QString &password)
{
    if (serverIndex < 0 || serverIndex >= m_servers.size())
        return;
    const QVariantMap rec = m_servers.at(serverIndex).toMap();
    const QString url = rec.value(QStringLiteral("url")).toString();

    setStatus(tr("Bejelentkezés…"));
    setBusy(++m_netInFlight > 0);
    const QString auth = QStringLiteral("MediaBrowser Client=\"Omaplayer\", Device=\"Omaplayer\", DeviceId=\"%1\", Version=\"0.2.0\"")
                             .arg(m_deviceId);
    QNetworkRequest req{QUrl(url + QStringLiteral("/Users/AuthenticateByName"))};
    req.setTransferTimeout(15000);
    req.setRawHeader("Authorization", auth.toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QJsonObject body{
        { QStringLiteral("Username"), username },
        { QStringLiteral("Pw"), password },
    };
    QNetworkReply *reply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, serverIndex, url, username] {
        reply->deleteLater();
        if (m_netInFlight > 0)
            --m_netInFlight;
        setBusy(m_netInFlight > 0);
        if (reply->error() != QNetworkReply::NoError) {
            setStatus(tr("Bejelentkezés sikertelen: %1").arg(reply->errorString()));
            return;
        }
        const QJsonObject res = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonObject user = res.value(QLatin1String("User")).toObject();
        const QString token = res.value(QLatin1String("AccessToken")).toString();
        if (token.isEmpty()) {
            setStatus(tr("Bejelentkezés sikertelen (nincs token)"));
            return;
        }
        QVariantMap rec = m_servers.at(serverIndex).toMap();
        rec.insert(QStringLiteral("token"), token);
        rec.insert(QStringLiteral("userId"), user.value(QLatin1String("Id")).toString());
        rec.insert(QStringLiteral("username"), user.value(QLatin1String("Name")).toString());
        m_servers.replace(serverIndex, rec);
        m_active = rec;
        m_serverName = rec.value(QStringLiteral("name")).toString();
        persistServers();
        Q_EMIT serversChanged();
        Q_EMIT connectionChanged();
        setStatus(tr("Sikeresen bejelentkezve: %1").arg(username));
        fetchViews();
    });
}

void JellyfinClient::selectServer(int index)
{
    if (index < 0 || index >= m_servers.size())
        return;
    const QVariantMap rec = m_servers.at(index).toMap();
    m_serverName = rec.value(QStringLiteral("name")).toString();
    if (!rec.value(QStringLiteral("token")).toString().isEmpty()) {
        m_active = rec;
        Q_EMIT connectionChanged();
        fetchViews();
    } else {
        m_active.clear();
        Q_EMIT connectionChanged();
        setStatus(tr("Jelentkezz be a szerverre"));
    }
}

void JellyfinClient::logout()
{
    // Find the stored record that matches the active session and drop its token.
    const QString url = baseUrl();
    for (int i = 0; i < m_servers.size(); ++i) {
        QVariantMap rec = m_servers.at(i).toMap();
        if (rec.value(QStringLiteral("url")) == url) {
            rec.insert(QStringLiteral("token"), QString());
            rec.insert(QStringLiteral("userId"), QString());
            rec.insert(QStringLiteral("username"), QString());
            m_servers.replace(i, rec);
            break;
        }
    }
    m_active.clear();
    m_serverName.clear();
    persistServers();
    Q_EMIT serversChanged();
    Q_EMIT connectionChanged();
    m_items.clear();
    Q_EMIT itemsChanged();
    setStatus(tr("Kijelentkezve"));
}

// ------------------------------------------------------------ browsing ----

QString JellyfinClient::baseUrl() const
{
    return m_active.value(QStringLiteral("url")).toString();
}

QString JellyfinClient::token() const
{
    return m_active.value(QStringLiteral("token")).toString();
}

QVariantMap JellyfinClient::normalizeItem(const QJsonObject &o) const
{
    QVariantMap m;
    m.insert(QStringLiteral("id"), o.value(QLatin1String("Id")).toString());
    m.insert(QStringLiteral("name"), o.value(QLatin1String("Name")).toString());
    m.insert(QStringLiteral("type"), o.value(QLatin1String("Type")).toString());
    m.insert(QStringLiteral("seriesId"), o.value(QLatin1String("SeriesId")).toString());
    m.insert(QStringLiteral("seasonId"), o.value(QLatin1String("SeasonId")).toString());
    m.insert(QStringLiteral("seriesName"), o.value(QLatin1String("SeriesName")).toString());
    m.insert(QStringLiteral("parentIndexNumber"),
             o.value(QLatin1String("ParentIndexNumber")).toInt());
    m.insert(QStringLiteral("indexNumber"), o.value(QLatin1String("IndexNumber")).toInt());
    m.insert(QStringLiteral("productionYear"),
             o.value(QLatin1String("ProductionYear")).toInt());
    m.insert(QStringLiteral("runTimeTicks"),
             o.value(QLatin1String("RunTimeTicks")).toDouble());
    m.insert(QStringLiteral("childCount"), o.value(QLatin1String("ChildCount")).toInt());
    m.insert(QStringLiteral("isFolder"), o.value(QLatin1String("IsFolder")).toBool());

    const QJsonObject tags = o.value(QLatin1String("ImageTags")).toObject();
    m.insert(QStringLiteral("imageTag"),
             tags.value(QLatin1String("Primary")).toString());
    m.insert(QStringLiteral("backdropTag"),
             tags.value(QLatin1String("Backdrop")).toString());

    // --- metadata enrichment for the info overlay ------------------------
    m.insert(QStringLiteral("overview"), o.value(QLatin1String("Overview")).toString());
    m.insert(QStringLiteral("communityRating"),
             o.value(QLatin1String("CommunityRating")).toDouble());
    m.insert(QStringLiteral("officialRating"),
             o.value(QLatin1String("OfficialRating")).toString());

    QVariantList genres;
    const QJsonArray ga = o.value(QLatin1String("Genres")).toArray();
    for (const QJsonValue &g : ga)
        genres.append(g.toString());
    m.insert(QStringLiteral("genres"), genres);

    // ProviderIds carries the TMDB/IMDb ids — that powers the external link
    // ("megnézem a TMDb-n/IMDb-n") without any separate lookup.
    const QJsonObject providers = o.value(QLatin1String("ProviderIds")).toObject();
    m.insert(QStringLiteral("tmdbId"), providers.value(QLatin1String("Tmdb")).toString());
    m.insert(QStringLiteral("imdbId"), providers.value(QLatin1String("Imdb")).toString());

    const QJsonObject ud = o.value(QLatin1String("UserData")).toObject();
    m.insert(QStringLiteral("playbackPositionTicks"),
             ud.value(QLatin1String("PlaybackPositionTicks")).toDouble());
    m.insert(QStringLiteral("played"), ud.value(QLatin1String("Played")).toBool());
    return m;
}

void JellyfinClient::setStatus(const QString &s)
{
    if (m_status == s)
        return;
    m_status = s;
    Q_EMIT statusChanged();
    if (m_statusTimer)
        m_statusTimer->start();
}

void JellyfinClient::setBusy(bool b)
{
    if (m_busy == b)
        return;
    m_busy = b;
    Q_EMIT busyChanged();
}

void JellyfinClient::fetchAsItems(const QString &urlString)
{
    get(QUrl(urlString), [this](const QJsonObject &obj) {
        QVariantList items;
        const QJsonArray arr = obj.value(QLatin1String("Items")).toArray();
        for (const QJsonValue &v : arr)
            items.append(normalizeItem(v.toObject()));
        m_items = items;
        Q_EMIT itemsChanged();
    });
}

void JellyfinClient::fetchViews()
{
    if (baseUrl().isEmpty() || token().isEmpty())
        return;
    fetchAsItems(baseUrl() + QStringLiteral("/Users/")
                 + m_active.value(QStringLiteral("userId")).toString()
                 + QStringLiteral("/Views"));
}

void JellyfinClient::fetchItems(const QString &parentId, int limit)
{
    if (baseUrl().isEmpty() || token().isEmpty())
        return;
    const QString url = baseUrl() + QStringLiteral("/Items?")
        + QStringLiteral("UserId=") + m_active.value(QStringLiteral("userId")).toString()
        + QStringLiteral("&ParentId=") + parentId
        + QStringLiteral("&Recursive=false")
        + QStringLiteral("&SortBy=SortName")
        + QStringLiteral("&SortOrder=Ascending")
        + QStringLiteral("&Fields=Overview,Genres,CommunityRating,ProviderIds,ProductionYear,Path,MediaSources")
        + QStringLiteral("&Limit=") + QString::number(limit)
        + QLatin1Char('&') + apiKeyQuery(token());
    fetchAsItems(url);
}

void JellyfinClient::fetchResume()
{
    if (baseUrl().isEmpty() || token().isEmpty())
        return;
    const QString url = baseUrl() + QStringLiteral("/UserItems/Resume?")
        + QStringLiteral("UserId=") + m_active.value(QStringLiteral("userId")).toString()
        + QStringLiteral("&MediaTypes=Video")
        + QStringLiteral("&Limit=20")
        + QStringLiteral("&Fields=") + QLatin1String(kItemFields)
        + QLatin1Char('&') + apiKeyQuery(token());
    fetchAsItems(url);
}

void JellyfinClient::fetchNextUp()
{
    if (baseUrl().isEmpty() || token().isEmpty())
        return;
    const QString url = baseUrl() + QStringLiteral("/Shows/NextUp?")
        + QStringLiteral("UserId=") + m_active.value(QStringLiteral("userId")).toString()
        + QStringLiteral("&Limit=20")
        + QStringLiteral("&Fields=") + QLatin1String(kItemFields)
        + QLatin1Char('&') + apiKeyQuery(token());
    fetchAsItems(url);
}

void JellyfinClient::fetchSeasons(const QString &seriesId)
{
    if (baseUrl().isEmpty() || token().isEmpty())
        return;
    const QString url = baseUrl() + QStringLiteral("/Shows/") + seriesId
        + QStringLiteral("/Seasons?")
        + QStringLiteral("UserId=") + m_active.value(QStringLiteral("userId")).toString()
        + QStringLiteral("&Fields=") + QLatin1String(kItemFields)
        + QLatin1Char('&') + apiKeyQuery(token());
    fetchAsItems(url);
}

void JellyfinClient::fetchEpisodes(const QString &seriesId, const QString &seasonId)
{
    if (baseUrl().isEmpty() || token().isEmpty())
        return;
    const QString url = baseUrl() + QStringLiteral("/Shows/") + seriesId
        + QStringLiteral("/Episodes?")
        + QStringLiteral("UserId=") + m_active.value(QStringLiteral("userId")).toString()
        + QStringLiteral("&seasonId=") + seasonId
        + QStringLiteral("&Fields=") + QLatin1String(kItemFields)
        + QLatin1Char('&') + apiKeyQuery(token());
    fetchAsItems(url);
}

void JellyfinClient::fetchSearch(const QString &query)
{
    if (baseUrl().isEmpty() || token().isEmpty() || query.trimmed().isEmpty())
        return;
    const QString url = baseUrl() + QStringLiteral("/Search/Hints?")
        + QStringLiteral("searchTerm=") + QUrl::toPercentEncoding(query)
        + QStringLiteral("&userId=") + m_active.value(QStringLiteral("userId")).toString()
        + QStringLiteral("&limit=30")
        + QStringLiteral("&includeItemTypes=Movie,Series,Episode")
        + QLatin1Char('&') + apiKeyQuery(token());
    get(QUrl(url), [this](const QJsonObject &obj) {
        QVariantList items;
        const QJsonArray arr = obj.value(QLatin1String("SearchHints")).toArray();
        for (const QJsonValue &v : arr) {
            const QJsonObject h = v.toObject();
            QVariantMap m;
            m.insert(QStringLiteral("id"), h.value(QLatin1String("ItemId")).toString());
            m.insert(QStringLiteral("name"), h.value(QLatin1String("Name")).toString());
            m.insert(QStringLiteral("type"), h.value(QLatin1String("Type")).toString());
            m.insert(QStringLiteral("seasonId"), h.value(QLatin1String("SeasonId")).toString());
            m.insert(QStringLiteral("seriesId"), h.value(QLatin1String("SeriesId")).toString());
            m.insert(QStringLiteral("productionYear"),
                     h.value(QLatin1String("ProductionYear")).toInt());
            m.insert(QStringLiteral("runTimeTicks"),
                     h.value(QLatin1String("RunTimeTicks")).toDouble());
            items.append(m);
        }
        m_items = items;
        Q_EMIT itemsChanged();
    });
}

// ------------------------------------------------------------ playback -----

QString JellyfinClient::displayTitle(const QVariantMap &item) const
{
    const QString name = item.value(QStringLiteral("name")).toString();
    if (item.value(QStringLiteral("type")).toString() != QLatin1String("Episode"))
        return name;

    const int season = item.value(QStringLiteral("parentIndexNumber")).toInt();
    const int ep = item.value(QStringLiteral("indexNumber")).toInt();
    QString t = item.value(QStringLiteral("seriesName")).toString();
    if (season > 0 && ep > 0)
        t += QStringLiteral(" S%1E%2").arg(season, 2, 10, QLatin1Char('0'))
                                    .arg(ep, 2, 10, QLatin1Char('0'));
    if (t.trimmed().isEmpty())
        return name;
    return t.trimmed();
}

void JellyfinClient::playItem(const QVariantMap &item)
{
    startPlayback(item);
}

void JellyfinClient::startPlayback(const QVariantMap &item)
{
    // A previous Jellyfin session must be closed first — this also happens when
    // the season auto-advances to the next episode mid-session.
    if (m_tracking)
        reportStop();
    m_advancing = false;

    const QString id = item.value(QStringLiteral("id")).toString();
    if (id.isEmpty() || baseUrl().isEmpty() || token().isEmpty())
        return;

    const QString type = item.value(QStringLiteral("type")).toString();
    const QString urlString = streamUrl(id, type);

    setStatus(tr("Előkapcsolás: %1").arg(item.value(QStringLiteral("name")).toString()));

    // 1) Resolve the playback session (PlaySessionId, MediaSourceId).
    get(QUrl(baseUrl() + QStringLiteral("/Items/") + id + QStringLiteral("/PlaybackInfo")
             + QLatin1Char('?') + apiKeyQuery(token())),
        [this, item, urlString](const QJsonObject &info) {
            const QString sessionId =
                info.value(QLatin1String("PlaySessionId")).toString();
            const QString mediaSourceId = info.value(QLatin1String("MediaSources"))
                                             .toArray().first().toObject()
                                             .value(QLatin1String("Id")).toString();

            const qint64 resumeBalance =
                qint64(item.value(QStringLiteral("playbackPositionTicks")).toDouble()) - 1'000'000'000;
            const bool haveResume = resumeBalance > 0;
            const double resumeSeconds = haveResume ? resumeBalance / qreal(kTicksPerSecond) : 0.0;

            // 2) Tell the server playback started. The UserId field is
            //    required field of the request body (else the server answers
            //    with 400 and never attaches the session/resume report).
            post(QUrl(baseUrl() + QStringLiteral("/Sessions/Playing")
                      + QLatin1Char('?') + apiKeyQuery(token())),
                 QJsonObject{
                     { QStringLiteral("UserId"), m_active.value(QStringLiteral("userId")).toString() },
                     { QStringLiteral("ItemId"), item.value(QStringLiteral("id")).toString() },
                     { QStringLiteral("MediaSourceId"), mediaSourceId },
                     { QStringLiteral("PlaySessionId"), sessionId },
                     { QStringLiteral("CanSeek"), true },
                     { QStringLiteral("PlayMethod"), QStringLiteral("DirectPlay") },
                     { QStringLiteral("PositionTicks"), double(item.value(QStringLiteral("playbackPositionTicks")).toDouble()) },
                 },
                 [this](const QJsonObject &) {});

            // 3) Open the stream in mpv (with a readable title, and jump to the
            //    resume point when the user was part-way through).
            m_streamUrl = urlString;
            MpvCore::instance()->open(urlString);
            MpvCore::instance()->setMediaTitle(displayTitle(item));
            if (haveResume)
                MpvCore::instance()->seek(resumeSeconds);

            // 4) Start progress tracking.
            m_tracking = true;
            m_trackItem = item;
            Q_EMIT playingItemChanged();
            m_trackItemId = item.value(QStringLiteral("id")).toString();
            m_trackSeriesId = item.value(QStringLiteral("seriesId")).toString();
            m_trackSession = sessionId;
            m_trackMediaSource = mediaSourceId;
            m_lastPositionSeconds = resumeSeconds;
            // 5) Server-side skip segments (intro/recap/outro, if the
            //    server has a provider). Best-effort, never blocks playback.
            fetchSegments(item.value(QStringLiteral("id")).toString());
            m_progressTimer->start();
        });
}

void JellyfinClient::fetchSegments(const QString &itemId)
{
    if (itemId.isEmpty() || baseUrl().isEmpty() || token().isEmpty())
        return;
    const QUrl url(baseUrl() + QStringLiteral("/MediaSegments/") + itemId
                   + QLatin1Char('?') + apiKeyQuery(token()));
    QNetworkReply *reply = m_nam->get(request(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return; // no provider / old server — silent, chapters still apply
        QVariantList out;
        const QJsonArray arr =
            QJsonDocument::fromJson(reply->readAll()).array();
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            const QString t = o.value(QStringLiteral("Type")).toString();
            QString kind;
            if (t == QLatin1String("Intro") || t == QLatin1String("Commercial"))
                kind = QStringLiteral("intro");
            else if (t == QLatin1String("Recap")
                     || t == QLatin1String("Preview"))
                kind = QStringLiteral("recap");
            else if (t == QLatin1String("Outro"))
                kind = QStringLiteral("credits");
            else
                continue;
            const double start =
                o.value(QStringLiteral("StartTicks")).toDouble()
                / double(kTicksPerSecond);
            const double end =
                o.value(QStringLiteral("EndTicks")).toDouble()
                / double(kTicksPerSecond);
            if (!(end > start))
                continue;
            out.append(QVariantMap{{QStringLiteral("type"), kind},
                                   {QStringLiteral("start"), start},
                                   {QStringLiteral("end"), end}});
        }
        if (!out.isEmpty())
            Q_EMIT segmentsReady(out);
    });
}

void JellyfinClient::stopPlayback()
{
    if (m_tracking)
        reportStop();
}

void JellyfinClient::reportTick()
{
    if (!m_tracking)
        return;
    MpvCore *core = MpvCore::instance();
    const double pos = core->position();
    const double dur = core->duration();
    m_lastPositionSeconds = pos;

    if (dur > 0.0 && pos >= dur - 2.0) {
        // End reached — mark watched, then advance to the next episode if the
        // item was part of a series (or just close the session otherwise).
        post(QUrl(baseUrl() + QStringLiteral("/UserPlayedItems/") + m_trackItemId
                  + QLatin1Char('?') + apiKeyQuery(token())),
             {},
             [this](const QJsonObject &) {});
        advanceToNextEpisode();
        return;
    }

    post(QUrl(baseUrl() + QStringLiteral("/Sessions/Playing/Progress")
              + QLatin1Char('?') + apiKeyQuery(token())),
QJsonObject{
                     { QStringLiteral("UserId"), m_active.value(QStringLiteral("userId")).toString() },
                     { QStringLiteral("ItemId"), m_trackItemId },
                     { QStringLiteral("MediaSourceId"), m_trackMediaSource },
                     { QStringLiteral("PlaySessionId"), m_trackSession },
                     { QStringLiteral("PositionTicks"),
                       double(qint64(m_lastPositionSeconds * kTicksPerSecond)) },
                     { QStringLiteral("CanSeek"), true },
                     { QStringLiteral("PlayMethod"), QStringLiteral("DirectPlay") },
                 },
                 [this](const QJsonObject &) {});
}

void JellyfinClient::advanceToNextEpisode()
{
    if (m_advancing)
        return;
    const QString seriesId = m_trackSeriesId;
    if (seriesId.isEmpty() || baseUrl().isEmpty() || token().isEmpty()) {
        reportStop();
        return;
    }
    m_advancing = true;

    // NextUp returns the next unwatched episode for the series.
    const QString url = baseUrl() + QStringLiteral("/Shows/NextUp?")
        + QStringLiteral("UserId=") + m_active.value(QStringLiteral("userId")).toString()
        + QStringLiteral("&SeriesId=") + seriesId
        + QStringLiteral("&Limit=1")
        + QStringLiteral("&Fields=") + QLatin1String(kItemFields)
        + QLatin1Char('&') + apiKeyQuery(token());
    get(QUrl(url), [this](const QJsonObject &obj) {
        m_advancing = false;
        const QJsonArray arr = obj.value(QLatin1String("Items")).toArray();
        if (arr.isEmpty()) {
            reportStop();
            return;
        }
        QVariantMap next = normalizeItem(arr.first().toObject());
        if (next.value(QStringLiteral("id")).toString().isEmpty()
            || next.value(QStringLiteral("id")).toString() == m_trackItemId) {
            reportStop();
            return;
        }
        startPlayback(next);
    }, [this] {
        // Fetch failed — don't leave the session dangling.
        m_advancing = false;
        reportStop();
    });
}

void JellyfinClient::onFileOpened(const QString &path)
{
    // The user left the Jellyfin stream (local file, CLI arg, playlist move):
    // end the server session so `playingItem` clears and refreshMeta() falls
    // back to the metadata lookup for whatever is actually playing now.
    if (m_tracking && !path.isEmpty() && path != m_streamUrl)
        reportStop();
}

void JellyfinClient::reportStop()
{
    if (m_tracking) {
        post(QUrl(baseUrl() + QStringLiteral("/Sessions/Playing/Stopped")
                  + QLatin1Char('?') + apiKeyQuery(token())),
             QJsonObject{
                 { QStringLiteral("UserId"), m_active.value(QStringLiteral("userId")).toString() },
                 { QStringLiteral("ItemId"), m_trackItemId },
                 { QStringLiteral("MediaSourceId"), m_trackMediaSource },
                 { QStringLiteral("PlaySessionId"), m_trackSession },
                 { QStringLiteral("PositionTicks"),
                   double(qint64(m_lastPositionSeconds * kTicksPerSecond)) },
                 { QStringLiteral("CanSeek"), true },
                 { QStringLiteral("PlayMethod"), QStringLiteral("DirectPlay") },
             },
             [this](const QJsonObject &) {});
    }
    m_tracking = false;
    m_trackItem.clear();
    Q_EMIT playingItemChanged();
    m_progressTimer->stop();
}
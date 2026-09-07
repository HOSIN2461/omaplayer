#include "SubtitleClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QSettings>
#include <QStandardPaths>
#include <QFile>
#include <QDir>
#include <QUrlQuery>
#include <QRegularExpression>
#include <QFileInfo>
#include <QDateTime>

using namespace Qt::Literals::StringLiterals;

static const char *kSettingsGroup  = "opensubtitles";
static const char *kSettingsKey    = "apiKey";
static const char *kUserAgent      = "omaplayer v" OMAPLAYER_VERSION;

SubtitleClient::SubtitleClient(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
}

bool SubtitleClient::hasApiKey() const
{
    return !apiKeyFromSettings().isEmpty();
}

QString SubtitleClient::apiKey() const
{
    return apiKeyFromSettings();
}

void SubtitleClient::setApiKey(const QString &key)
{
    saveApiKey(key.trimmed());
    Q_EMIT apiKeyChanged();
}

// --- QSettings helpers -------------------------------------------------

QString SubtitleClient::apiKeyFromSettings()
{
    QSettings s;
    s.beginGroup(kSettingsGroup);
    return s.value(kSettingsKey).toString();
}

void SubtitleClient::saveApiKey(const QString &key)
{
    QSettings s;
    s.beginGroup(kSettingsGroup);
    s.setValue(kSettingsKey, key);
}

// --- helpers ------------------------------------------------------------

void SubtitleClient::setBusy(bool b)
{
    if (m_busy == b) return;
    m_busy = b;
    Q_EMIT busyChanged();
}

void SubtitleClient::setStatus(const QString &s)
{
    if (m_status == s) return;
    m_status = s;
    Q_EMIT statusChanged();
}

// --- search -------------------------------------------------------------

void SubtitleClient::search(const QString &query)
{
    const QString key = apiKeyFromSettings();
    if (key.isEmpty()) {
        Q_EMIT errorOccurred(tr("OpenSubtitles API kulcs nincs beállítva."));
        return;
    }
    if (query.isEmpty()) {
        Q_EMIT errorOccurred(tr("Nincs megadva keresési kifejezés."));
        return;
    }

    setBusy(true);
    setStatus(tr("Keresés: %1").arg(query));
    m_results.clear();
    Q_EMIT resultsChanged();

    QUrl url(QStringLiteral("%1/subtitles").arg(QLatin1String(kBaseUrl)));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("query"), query);
    q.addQueryItem(QStringLiteral("languages"), QStringLiteral("hu,en"));
    q.addQueryItem(QStringLiteral("order_by"), QStringLiteral("download_count"));
    q.addQueryItem(QStringLiteral("order_direction"), QStringLiteral("desc"));
    q.addQueryItem(QStringLiteral("per_page"), QStringLiteral("20"));
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setRawHeader("Api-Key", key.toUtf8());
    req.setRawHeader("User-Agent", kUserAgent);
    req.setRawHeader("Accept", "application/json");

    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        setBusy(false);

        if (reply->error() != QNetworkReply::NoError) {
            const int code = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QString errMsg;
            if (code == 401 || code == 403) {
                errMsg = tr("Érvénytelen OpenSubtitles API kulcs (HTTP %1).").arg(code);
            } else if (code == 429) {
                errMsg = tr("Túl sok kérés — várj egy kicsit és próbáld újra.");
            } else {
                errMsg = tr("Hálózati hiba: %1").arg(reply->errorString());
            }
            setStatus(errMsg);
            Q_EMIT errorOccurred(errMsg);
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject root = doc.object();
        const QJsonArray data = root.value("data").toArray();

        if (data.isEmpty()) {
            setStatus(tr("Nincs találat."));
            Q_EMIT resultsChanged();
            return;
        }

        QVariantList out;
        for (const QJsonValue &entry : data) {
            const QJsonObject obj = entry.toObject();
            const QJsonObject attrs = obj.value("attributes").toObject();

            QVariantMap item;
            item["lang"]      = attrs.value("language").toString();
            item["langName"]  = attrs.value("language_name").toString();
            item["encoding"]  = attrs.value("encoding").toString();
            item["format"]    = attrs.value("format").toString();
            item["downloads"] = attrs.value("download_count").toInt();

            // The download endpoint wants the FILE id, not the subtitle id
            // (files[0].file_id). A subtitle without any file entry can't be
            // downloaded, so skip it.
            const QJsonArray files = attrs.value("files").toArray();
            if (files.isEmpty())
                continue;
            item["fileId"] = files.at(0).toObject().value("file_id").toInt();
            if (item["fileId"].toInt() <= 0)
                continue;

            // Build a readable label: "HU · S02E17 release — 1234 letöltés".
            // Language first (never elided), then the episode tag so the user
            // can pick the subtitle matching the currently open episode.
            const QString relName = attrs.value("release_name").toString();
            const QJsonObject fd = attrs.value("feature_details").toObject();
            const QString subj = relName.isEmpty()
                ? fd.value("title").toString()
                : relName;

            const int season = fd.value("season_number").toInt(-1);
            const int epNo   = fd.value("episode_number").toInt(-1);
            QString epTag;
            if (season >= 0 && epNo >= 0)
                epTag = QStringLiteral("S%1E%2")
                            .arg(season, 2, 10, QLatin1Char('0'))
                            .arg(epNo, 2, 10, QLatin1Char('0'));
            else if (epNo >= 0)
                epTag = QStringLiteral("E%1").arg(epNo, 2, 10, QLatin1Char('0'));
            else if (season >= 0)
                epTag = QStringLiteral("S%1").arg(season, 2, 10, QLatin1Char('0'));

            const QString dl = QStringLiteral("— %1 %2")
                .arg(attrs.value("download_count").toInt())
                .arg(tr("letöltés"));
            const QString label = epTag.isEmpty()
                ? QStringLiteral("%1 · %2 %3")
                      .arg(attrs.value("language").toString().toUpper(), subj, dl)
                : QStringLiteral("%1 · %2 %3 %4")
                      .arg(attrs.value("language").toString().toUpper(),
                           epTag, subj, dl);
            item["label"] = label;

            out.append(item);
        }

        m_results = out;
        setStatus(tr("%1 találat").arg(out.size()));
        Q_EMIT resultsChanged();
    });
}

void SubtitleClient::searchForCurrentFile(const QString &filePath,
                                          const QString &mediaTitle)
{
    // Use the media title if available, otherwise fall back to the filename
    // (without extension) as the search query.
    QString query = mediaTitle;
    if (query.isEmpty() && !filePath.isEmpty()) {
        QFileInfo fi(filePath);
        query = fi.completeBaseName();
    }

    // Strip common release-tag noise (720p, 1080p, x264, etc.) to get a
    // cleaner search hit — but keep the season/episode marker (S02E17)
    // so the API returns that specific episode's subtitles.
    query.remove(QRegularExpression(
        QStringLiteral("\\b(720p|1080p|2160p|4k|BluRay|BRRip|HDRip|"
                       "DVDRip|WEBRip|WEB-DL|x264|x265|HEVC|AAC|"
                       "DTS|FLAC|MP3|5\\.1|7\\.1|REMASTERED|"
                       "PROPER|EXTENDED|UNRATED|DC|IMAX)\\b"),
        QRegularExpression::CaseInsensitiveOption));
    query.replace(QRegularExpression(QStringLiteral("[._\\[\\]()]+")),
                  QStringLiteral(" "));
    query = query.simplified();

    // Strip common release-tag noise (720p, 1080p, x264, etc.) to get a
    // cleaner search hit.
    query.remove(QRegularExpression(
        QStringLiteral("\\b(720p|1080p|2160p|4k|BluRay|BRRip|HDRip|"
                       "DVDRip|WEBRip|WEB-DL|x264|x265|HEVC|AAC|"
                       "DTS|FLAC|MP3|5\\.1|7\\.1|REMASTERED|"
                       "PROPER|EXTENDED|UNRATED|DC|IMAX)\\b"),
        QRegularExpression::CaseInsensitiveOption));
    query.replace(QRegularExpression(QStringLiteral("[._\\[\\]()]+")),
                  QStringLiteral(" "));
    query = query.simplified();

    if (query.isEmpty()) {
        Q_EMIT errorOccurred(tr("Nincs megadható keresési kifejezés."));
        return;
    }
    search(query);
}

// --- download -----------------------------------------------------------

void SubtitleClient::download(int fileId)
{
    const QString key = apiKeyFromSettings();
    if (key.isEmpty()) {
        Q_EMIT errorOccurred(tr("OpenSubtitles API kulcs nincs beállítva."));
        return;
    }

    setBusy(true);
    setStatus(tr("Felirat letöltése…"));

    QUrl url(QStringLiteral("%1/download").arg(QLatin1String(kBaseUrl)));
    QJsonObject body;
    body["file_id"] = fileId;

    QNetworkRequest req(url);
    req.setRawHeader("Api-Key", key.toUtf8());
    req.setRawHeader("User-Agent", kUserAgent);
    req.setRawHeader("Content-Type", "application/json");
    req.setRawHeader("Accept", "application/json");

    QNetworkReply *reply = m_net->post(req, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        setBusy(false);

        if (reply->error() != QNetworkReply::NoError) {
            const QString errMsg = tr("Letöltési hiba: %1").arg(reply->errorString());
            setStatus(errMsg);
            Q_EMIT errorOccurred(errMsg);
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject root = doc.object();
        const QString link = root.value("link").toString();

        if (link.isEmpty()) {
            const QString msg = root.value("message").toString();
            const QString errMsg = msg.isEmpty()
                ? tr("Nem sikerült letölteni a feliratot.")
                : msg;
            setStatus(errMsg);
            Q_EMIT errorOccurred(errMsg);
            return;
        }

        setStatus(tr("Letöltés folyamatban…"));

        // Fetch the actual subtitle file from the temporary link.
        QNetworkRequest fileReq{QUrl(link)};
        fileReq.setRawHeader("User-Agent", kUserAgent);
        QNetworkReply *fileReply = m_net->get(fileReq);
        connect(fileReply, &QNetworkReply::finished, this,
                [this, fileReply]() {
            fileReply->deleteLater();
            setBusy(false);

            if (fileReply->error() != QNetworkReply::NoError) {
                const QString errMsg = tr("A feliratfájl letöltése sikertelen: %1")
                    .arg(fileReply->errorString());
                setStatus(errMsg);
                Q_EMIT errorOccurred(errMsg);
                return;
            }

            // Determine file extension from Content-Type or default to .srt.
            const QString contentType = fileReply->header(
                QNetworkRequest::ContentTypeHeader).toString();
            QString ext = ".srt";
            if (contentType.contains("ass") || contentType.contains("ssa"))
                ext = ".ass";
            else if (contentType.contains("vtt"))
                ext = ".vtt";

            // Write to a temp file.
            const QString tmpDir = QStandardPaths::writableLocation(
                QStandardPaths::TempLocation);
            const QString tmpPath = QStringLiteral("%1/omaplayer_sub_%2%3")
                .arg(tmpDir)
                .arg(QDateTime::currentMSecsSinceEpoch())
                .arg(ext);

            QFile file(tmpPath);
            if (!file.open(QIODevice::WriteOnly)) {
                const QString errMsg = tr("Nem sikerült létrehozni a ideiglenes fájlt.");
                setStatus(errMsg);
                Q_EMIT errorOccurred(errMsg);
                return;
            }
            file.write(fileReply->readAll());
            file.close();

            setStatus(tr("Felirat letöltve."));
            Q_EMIT downloaded(tmpPath);
        });
    });
}

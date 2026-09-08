#include "AudioIntroMatcher.h"
#include "CastDebug.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

constexpr int kMaxReferenceFiles = 4;
constexpr int kHelperTimeoutMs = 300000;
constexpr double kHeadSeconds = 720.0;  // intro lives in the first 12 min
constexpr double kTailSeconds = 900.0;  // outro lives in the last 15 min
constexpr double kRecapSeconds = 180.0; // recaps quote the first 3 minutes
constexpr double kRecapMaxEnd = 150.0;  // …and end within them

// Playlist entries whose names match these tokens are ignored as references
// (mirrors BAD_REFERENCE_FILENAME_REGEX in the upstream detector).
const QRegularExpression &badNameRe()
{
    static const QRegularExpression re(
        QStringLiteral("(?:^|[\\s._\\-\\[\\(])"
                       "(?:sample|trailer|extras?|ncop\\d*|nced\\d*|oped|"
                       "creditless|preview)"
                       "(?:$|[\\s._\\-\\)\\]])"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

const std::array<const char *, 24> &videoExtensions()
{
    static const std::array<const char *, 24> exts = {
        "mkv", "mp4", "avi", "m4v", "mov", "3gp", "ts",  "mts", "m2ts",
        "wmv", "flv", "f4v", "asf", "webm", "rm", "rmvb", "qt", "dv",
        "mpg", "mpeg", "mxf", "vob", "ogv",  "ogm",
    };
    return exts;
}

int toInt(const QString &s)
{
    bool ok = false;
    const int value = s.toInt(&ok);
    return ok ? value : -1;
}

} // namespace

AudioIntroMatcher::AudioIntroMatcher(QObject *parent)
    : QObject(parent)
{
    m_node = findExecutableOr(QStringLiteral("node"),
                              { QDir::homePath() + QStringLiteral("/.local/share/mise/shims/node"),
                                QDir::homePath() + QStringLiteral("/.local/bin/node"),
                                QStringLiteral("/usr/bin/node"),
                                QStringLiteral("/usr/local/bin/node") });
    m_ffmpeg = findExecutableOr(QStringLiteral("ffmpeg"),
                                { QStringLiteral("/usr/bin/ffmpeg"),
                                  QStringLiteral("/usr/local/bin/ffmpeg") });
}

QString AudioIntroMatcher::findExecutableOr(const QString &name,
                                            const QStringList &fallbacks)
{
    const QString inPath = QStandardPaths::findExecutable(name);
    if (!inPath.isEmpty())
        return inPath;
    for (const QString &candidate : fallbacks) {
        const QFileInfo info(candidate);
        if (info.isExecutable() && !info.isDir())
            return candidate;
    }
    return {};
}

bool AudioIntroMatcher::ready() const
{
    return !m_node.isEmpty() && !m_ffmpeg.isEmpty();
}

bool AudioIntroMatcher::isSeriesPath(const QString &path)
{
    AudioIntroMatcher tmp;
    const Parsed p = tmp.parseSeasonEpisode(filenameStem(path));
    return p.valid && !p.isSpecial;
}

double AudioIntroMatcher::probeDuration(const QString &ffmpeg,
                                        const QString &path)
{
    // ffprobe ships next to ffmpeg; fall back to PATH resolution.
    QString ffprobe = QFileInfo(ffmpeg).dir().filePath(QStringLiteral("ffprobe"));
    if (!QFileInfo(ffprobe).isExecutable())
        ffprobe = QStringLiteral("ffprobe");
    QProcess probe;
    probe.start(ffprobe,
                {QStringLiteral("-v"), QStringLiteral("error"),
                 QStringLiteral("-show_entries"),
                 QStringLiteral("format=duration"), QStringLiteral("-of"),
                 QStringLiteral("default=noprint_wrappers=1:nokey=1"), path});
    if (!probe.waitForFinished(8000) || probe.exitCode() != 0)
        return 0.0;
    return probe.readAllStandardOutput().trimmed().toDouble();
}

bool AudioIntroMatcher::isVideoPath(const QString &path)
{
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    if (dot < 0)
        return false;
    const QString ext = path.mid(dot + 1).toLower();
    for (const char *candidate : videoExtensions()) {
        if (ext == QLatin1String(candidate))
            return true;
    }
    return false;
}

QString AudioIntroMatcher::filenameStem(const QString &path)
{
    QString name = path;
    const int slash = name.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0)
        name = name.mid(slash + 1);
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot > 0)
        name = name.left(dot);
    return name;
}

bool AudioIntroMatcher::isBadReference(const QString &path)
{
    return badNameRe().match(filenameStem(path)).hasMatch();
}

AudioIntroMatcher::Parsed AudioIntroMatcher::parseSeasonEpisode(const QString &stem) const
{
    static const QRegularExpression sxpRe(
        QStringLiteral("(?:^|[\\s._\\-\\[\\(])s(\\d{1,2})[\\s._\\-\\]\\[]*"
                       "(ep|sp|e|x)[\\s._-]*(\\d{1,4})(?:v\\d+)?"
                       "(?=$|[\\s._\\-\\)\\]])"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression nxRe(
        QStringLiteral("(?:^|[\\s._\\-\\[\\(])(\\d{1,2})x(\\d{1,4})(?:v\\d+)?"
                       "(?=$|[\\s._\\-\\)\\]])"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression standaloneRe(
        QStringLiteral("(?:^|[\\s._\\-\\[\\(])(?:sp|special|ova|oav|oad)"
                       "[\\s._-]*(\\d{1,4})(?:v\\d+)?(?=$|[\\s._\\-\\)\\]])"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression seasonWordRe(
        QStringLiteral("\\b(?:season|saison|temporada|stagione|staffel|serie)"
                       "[. _-]?(\\d{1,2})\\b"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression seasonOnlyRe(
        QStringLiteral("(?:^|[\\s._\\-\\[\\(])s(\\d{1,2})(?!\\d)"
                       "(?=$|[\\s._\\-\\)\\]])"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression ordinalRe(
        QStringLiteral("\\b(\\d{1,2})(?:st|nd|rd|th)[. _-]*(?:season|saison|"
                       "temporada|stagione|staffel|serie)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression episodeWordRe(
        QStringLiteral("\\b(?:ep(?:isode)?|eps?|[ée]p(?:isode)?)"
                       "[. _-]?(\\d{1,4})(?:v\\d+)?\\b"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression singleERe(
        QStringLiteral("(?:^|[\\s._-])e[\\s._-]*(\\d{1,4})(?:v\\d+)?"
                       "(?=$|[\\s._\\-\\)\\]])"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression dashRe(
        QStringLiteral("^[\\s._\\-\\)\\]]*-[\\s._-]*(\\d{1,4})(?:v\\d+)?"
                       "(?=$|[\\s._\\-\\)\\]])"),
        QRegularExpression::CaseInsensitiveOption);

    const QRegularExpressionMatch standalone = standaloneRe.match(stem);
    const bool hasStandalone = standalone.hasMatch();

    // S01E02 / 1x02 forms.
    {
        const QRegularExpressionMatch m = sxpRe.match(stem);
        if (m.hasMatch()) {
            const int season = toInt(m.captured(1));
            const int episode = toInt(m.captured(3));
            if (season >= 0 && episode >= 0) {
                const QString kind = m.captured(2).toLower();
                return { season, episode, season == 0 || kind == QLatin1String("sp") || hasStandalone, true };
            }
        }
        const QRegularExpressionMatch mx = nxRe.match(stem);
        if (mx.hasMatch()) {
            const int season = toInt(mx.captured(1));
            const int episode = toInt(mx.captured(2));
            if (season >= 0 && episode >= 0)
                return { season, episode, false, true };
        }
    }

    // Named season ("season 3", "3rd season", ... "third season").
    struct Token { int season; int endIndex; };
    Token token;
    bool haveToken = false;
    const struct MatchKind {
        const QRegularExpression &re;
        int group;
    } seasonMatchers[] = {
        { seasonWordRe, 1 },
        { seasonOnlyRe, 1 },
        { ordinalRe, 1 },
    };
    auto tryWordSeason = [&](const QRegularExpression &re) -> bool {
        const QRegularExpressionMatch m = re.match(stem);
        if (!m.hasMatch())
            return false;
        // Number words ("third", "two", ...). Keep a common subset.
        static const QHash<QString, int> words = {
            { QStringLiteral("first"), 1 },     { QStringLiteral("second"), 2 },
            { QStringLiteral("third"), 3 },     { QStringLiteral("fourth"), 4 },
            { QStringLiteral("fifth"), 5 },     { QStringLiteral("sixth"), 6 },
            { QStringLiteral("seventh"), 7 },   { QStringLiteral("eighth"), 8 },
            { QStringLiteral("ninth"), 9 },     { QStringLiteral("tenth"), 10 },
            { QStringLiteral("eleventh"), 11 }, { QStringLiteral("twelfth"), 12 },
            { QStringLiteral("one"), 1 },       { QStringLiteral("two"), 2 },
            { QStringLiteral("three"), 3 },     { QStringLiteral("four"), 4 },
            { QStringLiteral("five"), 5 },      { QStringLiteral("six"), 6 },
            { QStringLiteral("seven"), 7 },     { QStringLiteral("eight"), 8 },
            { QStringLiteral("nine"), 9 },      { QStringLiteral("ten"), 10 },
        };
        const QString word = m.captured(1).toLower();
        const int value = words.value(word, -1);
        if (value < 0)
            return false;
        token = { value, static_cast<int>(m.capturedStart(1)) + static_cast<int>(m.captured(1).length()) };
        return true;
    };

    for (const MatchKind &mk : seasonMatchers) {
        const QRegularExpressionMatch m = mk.re.match(stem);
        if (m.hasMatch()) {
            const int season = toInt(m.captured(mk.group));
            if (season >= 0) {
                token = { season, static_cast<int>(m.capturedStart(mk.group)) + static_cast<int>(m.captured(mk.group).length()) };
                haveToken = true;
                break;
            }
        }
    }
    if (!haveToken) {
        static const QRegularExpression wordBeforeRe(
            QStringLiteral("\\b(?:first|second|third|fourth|fifth|sixth|"
                           "seventh|eighth|ninth|tenth|eleventh|twelfth|one|two|"
                           "three|four|five|six|seven|eight|nine|ten)"
                           "[. _-]*(?:season|saison|temporada|stagione|staffel|"
                           "serie)\\b"),
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression wordAfterRe(
            QStringLiteral("\\b(?:season|saison|temporada|stagione|staffel|"
                           "serie)[. _-]*(?:first|second|third|fourth|fifth|sixth|"
                           "seventh|eighth|ninth|tenth|eleventh|twelfth|one|two|"
                           "three|four|five|six|seven|eight|nine|ten)\\b"),
            QRegularExpression::CaseInsensitiveOption);
        if (tryWordSeason(wordBeforeRe) || tryWordSeason(wordAfterRe))
            haveToken = true;
    }

    if (!haveToken) {
        if (hasStandalone) {
            const int special = toInt(standalone.captured(1));
            if (special >= 0)
                return { -1, special, true, true };
        }
        return {};
    }

    int episode = -1;
    const QRegularExpressionMatch epWord = episodeWordRe.match(stem);
    if (epWord.hasMatch()) {
        episode = toInt(epWord.captured(1));
    } else if (const QRegularExpressionMatch singleE = singleERe.match(stem);
               singleE.hasMatch()) {
        episode = toInt(singleE.captured(1));
    } else {
        const QRegularExpressionMatch dash = dashRe.match(stem.mid(token.endIndex));
        if (dash.hasMatch())
            episode = toInt(dash.captured(1));
    }
    if (episode < 0)
        return {};

    return { token.season, episode, token.season == 0, true };
}

QStringList AudioIntroMatcher::selectReferences(const QStringList &playlist,
                                                int currentIndex,
                                                const Parsed &current) const
{
    struct Candidate {
        int index = 0;
        QString path;
        Parsed parsed;
    };
    QVector<Candidate> candidates;
    for (int i = 0; i < playlist.size(); ++i) {
        if (i == currentIndex)
            continue;
        const QString &path = playlist.at(i);
        if (!isVideoPath(path) || isBadReference(path) || !QFileInfo::exists(path))
            continue;
        candidates.push_back({ i, path, current.valid ? parseSeasonEpisode(filenameStem(path)) : Parsed{} });
    }

    QVector<Candidate> selected;
    if (current.valid && !current.isSpecial) {
        QHash<int, const Candidate *> byIndex;
        for (const Candidate &c : candidates)
            byIndex.insert(c.index, &c);

        // Same-season run around the current episode. Playlist order is NOT
        // episode order (reversed/shuffled lists exist), so no monotonicity
        // requirement — any same-season episode works as a reference, and
        // the distance sort below prefers close ones anyway.
        for (int i = currentIndex - 1; i >= 0; --i) {
            const Candidate *c = byIndex.value(i, nullptr);
            if (!c || !c->parsed.valid || c->parsed.isSpecial
                || c->parsed.season != current.season)
                continue;
            selected.push_back(*c);
        }
        for (int i = currentIndex + 1; i < playlist.size(); ++i) {
            const Candidate *c = byIndex.value(i, nullptr);
            if (!c || !c->parsed.valid || c->parsed.isSpecial
                || c->parsed.season != current.season)
                continue;
            selected.push_back(*c);
        }

        std::sort(selected.begin(), selected.end(), [&](const Candidate &a, const Candidate &b) {
            const int aOff = a.parsed.episode - current.episode;
            const int bOff = b.parsed.episode - current.episode;
            const int aDist = std::abs(aOff);
            const int bDist = std::abs(bOff);
            if (aDist != bDist)
                return aDist < bDist;
            const int aSide = aOff > 0 ? 0 : 1;
            const int bSide = bOff > 0 ? 0 : 1;
            if (aSide != bSide)
                return aSide < bSide;
            return a.index < b.index;
        });
    } else {
        std::sort(candidates.begin(), candidates.end(), [currentIndex](const Candidate &a, const Candidate &b) {
            const int aDist = std::abs(a.index - currentIndex);
            const int bDist = std::abs(b.index - currentIndex);
            if (aDist != bDist)
                return aDist < bDist;
            const int aPrev = a.index < currentIndex ? 0 : 1;
            const int bPrev = b.index < currentIndex ? 0 : 1;
            if (aPrev != bPrev)
                return aPrev < bPrev;
            return a.index < b.index;
        });
        selected = candidates;
    }

    QStringList refs;
    for (int i = 0; i < selected.size() && i < kMaxReferenceFiles; ++i)
        refs << selected.at(i).path;
    return refs;
}

QStringList AudioIntroMatcher::selectRecapReferences(
    const QStringList &playlist, int currentIndex, const Parsed &current) const
{
    // Earlier same-season episodes, closest first (recaps quote those).
    struct Candidate {
        int index = 0;
        QString path;
        int episode = 0;
    };
    QVector<Candidate> candidates;
    for (int i = 0; i < playlist.size(); ++i) {
        if (i == currentIndex)
            continue;
        const QString &path = playlist.at(i);
        if (!isVideoPath(path) || isBadReference(path)
            || !QFileInfo::exists(path))
            continue;
        const Parsed p = parseSeasonEpisode(filenameStem(path));
        if (!p.valid || p.isSpecial || p.season != current.season
            || p.episode >= current.episode)
            continue;
        candidates.push_back({ i, path, p.episode });
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) {
                  return a.episode > b.episode; // closest earlier first
              });
    QStringList refs;
    for (int i = 0; i < candidates.size() && i < 2; ++i)
        refs << candidates.at(i).path;
    return refs;
}

void AudioIntroMatcher::detect(const QStringList &playlist, int currentIndex)
{
    if (busy() || !ready()) {
        castDebug(QStringLiteral("skip: detect blocked busy=%1 ready=%2 node=%3")
                      .arg(busy())
                      .arg(ready())
                      .arg(!m_node.isEmpty()));
        return;
    }
    if (currentIndex < 0 || currentIndex >= playlist.size())
        return;

    const QString mainFile = playlist.at(currentIndex);
    // Cache first: repeats (and re-opens) never recompute.
    if (emitCached(mainFile)) {
        castDebug(QStringLiteral("skip: cache hit %1")
                      .arg(QFileInfo(mainFile).fileName()));
        return;
    }

    const Parsed current = parseSeasonEpisode(filenameStem(mainFile));
    const QStringList refs = selectReferences(playlist, currentIndex, current);
    m_recapRefs =
        (current.valid && !current.isSpecial)
        ? selectRecapReferences(playlist, currentIndex, current)
        : QStringList();
    if (refs.isEmpty()) {
        QStringList names;
        for (const QString &p : playlist)
            names << QFileInfo(p).fileName().left(40);
        castDebug(QStringLiteral("skip: no refs for %1 (list: %2)")
                      .arg(QFileInfo(mainFile).fileName(), names.join('|')));
        // No references: fingerprinting is impossible, but the single-file
        // credits heuristic still applies (movies included).
        startCreditsHeuristic(mainFile, probeDuration(m_ffmpeg, mainFile));
        return;
    }
    castDebug(QStringLiteral("skip: detect %1 refs=%2")
                  .arg(QFileInfo(mainFile).fileName()).arg(refs.size()));
    startExcerpts(mainFile, refs);
}

QString AudioIntroMatcher::cachePathFor(const QString &path) const
{
    const QFileInfo fi(path);
    const QString baseDir = QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation);
    if (baseDir.isEmpty())
        return {};
    const QString key = QStringLiteral("%1-%2-%3")
                            .arg(fi.size())
                            .arg(fi.lastModified().toSecsSinceEpoch())
                            .arg(QString::fromLatin1(
                                QCryptographicHash::hash(path.toUtf8(),
                                                         QCryptographicHash::Sha1)
                                    .toHex()
                                    .left(16)));
    return baseDir + QStringLiteral("/intro-v1/") + key
        + QStringLiteral(".json");
}

bool AudioIntroMatcher::emitCached(const QString &path)
{
    const QString cachePath = cachePathFor(path);
    if (cachePath.isEmpty())
        return false;
    QFile f(cachePath);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject root =
        QJsonDocument::fromJson(f.readAll()).object();
    bool any = false;
    const QJsonArray intro = root.value(QLatin1String("intro")).toArray();
    if (intro.size() == 2 && intro.at(1).toDouble() > intro.at(0).toDouble()) {
        Q_EMIT sectionFound(intro.at(0).toDouble(), intro.at(1).toDouble());
        any = true;
    }
    const QJsonArray outro = root.value(QLatin1String("outro")).toArray();
    if (outro.size() == 2 && outro.at(1).toDouble() > outro.at(0).toDouble()) {
        Q_EMIT outroFound(outro.at(0).toDouble(), outro.at(1).toDouble());
        any = true;
    }
    const QJsonArray recap = root.value(QLatin1String("recap")).toArray();
    if (recap.size() == 2 && recap.at(1).toDouble() > recap.at(0).toDouble()) {
        Q_EMIT recapFound(recap.at(0).toDouble(), recap.at(1).toDouble());
        any = true;
    }
    return any;
}

void AudioIntroMatcher::storeCache(const QString &key, const QString &which,
                                   double start, double end)
{
    if (key.isEmpty())
        return;
    QJsonObject root;
    QFile f(key);
    if (f.open(QIODevice::ReadOnly))
        root = QJsonDocument::fromJson(f.readAll()).object();
    QJsonArray arr;
    arr.append(start);
    arr.append(end);
    root[which] = arr;
    QDir().mkpath(QFileInfo(key).path());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void AudioIntroMatcher::startExcerpts(const QString &mainFile,
                                      const QStringList &refs)
{
    m_mainPath = mainFile;
    m_cacheKey = cachePathFor(mainFile);
    m_mainDur = probeDuration(m_ffmpeg, mainFile);
    if (m_mainDur <= 0.0) {
        Q_EMIT noMatch(tr("A fájl hossza nem olvasható"));
        return;
    }
    m_tailOffset = qMax(0.0, m_mainDur - kTailSeconds);
    m_excerptDir = QStandardPaths::writableLocation(
                       QStandardPaths::CacheLocation)
        + QStringLiteral("/audio-excerpts/")
        + QFileInfo(m_cacheKey.isEmpty() ? mainFile : m_cacheKey)
              .baseName();
    QDir().mkpath(m_excerptDir);

    // Head trims (intro hunt) + tail trims (outro hunt), video-copy fast.
    // Plus a 3-minute head of main (r0) and head/tail pairs of earlier
    // episodes (rh/rt) for the recap hunt (recaps quote those).
    m_trimQueue.clear();
    const QStringList all = QStringList{ mainFile } + refs;
    for (int i = 0; i < all.size(); ++i) {
        const QString head =
            m_excerptDir + QStringLiteral("/h%1.mkv").arg(i);
        m_trimQueue.push_back(
            { {QStringLiteral("-y"), QStringLiteral("-nostdin"),
               QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-i"),
               all.at(i), QStringLiteral("-t"),
               QString::number(kHeadSeconds, 'f', 0), QStringLiteral("-c"),
               QStringLiteral("copy"), head},
              head });
        const QString tail =
            m_excerptDir + QStringLiteral("/t%1.mkv").arg(i);
        m_trimQueue.push_back(
            { {QStringLiteral("-y"), QStringLiteral("-nostdin"),
               QStringLiteral("-v"), QStringLiteral("error"),
               QStringLiteral("-sseof"),
               QString::number(-kTailSeconds, 'f', 0), QStringLiteral("-i"),
               all.at(i), QStringLiteral("-c"), QStringLiteral("copy"), tail},
              tail });
    }
    const QString recapMain = m_excerptDir + QStringLiteral("/r0.mkv");
    m_trimQueue.push_back(
        { {QStringLiteral("-y"), QStringLiteral("-nostdin"),
           QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-i"),
           mainFile, QStringLiteral("-t"),
           QString::number(kRecapSeconds, 'f', 0), QStringLiteral("-c"),
           QStringLiteral("copy"), recapMain},
          recapMain });
    for (int i = 0; i < m_recapRefs.size(); ++i) {
        const QString rh =
            m_excerptDir + QStringLiteral("/rh%1.mkv").arg(i);
        m_trimQueue.push_back(
            { {QStringLiteral("-y"), QStringLiteral("-nostdin"),
               QStringLiteral("-v"), QStringLiteral("error"),
               QStringLiteral("-i"), m_recapRefs.at(i), QStringLiteral("-t"),
               QString::number(kHeadSeconds, 'f', 0), QStringLiteral("-c"),
               QStringLiteral("copy"), rh},
              rh });
        const QString rt =
            m_excerptDir + QStringLiteral("/rt%1.mkv").arg(i);
        m_trimQueue.push_back(
            { {QStringLiteral("-y"), QStringLiteral("-nostdin"),
               QStringLiteral("-v"), QStringLiteral("error"),
               QStringLiteral("-sseof"),
               QString::number(-kTailSeconds, 'f', 0), QStringLiteral("-i"),
               m_recapRefs.at(i), QStringLiteral("-c"), QStringLiteral("copy"),
               rt},
              rt });
    }
    m_helperKind = 0;
    runNextTrim();
}

void AudioIntroMatcher::runNextTrim()
{
    if (m_trimQueue.isEmpty()) {
        // All excerpts ready: intro run on heads.
        QStringList headRefs;
        for (int i = 1; QFile::exists(m_excerptDir
                                      + QStringLiteral("/h%1.mkv").arg(i));
             ++i)
            headRefs << m_excerptDir + QStringLiteral("/h%1.mkv").arg(i);
        m_helperKind = 0;
        startHelper(m_excerptDir + QStringLiteral("/h0.mkv"), headRefs);
        return;
    }
    const TrimJob job = m_trimQueue.takeFirst();
    if (m_trimProc) {
        m_trimProc->deleteLater();
        m_trimProc = nullptr;
    }
    m_trimProc = new QProcess(this);
    connect(m_trimProc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int, QProcess::ExitStatus) { runNextTrim(); });
    m_trimProc->start(m_ffmpeg, job.args);
}

QString AudioIntroMatcher::ensureScript() const
{
    // Test hook (mirrors OMAPLAYER_WIN_W/H in main.cpp): override the bundled
    // helper with a path when the Qt resource is unavailable in a harness.
    const QString overridePath =
        qEnvironmentVariable("OMAPLAYER_AUDIO_HELPER");
    if (!overridePath.isEmpty() && QFileInfo::exists(overridePath))
        return overridePath;

    const QString cacheDir = QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation);
    if (cacheDir.isEmpty())
        return {};
    QDir().mkpath(cacheDir);
    const QString scriptPath = cacheDir + QStringLiteral("/audio-intro-helper.mjs");

    QFile res(QStringLiteral(":/audio-intro/audio-intro-helper.mjs"));
    if (!res.open(QIODevice::ReadOnly))
        return {};
    const QByteArray content = res.readAll();
    res.close();

    QFileInfo existing(scriptPath);
    if (existing.exists() && existing.size() == content.size()) {
        QFile check(scriptPath);
        if (check.open(QIODevice::ReadOnly) && check.readAll() == content)
            return scriptPath;
    }

    QFile out(scriptPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    out.write(content);
    out.close();
    return scriptPath;
}

void AudioIntroMatcher::startHelper(const QString &mainFile, const QStringList &refs)
{
    const QString script = ensureScript();
    if (script.isEmpty()) {
        Q_EMIT noMatch(tr("Az audio-érzékelő script nem elérhető"));
        return;
    }

    const QString cacheDir = QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation)
        + QStringLiteral("/audio-intro-match-cache");
    QDir().mkpath(cacheDir);

    QJsonArray refArray;
    for (const QString &ref : refs)
        refArray.append(ref);
    const QString refsJson = QString::fromLatin1(
        QJsonDocument(refArray).toJson(QJsonDocument::Compact));

    QStringList args = {
        script,            QStringLiteral("--main"), mainFile,
        QStringLiteral("--refs-json"), refsJson,
        QStringLiteral("--ffmpeg"), m_ffmpeg,
        QStringLiteral("--cache-dir"), cacheDir,
    };

    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_proc, &QProcess::finished, this, [this](int code) {
        Q_UNUSED(code)
        onProcessFinished();
    });

    QTimer::singleShot(kHelperTimeoutMs, m_proc, &QProcess::kill);
    m_proc->start(m_node, args);
}

void AudioIntroMatcher::onProcessFinished()
{
    if (!m_proc)
        return;
    const QByteArray stdoutData = m_proc->readAllStandardOutput();
    m_proc->deleteLater();
    m_proc = nullptr;

    const QJsonObject root = QJsonDocument::fromJson(stdoutData).object();
    double start = -1.0, end = -1.0;
    if (root.value(QLatin1String("ok")).toBool(false)
        && root.contains(QLatin1String("output"))) {
        const QJsonObject intro = root.value(QLatin1String("output"))
                                      .toObject()
                                      .value(QLatin1String("intro"))
                                      .toObject();
        start = intro.value(QLatin1String("start_seconds")).toDouble(-1);
        end = intro.value(QLatin1String("end_seconds")).toDouble(-1);
    }
    const bool hit = (start >= 0.0 && end > start);
    castDebug(QStringLiteral("skip: helper kind=%1 hit=%2 %3-%4")
                  .arg(m_helperKind).arg(hit).arg(start, 0, 'f', 1)
                  .arg(end, 0, 'f', 1));
    if (m_helperKind == 0) {
        if (hit) {
            storeCache(m_cacheKey, QStringLiteral("intro"), start, end);
            Q_EMIT sectionFound(start, end);
        }
        // Outro run on the tail excerpts (offsets rebased below).
        QStringList tailRefs;
        for (int i = 1; QFile::exists(m_excerptDir
                                      + QStringLiteral("/t%1.mkv").arg(i));
             ++i)
            tailRefs << m_excerptDir + QStringLiteral("/t%1.mkv").arg(i);
        const QString tailMain =
            m_excerptDir + QStringLiteral("/t0.mkv");
        if (!tailRefs.isEmpty() && QFile::exists(tailMain)) {
            m_helperKind = 1;
            startHelper(tailMain, tailRefs);
            return;
        }
        QDir(m_excerptDir).removeRecursively();
        if (!hit)
            Q_EMIT noMatch(tr("Nincs ismétlődő intró a referencia epizódokban"));
    } else if (m_helperKind == 1) {
        if (hit) {
            start += m_tailOffset;
            end += m_tailOffset;
            storeCache(m_cacheKey, QStringLiteral("outro"), start, end);
            Q_EMIT outroFound(start, end);
        } else {
            // Fingerprint found no shared outro: fall back to the
            // single-file black+quiet heuristic on the same title.
            startCreditsHeuristic(m_mainPath, m_mainDur);
        }
        // Recap run next (its excerpts were trimmed up front).
        QStringList recapRefs;
        for (int i = 0; QFile::exists(m_excerptDir
                                      + QStringLiteral("/rh%1.mkv").arg(i));
             ++i) {
            recapRefs << m_excerptDir + QStringLiteral("/rh%1.mkv").arg(i);
            const QString rt =
                m_excerptDir + QStringLiteral("/rt%1.mkv").arg(i);
            if (QFile::exists(rt))
                recapRefs << rt;
        }
        const QString recapMain =
            m_excerptDir + QStringLiteral("/r0.mkv");
        if (!recapRefs.isEmpty() && QFile::exists(recapMain)) {
            m_helperKind = 2;
            startHelper(recapMain, recapRefs);
            return;
        }
        QDir(m_excerptDir).removeRecursively();
    } else {
        // Recap run: r0 starts at file position 0, so offsets are direct.
        // Only early matches count (a late match is the intro theme, which
        // the intro run reports with cache).
        if (hit && end <= kRecapMaxEnd) {
            storeCache(m_cacheKey, QStringLiteral("recap"), start, end);
            Q_EMIT recapFound(start, end);
        } else {
            castDebug(QStringLiteral("skip: recap no early hit"));
        }
        QDir(m_excerptDir).removeRecursively();
    }
}

void AudioIntroMatcher::startCreditsHeuristic(const QString &mainFile,
                                              double duration)
{
    if (mainFile.isEmpty() || duration < 8.0 * 60.0)
        return; // needs a tail worth scanning
    if (m_creditProc)
        return;
    // Check the cache first: a stored outro (fingerprint or heuristic)
    // answers instantly.
    const QString cachePath = cachePathFor(mainFile);
    if (!cachePath.isEmpty()) {
        QFile cf(cachePath);
        if (cf.open(QIODevice::ReadOnly)) {
            const QJsonArray outro = QJsonDocument::fromJson(cf.readAll())
                                         .object()
                                         .value(QLatin1String("outro"))
                                         .toArray();
            if (outro.size() == 2
                && outro.at(1).toDouble() > outro.at(0).toDouble()) {
                Q_EMIT outroFound(outro.at(0).toDouble(),
                                  outro.at(1).toDouble());
                return;
            }
        }
    }
    m_mainPath = mainFile;
    m_cacheKey = cachePath;
    const double winStart = qMax(0.0, duration - 8.0 * 60.0);
    m_creditBase = winStart;
    m_creditWin = duration - winStart;
    castDebug(QStringLiteral("skip: credits heuristic %1 from %2s")
                  .arg(QFileInfo(mainFile).fileName()).arg(winStart, 0, 'f', 0));
    m_creditProc = new QProcess(this);
    connect(m_creditProc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &AudioIntroMatcher::onCreditsFinished);
    // One decode pass over the tail: black frames + quiet spans.
    m_creditProc->start(
        m_ffmpeg,
        {QStringLiteral("-y"), QStringLiteral("-nostdin"),
         QStringLiteral("-v"), QStringLiteral("info"), QStringLiteral("-ss"),
         QString::number(winStart, 'f', 1), QStringLiteral("-i"), mainFile,
         QStringLiteral("-vf"),
         QStringLiteral("blackdetect=d=2:pix_th=0.10"),
         QStringLiteral("-af"),
         QStringLiteral("silencedetect=noise=-40dB:d=2"),
         QStringLiteral("-f"), QStringLiteral("null"),
         QStringLiteral("-")});
}

void AudioIntroMatcher::onCreditsFinished()
{
    if (!m_creditProc)
        return;
    const QByteArray log = m_creditProc->readAllStandardError();
    m_creditProc->deleteLater();
    m_creditProc = nullptr;

    // Collect black spans (ffmpeg timestamps relative to the -ss seek).
    // Credits = black reaching (nearly) to EOF: rolling text over black.
    // Quiet is NOT required — most credit rolls carry music. False positives
    // (dark finale scenes) are cut by the EOF-proximity rule below.
    struct Span {
        double s = 0.0, e = 0.0;
    };
    QVector<Span> blacks;
    static const QRegularExpression blackRe(
        QStringLiteral("black_start:(\\S+) black_end:(\\S+)"));
    for (const QByteArray &raw : log.split('\n')) {
        const QString line = QString::fromUtf8(raw);
        const QRegularExpressionMatch m = blackRe.match(line);
        if (m.hasMatch()) {
            blacks.push_back({ m.captured(1).toDouble(),
                               m.captured(2).toDouble() });
        }
    }
    double eof = 0.0;
    for (const Span &b : blacks)
        eof = qMax(eof, b.e);
    // Latest black must touch the true window end (within 20 s): a dark
    // scene followed by picture is not credits. Total black from the
    // candidate start ≥ 20 s.
    double best = -1.0;
    if (eof > 0.0 && m_creditWin > 0.0 && m_creditWin - eof <= 20.0) {
        for (const Span &b : blacks) {
            double total = 0.0;
            for (const Span &c : blacks) {
                if (c.e < b.s)
                    continue;
                total += qMin(c.e, eof) - qMax(c.s, b.s);
            }
            if (total >= 20.0 && m_creditWin - b.e <= 90.0) {
                if (best < 0.0 || b.s < best)
                    best = b.s;
            }
        }
    }
    if (best < 0.0) {
        castDebug(QStringLiteral("skip: credits heuristic no hit"));
        return;
    }
    // Rebase: the window started at (duration - 480 s); recover duration.
    const double duration = probeDuration(m_ffmpeg, m_mainPath);
    const double absStart = qMax(0.0, duration - 8.0 * 60.0) + best;
    castDebug(QStringLiteral("skip: credits heuristic %1-%2")
                  .arg(absStart, 0, 'f', 1).arg(duration, 0, 'f', 0));
    storeCache(m_cacheKey, QStringLiteral("outro"), absStart, duration);
    Q_EMIT outroFound(absStart, duration);
}
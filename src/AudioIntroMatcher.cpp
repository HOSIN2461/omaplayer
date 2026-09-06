#include "AudioIntroMatcher.h"

#include <QCoreApplication>
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

        // Contiguous same-season run around the current episode.
        int previousEpisode = current.episode;
        for (int i = currentIndex - 1; i >= 0; --i) {
            const Candidate *c = byIndex.value(i, nullptr);
            if (!c || !c->parsed.valid || c->parsed.isSpecial
                || c->parsed.season != current.season || c->parsed.episode >= previousEpisode)
                break;
            selected.push_back(*c);
            previousEpisode = c->parsed.episode;
        }
        int nextEpisode = current.episode;
        for (int i = currentIndex + 1; i < playlist.size(); ++i) {
            const Candidate *c = byIndex.value(i, nullptr);
            if (!c || !c->parsed.valid || c->parsed.isSpecial
                || c->parsed.season != current.season || c->parsed.episode <= nextEpisode)
                break;
            selected.push_back(*c);
            nextEpisode = c->parsed.episode;
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

void AudioIntroMatcher::detect(const QStringList &playlist, int currentIndex)
{
    if (busy() || !ready())
        return;
    if (currentIndex < 0 || currentIndex >= playlist.size())
        return;

    const Parsed current = parseSeasonEpisode(filenameStem(playlist.at(currentIndex)));
    const QStringList refs = selectReferences(playlist, currentIndex, current);
    if (refs.isEmpty()) {
        Q_EMIT noMatch(
            tr("Nincs használható referencia epizód a lejátszási listában"));
        return;
    }
    startHelper(playlist.at(currentIndex), refs);
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
    if (root.value(QLatin1String("ok")).toBool(false)
        && root.contains(QLatin1String("output"))) {
        const QJsonObject intro = root.value(QLatin1String("output"))
                                      .toObject()
                                      .value(QLatin1String("intro"))
                                      .toObject();
        const double start = intro.value(QLatin1String("start_seconds")).toDouble(-1);
        const double end = intro.value(QLatin1String("end_seconds")).toDouble(-1);
        if (start >= 0.0 && end > start)
            Q_EMIT sectionFound(start, end);
        else
            Q_EMIT noMatch(tr("Érvénytelen audio-mérkőzés eredmény"));
    } else {
        QString reason = root.value(QLatin1String("message")).toString();
        Q_EMIT noMatch(reason.isEmpty() ? tr("Nincs ismétlődő intró a referencia epizódokban")
                                        : reason);
    }
}
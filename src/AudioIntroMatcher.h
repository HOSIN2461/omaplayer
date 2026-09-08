#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

// Audio-fingerprint intro/outro detection for chapter-less episodes.
//
// The heavy lifting lives in the upstream iina-skip-intro "audio-intro-match"
// helper (https://github.com/pparanoiidd/iina-skip-intro) which we vendor as a
// Qt resource and run with any available `node`. This class only narrows the
// playlist down to a few sensible reference episodes (same-season neighbors,
// or playlist neighbors) and pipes the JSON result back.
//
// Speed comes from excerpts, not full files: the intro run compares the
// first 12 minutes, the outro run the last 15 minutes (video-copy trims,
// seconds each). Results are cached by content key, so repeats are instant.
// Episode-ness for the duration guards: series context (SxxEyy or playlist
// run) beats the 90-minute movie rule.
class AudioIntroMatcher : public QObject
{
    Q_OBJECT
public:
    explicit AudioIntroMatcher(QObject *parent = nullptr);

    // Runs detection over the width of the playlist, never overlapping:
    // a busy matcher ignores new requests. Signals below fire once.
    void detect(const QStringList &playlist, int currentIndex);
    bool busy() const
    {
        return m_proc != nullptr || m_trimProc != nullptr
            || m_creditProc != nullptr;
    }
    // True when both `node` and `ffmpeg` resolve on this system.
    bool ready() const;
    // Series-like path (SxxEyy …, not a special)? Used for duration guards.
    static bool isSeriesPath(const QString &path);

signals:
    void sectionFound(double start, double end);
    void outroFound(double start, double end);
    void recapFound(double start, double end);
    void noMatch(const QString &reason);

private:
    struct Parsed {
        int season = -1;
        int episode = -1;
        bool isSpecial = false;
        bool valid = false;
    };

    static QString findExecutableOr(const QString &name,
                                    const QStringList &fallbacks);
    static QString filenameStem(const QString &path);
    static bool isVideoPath(const QString &path);
    static bool isBadReference(const QString &path);
    static double probeDuration(const QString &ffprobe, const QString &path);
    Parsed parseSeasonEpisode(const QString &stem) const;
    QStringList selectReferences(const QStringList &playlist,
                                 int currentIndex, const Parsed &current) const;
    // Recap sources: same-season EARLIER episodes (recaps quote those).
    QStringList selectRecapReferences(const QStringList &playlist,
                                      int currentIndex,
                                      const Parsed &current) const;

    // Result cache (intro + outro JSON) keyed by path+size+mtime.
    QString cachePathFor(const QString &path) const;
    bool emitCached(const QString &path);
    void storeCache(const QString &key, const QString &which, double start,
                    double end);

    // Excerpt chain (video-copy trims) feeding the two helper runs.
    void startExcerpts(const QString &mainFile, const QStringList &refs);
    void runNextTrim();
    // Single-file end-credits fallback (no refs needed): sustained black +
    // quiet in the tail. Conservative by design (last 8 min only).
    void startCreditsHeuristic(const QString &mainFile, double duration);
    void onCreditsFinished();
    void startHelper(const QString &mainFile, const QStringList &refs);
    QString ensureScript() const;
    void onProcessFinished();

    QString m_node;
    QString m_ffmpeg;
    QProcess *m_proc = nullptr; // one-shot per detection run
    QProcess *m_trimProc = nullptr; // excerpt chain worker
    QProcess *m_creditProc = nullptr; // credits-heuristic worker
    double m_creditBase = 0.0; // window start (absolute s) of heuristic run
    double m_creditWin = 0.0; // window length (s) of heuristic run
    struct TrimJob {
        QStringList args;
        QString dst;
    };
    QVector<TrimJob> m_trimQueue;
    QString m_excerptDir;
    QString m_mainPath; // original main file (cache key + stale guard)
    QString m_cacheKey;
    QStringList m_recapRefs; // original earlier-episode files for recap hunt
    double m_mainDur = 0.0;
    int m_helperKind = 0; // 0 = intro (head), 1 = outro (tail)
    double m_tailOffset = 0.0; // added to outro hits
};
#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

// Audio-fingerprint intro detection for chapter-less episodes.
//
// The heavy lifting lives in the upstream iina-skip-intro "audio-intro-match"
// helper (https://github.com/pparanoiidd/iina-skip-intro) which we vendor as a
// Qt resource and run with any available `node`. This class only narrows the
// playlist down to a few sensible reference episodes (same-season neighbors,
// or playlist neighbors) and pipes the JSON result back.
class AudioIntroMatcher : public QObject
{
    Q_OBJECT
public:
    explicit AudioIntroMatcher(QObject *parent = nullptr);

    // Runs detection over the width of the playlist, never overlapping:
    // a busy matcher ignores new requests. Signals below fire once.
    void detect(const QStringList &playlist, int currentIndex);
    bool busy() const { return m_proc != nullptr; }
    // True when both `node` and `ffmpeg` resolve on this system.
    bool ready() const;

signals:
    void sectionFound(double start, double end);
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
    Parsed parseSeasonEpisode(const QString &stem) const;
    QStringList selectReferences(const QStringList &playlist,
                                 int currentIndex, const Parsed &current) const;

    void startHelper(const QString &mainFile, const QStringList &refs);
    QString ensureScript() const;
    void onProcessFinished();

    QString m_node;
    QString m_ffmpeg;
    QProcess *m_proc = nullptr; // one-shot per detection run
};
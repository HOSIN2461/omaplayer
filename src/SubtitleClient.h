#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration>

class QNetworkAccessManager;
class QNetworkReply;

// OpenSubtitles.com REST API v2 client — search and download subtitles for
// the currently playing video file.
//
// Usage: search() sends the current media title/filename to the API, results
// appear in the `results` list property, download(fileId) fetches the chosen
// subtitle to a temp file and emits downloaded(path).
//
// API key is stored in QSettings ("opensubtitles/apiKey") and configurable
// from QML via setApiKey().
class SubtitleClient : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool hasApiKey READ hasApiKey NOTIFY apiKeyChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)

public:
    explicit SubtitleClient(QObject *parent = nullptr);

    QVariantList results() const { return m_results; }
    bool busy() const { return m_busy; }
    bool hasApiKey() const;
    QString status() const { return m_status; }

    // API key management (stored in QSettings).
    Q_INVOKABLE void setApiKey(const QString &key);
    Q_INVOKABLE QString apiKey() const;

    // Search subtitles for the given query (filename or title).
    Q_INVOKABLE void search(const QString &query);

    // Download a subtitle file by file_id; emits downloaded(path) on success.
    Q_INVOKABLE void download(int fileId);

    // Shorthand: search by the currently open mpv file path.
    Q_INVOKABLE void searchForCurrentFile(const QString &filePath,
                                          const QString &mediaTitle = QString());

signals:
    void resultsChanged();
    void busyChanged();
    void apiKeyChanged();
    void statusChanged();
    void downloaded(const QString &path);
    void errorOccurred(const QString &message);

private:
    void setBusy(bool b);
    void setStatus(const QString &s);
    static QString apiKeyFromSettings();
    static void saveApiKey(const QString &key);

    QNetworkAccessManager *m_net = nullptr;
    QVariantList m_results;
    bool m_busy = false;
    QString m_status;
    static constexpr const char *kBaseUrl = "https://api.opensubtitles.com/api/v1";
};

#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QProcess;

// Self-update against the GitHub Releases feed (API). One upload-only
// dependency: the .pkg.tar.zst attached to the newest release. Install is
// delegated to `sudo pacman -U` inside a spawned foot terminal — pacman
// upgrades need root, and the session has no polkit agent for pkexec, so a
// visible terminal lets the user enter the password once.
class Updater : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY updateAvailableChanged)
    Q_PROPERTY(QString changelog READ changelog NOTIFY updateAvailableChanged)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY updateAvailableChanged)
    Q_PROPERTY(bool downloaded READ downloaded NOTIFY downloadedChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    explicit Updater(QObject *parent = nullptr);

    QString status() const { return m_status; }
    QString latestVersion() const { return m_latestVersion; }
    QString changelog() const { return m_changelog; }
    bool updateAvailable() const { return m_updateAvailable; }
    bool downloaded() const { return m_downloaded; }
    bool busy() const { return m_busy; }

    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void downloadPackage();
    Q_INVOKABLE void installPackage();

signals:
    void statusChanged();
    void updateAvailableChanged();
    void downloadedChanged();
    void busyChanged();

private:
    void setBusy(bool busy);
    void setStatus(const QString &status);

    QNetworkAccessManager *m_nam;
    QProcess *m_installProc = nullptr;
    QProcess *m_checkProc = nullptr;

    QString m_status;
    QString m_latestVersion;
    QString m_changelog;
    QString m_assetUrl;
    QString m_assetName;
    QString m_downloadPath;
    bool m_updateAvailable = false;
    bool m_downloaded = false;
    bool m_busy = false;
};
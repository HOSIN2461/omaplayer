#include "Updater.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>

namespace {

QString latestUrl()
{
    return QStringLiteral("https://api.github.com/repos/HOSIN2461/omaplayer/"
                          "releases/latest");
}

// True when |remote| is strictly newer than |current| (0.1.9 < 0.1.10).
bool versionNewer(const QString &remote, const QString &current)
{
    const QStringList a = remote.split(QLatin1Char('.'));
    const QStringList b = current.split(QLatin1Char('.'));
    const int n = qMax(a.size(), b.size());
    for (int i = 0; i < n; i++) {
        const int x = i < a.size() ? a[i].toInt() : 0;
        const int y = i < b.size() ? b[i].toInt() : 0;
        if (x != y)
            return x > y;
    }
    return false;
}

} // namespace

Updater::Updater(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

void Updater::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

void Updater::setStatus(const QString &status)
{
    if (m_status == status)
        return;
    m_status = status;
    emit statusChanged();
}

void Updater::checkForUpdates()
{
    if (m_busy)
        return;

    setBusy(true);
    setStatus(QStringLiteral("Frissítések keresése…"));

    QNetworkRequest req{QUrl{latestUrl()}};
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("omaplayer-updater/")
                      + QCoreApplication::applicationVersion());
    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            setStatus(QStringLiteral("Ellenőrzés nem sikerült: %1")
                          .arg(reply->errorString()));
            setBusy(false);
            return;
        }
        const QJsonObject root = QJsonDocument::fromJson(
                                     reply->readAll()).object();
        QString tag = root.value(QLatin1String("tag_name")).toString();
        if (!tag.isEmpty() && tag.startsWith(QLatin1Char('v')))
            tag.remove(0, 1);

        const QString current = QCoreApplication::applicationVersion();
        if (tag.isEmpty() || !versionNewer(tag, current)) {
            m_updateAvailable = false;
            m_downloaded = false;
            m_downloadPath.clear();
            m_latestVersion.clear();
            m_changelog.clear();
            m_assetUrl.clear();
            m_assetName.clear();
            setStatus(QStringLiteral("Naprakész (%1)").arg(current));
            emit updateAvailableChanged();
            emit downloadedChanged();
            setBusy(false);
            return;
        }

        m_updateAvailable = true;
        m_latestVersion = tag;
        m_changelog = root.value(QLatin1String("body")).toString();

        m_assetUrl.clear();
        m_assetName.clear();
        const QJsonArray assets = root.value(QLatin1String("assets")).toArray();
        for (const QJsonValue &v : assets) {
            const QJsonObject a = v.toObject();
            const QString name = a.value(QLatin1String("name")).toString();
            if (name.contains(QLatin1String("x86_64"))
                && name.endsWith(QLatin1String(".pkg.tar.zst"))) {
                m_assetName = name;
                m_assetUrl = a.value(QLatin1String("browser_download_url"))
                                 .toString();
                break;
            }
        }
        m_downloaded = false;
        m_downloadPath.clear();
        setStatus(QStringLiteral("Új verzió elérhető: %1").arg(tag));
        emit updateAvailableChanged();
        emit downloadedChanged();
        setBusy(false);
    });
}

void Updater::downloadPackage()
{
    if (m_busy || !m_updateAvailable || m_assetUrl.isEmpty() || m_downloaded)
        return;

    setBusy(true);
    setStatus(QStringLiteral("Csomag letöltése…"));

    const QString cacheDir = QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation);
    QDir().mkpath(cacheDir);
    m_downloadPath = cacheDir + QLatin1Char('/') + m_assetName;

    QNetworkRequest req{QUrl{m_assetUrl}};
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("omaplayer-updater/")
                      + QCoreApplication::applicationVersion());
    QNetworkReply *reply = m_nam->get(req);

    if (QFile::exists(m_downloadPath))
        QFile::remove(m_downloadPath);

    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            setStatus(QStringLiteral("Letöltés nem sikerült: %1")
                          .arg(reply->errorString()));
            setBusy(false);
            return;
        }
        QFile out(m_downloadPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || out.write(reply->readAll()) < 0) {
            setStatus(QStringLiteral("Csomag mentése nem sikerült"));
            setBusy(false);
            return;
        }
        out.close();
        m_downloaded = true;
        setStatus(QStringLiteral("Letöltve: %1").arg(m_downloadPath));
        emit downloadedChanged();
        setBusy(false);
    });
}

void Updater::installPackage()
{
    if (m_busy || !m_downloaded || m_downloadPath.isEmpty())
        return;

    setBusy(true);
    setStatus(QStringLiteral("Telepítő ablak megnyitása…"));

    m_installProc = new QProcess(this);
    connect(m_installProc, &QProcess::finished, this, [this](int code) {
        const bool ok = (m_installProc->exitStatus() == QProcess::NormalExit
                         && code == 0);
        setStatus(ok
                      ? QStringLiteral("Telepítés kész — indítsd újra az appot")
                      : QStringLiteral("Telepítés megszakítva (%1)").arg(code));
        m_installProc->deleteLater();
        m_installProc = nullptr;
        setBusy(false);
    });

    const QString cmd = QStringLiteral(
        "sudo pacman -U --noconfirm '%1'; echo; "
        "read -p 'Kész — nyomj Entert az ablak bezárásához'")
        .arg(m_downloadPath);
    m_installProc->start(QStringLiteral("foot"),
                         { QStringLiteral("-e"), QStringLiteral("sh"),
                           QStringLiteral("-lc"), cmd });
}
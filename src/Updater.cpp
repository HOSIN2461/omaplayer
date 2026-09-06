#include "Updater.h"
#include "MpvCore.h"

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

void Updater::autoUpdate()
{
    m_auto = true;
    checkForUpdates();
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
        if (m_auto && !m_assetUrl.isEmpty())
            downloadPackage();
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
        if (m_auto)
            installPackage();
    });
}

void Updater::installPackage()
{
    if (m_busy || !m_downloaded || m_downloadPath.isEmpty())
        return;

    setBusy(true);
    setStatus(QStringLiteral("Telepítés ~/.local könyvtárba…"));

    const QString cacheDir = QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation);
    const QString stageDir = cacheDir + QDir::separator()
        + QStringLiteral("stage-") + m_latestVersion;
    QDir stage(stageDir);
    if (stage.exists())
        stage.removeRecursively();
    QDir().mkpath(stageDir);

    // Extract the Arch package (structure: usr/bin, usr/share/...).
    const int rc = QProcess::execute(
        QStringLiteral("tar"),
        { QStringLiteral("--zstd"), QStringLiteral("-xf"), m_downloadPath,
          QStringLiteral("-C"), stageDir });
    if (rc != 0) {
        setStatus(QStringLiteral("Kicsomagolás nem sikerült (%1)").arg(rc));
        setBusy(false);
        return;
    }

    const QString home = QDir::homePath();
    const QString localBin = home + QStringLiteral("/.local/bin");
    const QString localShare = home + QStringLiteral("/.local/share");
    const QString usrDir = stageDir + QDir::separator() + QStringLiteral("usr");

    // Merge the package into the user's own tree — no root required.
    const QString srcBin = usrDir + QStringLiteral("/bin");
    const QString srcShare = usrDir + QStringLiteral("/share");
    auto copyTree = [](const QString &src, const QString &dest) {
        QDir().mkpath(dest);
        // Skip the existing destination files before copying: the updater
        // replaces its own (currently running) binary, and a plain `cp -a`
        // hits ETXTBSY ("Text file busy") when the target is executing.
        return QProcess::execute(QStringLiteral("cp"),
                                 { QStringLiteral("-a"),
                                   QStringLiteral("--remove-destination"),
                                   src + QStringLiteral("/."),
                                   dest + QStringLiteral("/") });
    };

    if (!QDir(srcBin).exists()
        || copyTree(srcBin, localBin) != 0) {
        setStatus(QStringLiteral("Másolás a ~/.local/bin-be nem sikerült"));
        setBusy(false);
        return;
    }
    if (copyTree(srcShare, localShare) != 0) {
        setStatus(QStringLiteral("Másolás a ~/.local/share-be nem sikerült"));
        setBusy(false);
        return;
    }

    // Let the desktop entry always start the user-local binary,
    // independent of PATH order.
    const QString desktopFile = localShare
        + QStringLiteral("/applications/omaplayer.desktop");
    if (QFile::exists(desktopFile)) {
        QFile f(desktopFile);
        if (f.open(QIODevice::ReadOnly)) {
            QString text = QString::fromUtf8(f.readAll());
            f.close();
            text.replace(QStringLiteral("Exec=omaplayer"),
                         QStringLiteral("Exec=") + localBin
                             + QStringLiteral("/omaplayer"));
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(text.toUtf8());
            f.close();
        }
    }

    // Version stamp so the updater knows what is deployed.
    QDir(localShare + QStringLiteral("/omaplayer")).mkpath(QStringLiteral("."));
    QFile stamp(localShare + QStringLiteral("/omaplayer/VERSION"));
    if (stamp.open(QIODevice::WriteOnly | QIODevice::Truncate))
        stamp.write(m_latestVersion.toUtf8());

    // Relaunch the freshly deployed binary (resuming the current file) and
    // close this instance.
    QStringList args;
    const QString curFile = MpvCore::instance()->filePath();
    if (!curFile.isEmpty())
        args << curFile;
    QProcess::startDetached(localBin + QStringLiteral("/omaplayer"), args);
    setStatus(QStringLiteral("Feltelepítve — újraindítás…"));
    QCoreApplication::exit(0);
}
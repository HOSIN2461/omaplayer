#include "DirModel.h"

DirModel::DirModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

QString DirModel::parentPath() const
{
    if (m_folder.isEmpty())
        return {};
    QDir d(m_folder);
    if (!d.cdUp())
        return m_folder;
    return d.path();
}

bool DirModel::canGoUp() const
{
    if (m_folder.isEmpty())
        return false;
    return parentPath() != m_folder;
}

void DirModel::setFolder(const QString &path)
{
    QString clean = path;
    if (clean.startsWith(QStringLiteral("file://")))
        clean = QUrl(clean).toLocalFile();
    clean = QDir::cleanPath(clean);
    if (clean == m_folder) {
        refresh();
        return;
    }
    QDir d(clean);
    if (!d.exists() || !d.isReadable())
        return;
    m_folder = clean;
    reload();
    Q_EMIT folderChanged();
}

void DirModel::cdUp()
{
    if (canGoUp())
        setFolder(parentPath());
}

void DirModel::refresh()
{
    reload();
    Q_EMIT folderChanged();
}

void DirModel::setNameFilters(const QStringList &filters)
{
    if (filters == m_filters)
        return;
    m_filters = filters;
    reload();
    Q_EMIT filtersChanged();
}

void DirModel::setShowHidden(bool on)
{
    if (on == m_hidden)
        return;
    m_hidden = on;
    reload();
    Q_EMIT filtersChanged();
}

int DirModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_entries.size();
}

QVariant DirModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0
        || index.row() >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(index.row());
    switch (role) {
    case NameRole:
        return e.name;
    case PathRole:
        return e.path;
    case IsDirRole:
        return e.isDir;
    case SizeRole:
        return e.size;
    default:
        return {};
    }
}

QHash<int, QByteArray> DirModel::roleNames() const
{
    return {
        {NameRole, "name"},
        {PathRole, "path"},
        {IsDirRole, "isDir"},
        {SizeRole, "size"},
    };
}

QVariantList DirModel::entries() const
{
    QVariantList out;
    for (const Entry &e : m_entries) {
        QVariantMap m;
        m.insert(QStringLiteral("path"), e.path);
        m.insert(QStringLiteral("isDir"), e.isDir);
        out.append(m);
    }
    return out;
}

void DirModel::reload()
{
    beginResetModel();
    m_entries.clear();
    QDir d(m_folder);
    if (d.exists() && d.isReadable()) {
        QDir::Filters ff = QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot
            | QDir::Readable;
        if (m_hidden)
            ff |= QDir::Hidden;
        const QFileInfoList infos = d.entryInfoList(ff, QDir::NoSort);
        for (const QFileInfo &info : infos) {
            if (info.fileName().isEmpty())
                continue;
            if (!info.isDir()) {
                bool kept = m_filters.isEmpty();
                for (const QString &pat : m_filters) {
                    if (QDir::match(pat.trimmed(), info.fileName())) {
                        kept = true;
                        break;
                    }
                }
                if (!kept)
                    continue;
            }
            Entry e;
            e.name = info.fileName();
            e.path = info.absoluteFilePath();
            e.isDir = info.isDir();
            e.size = info.isDir() ? 0 : info.size();
            m_entries.append(e);
        }
        std::sort(m_entries.begin(), m_entries.end(),
                  [](const Entry &a, const Entry &b) {
                      if (a.isDir != b.isDir)
                          return a.isDir > b.isDir;
                      return a.name.toLower() < b.name.toLower();
                  });
    }
    endResetModel();
}

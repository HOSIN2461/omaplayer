#pragma once

#include <QAbstractListModel>
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QUrl>
#include <QtQmlIntegration>

// Saját címtár-modell a QML FileBrowserhöz: a Qt.labs.folderlistmodel
// helyett, mert annak szerepnevei/szűrési viselkedése verziófüggően
// bizonytalan volt (mappák hol látszottak, hol nem; kattintás néma).
// Itt minden determinisztikus: szerepek (name/path/isDir/size),
// mappák mindig látszanak (csak fájlokra szűr), rendezés: mappa elöl,
// aztán név (kisbetű-érzéketlen).
class DirModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString folderPath READ folderPath WRITE setFolder NOTIFY folderChanged)
    Q_PROPERTY(QString parentPath READ parentPath NOTIFY folderChanged)
    Q_PROPERTY(bool canGoUp READ canGoUp NOTIFY folderChanged)
    Q_PROPERTY(QStringList nameFilters READ nameFilters WRITE setNameFilters NOTIFY filtersChanged)
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY filtersChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY folderChanged)

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        PathRole,
        IsDirRole,
        SizeRole,
    };

    explicit DirModel(QObject *parent = nullptr);

    QString folderPath() const { return m_folder; }
    QString parentPath() const;
    bool canGoUp() const;
    QStringList nameFilters() const { return m_filters; }
    bool showHidden() const { return m_hidden; }

    Q_INVOKABLE void setFolder(const QString &path);
    Q_INVOKABLE void cdUp();
    Q_INVOKABLE void refresh();
    // Minden sor {path, isDir} párja (tartomány-kijelöléshez a QML-ben).
    Q_INVOKABLE QVariantList entries() const;

    void setNameFilters(const QStringList &filters);
    void setShowHidden(bool on);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void folderChanged();
    void filtersChanged();

private:
    struct Entry {
        QString name;
        QString path;
        bool isDir = false;
        qlonglong size = 0;
    };
    void reload();

    QString m_folder;
    QStringList m_filters;
    bool m_hidden = false;
    QList<Entry> m_entries;
};

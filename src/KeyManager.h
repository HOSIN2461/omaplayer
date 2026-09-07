#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QPair>
#include <QtQmlIntegration>

// Configurable keyboard shortcuts.
//
// Every action the player ships with has a default keybinding; the user can
// remap any of them from the settings panel. Bindings are persisted in
// QSettings ("keys/<action>=<sequence>"). Each action is exposed as a QML
// property (playPause, seekBackward, …) that NOTIFYs on bindingsChanged, so
// the Shortcut items' `sequence` bindings refresh live when the user remaps.
class KeyManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QStringList actionIds READ actionIds CONSTANT)
    Q_PROPERTY(bool modified READ modified NOTIFY bindingsChanged)
    Q_PROPERTY(QString playPause READ key_playPause NOTIFY bindingsChanged)
    Q_PROPERTY(QString seekBackward READ key_seekBackward NOTIFY bindingsChanged)
    Q_PROPERTY(QString seekForward READ key_seekForward NOTIFY bindingsChanged)
    Q_PROPERTY(QString volumeUp READ key_volumeUp NOTIFY bindingsChanged)
    Q_PROPERTY(QString volumeDown READ key_volumeDown NOTIFY bindingsChanged)
    Q_PROPERTY(QString mute READ key_mute NOTIFY bindingsChanged)
    Q_PROPERTY(QString fullscreen READ key_fullscreen NOTIFY bindingsChanged)
    Q_PROPERTY(QString minimize READ key_minimize NOTIFY bindingsChanged)
    Q_PROPERTY(QString settings READ key_settings NOTIFY bindingsChanged)
    Q_PROPERTY(QString playlist READ key_playlist NOTIFY bindingsChanged)
    Q_PROPERTY(QString jellyfin READ key_jellyfin NOTIFY bindingsChanged)
    Q_PROPERTY(QString speedHalve READ key_speedHalve NOTIFY bindingsChanged)
    Q_PROPERTY(QString speedDouble READ key_speedDouble NOTIFY bindingsChanged)
    Q_PROPERTY(QString nextItem READ key_nextItem NOTIFY bindingsChanged)
    Q_PROPERTY(QString prevItem READ key_prevItem NOTIFY bindingsChanged)
    Q_PROPERTY(QString removeSelected READ key_removeSelected NOTIFY bindingsChanged)
    Q_PROPERTY(QString toggleSearch READ key_toggleSearch NOTIFY bindingsChanged)
    Q_PROPERTY(QString openFile READ key_openFile NOTIFY bindingsChanged)
    Q_PROPERTY(QString screenshot READ key_screenshot NOTIFY bindingsChanged)
    Q_PROPERTY(QString stats READ key_stats NOTIFY bindingsChanged)
    Q_PROPERTY(QString volume100 READ key_volume100 NOTIFY bindingsChanged)
    Q_PROPERTY(QString escape READ key_escape NOTIFY bindingsChanged)

public:
    explicit KeyManager(QObject *parent = nullptr);

    QStringList actionIds() const;
    bool modified() const;

    // The effective sequence for `action` — the user override if one exists,
    // otherwise the built-in default. Empty when unknown.
    Q_INVOKABLE QString binding(const QString &action) const;
    // True if the user stored an override for `action`.
    Q_INVOKABLE bool hasOverride(const QString &action) const;
    Q_INVOKABLE QString defaultFor(const QString &action) const;
    // Position in the settings list (stable ordering).
    Q_INVOKABLE int sortIndex(const QString &action) const;
    // Hungarian display name for the settings list.
    Q_INVOKABLE QString labelFor(const QString &action) const;
    // Group id for sectioning the settings list (playback/seek/volume/ui/other).
    Q_INVOKABLE QString groupFor(const QString &action) const;

    Q_INVOKABLE void setBinding(const QString &action, const QString &sequence);
    Q_INVOKABLE void resetBinding(const QString &action);
    Q_INVOKABLE void resetAll();

    QString key_playPause() const { return binding("playPause"); }
    QString key_seekBackward() const { return binding("seekBackward"); }
    QString key_seekForward() const { return binding("seekForward"); }
    QString key_volumeUp() const { return binding("volumeUp"); }
    QString key_volumeDown() const { return binding("volumeDown"); }
    QString key_mute() const { return binding("mute"); }
    QString key_fullscreen() const { return binding("fullscreen"); }
    QString key_minimize() const { return binding("minimize"); }
    QString key_settings() const { return binding("settings"); }
    QString key_playlist() const { return binding("playlist"); }
    QString key_jellyfin() const { return binding("jellyfin"); }
    QString key_speedHalve() const { return binding("speedHalve"); }
    QString key_speedDouble() const { return binding("speedDouble"); }
    QString key_nextItem() const { return binding("nextItem"); }
    QString key_prevItem() const { return binding("prevItem"); }
    QString key_removeSelected() const { return binding("removeSelected"); }
    QString key_toggleSearch() const { return binding("toggleSearch"); }
    QString key_openFile() const { return binding("openFile"); }
    QString key_screenshot() const { return binding("screenshot"); }
    QString key_stats() const { return binding("stats"); }
    QString key_volume100() const { return binding("volume100"); }
    QString key_escape() const { return binding("escape"); }

signals:
    void bindingsChanged();

private:
    // Default bindings table (action → default sequence + Hungarian label).
    struct Def {
        QString sequence;
        QString label;
        QString group;          // playback / seek / volume / ui / other
    };
    static const QList<QPair<QString, Def>> &definitions();
    static QString settingKey(const QString &action);

    QStringList m_actions;      // order from the definitions table
};
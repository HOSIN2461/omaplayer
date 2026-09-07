#include "KeyManager.h"

#include <QSettings>

using namespace Qt::Literals::StringLiterals;

const QList<QPair<QString, KeyManager::Def>> &KeyManager::definitions()
{
    // (action, { default sequence, Hungarian label, group }). Order = menu order.
    static const QList<QPair<QString, KeyManager::Def>> table = {
        { "playPause",     { "Space",    "Lejátszás / szünet", "playback" } },
        { "nextItem",      { "N",        "Következő média", "playback" } },
        { "prevItem",      { "P",        "Előző média", "playback" } },
        { "speedHalve",    { "[",        "Sebesség felezés", "playback" } },
        { "speedDouble",   { "]",        "Sebesség duplázás", "playback" } },
        { "seekBackward",  { "Left",     "Visszalépés 5 mp", "seek" } },
        { "seekForward",   { "Right",    "Előrelépés 5 mp", "seek" } },
        { "volumeUp",      { "Up",       "Hangerő növelés", "volume" } },
        { "volumeDown",    { "Down",     "Hangerő csökkentés", "volume" } },
        { "mute",          { "M",        "Némítás", "volume" } },
        { "volume100",     { "Ctrl+0",   "Hangerő 100%", "volume" } },
        { "fullscreen",    { "F",        "Teljes képernyő", "ui" } },
        { "minimize",      { "I",        "Kis méret", "ui" } },
        { "toggleSearch",  { "Ctrl+F",   "Keresés", "ui" } },
        { "stats",         { "Ctrl+I",   "Lejátszási statisztika", "ui" } },
        { "settings",      { "G",        "Beállítások", "other" } },
        { "playlist",      { "L",        "Lejátszási lista", "other" } },
        { "jellyfin",      { "J",        "Jellyfin", "other" } },
        { "openFile",      { "Ctrl+O",   "Média megnyitása", "other" } },
        { "screenshot",    { "Ctrl+S",   "Képernyőkép", "other" } },
        { "removeSelected",{ "Delete",   "Kiválasztott törlése", "other" } },
        { "escape",        { "Esc",      "Bezár / kivonás", "other" } },
    };
    return table;
}

const QString kGroup = QStringLiteral("keys");

KeyManager::KeyManager(QObject *parent)
    : QObject(parent)
{
    for (const auto &[action, d] : definitions())
        m_actions.append(action);
}

QStringList KeyManager::actionIds() const
{
    return m_actions;
}

bool KeyManager::modified() const
{
    QSettings s;
    s.beginGroup(kGroup);
    const auto stored = s.childKeys();
    for (const auto &action : m_actions) {
        if (stored.contains(action))
            return true;
    }
    return false;
}

QString KeyManager::settingKey(const QString &action)
{
    return action;
}

bool KeyManager::hasOverride(const QString &action, int) const
{
    QSettings s;
    s.beginGroup(kGroup);
    return s.contains(settingKey(action));
}

QString KeyManager::defaultFor(const QString &action) const
{
    for (const auto &[a, d] : definitions()) {
        if (a == action)
            return d.sequence;
    }
    return QString();
}

int KeyManager::sortIndex(const QString &action) const
{
    for (int i = 0; i < m_actions.size(); ++i) {
        if (m_actions.at(i) == action)
            return i;
    }
    return INT_MAX;
}

QString KeyManager::labelFor(const QString &action) const
{
    for (const auto &[a, d] : definitions()) {
        if (a == action)
            return d.label;
    }
    return action;
}

QString KeyManager::groupFor(const QString &action) const
{
    for (const auto &[a, d] : definitions()) {
        if (a == action)
            return d.group;
    }
    return QStringLiteral("other");
}

QString KeyManager::binding(const QString &action, int) const
{
    if (action.isEmpty())
        return QString();
    QSettings s;
    s.beginGroup(kGroup);
    const QString stored = s.value(settingKey(action)).toString();
    return stored.isEmpty() ? defaultFor(action) : stored;
}

void KeyManager::setBinding(const QString &action, const QString &sequence)
{
    const QString trimmed = sequence.trimmed();
    if (trimmed.isEmpty() || action.isEmpty())
        return;
    QSettings s;
    s.beginGroup(kGroup);
    s.setValue(settingKey(action), trimmed);
    ++m_revision;
    Q_EMIT bindingsChanged();
}

void KeyManager::resetBinding(const QString &action)
{
    QSettings s;
    s.beginGroup(kGroup);
    if (s.contains(settingKey(action))) {
        s.remove(settingKey(action));
        ++m_revision;
        Q_EMIT bindingsChanged();
    }
}

void KeyManager::resetAll()
{
    QSettings s;
    s.beginGroup(kGroup);
    s.remove(QString());
    ++m_revision;
    Q_EMIT bindingsChanged();
}

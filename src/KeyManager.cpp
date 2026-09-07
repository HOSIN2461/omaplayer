#include "KeyManager.h"

#include <QSettings>

using namespace Qt::Literals::StringLiterals;

const QList<QPair<QString, KeyManager::Def>> &KeyManager::definitions()
{
    // (action, { default sequence, Hungarian label }). Order = menu order.
    static const QList<QPair<QString, KeyManager::Def>> table = {
        { "playPause",     { "Space",    "Lejátszás / szünet" } },
        { "seekBackward",  { "Left",     "Visszalépés 5 mp" } },
        { "seekForward",   { "Right",    "Előrelépés 5 mp" } },
        { "volumeUp",      { "Up",       "Hangerő növelés" } },
        { "volumeDown",    { "Down",     "Hangerő csökkentés" } },
        { "mute",          { "M",        "Némítás" } },
        { "fullscreen",    { "F",        "Teljes képernyő" } },
        { "minimize",      { "I",        "Kis méret" } },
        { "settings",      { "G",        "Beállítások" } },
        { "playlist",      { "L",        "Lejátszási lista" } },
        { "jellyfin",      { "J",        "Jellyfin" } },
        { "speedHalve",    { "[",        "Sebesség felezés" } },
        { "speedDouble",   { "]",        "Sebesség duplázás" } },
        { "nextItem",      { "N",        "Következő média" } },
        { "prevItem",      { "P",        "Előző média" } },
        { "removeSelected",{ "Delete",   "Kiválasztott törlése" } },
        { "toggleSearch",  { "Ctrl+F",   "Keresés" } },
        { "openFile",      { "Ctrl+O",   "Média megnyitása" } },
        { "screenshot",    { "Ctrl+S",   "Képernyőkép" } },
        { "stats",         { "Ctrl+I",   "Lejátszási statisztika" } },
        { "volume100",     { "Ctrl+0",   "Hangerő 100%" } },
        { "escape",        { "Esc",      "Bezár / kivonás" } },
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

bool KeyManager::hasOverride(const QString &action) const
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

QString KeyManager::binding(const QString &action) const
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
    Q_EMIT bindingsChanged();
}

void KeyManager::resetBinding(const QString &action)
{
    QSettings s;
    s.beginGroup(kGroup);
    if (s.contains(settingKey(action))) {
        s.remove(settingKey(action));
        Q_EMIT bindingsChanged();
    }
}

void KeyManager::resetAll()
{
    QSettings s;
    s.beginGroup(kGroup);
    s.remove(QString());
    Q_EMIT bindingsChanged();
}
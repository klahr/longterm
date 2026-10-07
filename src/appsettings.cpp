#include "appsettings.h"

#include <QCryptographicHash>
#include <QStandardPaths>
#include <QUuid>

static const char TerminalFontSizeKey[] = "terminal/fontSize";
static const char TerminalColorSchemeKey[] = "terminal/colorScheme";
static const char ToolbarKeysKey[] = "terminal/toolbarKeys";
static const char BellVibrateKey[] = "terminal/bellVibrate";
static const char BellNotifyKey[] = "terminal/bellNotify";
static const char RemoteClipboardKey[] = "terminal/remoteClipboard";
static const char AutoReconnectKey[] = "connection/autoReconnect";
static const char TerminalFontFamilyKey[] = "terminal/fontFamily";
static const char SnippetsKey[] = "terminal/snippets";
static const char ToolbarAtTopKey[] = "terminal/toolbarAtTop";
static const char NameBarInLandscapeKey[] = "terminal/nameBarInLandscape";
static const char MoshPredictionKey[] = "connection/moshPrediction";
static const char LockHashKey[] = "lock/hash";
static const char LockSaltKey[] = "lock/salt";
static const char LockDelayKey[] = "lock/delay";
// A code of a few digits is quick to try anyway, the rounds only slow down
// guessing from a copy of the settings file
static const int LockHashRounds = 20000;

static QByteArray hashCode(const QString &code, const QByteArray &salt)
{
    QByteArray hash = salt + code.toUtf8();
    for (int i = 0; i < LockHashRounds; ++i)
        hash = QCryptographicHash::hash(hash + salt, QCryptographicHash::Sha256);
    return hash.toBase64();
}

AppSettings::AppSettings(QObject *parent)
    : QObject(parent)
    // The sandbox only allows writing inside the app's own config directory
    , m_settings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                 + QStringLiteral("/settings.conf"), QSettings::IniFormat)
    , m_locked(false)
{
}

int AppSettings::terminalFontSize() const
{
    return m_settings.value(QLatin1String(TerminalFontSizeKey), 0).toInt();
}

void AppSettings::setTerminalFontSize(int size)
{
    if (size == terminalFontSize())
        return;
    m_settings.setValue(QLatin1String(TerminalFontSizeKey), size);
    emit terminalFontSizeChanged();
}

QString AppSettings::terminalColorScheme() const
{
    return m_settings.value(QLatin1String(TerminalColorSchemeKey), QStringLiteral("default")).toString();
}

void AppSettings::setTerminalColorScheme(const QString &colorScheme)
{
    if (colorScheme == terminalColorScheme())
        return;
    m_settings.setValue(QLatin1String(TerminalColorSchemeKey), colorScheme);
    emit terminalColorSchemeChanged();
}

QStringList AppSettings::toolbarKeys() const
{
    static const QStringList defaultKeys = QStringList()
            << QStringLiteral("esc") << QStringLiteral("tab") << QStringLiteral("ctrl") << QStringLiteral("alt")
            << QStringLiteral("left") << QStringLiteral("up") << QStringLiteral("down") << QStringLiteral("right")
            << QStringLiteral("pipe") << QStringLiteral("slash") << QStringLiteral("dash");
    return m_settings.value(QLatin1String(ToolbarKeysKey), defaultKeys).toStringList();
}

void AppSettings::setToolbarKeys(const QStringList &keys)
{
    if (keys == toolbarKeys())
        return;
    m_settings.setValue(QLatin1String(ToolbarKeysKey), keys);
    emit toolbarKeysChanged();
}

bool AppSettings::bellVibrate() const
{
    return flag(BellVibrateKey, true);
}

void AppSettings::setBellVibrate(bool vibrate)
{
    if (setFlag(BellVibrateKey, true, vibrate))
        emit bellVibrateChanged();
}

bool AppSettings::bellNotify() const
{
    return flag(BellNotifyKey, true);
}

void AppSettings::setBellNotify(bool notify)
{
    if (setFlag(BellNotifyKey, true, notify))
        emit bellNotifyChanged();
}

bool AppSettings::remoteClipboard() const
{
    return flag(RemoteClipboardKey, true);
}

void AppSettings::setRemoteClipboard(bool allow)
{
    if (setFlag(RemoteClipboardKey, true, allow))
        emit remoteClipboardChanged();
}

bool AppSettings::autoReconnect() const
{
    return flag(AutoReconnectKey, true);
}

void AppSettings::setAutoReconnect(bool reconnect)
{
    if (setFlag(AutoReconnectKey, true, reconnect))
        emit autoReconnectChanged();
}

QString AppSettings::terminalFontFamily() const
{
    return m_settings.value(QLatin1String(TerminalFontFamilyKey)).toString();
}

void AppSettings::setTerminalFontFamily(const QString &family)
{
    if (family == terminalFontFamily())
        return;
    m_settings.setValue(QLatin1String(TerminalFontFamilyKey), family);
    emit terminalFontFamilyChanged();
}

QVariantList AppSettings::snippets() const
{
    return m_settings.value(QLatin1String(SnippetsKey)).toList();
}

void AppSettings::setSnippets(const QVariantList &snippets)
{
    m_settings.setValue(QLatin1String(SnippetsKey), snippets);
    emit snippetsChanged();
}

bool AppSettings::toolbarAtTop() const
{
    return flag(ToolbarAtTopKey, false);
}

void AppSettings::setToolbarAtTop(bool top)
{
    if (setFlag(ToolbarAtTopKey, false, top))
        emit toolbarAtTopChanged();
}

bool AppSettings::nameBarInLandscape() const
{
    return flag(NameBarInLandscapeKey, true);
}

void AppSettings::setNameBarInLandscape(bool show)
{
    if (setFlag(NameBarInLandscapeKey, true, show))
        emit nameBarInLandscapeChanged();
}

bool AppSettings::lockEnabled() const
{
    return !m_settings.value(QLatin1String(LockHashKey)).toString().isEmpty();
}

bool AppSettings::moshPrediction() const
{
    return flag(MoshPredictionKey, true);
}

void AppSettings::setMoshPrediction(bool predict)
{
    if (setFlag(MoshPredictionKey, true, predict))
        emit moshPredictionChanged();
}

void AppSettings::setLocked(bool locked)
{
    if (m_locked == locked)
        return;
    m_locked = locked;
    emit lockedChanged();
}

int AppSettings::lockDelay() const
{
    return m_settings.value(QLatin1String(LockDelayKey), 0).toInt();
}

void AppSettings::setLockDelay(int minutes)
{
    if (minutes == lockDelay())
        return;
    m_settings.setValue(QLatin1String(LockDelayKey), minutes);
    emit lockChanged();
}

void AppSettings::setLockCode(const QString &code)
{
    if (code.isEmpty()) {
        m_settings.remove(QLatin1String(LockHashKey));
        m_settings.remove(QLatin1String(LockSaltKey));
    } else {
        const QByteArray salt = QUuid::createUuid().toRfc4122();
        m_settings.setValue(QLatin1String(LockSaltKey), QString::fromLatin1(salt.toBase64()));
        m_settings.setValue(QLatin1String(LockHashKey), QString::fromLatin1(hashCode(code, salt)));
    }
    m_settings.sync();
    emit lockChanged();
}

bool AppSettings::checkLockCode(const QString &code) const
{
    const QByteArray salt = QByteArray::fromBase64(m_settings.value(QLatin1String(LockSaltKey)).toString().toLatin1());
    const QByteArray expected = m_settings.value(QLatin1String(LockHashKey)).toString().toLatin1();
    if (expected.isEmpty())
        return true;
    // Compared without stopping at the first difference
    const QByteArray actual = hashCode(code, salt);
    int difference = actual.size() ^ expected.size();
    for (int i = 0; i < qMin(actual.size(), expected.size()); ++i)
        difference |= actual.at(i) ^ expected.at(i);
    return difference == 0;
}

QVariantMap AppSettings::exportSettings() const
{
    QVariantMap map;
    for (const QString &key : m_settings.allKeys()) {
        if (!key.startsWith(QLatin1String("lock/")))
            map.insert(key, m_settings.value(key));
    }
    return map;
}

void AppSettings::restoreSettings(const QVariantMap &settings)
{
    for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
        if (it.key().startsWith(QLatin1String("terminal/")) || it.key().startsWith(QLatin1String("connection/")))
            m_settings.setValue(it.key(), it.value());
    }
    m_settings.sync();
    emit terminalFontSizeChanged();
    emit terminalColorSchemeChanged();
    emit toolbarKeysChanged();
    emit bellVibrateChanged();
    emit bellNotifyChanged();
    emit remoteClipboardChanged();
    emit autoReconnectChanged();
    emit terminalFontFamilyChanged();
    emit snippetsChanged();
    emit toolbarAtTopChanged();
    emit nameBarInLandscapeChanged();
    emit moshPredictionChanged();
}

bool AppSettings::flag(const char *key, bool defaultValue) const
{
    return m_settings.value(QLatin1String(key), defaultValue).toBool();
}

bool AppSettings::setFlag(const char *key, bool defaultValue, bool value)
{
    if (value == flag(key, defaultValue))
        return false;
    m_settings.setValue(QLatin1String(key), value);
    return true;
}

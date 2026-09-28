#include "appsettings.h"

#include <QStandardPaths>

static const char TerminalFontSizeKey[] = "terminal/fontSize";
static const char TerminalColorSchemeKey[] = "terminal/colorScheme";
static const char ToolbarKeysKey[] = "terminal/toolbarKeys";
static const char BellVibrateKey[] = "terminal/bellVibrate";
static const char BellNotifyKey[] = "terminal/bellNotify";
static const char RemoteClipboardKey[] = "terminal/remoteClipboard";
static const char AutoReconnectKey[] = "connection/autoReconnect";

AppSettings::AppSettings(QObject *parent)
    : QObject(parent)
    // The sandbox only allows writing inside the app's own config directory
    , m_settings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                 + QStringLiteral("/settings.conf"), QSettings::IniFormat)
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

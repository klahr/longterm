#include "appsettings.h"

#include <QStandardPaths>

static const char TerminalFontSizeKey[] = "terminal/fontSize";
static const char TerminalColorSchemeKey[] = "terminal/colorScheme";

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

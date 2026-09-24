#ifndef APPSETTINGS_H
#define APPSETTINGS_H

#include <QObject>
#include <QSettings>

class AppSettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString terminalColorScheme READ terminalColorScheme WRITE setTerminalColorScheme NOTIFY terminalColorSchemeChanged)
    // Zero means the theme default
    Q_PROPERTY(int terminalFontSize READ terminalFontSize WRITE setTerminalFontSize NOTIFY terminalFontSizeChanged)

public:
    explicit AppSettings(QObject *parent = nullptr);

    int terminalFontSize() const;
    void setTerminalFontSize(int size);
    QString terminalColorScheme() const;
    void setTerminalColorScheme(const QString &colorScheme);

signals:
    void terminalFontSizeChanged();
    void terminalColorSchemeChanged();

private:
    QSettings m_settings;
};

#endif // APPSETTINGS_H

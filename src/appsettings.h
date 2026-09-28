#ifndef APPSETTINGS_H
#define APPSETTINGS_H

#include <QObject>
#include <QSettings>
#include <QStringList>

class AppSettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString terminalColorScheme READ terminalColorScheme WRITE setTerminalColorScheme NOTIFY terminalColorSchemeChanged)
    // Zero means the theme default
    Q_PROPERTY(int terminalFontSize READ terminalFontSize WRITE setTerminalFontSize NOTIFY terminalFontSizeChanged)
    // Ids of the keys shown above the keyboard, see qml/components/Keys.js
    Q_PROPERTY(QStringList toolbarKeys READ toolbarKeys WRITE setToolbarKeys NOTIFY toolbarKeysChanged)
    Q_PROPERTY(bool bellVibrate READ bellVibrate WRITE setBellVibrate NOTIFY bellVibrateChanged)
    Q_PROPERTY(bool bellNotify READ bellNotify WRITE setBellNotify NOTIFY bellNotifyChanged)
    // Lets programs on the server set the clipboard with OSC 52
    Q_PROPERTY(bool remoteClipboard READ remoteClipboard WRITE setRemoteClipboard NOTIFY remoteClipboardChanged)
    Q_PROPERTY(bool autoReconnect READ autoReconnect WRITE setAutoReconnect NOTIFY autoReconnectChanged)

public:
    explicit AppSettings(QObject *parent = nullptr);

    int terminalFontSize() const;
    void setTerminalFontSize(int size);
    QString terminalColorScheme() const;
    void setTerminalColorScheme(const QString &colorScheme);
    QStringList toolbarKeys() const;
    void setToolbarKeys(const QStringList &keys);
    bool bellVibrate() const;
    void setBellVibrate(bool vibrate);
    bool bellNotify() const;
    void setBellNotify(bool notify);
    bool remoteClipboard() const;
    void setRemoteClipboard(bool allow);
    bool autoReconnect() const;
    void setAutoReconnect(bool reconnect);

signals:
    void terminalFontSizeChanged();
    void terminalColorSchemeChanged();
    void toolbarKeysChanged();
    void bellVibrateChanged();
    void bellNotifyChanged();
    void remoteClipboardChanged();
    void autoReconnectChanged();

private:
    bool flag(const char *key, bool defaultValue) const;
    // Returns false when the value did not change
    bool setFlag(const char *key, bool defaultValue, bool value);

    QSettings m_settings;
};

#endif // APPSETTINGS_H

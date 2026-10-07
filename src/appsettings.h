#ifndef APPSETTINGS_H
#define APPSETTINGS_H

#include <QObject>
#include <QSettings>
#include <QStringList>
#include <QVariantList>

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
    // Empty for the bundled Source Code Pro
    Q_PROPERTY(QString terminalFontFamily READ terminalFontFamily WRITE setTerminalFontFamily NOTIFY terminalFontFamilyChanged)
    // Maps with name, text, hostId (empty for every host) and run (sends Enter after the text)
    Q_PROPERTY(QVariantList snippets READ snippets WRITE setSnippets NOTIFY snippetsChanged)
    Q_PROPERTY(bool toolbarAtTop READ toolbarAtTop WRITE setToolbarAtTop NOTIFY toolbarAtTopChanged)
    // The bar with the connection's name, which landscape is short of room for
    Q_PROPERTY(bool nameBarInLandscape READ nameBarInLandscape WRITE setNameBarInLandscape NOTIFY nameBarInLandscapeChanged)
    // A salted hash of the code that unlocks the app, empty when it does not lock
    Q_PROPERTY(bool lockEnabled READ lockEnabled NOTIFY lockChanged)
    // Minutes in the background before the app locks again, 0 for right away
    Q_PROPERTY(int lockDelay READ lockDelay WRITE setLockDelay NOTIFY lockChanged)
    // Typing over mosh shows before the server echoes it
    Q_PROPERTY(bool moshPrediction READ moshPrediction WRITE setMoshPrediction NOTIFY moshPredictionChanged)
    // The app is locked right now, not saved
    Q_PROPERTY(bool locked READ locked WRITE setLocked NOTIFY lockedChanged)

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
    QString terminalFontFamily() const;
    void setTerminalFontFamily(const QString &family);
    QVariantList snippets() const;
    void setSnippets(const QVariantList &snippets);
    bool toolbarAtTop() const;
    void setToolbarAtTop(bool top);
    bool nameBarInLandscape() const;
    void setNameBarInLandscape(bool show);
    bool lockEnabled() const;
    int lockDelay() const;
    bool locked() const { return m_locked; }
    bool moshPrediction() const;
    void setMoshPrediction(bool predict);
    void setLocked(bool locked);
    void setLockDelay(int minutes);
    // An empty code turns the lock off
    Q_INVOKABLE void setLockCode(const QString &code);
    Q_INVOKABLE bool checkLockCode(const QString &code) const;
    // The terminal and connection settings, without the lock, for backups
    QVariantMap exportSettings() const;
    void restoreSettings(const QVariantMap &settings);

signals:
    void terminalFontSizeChanged();
    void terminalColorSchemeChanged();
    void toolbarKeysChanged();
    void bellVibrateChanged();
    void bellNotifyChanged();
    void remoteClipboardChanged();
    void autoReconnectChanged();
    void terminalFontFamilyChanged();
    void snippetsChanged();
    void toolbarAtTopChanged();
    void nameBarInLandscapeChanged();
    void lockChanged();
    void lockedChanged();
    void moshPredictionChanged();

private:
    bool flag(const char *key, bool defaultValue) const;
    // Returns false when the value did not change
    bool setFlag(const char *key, bool defaultValue, bool value);

    QSettings m_settings;
    bool m_locked;
};

#endif // APPSETTINGS_H

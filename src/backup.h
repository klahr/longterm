#ifndef BACKUP_H
#define BACKUP_H

#include <QObject>
#include <QVariantMap>

#include <functional>

class AppSettings;
class ColorSchemes;
class HostStore;
class KeyStore;
class SecretVault;

// Hosts, keys, saved passwords, snippets, settings and imported color schemes
// in one file encrypted with a passphrase, to move to another device or keep
class Backup : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    Backup(SecretVault *vault, HostStore *hosts, KeyStore *keys, AppSettings *settings, ColorSchemes *schemes,
           QObject *parent = nullptr);

    bool busy() const { return m_busy; }

    // Into Documents/Longterm, emits exported when done
    Q_INVOKABLE void exportBackup(const QString &passphrase);
    // Adds what the file has, keeping hosts and keys that are already here. Emits imported.
    Q_INVOKABLE void importBackup(const QString &path, const QString &passphrase);

signals:
    void busyChanged();
    // path is empty with error set when it failed
    void exported(const QString &path, const QString &error);
    // summary says what came in, empty with error set when it failed
    void imported(const QString &summary, const QString &error);

private:
    void setBusy(bool busy);
    // Fetches the secrets one after another, then calls done with them by id
    void fetchSecrets(QStringList ids, QVariantMap fetched, const std::function<void(const QVariantMap &)> &done);
    void storeSecrets(QVariantMap secrets, const std::function<void()> &done);

    SecretVault *m_vault;
    HostStore *m_hosts;
    KeyStore *m_keys;
    AppSettings *m_settings;
    ColorSchemes *m_schemes;
    bool m_busy;
};

#endif // BACKUP_H

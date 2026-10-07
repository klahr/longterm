#ifndef HOSTSTORE_H
#define HOSTSTORE_H

#include <QAbstractListModel>
#include <QList>
#include <QSettings>
#include <QStringList>
#include <QVariantMap>

class SecretVault;

class HostStore : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)

public:
    enum Roles {
        HostIdRole = Qt::UserRole + 1,
        NameRole,
        AddressRole,
        PortRole,
        UserRole,
        KeyIdRole,
        HasPasswordRole,
        SystemIdRole,
        SystemNameRole
    };

    struct Host {
        QString id;
        QString name;
        QString address;
        int port;
        QString user;
        QString keyId;
        bool hasPassword;
        // Another saved host to connect through, or empty
        QString jumpHostId;
        bool forwardAgent;
        // "localPort:host:remotePort" entries
        QStringList localForwards;
        // "remotePort:host:localPort" entries, the server listens and the phone connects onwards
        QStringList remoteForwards;
        // Local ports with a SOCKS proxy that connects out from the server
        QStringList dynamicForwards;
        // "NAME=value" entries sent before the shell starts
        QStringList environment;
        // Attaches to or creates this tmux session instead of a plain shell, empty for none
        QString tmuxSession;
        // Seconds, 0 for the defaults
        int keepAliveInterval = 0;
        int connectTimeout = 0;
        // The terminal goes over mosh, the SSH connection only starts the server
        bool mosh = false;
        QString systemId;
        QString systemName;
    };

    explicit HostStore(SecretVault *vault, QObject *parent = nullptr);

    int count() const { return m_hosts.size(); }
    QString errorString() const { return m_errorString; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Creates a host when hostId is empty. An empty password keeps
    // any remembered one, rememberPassword false forgets it. options holds
    // the advanced settings by their names in Host, missing ones are reset.
    // Returns the id.
    Q_INVOKABLE QString saveHost(const QString &hostId, const QString &name,
                                 const QString &address, int port, const QString &user,
                                 const QString &keyId, const QString &password,
                                 bool rememberPassword, const QVariantMap &options);
    Q_INVOKABLE void removeHost(const QString &hostId);
    // Keeps a password typed while connecting, the host logs in with it from now on
    void rememberPassword(const QString &hostId, const QString &password);
    // Hosts that used the deleted key ask for a password instead
    void forgetKey(const QString &keyId);
    void setSystem(const QString &hostId, const QString &id, const QString &name);
    Q_INVOKABLE QVariantMap host(const QString &hostId) const;
    Q_INVOKABLE QString hostIdAt(int row) const;
    Q_INVOKABLE int indexOf(const QString &hostId) const;
    // All hosts as an OpenSSH client configuration
    Q_INVOKABLE QString exportConfig() const;
    // Adds the hosts of an OpenSSH client configuration, updating those with
    // the same name. Returns how many were read.
    Q_INVOKABLE int importConfig(const QString &config);

    bool find(const QString &hostId, Host *host) const;
    static QString passwordSecretId(const QString &hostId);

signals:
    void countChanged();
    void errorStringChanged();

private:
    int indexOfName(const QString &name) const;
    void setHasPassword(const QString &hostId, bool hasPassword);
    void load();
    void save();
    void setError(const QString &message);

    SecretVault *m_vault;
    bool m_batch;
    QSettings m_settings;
    QList<Host> m_hosts;
    QString m_errorString;
};

#endif // HOSTSTORE_H

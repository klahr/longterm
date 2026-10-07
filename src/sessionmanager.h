#ifndef SESSIONMANAGER_H
#define SESSIONMANAGER_H

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QNetworkConfigurationManager>
#include <QSettings>

#include "hoststore.h"

class AppSettings;
class KeyStore;
class SecretVault;
class SshSession;

class SessionManager : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    // Connected sessions whose coding agent reports this, see Terminal::activity
    Q_PROPERTY(int workingCount READ workingCount NOTIFY activityCountsChanged)
    Q_PROPERTY(int waitingCount READ waitingCount NOTIFY activityCountsChanged)
    Q_PROPERTY(int doneCount READ doneCount NOTIFY activityCountsChanged)

public:
    enum Roles {
        SessionRole = Qt::UserRole + 1
    };

    SessionManager(SecretVault *vault, HostStore *hosts, KeyStore *keys, AppSettings *appSettings,
                   QObject *parent = nullptr);

    int count() const { return m_sessions.size(); }
    int workingCount() const { return m_working; }
    int waitingCount() const { return m_waiting; }
    int doneCount() const { return m_done; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // An empty keyId means password authentication. options are as for HostStore::saveHost.
    Q_INVOKABLE SshSession *openSession(const QString &name, const QString &host, int port,
                                        const QString &user, const QString &password,
                                        const QString &keyId, const QVariantMap &options = QVariantMap());
    // Uses the host's key or remembered password, or else the given one, which
    // may be empty when the server asks for everything itself. Returns null for
    // an unknown host.
    Q_INVOKABLE SshSession *openHost(const QString &hostId, const QString &password = QString());
    Q_INVOKABLE void closeSession(SshSession *session);
    // Sends a Wake-on-LAN packet to the host's MAC address, returns why it could not
    Q_INVOKABLE QString wakeHost(const QString &hostId);
    // Logs in to the host the way it is set up and adds the key to its
    // authorized_keys, like ssh-copy-id. With useKey the host logs in with the
    // key from then on, once it is in place.
    Q_INVOKABLE SshSession *installKey(const QString &hostId, const QString &keyId, bool useKey);
    // See SftpBrowser::writeSharedText()
    Q_INVOKABLE QString writeSharedText(const QString &name, const QString &text) const;
    Q_INVOKABLE int indexOf(SshSession *session) const { return m_sessions.indexOf(session); }
    Q_INVOKABLE SshSession *sessionAt(int index) const { return m_sessions.value(index); }

signals:
    void countChanged();
    void activityCountsChanged();

private:
    // What a session needs to be restored after a restart, a saved host or a one-off key
    struct Origin {
        QString hostId;
        QString keyId;
        // The settings of a one-off connection
        QVariantMap options;
    };

    SshSession *addSession(const QString &name, const QString &host, int port, const QString &user);
    // Applies the saved host's settings, again whenever they change, so the
    // next connect uses them
    void configure(SshSession *session, const HostStore::Host &host);
    void configureJump(SshSession *session, const QString &jumpHostId);
    void onHostsChanged();
    void rememberPassword(SshSession *session, const QString &password);
    void onNetworkChanged();
    void countActivities();
    void load();
    void save();

    SecretVault *m_vault;
    HostStore *m_hosts;
    KeyStore *m_keys;
    QList<SshSession *> m_sessions;
    QHash<const SshSession *, Origin> m_origins;
    QSettings m_settings;
    AppSettings *m_appSettings;
    bool m_loading;
    int m_working;
    int m_waiting;
    int m_done;
    QNetworkConfigurationManager m_network;
    // The active network configurations last seen, to tell real changes from noise
    QStringList m_activeNetworks;
};

#endif // SESSIONMANAGER_H

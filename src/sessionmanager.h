#ifndef SESSIONMANAGER_H
#define SESSIONMANAGER_H

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QSettings>

class HostStore;
class SecretVault;
class SshSession;

class SessionManager : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        SessionRole = Qt::UserRole + 1
    };

    SessionManager(SecretVault *vault, HostStore *hosts, QObject *parent = nullptr);

    int count() const { return m_sessions.size(); }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // An empty keyId means password authentication
    Q_INVOKABLE SshSession *openSession(const QString &name, const QString &host, int port,
                                        const QString &user, const QString &password,
                                        const QString &keyId);
    // Uses the host's key or remembered password, returns null if it has neither
    Q_INVOKABLE SshSession *openHost(const QString &hostId);
    Q_INVOKABLE void closeSession(SshSession *session);
    Q_INVOKABLE int indexOf(SshSession *session) const { return m_sessions.indexOf(session); }

signals:
    void countChanged();

private:
    // What a session needs to be restored after a restart, a saved host or a one-off key
    struct Origin {
        QString hostId;
        QString keyId;
    };

    SshSession *addSession(const QString &name, const QString &host, int port, const QString &user);
    void load();
    void save();

    SecretVault *m_vault;
    HostStore *m_hosts;
    QList<SshSession *> m_sessions;
    QHash<const SshSession *, Origin> m_origins;
    QSettings m_settings;
};

#endif // SESSIONMANAGER_H

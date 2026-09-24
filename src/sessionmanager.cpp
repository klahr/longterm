#include "sessionmanager.h"

#include <QQmlEngine>

#include "hoststore.h"
#include "sshsession.h"

SessionManager::SessionManager(SecretVault *vault, HostStore *hosts, QObject *parent)
    : QAbstractListModel(parent)
    , m_vault(vault)
    , m_hosts(hosts)
{
}

int SessionManager::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_sessions.size();
}

QVariant SessionManager::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_sessions.size() || role != SessionRole)
        return QVariant();
    return QVariant::fromValue(static_cast<QObject *>(m_sessions.at(index.row())));
}

QHash<int, QByteArray> SessionManager::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[SessionRole] = "session";
    return roles;
}

SshSession *SessionManager::openSession(const QString &name, const QString &host, int port,
                                        const QString &user, const QString &password,
                                        const QString &keyId)
{
    SshSession *session = addSession(name, host, port, user);
    if (keyId.isEmpty())
        session->connectToHost(password);
    else
        session->connectWithSecret(m_vault, keyId, SshSession::PrivateKey);
    return session;
}

SshSession *SessionManager::openHost(const QString &hostId)
{
    HostStore::Host host;
    if (!m_hosts->find(hostId, &host) || (host.keyId.isEmpty() && !host.hasPassword))
        return nullptr;

    SshSession *session = addSession(host.name, host.address, host.port, host.user);
    if (host.keyId.isEmpty())
        session->connectWithSecret(m_vault, HostStore::passwordSecretId(host.id), SshSession::Password);
    else
        session->connectWithSecret(m_vault, host.keyId, SshSession::PrivateKey);
    return session;
}

SshSession *SessionManager::addSession(const QString &name, const QString &host, int port,
                                       const QString &user)
{
    // Several connections to the same host need telling apart
    QString uniqueName = name;
    if (!name.isEmpty()) {
        int instance = 1;
        for (const SshSession *existing : m_sessions) {
            if (existing->name() == uniqueName)
                uniqueName = QStringLiteral("%1 #%2").arg(name).arg(++instance);
        }
    }

    SshSession *session = new SshSession(uniqueName, host, port, user, this);
    // Returned to QML from an invokable, which would otherwise hand ownership to JS
    QQmlEngine::setObjectOwnership(session, QQmlEngine::CppOwnership);
    connect(session, &SshSession::shellExited, this, [this, session]() { closeSession(session); });

    beginInsertRows(QModelIndex(), m_sessions.size(), m_sessions.size());
    m_sessions.append(session);
    endInsertRows();
    emit countChanged();
    return session;
}

void SessionManager::closeSession(SshSession *session)
{
    const int row = m_sessions.indexOf(session);
    if (row < 0)
        return;

    beginRemoveRows(QModelIndex(), row, row);
    m_sessions.removeAt(row);
    endRemoveRows();
    emit countChanged();

    // Deleting a live session blocks on its worker thread, so let it wind down first
    if (session->state() == SshSession::Disconnected) {
        session->deleteLater();
        return;
    }
    connect(session, &SshSession::stateChanged, session, [session]() {
        if (session->state() == SshSession::Disconnected)
            session->deleteLater();
    });
    session->disconnectFromHost();
}

#include "sessionmanager.h"

#include <QQmlEngine>
#include <QStandardPaths>

#include "hoststore.h"
#include "sshsession.h"

SessionManager::SessionManager(SecretVault *vault, HostStore *hosts, QObject *parent)
    : QAbstractListModel(parent)
    , m_vault(vault)
    , m_hosts(hosts)
    , m_settings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                 + QStringLiteral("/sessions.conf"), QSettings::IniFormat)
{
    load();
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
    if (keyId.isEmpty()) {
        // A typed password is not kept, so the session cannot come back after a restart
        session->connectToHost(password);
        return session;
    }
    m_origins.insert(session, Origin { QString(), keyId });
    save();
    session->connectWithSecret(m_vault, keyId, SshSession::PrivateKey);
    return session;
}

SshSession *SessionManager::openHost(const QString &hostId)
{
    HostStore::Host host;
    if (!m_hosts->find(hostId, &host) || (host.keyId.isEmpty() && !host.hasPassword))
        return nullptr;

    SshSession *session = addSession(host.name, host.address, host.port, host.user);
    m_origins.insert(session, Origin { host.id, QString() });
    save();
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
    connect(session, &SshSession::nameChanged, this, &SessionManager::save);

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
    m_origins.remove(session);
    save();

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

// Sessions come back disconnected, connecting only when the user asks
void SessionManager::load()
{
    const int size = m_settings.beginReadArray(QStringLiteral("sessions"));
    for (int i = 0; i < size; ++i) {
        m_settings.setArrayIndex(i);
        const QString name = m_settings.value(QStringLiteral("name")).toString();
        const QString hostId = m_settings.value(QStringLiteral("hostId")).toString();
        const QString keyId = m_settings.value(QStringLiteral("keyId")).toString();

        if (!hostId.isEmpty()) {
            HostStore::Host host;
            if (!m_hosts->find(hostId, &host) || (host.keyId.isEmpty() && !host.hasPassword))
                continue;
            SshSession *session = addSession(name, host.address, host.port, host.user);
            if (host.keyId.isEmpty())
                session->setSecret(m_vault, HostStore::passwordSecretId(host.id), SshSession::Password);
            else
                session->setSecret(m_vault, host.keyId, SshSession::PrivateKey);
            m_origins.insert(session, Origin { hostId, QString() });
        } else if (!keyId.isEmpty()) {
            SshSession *session = addSession(name, m_settings.value(QStringLiteral("host")).toString(),
                                             m_settings.value(QStringLiteral("port"), 22).toInt(),
                                             m_settings.value(QStringLiteral("user")).toString());
            session->setSecret(m_vault, keyId, SshSession::PrivateKey);
            m_origins.insert(session, Origin { QString(), keyId });
        }
    }
    m_settings.endArray();
}

void SessionManager::save()
{
    m_settings.remove(QStringLiteral("sessions"));
    m_settings.beginWriteArray(QStringLiteral("sessions"));
    int index = 0;
    for (const SshSession *session : m_sessions) {
        if (!m_origins.contains(session))
            continue;
        const Origin origin = m_origins.value(session);
        m_settings.setArrayIndex(index++);
        m_settings.setValue(QStringLiteral("name"), session->name());
        m_settings.setValue(QStringLiteral("hostId"), origin.hostId);
        m_settings.setValue(QStringLiteral("keyId"), origin.keyId);
        m_settings.setValue(QStringLiteral("host"), session->host());
        m_settings.setValue(QStringLiteral("port"), session->port());
        m_settings.setValue(QStringLiteral("user"), session->user());
    }
    m_settings.endArray();
    m_settings.sync();
}

#include "sessionmanager.h"

#include <QQmlEngine>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

#include "appsettings.h"
#include "sshsession.h"

// Gives the network a moment to settle after a connection drops
static const int ReconnectDelayMs = 2000;

SessionManager::SessionManager(SecretVault *vault, HostStore *hosts, AppSettings *appSettings, QObject *parent)
    : QAbstractListModel(parent)
    , m_vault(vault)
    , m_hosts(hosts)
    , m_settings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                 + QStringLiteral("/sessions.conf"), QSettings::IniFormat)
    , m_appSettings(appSettings)
    , m_loading(false)
{
    load();
    for (const QNetworkConfiguration &configuration : m_network.allConfigurations(QNetworkConfiguration::Active))
        m_activeNetworks.append(configuration.identifier());
    m_activeNetworks.sort();
    connect(&m_network, &QNetworkConfigurationManager::onlineStateChanged, this, &SessionManager::onNetworkChanged);
    connect(&m_network, &QNetworkConfigurationManager::configurationChanged, this, &SessionManager::onNetworkChanged);
    // A removed host also clears it as the jump host of others, which reports those as changed
    connect(m_hosts, &QAbstractItemModel::dataChanged, this, &SessionManager::onHostsChanged);
    connect(m_hosts, &QAbstractItemModel::rowsRemoved, this, &SessionManager::onHostsChanged);
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

SshSession *SessionManager::openHost(const QString &hostId, const QString &password)
{
    HostStore::Host host;
    if (!m_hosts->find(hostId, &host))
        return nullptr;

    SshSession *session = addSession(host.name, host.address, host.port, host.user);
    configure(session, host);
    m_origins.insert(session, Origin { host.id, QString() });
    save();
    if (host.keyId.isEmpty() && (!password.isEmpty() || !host.hasPassword)) {
        // Without a password the session asks for one when the server wants it
        session->connectToHost(password);
        return session;
    }
    if (host.keyId.isEmpty())
        session->connectWithSecret(m_vault, HostStore::passwordSecretId(host.id), SshSession::Password);
    else
        session->connectWithSecret(m_vault, host.keyId, SshSession::PrivateKey);
    return session;
}

void SessionManager::configure(SshSession *session, const HostStore::Host &host)
{
    session->setEndpoint(host.address, host.port, host.user);
    if (!host.keyId.isEmpty())
        session->setSecret(m_vault, host.keyId, SshSession::PrivateKey);
    else if (host.hasPassword)
        session->setSecret(m_vault, HostStore::passwordSecretId(host.id), SshSession::Password);
    else
        // Asks for the password on connect
        session->clearStoredSecret();
    session->setCanRememberPassword(host.keyId.isEmpty());
    session->setForwardAgent(host.forwardAgent && !host.keyId.isEmpty());
    session->setLocalForwards(host.localForwards);
    if (!host.systemId.isEmpty())
        session->setSystem(host.systemId, host.systemName);
    HostStore::Host jump;
    if (host.jumpHostId.isEmpty() || !m_hosts->find(host.jumpHostId, &jump)) {
        session->clearJumpHost();
        return;
    }
    if (!jump.keyId.isEmpty())
        session->setJumpHost(jump.address, jump.port, jump.user, m_vault, jump.keyId, SshSession::PrivateKey);
    else if (jump.hasPassword)
        session->setJumpHost(jump.address, jump.port, jump.user, m_vault,
                             HostStore::passwordSecretId(jump.id), SshSession::Password);
    else
        session->setJumpHost(jump.address, jump.port, jump.user, nullptr, QString(), SshSession::Password);
}

void SessionManager::onHostsChanged()
{
    QList<SshSession *> deleted;
    for (SshSession *session : m_sessions) {
        HostStore::Host host;
        const QString hostId = m_origins.value(session).hostId;
        if (hostId.isEmpty())
            continue;
        if (m_hosts->find(hostId, &host))
            configure(session, host);
        else
            deleted.append(session);
    }
    for (SshSession *session : deleted)
        closeSession(session);
}

void SessionManager::rememberPassword(SshSession *session, const QString &password)
{
    const QString hostId = m_origins.value(session).hostId;
    if (hostId.isEmpty())
        return;
    // The session keeps using the typed password until the next start, which reads the stored one
    m_hosts->rememberPassword(hostId, password);
}

void SessionManager::onNetworkChanged()
{
    QStringList active;
    for (const QNetworkConfiguration &configuration : m_network.allConfigurations(QNetworkConfiguration::Active))
        active.append(configuration.identifier());
    active.sort();
    if (active == m_activeNetworks)
        return;
    m_activeNetworks = active;
    if (!m_appSettings->autoReconnect())
        return;

    const bool online = m_network.isOnline();
    for (SshSession *session : m_sessions) {
        // A connection from an address the device no longer has is dead,
        // however long TCP would take to notice
        if (session->state() == SshSession::Connected && !session->hasLocalAddress())
            session->dropConnection();
        else if (online && session->isLost() && session->state() == SshSession::Disconnected)
            session->reconnect();
    }
}

SshSession *SessionManager::addSession(const QString &name, const QString &host, int port,
                                       const QString &user)
{
    // Several connections to the same host need telling apart
    QString uniqueName = name;
    if (!name.isEmpty()) {
        int instance = 1;
        const auto taken = [this](const QString &candidate) {
            return std::any_of(m_sessions.cbegin(), m_sessions.cend(),
                               [&candidate](const SshSession *existing) { return existing->name() == candidate; });
        };
        while (taken(uniqueName))
            uniqueName = QStringLiteral("%1 #%2").arg(name).arg(++instance);
    }

    SshSession *session = new SshSession(uniqueName, host, port, user, this);
    // Returned to QML from an invokable, which would otherwise hand ownership to JS
    QQmlEngine::setObjectOwnership(session, QQmlEngine::CppOwnership);
    connect(session, &SshSession::shellExited, this, [this, session]() { closeSession(session); });
    connect(session, &SshSession::nameChanged, this, &SessionManager::save);
    connect(session, &SshSession::startupScriptChanged, this, &SessionManager::save);
    connect(session, &SshSession::passwordRemembered, this, [this, session](const QString &password) {
        rememberPassword(session, password);
    });
    connect(session, &SshSession::colorSchemeChanged, this, &SessionManager::save);
    connect(session, &SshSession::systemChanged, this, [this, session]() {
        const QString hostId = m_origins.value(session).hostId;
        if (hostId.isEmpty())
            save();
        else
            m_hosts->setSystem(hostId, session->systemId(), session->systemName());
    });
    connect(session, &SshSession::connectionLost, this, [this, session]() {
        // Tried even when the bearer says offline, a failure waits for the next network change
        if (!m_appSettings->autoReconnect())
            return;
        QTimer::singleShot(ReconnectDelayMs, session, [session]() {
            if (session->isLost())
                session->reconnect();
        });
    });

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

// A damaged file can claim any number of entries
static const int MaxSessions = 1000;

// Sessions come back disconnected, connecting only when the user asks
void SessionManager::load()
{
    m_loading = true;
    const int size = qMin(m_settings.beginReadArray(QStringLiteral("sessions")), MaxSessions);
    for (int i = 0; i < size; ++i) {
        m_settings.setArrayIndex(i);
        const QString name = m_settings.value(QStringLiteral("name")).toString();
        const QString hostId = m_settings.value(QStringLiteral("hostId")).toString();
        const QString keyId = m_settings.value(QStringLiteral("keyId")).toString();

        if (!hostId.isEmpty()) {
            HostStore::Host host;
            if (!m_hosts->find(hostId, &host))
                continue;
            SshSession *session = addSession(name, host.address, host.port, host.user);
            configure(session, host);
            m_origins.insert(session, Origin { hostId, QString() });
        } else if (!keyId.isEmpty()) {
            int port = m_settings.value(QStringLiteral("port"), 22).toInt();
            if (port < 1 || port > 65535)
                port = 22;
            SshSession *session = addSession(name, m_settings.value(QStringLiteral("host")).toString(), port,
                                             m_settings.value(QStringLiteral("user")).toString());
            session->setSecret(m_vault, keyId, SshSession::PrivateKey);
            session->setSystem(m_settings.value(QStringLiteral("systemId")).toString(),
                               m_settings.value(QStringLiteral("systemName")).toString());
            m_origins.insert(session, Origin { QString(), keyId });
        } else {
            continue;
        }
        m_sessions.last()->setStartupScript(m_settings.value(QStringLiteral("startupScript")).toString());
        m_sessions.last()->setColorScheme(m_settings.value(QStringLiteral("colorScheme")).toString());
    }
    m_settings.endArray();
    m_loading = false;
}

void SessionManager::save()
{
    if (m_loading)
        return;
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
        m_settings.setValue(QStringLiteral("startupScript"), session->startupScript());
        m_settings.setValue(QStringLiteral("colorScheme"), session->colorScheme());
        if (origin.hostId.isEmpty()) {
            m_settings.setValue(QStringLiteral("systemId"), session->systemId());
            m_settings.setValue(QStringLiteral("systemName"), session->systemName());
        }
    }
    m_settings.endArray();
    m_settings.sync();
}

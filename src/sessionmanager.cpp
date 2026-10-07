#include "sessionmanager.h"

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QQmlEngine>
#include <QRegExp>
#include <QStandardPaths>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>

#include <algorithm>

#include "appsettings.h"
#include "keystore.h"
#include "sftpbrowser.h"
#include "sshsession.h"

// Gives the network a moment to settle after a connection drops
static const int ReconnectDelayMs = 2000;

// The names are those of HostStore::host() and HostStore::saveHost()
static SshOptions optionsFromMap(const QVariantMap &map)
{
    SshOptions options;
    options.forwardAgent = map.value(QStringLiteral("forwardAgent")).toBool();
    options.localForwards = map.value(QStringLiteral("localForwards")).toStringList();
    options.remoteForwards = map.value(QStringLiteral("remoteForwards")).toStringList();
    options.dynamicForwards = map.value(QStringLiteral("dynamicForwards")).toStringList();
    options.environment = map.value(QStringLiteral("environment")).toStringList();
    options.tmuxSession = map.value(QStringLiteral("tmuxSession")).toString();
    options.keepAliveInterval = map.value(QStringLiteral("keepAliveInterval")).toInt();
    options.connectTimeout = map.value(QStringLiteral("connectTimeout")).toInt();
    options.mosh = map.value(QStringLiteral("mosh")).toBool();
    options.moshServer = map.value(QStringLiteral("moshServer")).toString();
    options.logging = map.value(QStringLiteral("logging")).toBool();
    return options;
}

// A magic packet is six 0xff bytes and the MAC address sixteen times
static QString sendWakeOnLan(const QString &mac, const QString &address)
{
    const QString hex = QString(mac).remove(QRegExp(QStringLiteral("[:\\-. ]")));
    const QByteArray bytes = QByteArray::fromHex(hex.toLatin1());
    if (hex.size() != 12 || bytes.size() != 6 || !QRegExp(QStringLiteral("[0-9A-Fa-f]{12}")).exactMatch(hex))
        return SessionManager::tr("%1 is not a MAC address").arg(mac);
    QByteArray packet(6, char(0xff));
    for (int i = 0; i < 16; ++i)
        packet += bytes;
    QUdpSocket socket;
    bool sent = socket.writeDatagram(packet, QHostAddress::Broadcast, 9) == packet.size();
    // Routers that pass on directed broadcasts can wake a host on another network
    const QHostAddress host(address);
    if (!host.isNull())
        sent = socket.writeDatagram(packet, host, 9) == packet.size() || sent;
    return sent ? QString() : SessionManager::tr("Could not send the wake-up packet: %1").arg(socket.errorString());
}

SessionManager::SessionManager(SecretVault *vault, HostStore *hosts, KeyStore *keys, AppSettings *appSettings,
                               QObject *parent)
    : QAbstractListModel(parent)
    , m_vault(vault)
    , m_hosts(hosts)
    , m_keys(keys)
    , m_settings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                 + QStringLiteral("/sessions.conf"), QSettings::IniFormat)
    , m_appSettings(appSettings)
    , m_loading(false)
    , m_working(0)
    , m_waiting(0)
    , m_done(0)
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
                                        const QString &keyId, const QVariantMap &options)
{
    SshSession *session = addSession(name, host, port, user);
    SshOptions sshOptions = optionsFromMap(options);
    sshOptions.forwardAgent = sshOptions.forwardAgent && !keyId.isEmpty();
    session->setOptions(sshOptions);
    session->setCertificate(m_keys->certificate(keyId));
    configureJump(session, options.value(QStringLiteral("jumpHostId")).toString());
    if (keyId.isEmpty()) {
        // A typed password is not kept, so the session cannot come back after a restart
        session->connectToHost(password);
        return session;
    }
    m_origins.insert(session, Origin { QString(), keyId, options });
    save();
    session->connectWithSecret(m_vault, keyId, SshSession::PrivateKey);
    return session;
}

// Single quotes for a POSIX shell
static QString shellQuote(const QString &text)
{
    return QLatin1Char('\'') + QString(text).replace(QLatin1Char('\''), QStringLiteral("'\\''")) + QLatin1Char('\'');
}

SshSession *SessionManager::installKey(const QString &hostId, const QString &keyId, bool useKey)
{
    const QString publicKey = m_keys->publicKey(keyId).simplified();
    HostStore::Host host;
    if (publicKey.isEmpty() || !m_hosts->find(hostId, &host))
        return nullptr;
    // Run by sh whatever the login shell is, and leaves the file alone when the key is in it
    const QString script = QStringLiteral(
                "umask 077 && mkdir -p ~/.ssh && touch ~/.ssh/authorized_keys"
                " && { grep -qxF %1 ~/.ssh/authorized_keys || printf '%s\\n' %1 >> ~/.ssh/authorized_keys; }"
                " && echo %2").arg(shellQuote(publicKey), shellQuote(tr("The key is installed")));
    SshSession *session = addSession(tr("%1: install key").arg(host.name), host.address, host.port, host.user);
    configure(session, host);
    SshOptions options = session->options();
    options.command = QStringLiteral("sh -c ") + shellQuote(script);
    // Only the login is wanted, nothing else the host would set up
    options.mosh = false;
    options.tmuxSession.clear();
    options.localForwards.clear();
    options.remoteForwards.clear();
    options.dynamicForwards.clear();
    options.logging = false;
    session->setOptions(options);
    const QString keyName = keyId;
    connect(session, &SshSession::commandFinished, this, [this, hostId, keyName, useKey](int status) {
        if (status == 0 && useKey)
            m_hosts->setKey(hostId, keyName);
    });
    if (!host.keyId.isEmpty())
        session->connectWithSecret(m_vault, host.keyId, SshSession::PrivateKey);
    else if (host.hasPassword)
        session->connectWithSecret(m_vault, HostStore::passwordSecretId(host.id), SshSession::Password);
    else
        session->connectToHost(QString());
    return session;
}

QString SessionManager::writeSharedText(const QString &name, const QString &text) const
{
    return SftpBrowser::writeSharedText(name, text);
}

QString SessionManager::wakeHost(const QString &hostId)
{
    HostStore::Host host;
    if (!m_hosts->find(hostId, &host) || host.macAddress.isEmpty())
        return tr("The host has no MAC address");
    return sendWakeOnLan(host.macAddress, host.address);
}

SshSession *SessionManager::openHost(const QString &hostId, const QString &password)
{
    HostStore::Host host;
    if (!m_hosts->find(hostId, &host))
        return nullptr;

    SshSession *session = addSession(host.name, host.address, host.port, host.user);
    configure(session, host);
    m_origins.insert(session, Origin { host.id, QString(), QVariantMap() });
    save();
    m_hosts->markUsed(host.id);
    // A sleeping host gets a moment more while the connection is set up
    if (!host.macAddress.isEmpty())
        sendWakeOnLan(host.macAddress, host.address);
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
    session->setHostId(host.id);
    SshOptions options = optionsFromMap(m_hosts->host(host.id));
    options.forwardAgent = options.forwardAgent && !host.keyId.isEmpty();
    // A session running a command, such as installing a key, keeps it
    options.command = session->options().command;
    session->setOptions(options);
    session->setCertificate(m_keys->certificate(host.keyId));
    if (!host.systemId.isEmpty())
        session->setSystem(host.systemId, host.systemName);
    configureJump(session, host.jumpHostId);
}

void SessionManager::configureJump(SshSession *session, const QString &jumpHostId)
{
    HostStore::Host jump;
    if (jumpHostId.isEmpty() || !m_hosts->find(jumpHostId, &jump)) {
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

void SessionManager::countActivities()
{
    int working = 0;
    int waiting = 0;
    int done = 0;
    for (const SshSession *session : m_sessions) {
        if (session->state() != SshSession::Connected)
            continue;
        const QString activity = session->terminal()->activity();
        working += activity == QLatin1String("working");
        waiting += activity == QLatin1String("waiting");
        done += activity == QLatin1String("done");
    }
    if (working == m_working && waiting == m_waiting && done == m_done)
        return;
    m_working = working;
    m_waiting = waiting;
    m_done = done;
    emit activityCountsChanged();
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

    // mosh goes on from the new network, its server follows wherever the packets come from
    for (SshSession *session : m_sessions) {
        if (session->state() == SshSession::Connected && session->usesMosh())
            session->roam();
    }
    if (!m_appSettings->autoReconnect())
        return;

    const bool online = m_network.isOnline();
    for (SshSession *session : m_sessions) {
        if (session->usesMosh())
            continue;
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
    session->setSessionId(QUuid::createUuid().toString().remove(QRegExp(QStringLiteral("[{}-]"))));
    session->setMoshVault(m_vault);
    session->setPredictionEnabled(m_appSettings->moshPrediction());
    connect(m_appSettings, &AppSettings::moshPredictionChanged, session, [this, session]() {
        session->setPredictionEnabled(m_appSettings->moshPrediction());
    });
    // Returned to QML from an invokable, which would otherwise hand ownership to JS
    QQmlEngine::setObjectOwnership(session, QQmlEngine::CppOwnership);
    connect(session, &SshSession::shellExited, session, &SshSession::forgetMosh);
    connect(session, &SshSession::nameChanged, this, &SessionManager::save);
    connect(session, &SshSession::startupScriptChanged, this, &SessionManager::save);
    connect(session, &SshSession::tmuxChanged, this, &SessionManager::save);
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
    connect(session, &SshSession::stateChanged, this, [this, session]() {
        const int row = m_sessions.indexOf(session);
        if (row >= 0)
            emit dataChanged(index(row), index(row));
        countActivities();
    });
    connect(session->terminal(), &Terminal::activityChanged, this, &SessionManager::countActivities);
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
    countActivities();
    // A session that goes for good leaves nothing to resume
    if (session->state() == SshSession::Disconnected)
        session->forgetMosh();

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
            m_origins.insert(session, Origin { hostId, QString(), QVariantMap() });
        } else if (!keyId.isEmpty()) {
            int port = m_settings.value(QStringLiteral("port"), 22).toInt();
            if (port < 1 || port > 65535)
                port = 22;
            SshSession *session = addSession(name, m_settings.value(QStringLiteral("host")).toString(), port,
                                             m_settings.value(QStringLiteral("user")).toString());
            session->setSecret(m_vault, keyId, SshSession::PrivateKey);
            session->setSystem(m_settings.value(QStringLiteral("systemId")).toString(),
                               m_settings.value(QStringLiteral("systemName")).toString());
            const QVariantMap options = m_settings.value(QStringLiteral("options")).toMap();
            session->setOptions(optionsFromMap(options));
            session->setCertificate(m_keys->certificate(keyId));
            configureJump(session, options.value(QStringLiteral("jumpHostId")).toString());
            m_origins.insert(session, Origin { QString(), keyId, options });
        } else {
            continue;
        }
        const QString id = m_settings.value(QStringLiteral("id")).toString();
        if (!id.isEmpty())
            m_sessions.last()->setSessionId(id);
        m_sessions.last()->setStartupScript(m_settings.value(QStringLiteral("startupScript")).toString());
        m_sessions.last()->setColorScheme(m_settings.value(QStringLiteral("colorScheme")).toString());
        m_sessions.last()->setOwnTmux(m_settings.value(QStringLiteral("ownTmux")).toBool());
        m_sessions.last()->setTmuxSession(m_settings.value(QStringLiteral("tmuxSession")).toString());
    }
    m_settings.endArray();
    m_loading = false;

    // Journals of sessions that are gone have nothing left to resume
    QStringList kept;
    for (const SshSession *session : m_sessions)
        kept.append(session->sessionId() + QStringLiteral(".journal"));
    const QDir journals(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/mosh"));
    for (const QString &name : journals.entryList(QStringList(QStringLiteral("*.journal")), QDir::Files)) {
        if (!kept.contains(name))
            QFile::remove(journals.filePath(name));
    }
    // mosh sessions the app left running pick up where they were, they need no login
    for (SshSession *session : m_sessions) {
        if (session->canResume())
            session->reconnect();
    }
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
        m_settings.setValue(QStringLiteral("id"), session->sessionId());
        m_settings.setValue(QStringLiteral("hostId"), origin.hostId);
        m_settings.setValue(QStringLiteral("keyId"), origin.keyId);
        m_settings.setValue(QStringLiteral("host"), session->host());
        m_settings.setValue(QStringLiteral("port"), session->port());
        m_settings.setValue(QStringLiteral("user"), session->user());
        m_settings.setValue(QStringLiteral("startupScript"), session->startupScript());
        m_settings.setValue(QStringLiteral("colorScheme"), session->colorScheme());
        m_settings.setValue(QStringLiteral("ownTmux"), session->ownTmux());
        m_settings.setValue(QStringLiteral("tmuxSession"), session->tmuxSession());
        if (origin.hostId.isEmpty()) {
            m_settings.setValue(QStringLiteral("systemId"), session->systemId());
            m_settings.setValue(QStringLiteral("systemName"), session->systemName());
            m_settings.setValue(QStringLiteral("options"), origin.options);
        }
    }
    m_settings.endArray();
    m_settings.sync();
}

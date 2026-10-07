#include "hoststore.h"

#include <QRegExp>
#include <QStandardPaths>
#include <QUuid>

#include "secretvault.h"

// A damaged file can claim any number of entries
static const int MaxHosts = 1000;
static const int MaxKeepAliveInterval = 3600;
static const int MaxConnectTimeout = 300;

static QString singleLine(const QString &text)
{
    return QString(text).replace(QRegExp(QStringLiteral("[\\x0000-\\x001f\\x007f]")), QStringLiteral(" ")).trimmed();
}

static QStringList singleLines(const QVariant &value)
{
    QStringList lines;
    for (const QString &entry : value.toStringList()) {
        const QString line = singleLine(entry);
        if (!line.isEmpty())
            lines.append(line);
    }
    return lines;
}

static int seconds(const QVariant &value, int maximum)
{
    return qBound(0, value.toInt(), maximum);
}

HostStore::HostStore(SecretVault *vault, QObject *parent)
    : QAbstractListModel(parent)
    , m_vault(vault)
    , m_batch(false)
    // Passwords are never written here, they go to Sailfish Secrets
    , m_settings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                 + QStringLiteral("/hosts.conf"), QSettings::IniFormat)
{
    load();
}

int HostStore::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_hosts.size();
}

QVariant HostStore::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_hosts.size())
        return QVariant();
    const Host &host = m_hosts.at(index.row());
    switch (role) {
    case HostIdRole: return host.id;
    case NameRole: return host.name;
    case AddressRole: return host.address;
    case PortRole: return host.port;
    case UserRole: return host.user;
    case KeyIdRole: return host.keyId;
    case HasPasswordRole: return host.hasPassword;
    case SystemIdRole: return host.systemId;
    case SystemNameRole: return host.systemName;
    default: return QVariant();
    }
}

QHash<int, QByteArray> HostStore::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[HostIdRole] = "hostId";
    roles[NameRole] = "name";
    roles[AddressRole] = "address";
    roles[PortRole] = "port";
    roles[UserRole] = "user";
    roles[KeyIdRole] = "keyId";
    roles[HasPasswordRole] = "hasPassword";
    roles[SystemIdRole] = "systemId";
    roles[SystemNameRole] = "systemName";
    return roles;
}

QString HostStore::saveHost(const QString &hostId, const QString &name,
                            const QString &address, int port, const QString &user,
                            const QString &keyId, const QString &password,
                            bool rememberPassword, const QVariantMap &options)
{
    Host host;
    int row = indexOf(hostId);
    if (row >= 0) {
        host = m_hosts.at(row);
    } else if (m_hosts.size() >= MaxHosts) {
        setError(tr("Too many hosts, %1 can be saved").arg(MaxHosts));
        return QString();
    } else {
        host.id = QUuid::createUuid().toString().remove(QRegExp(QStringLiteral("[{}-]")));
        host.hasPassword = false;
    }
    host.name = singleLine(name);
    if (host.address != singleLine(address)) {
        host.systemId.clear();
        host.systemName.clear();
    }
    host.address = singleLine(address);
    host.port = port >= 1 && port <= 65535 ? port : 22;
    host.user = singleLine(user);
    host.keyId = keyId;
    const QString jumpHostId = options.value(QStringLiteral("jumpHostId")).toString();
    host.jumpHostId = jumpHostId == host.id ? QString() : jumpHostId;
    host.forwardAgent = options.value(QStringLiteral("forwardAgent")).toBool();
    host.localForwards = singleLines(options.value(QStringLiteral("localForwards")));
    host.remoteForwards = singleLines(options.value(QStringLiteral("remoteForwards")));
    host.dynamicForwards = singleLines(options.value(QStringLiteral("dynamicForwards")));
    host.environment = singleLines(options.value(QStringLiteral("environment")));
    host.tmuxSession = singleLine(options.value(QStringLiteral("tmuxSession")).toString());
    host.keepAliveInterval = seconds(options.value(QStringLiteral("keepAliveInterval")), MaxKeepAliveInterval);
    host.connectTimeout = seconds(options.value(QStringLiteral("connectTimeout")), MaxConnectTimeout);
    host.mosh = options.value(QStringLiteral("mosh")).toBool();

    if (row >= 0) {
        m_hosts[row] = host;
        emit dataChanged(index(row), index(row));
    } else {
        row = m_hosts.size();
        beginInsertRows(QModelIndex(), row, row);
        m_hosts.append(host);
        endInsertRows();
        emit countChanged();
    }
    if (!m_batch)
        save();

    const QString id = host.id;
    setError(QString());
    if (keyId.isEmpty() && rememberPassword && !password.isEmpty()) {
        m_vault->store(passwordSecretId(id), password.toUtf8(), this, [this, id](const QString &error) {
            if (!error.isEmpty())
                setError(tr("Could not remember password: %1").arg(error));
            else if (indexOf(id) < 0)
                m_vault->remove(passwordSecretId(id), this, [](const QString &) {});
            else
                setHasPassword(id, true);
        });
    } else if ((!keyId.isEmpty() || !rememberPassword) && host.hasPassword) {
        setHasPassword(id, false);
        m_vault->remove(passwordSecretId(id), this, [](const QString &) {});
    }
    return id;
}

void HostStore::rememberPassword(const QString &hostId, const QString &password)
{
    Host host;
    if (!find(hostId, &host) || !host.keyId.isEmpty() || password.isEmpty())
        return;
    const QString id = host.id;
    m_vault->store(passwordSecretId(id), password.toUtf8(), this, [this, id](const QString &error) {
        if (!error.isEmpty())
            setError(tr("Could not remember password: %1").arg(error));
        else if (indexOf(id) < 0)
            m_vault->remove(passwordSecretId(id), this, [](const QString &) {});
        else
            setHasPassword(id, true);
    });
}

void HostStore::forgetKey(const QString &keyId)
{
    bool changed = false;
    for (int row = 0; row < m_hosts.size(); ++row) {
        if (m_hosts.at(row).keyId != keyId)
            continue;
        m_hosts[row].keyId.clear();
        m_hosts[row].forwardAgent = false;
        emit dataChanged(index(row), index(row));
        changed = true;
    }
    if (changed)
        save();
}

void HostStore::setSystem(const QString &hostId, const QString &id, const QString &name)
{
    const int row = indexOf(hostId);
    if (row < 0 || (m_hosts.at(row).systemId == id && m_hosts.at(row).systemName == name))
        return;
    m_hosts[row].systemId = id;
    m_hosts[row].systemName = name;
    save();
    emit dataChanged(index(row), index(row), { SystemIdRole, SystemNameRole });
}

void HostStore::removeHost(const QString &hostId)
{
    const int row = indexOf(hostId);
    if (row < 0)
        return;
    const bool hadPassword = m_hosts.at(row).hasPassword;
    beginRemoveRows(QModelIndex(), row, row);
    m_hosts.removeAt(row);
    endRemoveRows();
    // Hosts that went through this one connect directly from now on
    for (int other = 0; other < m_hosts.size(); ++other) {
        if (m_hosts.at(other).jumpHostId == hostId) {
            m_hosts[other].jumpHostId.clear();
            emit dataChanged(index(other), index(other));
        }
    }
    save();
    emit countChanged();
    if (hadPassword)
        m_vault->remove(passwordSecretId(hostId), this, [](const QString &) {});
}

QVariantMap HostStore::host(const QString &hostId) const
{
    QVariantMap map;
    Host host;
    if (!find(hostId, &host))
        return map;
    map.insert(QStringLiteral("name"), host.name);
    map.insert(QStringLiteral("address"), host.address);
    map.insert(QStringLiteral("port"), host.port);
    map.insert(QStringLiteral("user"), host.user);
    map.insert(QStringLiteral("keyId"), host.keyId);
    map.insert(QStringLiteral("hasPassword"), host.hasPassword);
    map.insert(QStringLiteral("jumpHostId"), host.jumpHostId);
    map.insert(QStringLiteral("forwardAgent"), host.forwardAgent);
    map.insert(QStringLiteral("localForwards"), host.localForwards);
    map.insert(QStringLiteral("remoteForwards"), host.remoteForwards);
    map.insert(QStringLiteral("dynamicForwards"), host.dynamicForwards);
    map.insert(QStringLiteral("environment"), host.environment);
    map.insert(QStringLiteral("tmuxSession"), host.tmuxSession);
    map.insert(QStringLiteral("keepAliveInterval"), host.keepAliveInterval);
    map.insert(QStringLiteral("connectTimeout"), host.connectTimeout);
    map.insert(QStringLiteral("mosh"), host.mosh);
    return map;
}

QString HostStore::hostIdAt(int row) const
{
    return row >= 0 && row < m_hosts.size() ? m_hosts.at(row).id : QString();
}

static QStringList configArguments(const QString &text)
{
    QStringList arguments;
    QString current;
    bool inArgument = false;
    bool quoted = false;
    for (const QChar c : text) {
        if (quoted) {
            if (c == QLatin1Char('"'))
                quoted = false;
            else
                current += c;
        } else if (c == QLatin1Char('"')) {
            quoted = true;
            inArgument = true;
        } else if (c.isSpace()) {
            if (inArgument)
                arguments.append(current);
            current.clear();
            inArgument = false;
        } else if (c == QLatin1Char('#') && !inArgument) {
            break;
        } else {
            current += c;
            inArgument = true;
        }
    }
    if (inArgument)
        arguments.append(current);
    return arguments;
}

static bool isPort(const QString &text)
{
    bool ok = false;
    const int port = text.toInt(&ok);
    return ok && port >= 1 && port <= 65535;
}

// Single quotes for a POSIX shell, left out where nothing needs them
static QString shellQuote(const QString &text)
{
    if (!text.isEmpty() && !text.contains(QRegExp(QStringLiteral("[^A-Za-z0-9_.,:@%+/-]"))))
        return text;
    return QLatin1Char('\'') + QString(text).replace(QLatin1Char('\''), QStringLiteral("'\\''")) + QLatin1Char('\'');
}

static QString configAlias(const QString &name)
{
    return name.simplified().replace(QLatin1Char(' '), QLatin1Char('-'));
}

QString HostStore::exportConfig() const
{
    QString config;
    for (const Host &host : m_hosts) {
        config += QStringLiteral("Host %1\n").arg(configAlias(host.name));
        config += QStringLiteral("    HostName %1\n").arg(host.address);
        if (host.port != 22)
            config += QStringLiteral("    Port %1\n").arg(host.port);
        if (!host.user.isEmpty())
            config += QStringLiteral("    User %1\n").arg(host.user);
        Host jump;
        if (!host.jumpHostId.isEmpty() && find(host.jumpHostId, &jump))
            config += QStringLiteral("    ProxyJump %1\n").arg(configAlias(jump.name));
        if (host.forwardAgent)
            config += QStringLiteral("    ForwardAgent yes\n");
        for (const QString &forward : host.localForwards) {
            const int colon = forward.indexOf(QLatin1Char(':'));
            config += QStringLiteral("    LocalForward %1 %2\n").arg(forward.left(colon), forward.mid(colon + 1));
        }
        for (const QString &forward : host.remoteForwards) {
            const int colon = forward.indexOf(QLatin1Char(':'));
            config += QStringLiteral("    RemoteForward %1 %2\n").arg(forward.left(colon), forward.mid(colon + 1));
        }
        for (const QString &port : host.dynamicForwards)
            config += QStringLiteral("    DynamicForward %1\n").arg(port);
        for (const QString &variable : host.environment) {
            const int equals = variable.indexOf(QLatin1Char('='));
            // OpenSSH has no escape for a quote inside a quoted value
            QString value = variable.mid(equals + 1).remove(QLatin1Char('"'));
            if (value.isEmpty() || value.contains(QRegExp(QStringLiteral("[\\s#]"))))
                value = QLatin1Char('"') + value + QLatin1Char('"');
            config += QStringLiteral("    SetEnv %1=%2\n").arg(variable.left(equals), value);
        }
        if (host.keepAliveInterval > 0)
            config += QStringLiteral("    ServerAliveInterval %1\n").arg(host.keepAliveInterval);
        if (host.connectTimeout > 0)
            config += QStringLiteral("    ConnectTimeout %1\n").arg(host.connectTimeout);
        if (!host.tmuxSession.isEmpty()) {
            config += QStringLiteral("    RemoteCommand tmux new-session -A -s %1\n").arg(shellQuote(host.tmuxSession));
            config += QStringLiteral("    RequestTTY yes\n");
        }
        config += QLatin1Char('\n');
    }
    return config;
}

int HostStore::importConfig(const QString &config)
{
    struct Entry {
        QString name;
        QString address;
        int port = 22;
        QString user;
        QString jump;
        bool forwardAgent = false;
        QStringList localForwards;
        QStringList remoteForwards;
        QStringList dynamicForwards;
        QStringList environment;
        QString tmuxSession;
        int keepAliveInterval = 0;
        int connectTimeout = 0;
    };
    QList<Entry> entries;
    Entry *entry = nullptr;

    const QStringList lines = config.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')))
            continue;
        // "Key value" or "Key=value"
        const int split = trimmed.indexOf(QRegExp(QStringLiteral("[\\s=]")));
        if (split < 0)
            continue;
        const QString key = trimmed.left(split).toLower();
        QString rest = trimmed.mid(split).trimmed();
        if (rest.startsWith(QLatin1Char('=')))
            rest = rest.mid(1).trimmed();
        const QStringList arguments = configArguments(rest);
        const QString value = arguments.value(0);

        if (key == QLatin1String("host")) {
            const QString alias = value;
            // Patterns apply to many hosts, there is nothing to save for them
            if (alias.isEmpty() || alias.contains(QRegExp(QStringLiteral("[*?!]")))) {
                entry = nullptr;
                continue;
            }
            entries.append(Entry());
            entry = &entries.last();
            entry->name = alias;
            entry->address = alias;
        } else if (key == QLatin1String("match")) {
            entry = nullptr;
        } else if (!entry) {
            continue;
        } else if (key == QLatin1String("hostname")) {
            entry->address = value;
        } else if (key == QLatin1String("port")) {
            if (isPort(value))
                entry->port = value.toInt();
        } else if (key == QLatin1String("user")) {
            entry->user = value;
        } else if (key == QLatin1String("proxyjump")) {
            QString jump = value.split(QLatin1Char(',')).value(0).trimmed();
            jump = jump.mid(jump.lastIndexOf(QLatin1Char('@')) + 1);
            if (jump.startsWith(QLatin1Char('[')) && jump.contains(QLatin1Char(']')))
                jump = jump.mid(1, jump.indexOf(QLatin1Char(']')) - 1);
            else if (jump.count(QLatin1Char(':')) == 1)
                jump = jump.section(QLatin1Char(':'), 0, 0);
            entry->jump = jump;
        } else if (key == QLatin1String("forwardagent")) {
            entry->forwardAgent = value.toLower() == QLatin1String("yes");
        } else if (key == QLatin1String("localforward")) {
            const QString target = arguments.value(1);
            if (arguments.size() == 2 && isPort(value.section(QLatin1Char(':'), -1))
                    && target.contains(QLatin1Char(':')) && isPort(target.section(QLatin1Char(':'), -1)))
                entry->localForwards.append(value.section(QLatin1Char(':'), -1) + QLatin1Char(':') + target);
        } else if (key == QLatin1String("remoteforward")) {
            // The single argument form is a SOCKS proxy on the server, which is not supported
            const QString target = arguments.value(1);
            if (arguments.size() == 2 && isPort(value.section(QLatin1Char(':'), -1))
                    && target.contains(QLatin1Char(':')) && isPort(target.section(QLatin1Char(':'), -1)))
                entry->remoteForwards.append(value.section(QLatin1Char(':'), -1) + QLatin1Char(':') + target);
        } else if (key == QLatin1String("dynamicforward")) {
            if (isPort(value.section(QLatin1Char(':'), -1)))
                entry->dynamicForwards.append(value.section(QLatin1Char(':'), -1));
        } else if (key == QLatin1String("setenv")) {
            for (const QString &variable : arguments) {
                if (variable.contains(QRegExp(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*="))))
                    entry->environment.append(variable);
            }
        } else if (key == QLatin1String("serveraliveinterval")) {
            entry->keepAliveInterval = seconds(value, MaxKeepAliveInterval);
        } else if (key == QLatin1String("connecttimeout")) {
            entry->connectTimeout = seconds(value, MaxConnectTimeout);
        } else if (key == QLatin1String("remotecommand")) {
            // Only the tmux sessions exported above, other commands have no setting here
            QRegExp tmux(QStringLiteral("^tmux\\s+(?:new-session|new)\\s+-A\\s+-s\\s*(?:'([^']*)'|(\\S+))$"));
            if (tmux.exactMatch(rest))
                entry->tmuxSession = tmux.cap(1).isEmpty() ? tmux.cap(2) : tmux.cap(1);
        }
    }

    const auto findName = [this](const QString &name) {
        int row = indexOfName(name);
        for (int other = 0; row < 0 && other < m_hosts.size(); ++other) {
            if (configAlias(m_hosts.at(other).name) == name)
                row = other;
        }
        return row;
    };
    QStringList ids;
    m_batch = true;
    for (const Entry &imported : entries) {
        const int row = findName(imported.name);
        const Host existing = row >= 0 ? m_hosts.at(row) : Host();
        QVariantMap options;
        options.insert(QStringLiteral("forwardAgent"), imported.forwardAgent);
        options.insert(QStringLiteral("localForwards"), imported.localForwards);
        options.insert(QStringLiteral("remoteForwards"), imported.remoteForwards);
        options.insert(QStringLiteral("dynamicForwards"), imported.dynamicForwards);
        options.insert(QStringLiteral("environment"), imported.environment);
        options.insert(QStringLiteral("tmuxSession"), imported.tmuxSession);
        options.insert(QStringLiteral("keepAliveInterval"), imported.keepAliveInterval);
        options.insert(QStringLiteral("connectTimeout"), imported.connectTimeout);
        // ssh_config has no mosh, so an updated host keeps its own
        options.insert(QStringLiteral("mosh"), row >= 0 && existing.mosh);
        ids.append(saveHost(row >= 0 ? existing.id : QString(), imported.name, imported.address,
                            imported.port, imported.user, row >= 0 ? existing.keyId : QString(),
                            QString(), row >= 0 && existing.hasPassword, options));
    }
    m_batch = false;
    // Jumps can name hosts that come later in the file
    for (int i = 0; i < entries.size(); ++i) {
        if (entries.at(i).jump.isEmpty())
            continue;
        const int jumpRow = findName(entries.at(i).jump);
        const int row = indexOf(ids.at(i));
        if (jumpRow >= 0 && row >= 0 && jumpRow != row) {
            m_hosts[row].jumpHostId = m_hosts.at(jumpRow).id;
            emit dataChanged(index(row), index(row));
        }
    }
    save();
    return entries.size();
}

bool HostStore::find(const QString &hostId, Host *host) const
{
    const int row = indexOf(hostId);
    if (row < 0)
        return false;
    *host = m_hosts.at(row);
    return true;
}

QString HostStore::passwordSecretId(const QString &hostId)
{
    return QStringLiteral("pw") + hostId;
}

int HostStore::indexOf(const QString &hostId) const
{
    for (int row = 0; row < m_hosts.size(); ++row) {
        if (m_hosts.at(row).id == hostId)
            return row;
    }
    return -1;
}

int HostStore::indexOfName(const QString &name) const
{
    for (int row = 0; row < m_hosts.size(); ++row) {
        if (m_hosts.at(row).name == name)
            return row;
    }
    return -1;
}

void HostStore::setHasPassword(const QString &hostId, bool hasPassword)
{
    const int row = indexOf(hostId);
    if (row < 0 || m_hosts.at(row).hasPassword == hasPassword)
        return;
    m_hosts[row].hasPassword = hasPassword;
    save();
    emit dataChanged(index(row), index(row));
}

void HostStore::load()
{
    const int size = qMin(m_settings.beginReadArray(QStringLiteral("hosts")), MaxHosts);
    for (int i = 0; i < size; ++i) {
        m_settings.setArrayIndex(i);
        Host host;
        host.id = m_settings.value(QStringLiteral("id")).toString();
        host.name = m_settings.value(QStringLiteral("name")).toString();
        host.address = m_settings.value(QStringLiteral("address")).toString();
        host.port = m_settings.value(QStringLiteral("port"), 22).toInt();
        if (host.port < 1 || host.port > 65535)
            host.port = 22;
        host.user = m_settings.value(QStringLiteral("user")).toString();
        host.keyId = m_settings.value(QStringLiteral("keyId")).toString();
        host.hasPassword = m_settings.value(QStringLiteral("hasPassword"), false).toBool();
        host.jumpHostId = m_settings.value(QStringLiteral("jumpHostId")).toString();
        host.forwardAgent = m_settings.value(QStringLiteral("forwardAgent"), false).toBool();
        host.localForwards = m_settings.value(QStringLiteral("localForwards")).toStringList();
        host.remoteForwards = m_settings.value(QStringLiteral("remoteForwards")).toStringList();
        host.dynamicForwards = m_settings.value(QStringLiteral("dynamicForwards")).toStringList();
        host.environment = m_settings.value(QStringLiteral("environment")).toStringList();
        host.tmuxSession = m_settings.value(QStringLiteral("tmuxSession")).toString();
        host.keepAliveInterval = seconds(m_settings.value(QStringLiteral("keepAliveInterval")), MaxKeepAliveInterval);
        host.connectTimeout = seconds(m_settings.value(QStringLiteral("connectTimeout")), MaxConnectTimeout);
        host.mosh = m_settings.value(QStringLiteral("mosh"), false).toBool();
        host.systemId = m_settings.value(QStringLiteral("systemId")).toString();
        host.systemName = m_settings.value(QStringLiteral("systemName")).toString();
        if (host.id.isEmpty())
            continue;
        m_hosts.append(host);
    }
    m_settings.endArray();
}

void HostStore::save()
{
    m_settings.remove(QStringLiteral("hosts"));
    m_settings.beginWriteArray(QStringLiteral("hosts"), m_hosts.size());
    for (int i = 0; i < m_hosts.size(); ++i) {
        const Host &host = m_hosts.at(i);
        m_settings.setArrayIndex(i);
        m_settings.setValue(QStringLiteral("id"), host.id);
        m_settings.setValue(QStringLiteral("name"), host.name);
        m_settings.setValue(QStringLiteral("address"), host.address);
        m_settings.setValue(QStringLiteral("port"), host.port);
        m_settings.setValue(QStringLiteral("user"), host.user);
        m_settings.setValue(QStringLiteral("keyId"), host.keyId);
        m_settings.setValue(QStringLiteral("hasPassword"), host.hasPassword);
        m_settings.setValue(QStringLiteral("jumpHostId"), host.jumpHostId);
        m_settings.setValue(QStringLiteral("forwardAgent"), host.forwardAgent);
        m_settings.setValue(QStringLiteral("localForwards"), host.localForwards);
        m_settings.setValue(QStringLiteral("remoteForwards"), host.remoteForwards);
        m_settings.setValue(QStringLiteral("dynamicForwards"), host.dynamicForwards);
        m_settings.setValue(QStringLiteral("environment"), host.environment);
        m_settings.setValue(QStringLiteral("tmuxSession"), host.tmuxSession);
        m_settings.setValue(QStringLiteral("keepAliveInterval"), host.keepAliveInterval);
        m_settings.setValue(QStringLiteral("connectTimeout"), host.connectTimeout);
        m_settings.setValue(QStringLiteral("mosh"), host.mosh);
        m_settings.setValue(QStringLiteral("systemId"), host.systemId);
        m_settings.setValue(QStringLiteral("systemName"), host.systemName);
    }
    m_settings.endArray();
    m_settings.sync();
}

void HostStore::setError(const QString &message)
{
    if (m_errorString == message)
        return;
    m_errorString = message;
    emit errorStringChanged();
}

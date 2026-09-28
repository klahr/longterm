#include "hoststore.h"

#include <QRegExp>
#include <QStandardPaths>
#include <QUuid>

#include "secretvault.h"

HostStore::HostStore(SecretVault *vault, QObject *parent)
    : QAbstractListModel(parent)
    , m_vault(vault)
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
    return roles;
}

QString HostStore::saveHost(const QString &hostId, const QString &name,
                            const QString &address, int port, const QString &user,
                            const QString &keyId, const QString &password,
                            bool rememberPassword, const QString &jumpHostId,
                            bool forwardAgent, const QStringList &localForwards)
{
    Host host;
    int row = indexOf(hostId);
    if (row >= 0) {
        host = m_hosts.at(row);
    } else {
        host.id = QUuid::createUuid().toString().remove(QRegExp(QStringLiteral("[{}-]")));
        host.hasPassword = false;
    }
    host.name = name.trimmed();
    host.address = address.trimmed();
    host.port = port;
    host.user = user.trimmed();
    host.keyId = keyId;
    host.jumpHostId = jumpHostId == host.id ? QString() : jumpHostId;
    host.forwardAgent = forwardAgent;
    host.localForwards = localForwards;

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
    save();

    const QString id = host.id;
    setError(QString());
    if (keyId.isEmpty() && rememberPassword && !password.isEmpty()) {
        m_vault->store(passwordSecretId(id), password.toUtf8(), this, [this, id](const QString &error) {
            if (error.isEmpty())
                setHasPassword(id, true);
            else
                setError(tr("Could not remember password: %1").arg(error));
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
        if (error.isEmpty())
            setHasPassword(id, true);
        else
            setError(tr("Could not remember password: %1").arg(error));
    });
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
    return map;
}

QString HostStore::hostIdAt(int row) const
{
    return row >= 0 && row < m_hosts.size() ? m_hosts.at(row).id : QString();
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
        config += QLatin1Char('\n');
    }
    return config;
}

int HostStore::importConfig(const QString &config)
{
    struct Entry {
        QString name;
        QString address;
        int port;
        QString user;
        QString jump;
        bool forwardAgent;
        QStringList localForwards;
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
        QString value = trimmed.mid(split).trimmed();
        if (value.startsWith(QLatin1Char('=')))
            value = value.mid(1).trimmed();
        if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
            value = value.mid(1, value.size() - 2);

        if (key == QLatin1String("host")) {
            const QString alias = value.split(QRegExp(QStringLiteral("\\s+"))).value(0);
            // Patterns apply to many hosts, there is nothing to save for them
            if (alias.isEmpty() || alias.contains(QRegExp(QStringLiteral("[*?!]")))) {
                entry = nullptr;
                continue;
            }
            entries.append(Entry { alias, alias, 22, QString(), QString(), false, QStringList() });
            entry = &entries.last();
        } else if (key == QLatin1String("match")) {
            entry = nullptr;
        } else if (!entry) {
            continue;
        } else if (key == QLatin1String("hostname")) {
            entry->address = value;
        } else if (key == QLatin1String("port")) {
            entry->port = qBound(1, value.toInt(), 65535);
        } else if (key == QLatin1String("user")) {
            entry->user = value;
        } else if (key == QLatin1String("proxyjump")) {
            // Only the first hop, and only when it names a saved host
            entry->jump = value.split(QLatin1Char(',')).value(0).trimmed();
        } else if (key == QLatin1String("forwardagent")) {
            entry->forwardAgent = value.toLower() == QLatin1String("yes");
        } else if (key == QLatin1String("localforward")) {
            // "[bind:]port host:hostport"
            const QStringList parts = value.split(QRegExp(QStringLiteral("\\s+")));
            if (parts.size() == 2)
                entry->localForwards.append(parts.at(0).section(QLatin1Char(':'), -1) + QLatin1Char(':') + parts.at(1));
        }
    }

    QStringList ids;
    for (const Entry &imported : entries) {
        const int row = indexOfName(imported.name);
        const Host existing = row >= 0 ? m_hosts.at(row) : Host();
        ids.append(saveHost(row >= 0 ? existing.id : QString(), imported.name, imported.address,
                            imported.port, imported.user, row >= 0 ? existing.keyId : QString(),
                            QString(), row >= 0 && existing.hasPassword, QString(),
                            imported.forwardAgent, imported.localForwards));
    }
    // Jumps can name hosts that come later in the file
    for (int i = 0; i < entries.size(); ++i) {
        if (entries.at(i).jump.isEmpty())
            continue;
        int jumpRow = indexOfName(entries.at(i).jump);
        if (jumpRow < 0) {
            for (int row = 0; row < m_hosts.size(); ++row) {
                if (configAlias(m_hosts.at(row).name) == entries.at(i).jump)
                    jumpRow = row;
            }
        }
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
    const int size = m_settings.beginReadArray(QStringLiteral("hosts"));
    for (int i = 0; i < size; ++i) {
        m_settings.setArrayIndex(i);
        Host host;
        host.id = m_settings.value(QStringLiteral("id")).toString();
        host.name = m_settings.value(QStringLiteral("name")).toString();
        host.address = m_settings.value(QStringLiteral("address")).toString();
        host.port = m_settings.value(QStringLiteral("port"), 22).toInt();
        host.user = m_settings.value(QStringLiteral("user")).toString();
        host.keyId = m_settings.value(QStringLiteral("keyId")).toString();
        host.hasPassword = m_settings.value(QStringLiteral("hasPassword"), false).toBool();
        host.jumpHostId = m_settings.value(QStringLiteral("jumpHostId")).toString();
        host.forwardAgent = m_settings.value(QStringLiteral("forwardAgent"), false).toBool();
        host.localForwards = m_settings.value(QStringLiteral("localForwards")).toStringList();
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

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
                            bool rememberPassword)
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

void HostStore::removeHost(const QString &hostId)
{
    const int row = indexOf(hostId);
    if (row < 0)
        return;
    const bool hadPassword = m_hosts.at(row).hasPassword;
    beginRemoveRows(QModelIndex(), row, row);
    m_hosts.removeAt(row);
    endRemoveRows();
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
    return map;
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

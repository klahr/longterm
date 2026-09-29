#include "knownhosts.h"

#include <QFile>
#include <QSaveFile>

#include <libssh/libssh.h>

#include "sshsession.h"

KnownHosts::KnownHosts(QObject *parent)
    : QAbstractListModel(parent)
{
    reload();
}

int KnownHosts::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_entries.size();
}

QVariant KnownHosts::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_entries.size())
        return QVariant();
    const Entry &entry = m_entries.at(index.row());
    switch (role) {
    case HostsRole: return entry.hosts;
    case KeyTypeRole: return entry.keyType;
    case FingerprintRole: return entry.fingerprint;
    case LineRole: return QString::fromLatin1(entry.line);
    default: return QVariant();
    }
}

QHash<int, QByteArray> KnownHosts::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[HostsRole] = "hosts";
    roles[KeyTypeRole] = "keyType";
    roles[FingerprintRole] = "fingerprint";
    roles[LineRole] = "line";
    return roles;
}

void KnownHosts::reload()
{
    QList<Entry> entries;
    QFile file(SshSession::knownHostsPath());
    if (file.open(QIODevice::ReadOnly)) {
        while (!file.atEnd()) {
            const QByteArray line = file.readLine();
            const QList<QByteArray> fields = line.simplified().split(' ');
            if (fields.size() < 3 || fields.at(0).startsWith('#'))
                continue;

            Entry entry;
            entry.hosts = QString::fromUtf8(fields.at(0)).replace(QLatin1Char(','), QStringLiteral(", "));
            entry.keyType = QString::fromLatin1(fields.at(1));
            entry.line = line;

            ssh_key key = nullptr;
            unsigned char *hash = nullptr;
            size_t hashLength = 0;
            if (ssh_pki_import_pubkey_base64(fields.at(2).constData(), ssh_key_type_from_name(fields.at(1).constData()),
                                             &key) == SSH_OK
                    && ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hashLength) == SSH_OK) {
                char *fingerprint = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hashLength);
                entry.fingerprint = QString::fromLatin1(fingerprint);
                ssh_string_free_char(fingerprint);
                ssh_clean_pubkey_hash(&hash);
            }
            ssh_key_free(key);
            entries.append(entry);
        }
    }

    beginResetModel();
    m_entries = entries;
    endResetModel();
    emit countChanged();
}

void KnownHosts::remove(const QString &line)
{
    const QByteArray removed = line.toLatin1();
    if (removed.isEmpty())
        return;

    QFile file(SshSession::knownHostsPath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    QByteArray kept;
    bool found = false;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine();
        if (!found && line == removed)
            found = true;
        else
            kept.append(line);
    }
    file.close();
    QSaveFile saved(file.fileName());
    if (found && saved.open(QIODevice::WriteOnly)) {
        saved.write(kept);
        saved.commit();
    }
    reload();
}

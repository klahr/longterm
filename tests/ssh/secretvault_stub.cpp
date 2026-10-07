// Secrets are files named by their id in the folder LONGTERM_TEST_SECRETS
#include "secretvault.h"

#include <QFile>

static QString secretPath(const QString &secretId)
{
    return QString::fromLocal8Bit(qgetenv("LONGTERM_TEST_SECRETS")) + QLatin1Char('/') + secretId;
}

SecretVault::SecretVault(QObject *parent)
    : QObject(parent)
    , m_pendingRequests(0)
{
}

void SecretVault::store(const QString &secretId, const QByteArray &data, QObject *, const StoreCallback &callback)
{
    QFile file(secretPath(secretId));
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
        callback(file.errorString());
        return;
    }
    file.close();
    callback(QString());
}

void SecretVault::fetch(const QString &secretId, QObject *, const FetchCallback &callback)
{
    QFile file(secretPath(secretId));
    if (!file.open(QIODevice::ReadOnly)) {
        callback(QByteArray(), file.errorString());
        return;
    }
    callback(file.readAll(), QString());
}

void SecretVault::remove(const QString &secretId, QObject *, const StoreCallback &callback)
{
    QFile::remove(secretPath(secretId));
    callback(QString());
}

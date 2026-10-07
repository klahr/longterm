// Secrets are files named by their id in the folder LONGTERM_TEST_SECRETS
#include "secretvault.h"

#include <QFile>

SecretVault::SecretVault(QObject *parent)
    : QObject(parent)
    , m_pendingRequests(0)
{
}

void SecretVault::store(const QString &, const QByteArray &, QObject *, const StoreCallback &callback)
{
    callback(QStringLiteral("no vault in tests"));
}

void SecretVault::fetch(const QString &secretId, QObject *, const FetchCallback &callback)
{
    QFile file(QString::fromLocal8Bit(qgetenv("LONGTERM_TEST_SECRETS")) + QLatin1Char('/') + secretId);
    if (!file.open(QIODevice::ReadOnly)) {
        callback(QByteArray(), file.errorString());
        return;
    }
    callback(file.readAll(), QString());
}

void SecretVault::remove(const QString &, QObject *, const StoreCallback &callback)
{
    callback(QString());
}

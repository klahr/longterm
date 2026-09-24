#ifndef SECRETVAULT_H
#define SECRETVAULT_H

#include <QByteArray>
#include <QObject>
#include <QString>

#include <Secrets/secretmanager.h>

#include <functional>

// Stores blobs in an app-private, device-lock protected Sailfish Secrets collection
class SecretVault : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    typedef std::function<void(const QString &error)> StoreCallback;
    typedef std::function<void(const QByteArray &data, const QString &error)> FetchCallback;

    explicit SecretVault(QObject *parent = nullptr);

    bool busy() const { return m_pendingRequests > 0; }

    // Callbacks are not invoked if context is destroyed first
    void store(const QString &secretId, const QByteArray &data, QObject *context, const StoreCallback &callback);
    void fetch(const QString &secretId, QObject *context, const FetchCallback &callback);
    void remove(const QString &secretId, QObject *context, const StoreCallback &callback);

signals:
    void busyChanged();

private:
    void beginRequest();
    void endRequest();

    Sailfish::Secrets::SecretManager m_secretManager;
    int m_pendingRequests;
};

#endif // SECRETVAULT_H

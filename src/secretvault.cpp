#include "secretvault.h"

#include <QPointer>

#include <Secrets/createcollectionrequest.h>
#include <Secrets/deletesecretrequest.h>
#include <Secrets/result.h>
#include <Secrets/secret.h>
#include <Secrets/storedsecretrequest.h>
#include <Secrets/storesecretrequest.h>

using namespace Sailfish::Secrets;

static const QString CollectionName = QStringLiteral("longtermkeys");

static Secret::Identifier secretIdentifier(const QString &secretId)
{
    return Secret::Identifier(secretId, CollectionName, SecretManager::DefaultEncryptedStoragePluginName);
}

SecretVault::SecretVault(QObject *parent)
    : QObject(parent)
    , m_pendingRequests(0)
{
}

void SecretVault::store(const QString &secretId, const QByteArray &data, QObject *context,
                        const StoreCallback &callback)
{
    QPointer<QObject> guard(context);

    Secret secret(secretIdentifier(secretId));
    secret.setType(Secret::TypeBlob);
    secret.setData(data);

    StoreSecretRequest *storeRequest = new StoreSecretRequest(this);
    storeRequest->setManager(&m_secretManager);
    storeRequest->setSecretStorageType(StoreSecretRequest::CollectionSecret);
    storeRequest->setUserInteractionMode(SecretManager::SystemInteraction);
    storeRequest->setSecret(secret);
    connect(storeRequest, &Request::statusChanged, this, [this, storeRequest, guard, callback]() {
        if (storeRequest->status() != Request::Finished)
            return;
        storeRequest->deleteLater();
        endRequest();
        if (!guard)
            return;
        if (storeRequest->result().code() == Result::Succeeded)
            callback(QString());
        else
            callback(storeRequest->result().errorMessage());
    });

    // The collection is created on first use, an existing one is fine
    CreateCollectionRequest *collectionRequest = new CreateCollectionRequest(this);
    collectionRequest->setManager(&m_secretManager);
    collectionRequest->setCollectionName(CollectionName);
    collectionRequest->setCollectionLockType(CreateCollectionRequest::DeviceLock);
    collectionRequest->setDeviceLockUnlockSemantic(SecretManager::DeviceLockKeepUnlocked);
    collectionRequest->setAccessControlMode(SecretManager::OwnerOnlyMode);
    collectionRequest->setStoragePluginName(SecretManager::DefaultEncryptedStoragePluginName);
    collectionRequest->setEncryptionPluginName(SecretManager::DefaultEncryptedStoragePluginName);
    collectionRequest->setAuthenticationPluginName(SecretManager::DefaultAuthenticationPluginName);
    collectionRequest->setUserInteractionMode(SecretManager::SystemInteraction);
    connect(collectionRequest, &Request::statusChanged, this, [this, collectionRequest, storeRequest, guard, callback]() {
        if (collectionRequest->status() != Request::Finished)
            return;
        collectionRequest->deleteLater();
        const Result result = collectionRequest->result();
        if (result.code() == Result::Succeeded || result.errorCode() == Result::CollectionAlreadyExistsError) {
            storeRequest->startRequest();
            return;
        }
        storeRequest->deleteLater();
        endRequest();
        if (guard)
            callback(result.errorMessage());
    });

    beginRequest();
    collectionRequest->startRequest();
}

void SecretVault::fetch(const QString &secretId, QObject *context, const FetchCallback &callback)
{
    QPointer<QObject> guard(context);
    StoredSecretRequest *request = new StoredSecretRequest(this);
    request->setManager(&m_secretManager);
    request->setIdentifier(secretIdentifier(secretId));
    request->setUserInteractionMode(SecretManager::SystemInteraction);
    connect(request, &Request::statusChanged, this, [this, request, guard, callback]() {
        if (request->status() != Request::Finished)
            return;
        request->deleteLater();
        endRequest();
        if (!guard)
            return;
        if (request->result().code() == Result::Succeeded)
            callback(request->secret().data(), QString());
        else
            callback(QByteArray(), request->result().errorMessage());
    });
    beginRequest();
    request->startRequest();
}

void SecretVault::remove(const QString &secretId, QObject *context, const StoreCallback &callback)
{
    QPointer<QObject> guard(context);
    DeleteSecretRequest *request = new DeleteSecretRequest(this);
    request->setManager(&m_secretManager);
    request->setIdentifier(secretIdentifier(secretId));
    request->setUserInteractionMode(SecretManager::SystemInteraction);
    connect(request, &Request::statusChanged, this, [this, request, guard, callback]() {
        if (request->status() != Request::Finished)
            return;
        request->deleteLater();
        endRequest();
        if (!guard)
            return;
        if (request->result().code() == Result::Succeeded)
            callback(QString());
        else
            callback(request->result().errorMessage());
    });
    beginRequest();
    request->startRequest();
}

void SecretVault::beginRequest()
{
    if (m_pendingRequests++ == 0)
        emit busyChanged();
}

void SecretVault::endRequest()
{
    if (--m_pendingRequests == 0)
        emit busyChanged();
}

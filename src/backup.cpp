#include "backup.h"

#include <QDate>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QtConcurrent>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include "appsettings.h"
#include "colorschemes.h"
#include "hoststore.h"
#include "keystore.h"
#include "secretvault.h"

// "LTBACKUP1", salt, nonce, GCM tag, then the encrypted JSON
static const QByteArray Magic("LTBACKUP1");
static const int SaltSize = 16;
static const int NonceSize = 12;
static const int TagSize = 16;
// Slows down guessing the passphrase of a copied file, a second or so on a phone
static const int KdfIterations = 300000;
static const int MaxBackupSize = 64 * 1024 * 1024;

namespace {

QByteArray deriveKey(const QByteArray &passphrase, const QByteArray &salt)
{
    QByteArray key(32, Qt::Uninitialized);
    if (PKCS5_PBKDF2_HMAC(passphrase.constData(), passphrase.size(),
                          reinterpret_cast<const unsigned char *>(salt.constData()), salt.size(), KdfIterations,
                          EVP_sha256(), key.size(), reinterpret_cast<unsigned char *>(key.data())) != 1)
        return QByteArray();
    return key;
}

// Empty when it failed
QByteArray encrypt(const QByteArray &plaintext, const QByteArray &passphrase)
{
    QByteArray salt(SaltSize, Qt::Uninitialized);
    QByteArray nonce(NonceSize, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(salt.data()), SaltSize) != 1
            || RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), NonceSize) != 1)
        return QByteArray();
    QByteArray key = deriveKey(passphrase, salt);
    if (key.isEmpty())
        return QByteArray();
    QByteArray ciphertext(plaintext.size() + 16, Qt::Uninitialized);
    QByteArray tag(TagSize, Qt::Uninitialized);
    int length = 0;
    int finalLength = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    const bool ok = ctx && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
            && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, NonceSize, nullptr) == 1
            && EVP_EncryptInit_ex(ctx, nullptr, nullptr, reinterpret_cast<const unsigned char *>(key.constData()),
                                  reinterpret_cast<const unsigned char *>(nonce.constData())) == 1
            && EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(ciphertext.data()), &length,
                                 reinterpret_cast<const unsigned char *>(plaintext.constData()), plaintext.size()) == 1
            && EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(ciphertext.data()) + length, &finalLength) == 1
            && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TagSize, tag.data()) == 1;
    EVP_CIPHER_CTX_free(ctx);
    key.fill('\0');
    if (!ok)
        return QByteArray();
    ciphertext.truncate(length + finalLength);
    return Magic + salt + nonce + tag + ciphertext;
}

// Empty with error set when the file is no backup or the passphrase is wrong
QByteArray decrypt(const QByteArray &data, const QByteArray &passphrase, QString *error)
{
    const int header = Magic.size() + SaltSize + NonceSize + TagSize;
    if (!data.startsWith(Magic) || data.size() < header) {
        *error = Backup::tr("This is not a Longterm backup");
        return QByteArray();
    }
    const QByteArray salt = data.mid(Magic.size(), SaltSize);
    const QByteArray nonce = data.mid(Magic.size() + SaltSize, NonceSize);
    QByteArray tag = data.mid(Magic.size() + SaltSize + NonceSize, TagSize);
    const QByteArray ciphertext = data.mid(header);
    QByteArray key = deriveKey(passphrase, salt);
    QByteArray plaintext(ciphertext.size() + 16, Qt::Uninitialized);
    int length = 0;
    int finalLength = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    const bool ok = ctx && !key.isEmpty() && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
            && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, NonceSize, nullptr) == 1
            && EVP_DecryptInit_ex(ctx, nullptr, nullptr, reinterpret_cast<const unsigned char *>(key.constData()),
                                  reinterpret_cast<const unsigned char *>(nonce.constData())) == 1
            && EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(plaintext.data()), &length,
                                 reinterpret_cast<const unsigned char *>(ciphertext.constData()), ciphertext.size()) == 1
            && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TagSize, tag.data()) == 1
            && EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(plaintext.data()) + length, &finalLength) == 1;
    EVP_CIPHER_CTX_free(ctx);
    key.fill('\0');
    if (!ok) {
        plaintext.fill('\0');
        *error = Backup::tr("Wrong passphrase, or the file is damaged");
        return QByteArray();
    }
    plaintext.truncate(length + finalLength);
    return plaintext;
}

}

Backup::Backup(SecretVault *vault, HostStore *hosts, KeyStore *keys, AppSettings *settings, ColorSchemes *schemes,
               QObject *parent)
    : QObject(parent)
    , m_vault(vault)
    , m_hosts(hosts)
    , m_keys(keys)
    , m_settings(settings)
    , m_schemes(schemes)
    , m_busy(false)
{
}

void Backup::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

void Backup::fetchSecrets(QStringList ids, QVariantMap fetched, const std::function<void(const QVariantMap &)> &done)
{
    if (ids.isEmpty()) {
        done(fetched);
        return;
    }
    const QString id = ids.takeFirst();
    m_vault->fetch(id, this, [this, id, ids, fetched, done](const QByteArray &secret, const QString &error) mutable {
        // A secret that cannot be read is left out, the rest still goes in
        if (error.isEmpty())
            fetched.insert(id, QString::fromLatin1(secret.toBase64()));
        fetchSecrets(ids, fetched, done);
    });
}

void Backup::storeSecrets(QVariantMap secrets, const std::function<void()> &done)
{
    if (secrets.isEmpty()) {
        done();
        return;
    }
    const QString id = secrets.firstKey();
    const QByteArray secret = QByteArray::fromBase64(secrets.take(id).toString().toLatin1());
    m_vault->store(id, secret, this, [this, secrets, done](const QString &) {
        storeSecrets(secrets, done);
    });
}

void Backup::exportBackup(const QString &passphrase)
{
    if (m_busy)
        return;
    if (passphrase.isEmpty()) {
        emit exported(QString(), tr("A backup needs a passphrase"));
        return;
    }
    setBusy(true);
    const QVariantList hosts = m_hosts->exportHosts();
    const QVariantList keys = m_keys->exportKeys();
    QStringList secretIds;
    for (const QVariant &key : keys)
        secretIds.append(key.toMap().value(QStringLiteral("id")).toString());
    for (const QVariant &host : hosts) {
        if (host.toMap().value(QStringLiteral("hasPassword")).toBool())
            secretIds.append(HostStore::passwordSecretId(host.toMap().value(QStringLiteral("id")).toString()));
    }
    fetchSecrets(secretIds, QVariantMap(), [this, hosts, keys, passphrase](const QVariantMap &secrets) {
        QVariantMap contents;
        contents.insert(QStringLiteral("version"), 1);
        contents.insert(QStringLiteral("hosts"), hosts);
        contents.insert(QStringLiteral("keys"), keys);
        contents.insert(QStringLiteral("secrets"), secrets);
        contents.insert(QStringLiteral("settings"), m_settings->exportSettings());
        contents.insert(QStringLiteral("colorSchemes"), m_schemes->exportSchemes());
        const QByteArray json = QJsonDocument::fromVariant(contents).toJson(QJsonDocument::Compact);
        const QByteArray passphraseData = passphrase.toUtf8();

        QFutureWatcher<QByteArray> *watcher = new QFutureWatcher<QByteArray>(this);
        connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [this, watcher]() {
            const QByteArray data = watcher->result();
            watcher->deleteLater();
            setBusy(false);
            if (data.isEmpty()) {
                emit exported(QString(), tr("Could not encrypt the backup"));
                return;
            }
            const QString directory = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                    + QStringLiteral("/Longterm");
            QDir().mkpath(directory);
            QString path = QStringLiteral("%1/longterm-%2.ltbackup").arg(directory, QDate::currentDate().toString(Qt::ISODate));
            for (int i = 2; QFile::exists(path); ++i)
                path = QStringLiteral("%1/longterm-%2-%3.ltbackup").arg(directory, QDate::currentDate().toString(Qt::ISODate)).arg(i);
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
                emit exported(QString(), tr("Could not save the backup: %1").arg(file.errorString()));
                return;
            }
            emit exported(path, QString());
        });
        watcher->setFuture(QtConcurrent::run([json, passphraseData]() {
            return encrypt(json, passphraseData);
        }));
    });
}

void Backup::importBackup(const QString &path, const QString &passphrase)
{
    if (m_busy)
        return;
    const QString local = path.startsWith(QLatin1String("file://")) ? QUrl(path).toLocalFile() : path;
    QFile file(local);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaxBackupSize) {
        emit imported(QString(), tr("Could not read %1").arg(local));
        return;
    }
    const QByteArray data = file.readAll();
    const QByteArray passphraseData = passphrase.toUtf8();
    setBusy(true);

    struct Result {
        QByteArray json;
        QString error;
    };
    QFutureWatcher<Result> *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher]() {
        Result result = watcher->result();
        watcher->deleteLater();
        if (!result.error.isEmpty()) {
            setBusy(false);
            emit imported(QString(), result.error);
            return;
        }
        const QVariantMap contents = QJsonDocument::fromJson(result.json).toVariant().toMap();
        result.json.fill('\0');
        if (contents.value(QStringLiteral("version")).toInt() != 1) {
            setBusy(false);
            emit imported(QString(), tr("The backup is from a newer version of Longterm"));
            return;
        }
        const QVariantList hosts = contents.value(QStringLiteral("hosts")).toList();
        const QVariantList keys = contents.value(QStringLiteral("keys")).toList();
        const QVariantMap secrets = contents.value(QStringLiteral("secrets")).toMap();

        // Keys go in once their private part is stored, host passwords before their hosts
        int keysAdded = 0;
        for (const QVariant &key : keys) {
            const QVariantMap map = key.toMap();
            const QString id = map.value(QStringLiteral("id")).toString();
            if (secrets.contains(id) && m_keys->indexOf(id) < 0) {
                m_keys->restoreKey(map, QByteArray::fromBase64(secrets.value(id).toString().toLatin1()));
                ++keysAdded;
            }
        }
        QVariantMap passwords;
        for (const QVariant &host : hosts) {
            const QString id = HostStore::passwordSecretId(host.toMap().value(QStringLiteral("id")).toString());
            if (secrets.contains(id))
                passwords.insert(id, secrets.value(id));
        }
        m_settings->restoreSettings(contents.value(QStringLiteral("settings")).toMap());
        m_schemes->restoreSchemes(contents.value(QStringLiteral("colorSchemes")).toList());
        const int hostCount = hosts.size();
        storeSecrets(passwords, [this, hosts, passwords, hostCount, keysAdded]() {
            for (const QVariant &host : hosts) {
                const QVariantMap map = host.toMap();
                m_hosts->restoreHost(map, passwords.contains(HostStore::passwordSecretId(map.value(QStringLiteral("id")).toString())));
            }
            setBusy(false);
            emit imported(tr("Restored %n host(s)", "", hostCount) + QStringLiteral(", ")
                          + tr("%n new key(s)", "", keysAdded), QString());
        });
    });
    watcher->setFuture(QtConcurrent::run([data, passphraseData]() {
        Result result;
        result.json = decrypt(data, passphraseData, &result.error);
        return result;
    }));
}

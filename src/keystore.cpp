#include "keystore.h"

#include <QFutureWatcher>
#include <QRegExp>
#include <QStandardPaths>
#include <QUuid>
#include <QtConcurrent>

#include <libssh/libssh.h>

#include <algorithm>

#include "secretvault.h"

namespace {

bool readString(const QByteArray &data, int *pos, QByteArray *value)
{
    if (data.size() - *pos < 4)
        return false;
    const uchar *p = reinterpret_cast<const uchar *>(data.constData()) + *pos;
    const quint32 length = quint32(p[0]) << 24 | quint32(p[1]) << 16 | quint32(p[2]) << 8 | quint32(p[3]);
    if (quint32(data.size() - *pos - 4) < length)
        return false;
    *value = data.mid(*pos + 4, int(length));
    *pos += 4 + int(length);
    return true;
}

// The OpenSSH format keeps the public key readable in front of the encrypted part
bool openSshPublicKey(const QByteArray &privateKey, QByteArray *publicKeyBlob, bool *encrypted)
{
    static const QByteArray begin("-----BEGIN OPENSSH PRIVATE KEY-----");
    static const QByteArray end("-----END OPENSSH PRIVATE KEY-----");
    const int start = privateKey.indexOf(begin);
    const int stop = privateKey.indexOf(end);
    if (start < 0 || stop < start)
        return false;
    const QByteArray data = QByteArray::fromBase64(privateKey.mid(start + begin.size(), stop - start - begin.size()).simplified()
                                                   .replace(' ', QByteArray()));
    static const QByteArray magic("openssh-key-v1", 15);
    if (!data.startsWith(magic))
        return false;
    int pos = magic.size();
    QByteArray cipher;
    QByteArray kdf;
    QByteArray kdfOptions;
    if (!readString(data, &pos, &cipher) || !readString(data, &pos, &kdf) || !readString(data, &pos, &kdfOptions)
            || data.size() - pos < 4)
        return false;
    pos += 4; // number of keys, always one
    if (!readString(data, &pos, publicKeyBlob))
        return false;
    if (encrypted)
        *encrypted = cipher != "none";
    return true;
}

// The first field of a public key blob
QByteArray keyTypeName(const QByteArray &publicKeyBlob)
{
    int pos = 0;
    QByteArray name;
    readString(publicKeyBlob, &pos, &name);
    return name;
}

}

KeyStore::KeyStore(SecretVault *vault, QObject *parent)
    : QAbstractListModel(parent)
    , m_vault(vault)
    , m_generating(0)
    // Only public data lives here, private keys are kept in Sailfish Secrets
    , m_index(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
              + QStringLiteral("/keys.conf"), QSettings::IniFormat)
{
    loadIndex();
    connect(m_vault, &SecretVault::busyChanged, this, &KeyStore::busyChanged);
}

bool KeyStore::busy() const
{
    return m_vault->busy() || m_generating > 0;
}

int KeyStore::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_keys.size();
}

QVariant KeyStore::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_keys.size())
        return QVariant();
    const Key &key = m_keys.at(index.row());
    switch (role) {
    case KeyIdRole: return key.id;
    case NameRole: return key.name;
    case PublicKeyRole: return key.publicKey;
    case FingerprintRole: return key.fingerprint;
    case EncryptedRole: return key.encrypted;
    default: return QVariant();
    }
}

QHash<int, QByteArray> KeyStore::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[KeyIdRole] = "keyId";
    roles[NameRole] = "name";
    roles[PublicKeyRole] = "publicKey";
    roles[FingerprintRole] = "fingerprint";
    roles[EncryptedRole] = "encrypted";
    return roles;
}

QString KeyStore::keyIdAt(int row) const
{
    return row >= 0 && row < m_keys.size() ? m_keys.at(row).id : QString();
}

int KeyStore::indexOf(const QString &keyId) const
{
    for (int row = 0; row < m_keys.size(); ++row) {
        if (m_keys.at(row).id == keyId)
            return row;
    }
    return -1;
}

QString KeyStore::validatePrivateKey(const QString &privateKey, const QString &passphrase) const
{
    if (privateKey.trimmed().isEmpty())
        return tr("Paste a private key");
    if (passphrase.isEmpty() && isEncrypted(privateKey.toUtf8())) {
        QByteArray blob;
        return openSshPublicKey(privateKey.toUtf8(), &blob, nullptr) ? QString()
                                                                     : tr("This older key format needs its passphrase to be imported");
    }
    ssh_key key = nullptr;
    const QByteArray passphraseData = passphrase.toUtf8();
    if (ssh_pki_import_privkey_base64(privateKey.trimmed().toUtf8().constData(),
                                      passphrase.isEmpty() ? nullptr : passphraseData.constData(),
                                      nullptr, nullptr, &key) != SSH_OK) {
        return passphrase.isEmpty() ? tr("Not a valid private key, or it needs a passphrase")
                                    : tr("Not a valid private key, or wrong passphrase");
    }
    ssh_key_free(key);
    return QString();
}

void KeyStore::generateKey(const QString &name, const QString &type)
{
    enum ssh_keytypes_e keyType = SSH_KEYTYPE_ED25519;
    if (type == QLatin1String("ecdsa"))
        keyType = SSH_KEYTYPE_ECDSA_P256;
    else if (type == QLatin1String("rsa"))
        keyType = SSH_KEYTYPE_RSA;

    ++m_generating;
    emit busyChanged();
    // The key crosses threads as a plain pointer, it is freed on this one
    QFutureWatcher<ssh_key> *watcher = new QFutureWatcher<ssh_key>(this);
    connect(watcher, &QFutureWatcher<ssh_key>::finished, this, [this, watcher, name]() {
        ssh_key key = watcher->result();
        watcher->deleteLater();
        --m_generating;
        emit busyChanged();
        if (!key) {
            setError(tr("Could not generate key"));
            return;
        }
        addKey(name, key);
        ssh_key_free(key);
    });
    watcher->setFuture(QtConcurrent::run([keyType]() -> ssh_key {
        ssh_pki_ctx context = ssh_pki_ctx_new();
        if (!context)
            return nullptr;
        // OpenSSH's default size
        int bits = 3072;
        ssh_key key = nullptr;
        if ((keyType == SSH_KEYTYPE_RSA && ssh_pki_ctx_options_set(context, SSH_PKI_OPTION_RSA_KEY_SIZE, &bits) != SSH_OK)
                || ssh_pki_generate_key(keyType, context, &key) != SSH_OK) {
            key = nullptr;
        }
        ssh_pki_ctx_free(context);
        return key;
    }));
}

bool KeyStore::isEncrypted(const QByteArray &privateKey)
{
    QByteArray blob;
    bool encrypted = false;
    if (openSshPublicKey(privateKey, &blob, &encrypted))
        return encrypted;
    // PEM, "Proc-Type: 4,ENCRYPTED" or "BEGIN ENCRYPTED PRIVATE KEY"
    return privateKey.contains("ENCRYPTED");
}

void KeyStore::importKey(const QString &name, const QString &privateKey, const QString &passphrase)
{
    if (passphrase.isEmpty() && isEncrypted(privateKey.toUtf8())) {
        addEncryptedKey(name, privateKey);
        return;
    }
    ssh_key key = nullptr;
    const QByteArray passphraseData = passphrase.toUtf8();
    if (ssh_pki_import_privkey_base64(privateKey.trimmed().toUtf8().constData(),
                                      passphrase.isEmpty() ? nullptr : passphraseData.constData(),
                                      nullptr, nullptr, &key) != SSH_OK) {
        setError(tr("Could not read private key"));
        return;
    }
    // Stored without the passphrase, Sailfish Secrets encrypts it instead
    addKey(name, key);
    ssh_key_free(key);
}

void KeyStore::removeKey(const QString &keyId)
{
    for (int row = 0; row < m_keys.size(); ++row) {
        if (m_keys.at(row).id != keyId)
            continue;
        beginRemoveRows(QModelIndex(), row, row);
        m_keys.removeAt(row);
        endRemoveRows();
        saveIndex();
        emit countChanged();
        break;
    }
    emit keyRemoved(keyId);

    m_vault->remove(keyId, this, [this](const QString &error) {
        if (!error.isEmpty())
            setError(tr("Could not delete private key: %1").arg(error));
    });
}

void KeyStore::exportPrivateKey(const QString &keyId, const QString &passphrase)
{
    m_vault->fetch(keyId, this, [this, keyId, passphrase](const QByteArray &secret, const QString &error) {
        if (!error.isEmpty()) {
            setError(tr("Could not read private key: %1").arg(error));
            return;
        }
        if (isEncrypted(secret)) {
            setError(QString());
            emit privateKeyExported(keyId, QString::fromLatin1(secret));
            return;
        }
        ssh_key key = nullptr;
        char *exported = nullptr;
        const QByteArray passphraseData = passphrase.toUtf8();
        const bool ok = ssh_pki_import_privkey_base64(secret.constData(), nullptr, nullptr, nullptr, &key) == SSH_OK
                && ssh_pki_export_privkey_base64(key, passphrase.isEmpty() ? nullptr : passphraseData.constData(),
                                                 nullptr, nullptr, &exported) == SSH_OK;
        ssh_key_free(key);
        if (!ok) {
            ssh_string_free_char(exported);
            setError(tr("Could not export key"));
            return;
        }
        setError(QString());
        const QString privateKey = QString::fromLatin1(exported);
        std::fill(exported, exported + qstrlen(exported), '\0');
        ssh_string_free_char(exported);
        emit privateKeyExported(keyId, privateKey);
    });
}

void KeyStore::loadIndex()
{
    const int size = m_index.beginReadArray(QStringLiteral("keys"));
    for (int i = 0; i < size; ++i) {
        m_index.setArrayIndex(i);
        Key key;
        key.id = m_index.value(QStringLiteral("id")).toString();
        key.name = m_index.value(QStringLiteral("name")).toString();
        key.publicKey = m_index.value(QStringLiteral("publicKey")).toString();
        key.fingerprint = m_index.value(QStringLiteral("fingerprint")).toString();
        key.encrypted = m_index.value(QStringLiteral("encrypted"), false).toBool();
        m_keys.append(key);
    }
    m_index.endArray();
}

void KeyStore::saveIndex()
{
    m_index.remove(QStringLiteral("keys"));
    m_index.beginWriteArray(QStringLiteral("keys"), m_keys.size());
    for (int i = 0; i < m_keys.size(); ++i) {
        m_index.setArrayIndex(i);
        m_index.setValue(QStringLiteral("id"), m_keys.at(i).id);
        m_index.setValue(QStringLiteral("name"), m_keys.at(i).name);
        m_index.setValue(QStringLiteral("publicKey"), m_keys.at(i).publicKey);
        m_index.setValue(QStringLiteral("fingerprint"), m_keys.at(i).fingerprint);
        m_index.setValue(QStringLiteral("encrypted"), m_keys.at(i).encrypted);
    }
    m_index.endArray();
    m_index.sync();
}

void KeyStore::addKey(const QString &name, ssh_key_struct *sshKey)
{
    Key key;
    char *privateBase64 = nullptr;
    if (!describeKey(name, sshKey, &key)
            || ssh_pki_export_privkey_base64(sshKey, nullptr, nullptr, nullptr, &privateBase64) != SSH_OK) {
        ssh_string_free_char(privateBase64);
        setError(tr("Could not export key"));
        return;
    }
    key.encrypted = false;
    QByteArray privateKey(privateBase64);
    std::fill(privateBase64, privateBase64 + qstrlen(privateBase64), '\0');
    ssh_string_free_char(privateBase64);
    storeKey(key, privateKey);
}

void KeyStore::addEncryptedKey(const QString &name, const QString &privateKey)
{
    QByteArray blob;
    ssh_key publicKey = nullptr;
    Key key;
    const bool ok = openSshPublicKey(privateKey.toUtf8(), &blob, nullptr)
            && ssh_pki_import_pubkey_base64(blob.toBase64().constData(), ssh_key_type_from_name(keyTypeName(blob).constData()),
                                            &publicKey) == SSH_OK
            && describeKey(name, publicKey, &key);
    ssh_key_free(publicKey);
    if (!ok) {
        setError(tr("Could not read private key"));
        return;
    }
    key.encrypted = true;
    storeKey(key, privateKey.trimmed().toUtf8());
}

bool KeyStore::describeKey(const QString &name, ssh_key_struct *sshKey, Key *key) const
{
    char *publicBase64 = nullptr;
    unsigned char *hash = nullptr;
    size_t hashLength = 0;
    if (ssh_pki_export_pubkey_base64(sshKey, &publicBase64) != SSH_OK
            || ssh_get_publickey_hash(sshKey, SSH_PUBLICKEY_HASH_SHA256, &hash, &hashLength) != SSH_OK) {
        ssh_string_free_char(publicBase64);
        return false;
    }
    char *fingerprint = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hashLength);
    ssh_clean_pubkey_hash(&hash);

    key->id = QUuid::createUuid().toString().remove(QRegExp(QStringLiteral("[{}-]")));
    key->name = name.trimmed();
    key->publicKey = QStringLiteral("%1 %2 %3").arg(QString::fromLatin1(ssh_key_type_to_char(ssh_key_type(sshKey))),
                                                    QString::fromLatin1(publicBase64), key->name);
    key->fingerprint = QString::fromLatin1(fingerprint);
    ssh_string_free_char(fingerprint);
    ssh_string_free_char(publicBase64);
    return true;
}

void KeyStore::storeKey(const Key &key, QByteArray privateKey)
{
    setError(QString());
    m_vault->store(key.id, privateKey, this, [this, key](const QString &error) {
        if (!error.isEmpty()) {
            setError(tr("Could not store private key: %1").arg(error));
            return;
        }
        beginInsertRows(QModelIndex(), m_keys.size(), m_keys.size());
        m_keys.append(key);
        endInsertRows();
        saveIndex();
        emit countChanged();
    });
    privateKey.fill('\0');
}

void KeyStore::setError(const QString &message)
{
    if (m_errorString == message)
        return;
    m_errorString = message;
    emit errorStringChanged();
}

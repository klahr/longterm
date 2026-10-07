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

// bcrypt rounds make a passphrase slower to guess, and slower to check on
// every connect. OpenSSH uses 16 by default, ssh-keygen -a raises it.
const quint32 MaxKdfRounds = 1000;

// A damaged index can claim any number of entries
const int MaxKeys = 1000;

// The OpenSSH format keeps the public key readable in front of the encrypted part
bool openSshPublicKey(const QByteArray &privateKey, QByteArray *publicKeyBlob, bool *encrypted,
                      quint32 *kdfRounds = nullptr, QByteArray *cipherName = nullptr, QByteArray *kdfName = nullptr)
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
    if (cipherName)
        *cipherName = cipher;
    if (kdfName)
        *kdfName = kdf;
    if (kdfRounds) {
        // kdfoptions for bcrypt are the salt followed by the rounds
        int optionsPos = 0;
        QByteArray salt;
        *kdfRounds = 0;
        if (readString(kdfOptions, &optionsPos, &salt) && kdfOptions.size() - optionsPos >= 4) {
            const uchar *p = reinterpret_cast<const uchar *>(kdfOptions.constData()) + optionsPos;
            *kdfRounds = quint32(p[0]) << 24 | quint32(p[1]) << 16 | quint32(p[2]) << 8 | quint32(p[3]);
        }
    }
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

bool isSecurityKey(const char *typeName)
{
    return typeName && qstrncmp(typeName, "sk-", 3) == 0;
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

KeyStore::~KeyStore()
{
    for (QFutureWatcher<ssh_key> *watcher : findChildren<QFutureWatcher<ssh_key> *>(QString(), Qt::FindDirectChildrenOnly)) {
        watcher->disconnect(this);
        watcher->waitForFinished();
        ssh_key_free(watcher->result());
    }
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
    case CertificateRole: return key.certificate;
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
    const QByteArray data = privateKey.toUtf8();
    QByteArray blob;
    bool encrypted = false;
    const QString problem = encryptionProblem(data);
    if (!problem.isEmpty())
        return problem;
    if (openSshPublicKey(data, &blob, &encrypted) && isSecurityKey(keyTypeName(blob).constData()))
        return tr("Security keys (FIDO) are not supported");
    if (encrypted) {
        // Checking the passphrase takes a noticeable moment, so it is left to
        // importKey instead of being done for every character typed
        return QString();
    }
    if (passphrase.isEmpty() && isEncrypted(data))
        return tr("This older key format needs its passphrase to be imported");
    ssh_key key = nullptr;
    const QByteArray passphraseData = passphrase.toUtf8();
    if (ssh_pki_import_privkey_base64(privateKey.trimmed().toUtf8().constData(),
                                      passphrase.isEmpty() ? nullptr : passphraseData.constData(),
                                      nullptr, nullptr, &key) != SSH_OK) {
        return passphrase.isEmpty() ? tr("Not a valid private key, or it needs a passphrase")
                                    : tr("Not a valid private key, or wrong passphrase");
    }
    const bool securityKey = isSecurityKey(ssh_key_type_to_char(ssh_key_type(key)));
    ssh_key_free(key);
    return securityKey ? tr("Security keys (FIDO) are not supported") : QString();
}

void KeyStore::generateKey(const QString &name, const QString &type)
{
    enum ssh_keytypes_e keyType = SSH_KEYTYPE_ED25519;
    if (type == QLatin1String("ecdsa")) {
        keyType = SSH_KEYTYPE_ECDSA_P256;
    } else if (type == QLatin1String("rsa")) {
        keyType = SSH_KEYTYPE_RSA;
    } else if (type != QLatin1String("ed25519")) {
        setError(tr("Unknown key type %1").arg(type));
        return;
    }
    if (m_keys.size() >= MaxKeys) {
        setError(tr("Too many keys, %1 can be kept").arg(MaxKeys));
        return;
    }

    ++m_generating;
    emit busyChanged();
    // The key crosses threads as a plain pointer, it is freed on this one
    QFutureWatcher<ssh_key> *watcher = new QFutureWatcher<ssh_key>(this);
    connect(watcher, &QFutureWatcher<ssh_key>::finished, this, [this, watcher, name]() {
        ssh_key key = watcher->result();
        watcher->setParent(nullptr);
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

QString KeyStore::encryptionProblem(const QByteArray &privateKey)
{
    QByteArray blob;
    bool encrypted = false;
    quint32 rounds = 0;
    QByteArray cipher;
    QByteArray kdf;
    if (!openSshPublicKey(privateKey, &blob, &encrypted, &rounds, &cipher, &kdf) || !encrypted)
        return QString();
    static const QList<QByteArray> ciphers = {
        "aes128-ctr", "aes192-ctr", "aes256-ctr", "aes128-cbc", "aes192-cbc", "aes256-cbc", "3des-cbc"
    };
    if (kdf != "bcrypt")
        return tr("The key's encryption settings cannot be read");
    if (!ciphers.contains(cipher))
        return tr("Keys encrypted with %1 are not supported, re-encrypt it with ssh-keygen -p -Z aes256-ctr")
                .arg(QString::fromLatin1(cipher));
    if (rounds == 0)
        return tr("The key's encryption settings cannot be read");
    if (rounds > MaxKdfRounds)
        return tr("The key's passphrase takes %1 rounds to check, too slow to use on every connect").arg(rounds);
    return QString();
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
    const QString problem = encryptionProblem(privateKey.toUtf8());
    if (!problem.isEmpty()) {
        setError(problem);
        return;
    }
    if (m_keys.size() >= MaxKeys) {
        setError(tr("Too many keys, %1 can be kept").arg(MaxKeys));
        return;
    }
    if (passphrase.isEmpty() && isEncrypted(privateKey.toUtf8())) {
        addEncryptedKey(name, privateKey);
        return;
    }
    ++m_generating;
    emit busyChanged();
    QFutureWatcher<ssh_key> *watcher = new QFutureWatcher<ssh_key>(this);
    connect(watcher, &QFutureWatcher<ssh_key>::finished, this, [this, watcher, name, passphrase]() {
        ssh_key key = watcher->result();
        watcher->setParent(nullptr);
        watcher->deleteLater();
        --m_generating;
        emit busyChanged();
        if (!key) {
            setError(passphrase.isEmpty() ? tr("Could not read private key")
                                          : tr("Wrong passphrase, the key was not imported"));
            return;
        }
        // Stored without the passphrase, Sailfish Secrets encrypts it instead
        addKey(name, key);
        ssh_key_free(key);
    });
    const QByteArray keyData = privateKey.trimmed().toUtf8();
    const QByteArray passphraseData = passphrase.toUtf8();
    watcher->setFuture(QtConcurrent::run([keyData, passphraseData]() -> ssh_key {
        ssh_key key = nullptr;
        if (ssh_pki_import_privkey_base64(keyData.constData(), passphraseData.isEmpty() ? nullptr : passphraseData.constData(),
                                          nullptr, nullptr, &key) != SSH_OK)
            key = nullptr;
        return key;
    }));
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

QString KeyStore::setCertificate(const QString &keyId, const QString &certificate)
{
    const QString problem = checkCertificate(keyId, certificate);
    if (!problem.isEmpty())
        return problem;
    const int row = indexOf(keyId);
    m_keys[row].certificate = certificate.simplified();
    saveIndex();
    emit dataChanged(index(row), index(row), { CertificateRole });
    return QString();
}

QString KeyStore::checkCertificate(const QString &keyId, const QString &certificate) const
{
    const int row = indexOf(keyId);
    if (row < 0)
        return tr("The key is gone");
    const QString text = certificate.simplified();
    if (!text.isEmpty()) {
        const QStringList fields = text.split(QLatin1Char(' '));
        const QStringList keyFields = m_keys.at(row).publicKey.split(QLatin1Char(' '));
        ssh_key cert = nullptr;
        ssh_key publicKey = nullptr;
        const bool parsed = fields.size() >= 2 && fields.at(0).contains(QLatin1String("-cert-v01@openssh.com"))
                && ssh_pki_import_cert_base64(fields.at(1).toLatin1().constData(),
                                              ssh_key_type_from_name(fields.at(0).toLatin1().constData()), &cert) == SSH_OK;
        // A certificate for some other key would never log in
        const bool matches = parsed && keyFields.size() >= 2
                && ssh_pki_import_pubkey_base64(keyFields.at(1).toLatin1().constData(),
                                                ssh_key_type_from_name(keyFields.at(0).toLatin1().constData()),
                                                &publicKey) == SSH_OK
                && ssh_key_cmp(cert, publicKey, SSH_KEY_CMP_PUBLIC) == 0;
        ssh_key_free(cert);
        ssh_key_free(publicKey);
        if (!parsed)
            return tr("This is not an OpenSSH certificate, it is the line in the -cert.pub file");
        if (!matches)
            return tr("The certificate is for another key");
    }
    return QString();
}

QVariantList KeyStore::exportKeys() const
{
    QVariantList list;
    for (const Key &key : m_keys) {
        QVariantMap map;
        map.insert(QStringLiteral("id"), key.id);
        map.insert(QStringLiteral("name"), key.name);
        map.insert(QStringLiteral("publicKey"), key.publicKey);
        map.insert(QStringLiteral("fingerprint"), key.fingerprint);
        map.insert(QStringLiteral("encrypted"), key.encrypted);
        map.insert(QStringLiteral("certificate"), key.certificate);
        list.append(map);
    }
    return list;
}

void KeyStore::restoreKey(const QVariantMap &map, const QByteArray &privateKey)
{
    Key key;
    key.id = map.value(QStringLiteral("id")).toString();
    key.name = map.value(QStringLiteral("name")).toString();
    key.publicKey = map.value(QStringLiteral("publicKey")).toString();
    key.fingerprint = map.value(QStringLiteral("fingerprint")).toString();
    key.encrypted = map.value(QStringLiteral("encrypted")).toBool();
    key.certificate = map.value(QStringLiteral("certificate")).toString();
    if (key.id.isEmpty() || privateKey.isEmpty() || indexOf(key.id) >= 0 || m_keys.size() >= MaxKeys)
        return;
    storeKey(key, privateKey);
}

QByteArray KeyStore::certificate(const QString &keyId) const
{
    const int row = indexOf(keyId);
    return row < 0 ? QByteArray() : m_keys.at(row).certificate.toLatin1();
}

QString KeyStore::publicKey(const QString &keyId) const
{
    const int row = indexOf(keyId);
    return row < 0 ? QString() : m_keys.at(row).publicKey;
}

void KeyStore::loadIndex()
{
    const int size = qMin(m_index.beginReadArray(QStringLiteral("keys")), MaxKeys);
    for (int i = 0; i < size; ++i) {
        m_index.setArrayIndex(i);
        Key key;
        key.id = m_index.value(QStringLiteral("id")).toString();
        key.name = m_index.value(QStringLiteral("name")).toString();
        key.publicKey = m_index.value(QStringLiteral("publicKey")).toString();
        key.fingerprint = m_index.value(QStringLiteral("fingerprint")).toString();
        key.encrypted = m_index.value(QStringLiteral("encrypted"), false).toBool();
        key.certificate = m_index.value(QStringLiteral("certificate")).toString();
        if (key.id.isEmpty() || indexOf(key.id) >= 0)
            continue;
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
        m_index.setValue(QStringLiteral("certificate"), m_keys.at(i).certificate);
    }
    m_index.endArray();
    m_index.sync();
}

void KeyStore::addKey(const QString &name, ssh_key_struct *sshKey)
{
    if (isSecurityKey(ssh_key_type_to_char(ssh_key_type(sshKey)))) {
        setError(tr("Security keys (FIDO) are not supported"));
        return;
    }
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
    if (openSshPublicKey(privateKey.toUtf8(), &blob, nullptr) && isSecurityKey(keyTypeName(blob).constData())) {
        setError(tr("Security keys (FIDO) are not supported"));
        return;
    }
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
    key->name = name.simplified();
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

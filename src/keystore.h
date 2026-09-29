#ifndef KEYSTORE_H
#define KEYSTORE_H

#include <QAbstractListModel>
#include <QList>
#include <QObject>
#include <QSettings>

struct ssh_key_struct;
class SecretVault;

class KeyStore : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)

public:
    enum Roles {
        KeyIdRole = Qt::UserRole + 1,
        NameRole,
        PublicKeyRole,
        FingerprintRole,
        EncryptedRole
    };

    explicit KeyStore(SecretVault *vault, QObject *parent = nullptr);
    ~KeyStore();

    int count() const { return m_keys.size(); }
    bool busy() const;
    QString errorString() const { return m_errorString; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE QString keyIdAt(int row) const;
    Q_INVOKABLE int indexOf(const QString &keyId) const;
    // Returns an empty string for a usable key, otherwise why it is not. An
    // encrypted key without its passphrase is usable, it is asked for on connect.
    Q_INVOKABLE QString validatePrivateKey(const QString &privateKey, const QString &passphrase) const;
    // Why an encrypted key cannot be decrypted here, or empty
    static QString encryptionProblem(const QByteArray &privateKey);
    // Whether the key only loads with a passphrase
    static bool isEncrypted(const QByteArray &privateKey);
    // type is "ed25519", "ecdsa" or "rsa". RSA takes a while, so keys are made off the UI thread.
    Q_INVOKABLE void generateKey(const QString &name, const QString &type);
    Q_INVOKABLE void importKey(const QString &name, const QString &privateKey, const QString &passphrase);
    Q_INVOKABLE void removeKey(const QString &keyId);
    // Emits privateKeyExported with the key in OpenSSH format, encrypted when a passphrase is given.
    // Keys kept encrypted come out as they are, with their own passphrase.
    Q_INVOKABLE void exportPrivateKey(const QString &keyId, const QString &passphrase);

signals:
    void countChanged();
    void busyChanged();
    void errorStringChanged();
    void privateKeyExported(const QString &keyId, const QString &privateKey);
    void keyRemoved(const QString &keyId);

private:
    struct Key {
        QString id;
        QString name;
        QString publicKey;
        QString fingerprint;
        // Kept with its passphrase, which is asked for on every connect
        bool encrypted;
    };

    void loadIndex();
    void saveIndex();
    void addKey(const QString &name, ssh_key_struct *sshKey);
    void addEncryptedKey(const QString &name, const QString &privateKey);
    // Fills in the public parts, returns false when the key cannot be described
    bool describeKey(const QString &name, ssh_key_struct *sshKey, Key *key) const;
    void storeKey(const Key &key, QByteArray privateKey);
    void setError(const QString &message);

    SecretVault *m_vault;
    int m_generating;
    QSettings m_index;
    QList<Key> m_keys;
    QString m_errorString;
};

#endif // KEYSTORE_H

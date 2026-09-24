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
        FingerprintRole
    };

    explicit KeyStore(SecretVault *vault, QObject *parent = nullptr);

    int count() const { return m_keys.size(); }
    bool busy() const;
    QString errorString() const { return m_errorString; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE QString keyIdAt(int row) const;
    Q_INVOKABLE int indexOf(const QString &keyId) const;
    // Returns an empty string for a usable key, otherwise why it is not
    Q_INVOKABLE QString validatePrivateKey(const QString &privateKey, const QString &passphrase) const;
    Q_INVOKABLE void generateKey(const QString &name);
    Q_INVOKABLE void importKey(const QString &name, const QString &privateKey, const QString &passphrase);
    Q_INVOKABLE void removeKey(const QString &keyId);

signals:
    void countChanged();
    void busyChanged();
    void errorStringChanged();

private:
    struct Key {
        QString id;
        QString name;
        QString publicKey;
        QString fingerprint;
    };

    void loadIndex();
    void saveIndex();
    void addKey(const QString &name, ssh_key_struct *sshKey);
    void setError(const QString &message);

    SecretVault *m_vault;
    QSettings m_index;
    QList<Key> m_keys;
    QString m_errorString;
};

#endif // KEYSTORE_H

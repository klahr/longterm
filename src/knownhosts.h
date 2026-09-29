#ifndef KNOWNHOSTS_H
#define KNOWNHOSTS_H

#include <QAbstractListModel>
#include <QByteArray>
#include <QList>

// The host keys trusted so far, as saved in the known_hosts file
class KnownHosts : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        HostsRole = Qt::UserRole + 1,
        KeyTypeRole,
        FingerprintRole,
        LineRole
    };

    explicit KnownHosts(QObject *parent = nullptr);

    int count() const { return m_entries.size(); }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Connections add entries, so this is called when the list is shown
    Q_INVOKABLE void reload();
    Q_INVOKABLE void remove(const QString &line);

signals:
    void countChanged();

private:
    struct Entry {
        QString hosts;
        QString keyType;
        QString fingerprint;
        QByteArray line;
    };

    QList<Entry> m_entries;
};

#endif // KNOWNHOSTS_H

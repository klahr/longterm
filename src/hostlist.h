#ifndef HOSTLIST_H
#define HOSTLIST_H

#include <QAbstractListModel>
#include <QPointer>
#include <QVector>

#include "hoststore.h"

// The saved hosts as the start page lists them: favorites first, then by
// group, the most recently used first within each, narrowed by a search
class HostList : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(HostStore *source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        // The heading a host is listed under, empty for none
        SectionRole = HostStore::MacAddressRole + 1,
        // The first host under its heading
        SectionStartRole
    };

    explicit HostList(QObject *parent = nullptr);

    HostStore *source() const { return m_source; }
    void setSource(HostStore *source);
    QString searchText() const { return m_searchText; }
    void setSearchText(const QString &text);
    int count() const { return m_rows.size(); }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void sourceChanged();
    void searchTextChanged();
    void countChanged();

private:
    void rebuild();
    QString section(int sourceRow) const;

    QPointer<HostStore> m_source;
    QString m_searchText;
    bool m_headed = false;
    // Source rows in the order shown
    QVector<int> m_rows;
};

#endif // HOSTLIST_H

#include "hostlist.h"

#include <algorithm>

HostList::HostList(QObject *parent)
    : QAbstractListModel(parent)
{
}

void HostList::setSource(HostStore *source)
{
    if (m_source == source)
        return;
    if (m_source)
        m_source->disconnect(this);
    m_source = source;
    if (m_source) {
        // Any change can move hosts or headings around, and there are few hosts
        connect(m_source, &QAbstractItemModel::dataChanged, this, &HostList::rebuild);
        connect(m_source, &QAbstractItemModel::rowsInserted, this, &HostList::rebuild);
        connect(m_source, &QAbstractItemModel::rowsRemoved, this, &HostList::rebuild);
        connect(m_source, &QAbstractItemModel::modelReset, this, &HostList::rebuild);
    }
    rebuild();
    emit sourceChanged();
}

void HostList::setSearchText(const QString &text)
{
    if (m_searchText == text)
        return;
    m_searchText = text;
    rebuild();
    emit searchTextChanged();
}

QString HostList::section(int sourceRow) const
{
    const QModelIndex index = m_source->index(sourceRow);
    if (index.data(HostStore::FavoriteRole).toBool())
        return tr("Favorites");
    return index.data(HostStore::GroupRole).toString();
}

void HostList::rebuild()
{
    const int before = m_rows.size();
    beginResetModel();
    m_rows.clear();
    const QString search = m_searchText.trimmed();
    for (int row = 0; m_source && row < m_source->rowCount(); ++row) {
        const QModelIndex index = m_source->index(row);
        if (!search.isEmpty()) {
            const QString text = QStringList {
                index.data(HostStore::NameRole).toString(),
                index.data(HostStore::AddressRole).toString(),
                index.data(HostStore::UserRole).toString(),
                index.data(HostStore::GroupRole).toString()
            }.join(QLatin1Char(' '));
            if (!text.contains(search, Qt::CaseInsensitive))
                continue;
        }
        m_rows.append(row);
    }
    std::stable_sort(m_rows.begin(), m_rows.end(), [this](int a, int b) {
        const QModelIndex left = m_source->index(a);
        const QModelIndex right = m_source->index(b);
        const bool leftFavorite = left.data(HostStore::FavoriteRole).toBool();
        const bool rightFavorite = right.data(HostStore::FavoriteRole).toBool();
        if (leftFavorite != rightFavorite)
            return leftFavorite;
        if (!leftFavorite) {
            // Hosts without a group come last
            const QString leftGroup = left.data(HostStore::GroupRole).toString();
            const QString rightGroup = right.data(HostStore::GroupRole).toString();
            if (leftGroup.isEmpty() != rightGroup.isEmpty())
                return !leftGroup.isEmpty();
            const int groups = QString::localeAwareCompare(leftGroup.toLower(), rightGroup.toLower());
            if (groups != 0)
                return groups < 0;
        }
        const QDateTime leftUsed = left.data(HostStore::LastUsedRole).toDateTime();
        const QDateTime rightUsed = right.data(HostStore::LastUsedRole).toDateTime();
        if (leftUsed != rightUsed)
            return leftUsed > rightUsed;
        return QString::localeAwareCompare(left.data(HostStore::NameRole).toString().toLower(),
                                           right.data(HostStore::NameRole).toString().toLower()) < 0;
    });
    endResetModel();
    if (before != m_rows.size())
        emit countChanged();
}

int HostList::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant HostList::data(const QModelIndex &index, int role) const
{
    if (!m_source || !index.isValid() || index.row() >= m_rows.size())
        return QVariant();
    const int sourceRow = m_rows.at(index.row());
    if (role == SectionRole)
        return section(sourceRow);
    if (role == SectionStartRole)
        return index.row() == 0 || section(m_rows.at(index.row() - 1)) != section(sourceRow);
    return m_source->index(sourceRow).data(role);
}

QHash<int, QByteArray> HostList::roleNames() const
{
    QHash<int, QByteArray> roles = m_source ? m_source->roleNames() : QHash<int, QByteArray>();
    roles[SectionRole] = "section";
    roles[SectionStartRole] = "sectionStart";
    return roles;
}

#include "sessionfilter.h"

#include "sessionmanager.h"

SessionFilter::SessionFilter(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    connect(this, &QAbstractItemModel::rowsInserted, this, &SessionFilter::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &SessionFilter::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &SessionFilter::countChanged);
    connect(this, &QAbstractItemModel::layoutChanged, this, &SessionFilter::countChanged);
}

void SessionFilter::setSource(QAbstractItemModel *source)
{
    if (source == sourceModel())
        return;
    setSourceModel(source);
    emit sourceChanged();
}

void SessionFilter::setKeep(SshSession *session)
{
    if (session == m_keep)
        return;
    m_keep = session;
    invalidateFilter();
    emit keepChanged();
}

int SessionFilter::indexOf(SshSession *session) const
{
    for (int row = 0; row < rowCount(); ++row) {
        if (data(index(row, 0), SessionManager::SessionRole).value<QObject *>() == session)
            return row;
    }
    return -1;
}

bool SessionFilter::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    const QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    SshSession *session = qobject_cast<SshSession *>(index.data(SessionManager::SessionRole).value<QObject *>());
    return session && (session == m_keep || session->state() != SshSession::Disconnected);
}

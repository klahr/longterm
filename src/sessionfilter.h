#ifndef SESSIONFILTER_H
#define SESSIONFILTER_H

#include <QPointer>
#include <QSortFilterProxyModel>

#include "sshsession.h"

// The sessions that are not disconnected, plus the kept one whatever its state
class SessionFilter : public QSortFilterProxyModel
{
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel *source READ sourceModel WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(SshSession *keep READ keep WRITE setKeep NOTIFY keepChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit SessionFilter(QObject *parent = nullptr);

    void setSource(QAbstractItemModel *source);
    SshSession *keep() const { return m_keep; }
    void setKeep(SshSession *session);
    int count() const { return rowCount(); }

    Q_INVOKABLE int indexOf(SshSession *session) const;

signals:
    void sourceChanged();
    void keepChanged();
    void countChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    QPointer<SshSession> m_keep;
};

#endif // SESSIONFILTER_H

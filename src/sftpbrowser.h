#ifndef SFTPBROWSER_H
#define SFTPBROWSER_H

#include <QAbstractListModel>
#include <QHash>
#include <QVariantList>

class SshSession;
struct SftpRequest;

// The files of a session's server, one folder at a time, and the transfers
// to and from it. Everything goes over the session's SSH connection.
class SftpBrowser : public QAbstractListModel
{
    Q_OBJECT
    // The folder shown, empty until the first listing arrives
    Q_PROPERTY(QString path READ path NOTIFY pathChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    // The last thing that went wrong, cleared by the next request
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    // Maps with id, name, upload, bytes and total, the first one is under way
    Q_PROPERTY(QVariantList transfers READ transfers NOTIFY transfersChanged)

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        DirectoryRole,
        LinkRole,
        SizeRole,
        ModifiedRole,
        PermissionsRole
    };

    explicit SftpBrowser(SshSession *session);

    QString path() const { return m_path; }
    bool loading() const { return m_loading; }
    QString errorString() const { return m_errorString; }
    QVariantList transfers() const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // An empty path is the home folder
    Q_INVOKABLE void open(const QString &path);
    Q_INVOKABLE void up();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE QString childPath(const QString &name) const;
    Q_INVOKABLE bool contains(const QString &name) const;
    // Into the Downloads folder
    Q_INVOKABLE void download(const QString &remotePath);
    // Into the folder remoteDirectory, the one shown when it is empty
    Q_INVOKABLE void upload(const QString &localPath, const QString &remoteDirectory, bool overwrite);
    Q_INVOKABLE void makeDirectory(const QString &name);
    Q_INVOKABLE void remove(const QString &name);
    Q_INVOKABLE void rename(const QString &name, const QString &newName);
    // 0 cancels them all
    Q_INVOKABLE void cancel(int id);

    // From the session, which passes on what its connection reports
    void onListed(int id, const QString &path, const QVariantList &entries, const QString &error);
    void onDone(int id, const QString &error);
    void onProgress(int id, qint64 bytes, qint64 total);
    void onTransferred(int id, const QString &localPath, const QString &error);
    // The connection is gone along with whatever was under way
    void onDisconnected();

signals:
    void pathChanged();
    void loadingChanged();
    void errorStringChanged();
    void transfersChanged();
    // localPath is where a download went, error is empty on success
    void transferFinished(const QString &name, bool upload, const QString &localPath, const QString &error);

private:
    struct Transfer {
        int id;
        QString name;
        bool upload;
        qint64 bytes;
        qint64 total;
    };

    int send(SftpRequest *request);
    void setLoading(bool loading);
    void setError(const QString &error);

    SshSession *m_session;
    QString m_path;
    QVariantList m_entries;
    int m_listId;
    bool m_loading;
    QString m_errorString;
    QList<Transfer> m_transfers;
    // Folder changes, which refresh the listing once done
    QHash<int, QString> m_operations;
    int m_nextId;
};

#endif // SFTPBROWSER_H

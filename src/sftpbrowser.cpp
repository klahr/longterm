#include "sftpbrowser.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

#include "sftpengine.h"
#include "sshsession.h"

SftpBrowser::SftpBrowser(SshSession *session)
    : QAbstractListModel(session)
    , m_session(session)
    , m_listId(0)
    , m_loading(false)
    , m_nextId(1)
{
}

QVariantList SftpBrowser::transfers() const
{
    QVariantList list;
    for (const Transfer &transfer : m_transfers) {
        QVariantMap map;
        map.insert(QStringLiteral("id"), transfer.id);
        map.insert(QStringLiteral("name"), transfer.name);
        map.insert(QStringLiteral("upload"), transfer.upload);
        map.insert(QStringLiteral("bytes"), transfer.bytes);
        map.insert(QStringLiteral("total"), transfer.total);
        list.append(map);
    }
    return list;
}

int SftpBrowser::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_entries.size();
}

QVariant SftpBrowser::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_entries.size())
        return QVariant();
    const QVariantMap entry = m_entries.at(index.row()).toMap();
    switch (role) {
    case NameRole: return entry.value(QStringLiteral("name"));
    case DirectoryRole: return entry.value(QStringLiteral("directory"));
    case LinkRole: return entry.value(QStringLiteral("link"));
    case SizeRole: return entry.value(QStringLiteral("size"));
    case ModifiedRole: return entry.value(QStringLiteral("modified"));
    case PermissionsRole: return entry.value(QStringLiteral("permissions"));
    default: return QVariant();
    }
}

QHash<int, QByteArray> SftpBrowser::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[NameRole] = "name";
    roles[DirectoryRole] = "directory";
    roles[LinkRole] = "link";
    roles[SizeRole] = "size";
    roles[ModifiedRole] = "modified";
    roles[PermissionsRole] = "permissions";
    return roles;
}

int SftpBrowser::send(SftpRequest *request)
{
    request->id = m_nextId++;
    setError(QString());
    if (!m_session->sendSftp(*request)) {
        setError(tr("Not connected"));
        return 0;
    }
    return request->id;
}

void SftpBrowser::open(const QString &path)
{
    SftpRequest request { SftpRequest::List, 0, path, QString(), false };
    m_listId = send(&request);
    setLoading(m_listId != 0);
}

void SftpBrowser::up()
{
    if (m_path.isEmpty() || m_path == QLatin1String("/"))
        return;
    const int slash = m_path.lastIndexOf(QLatin1Char('/'));
    open(slash <= 0 ? QStringLiteral("/") : m_path.left(slash));
}

void SftpBrowser::refresh()
{
    open(m_path);
}

QString SftpBrowser::childPath(const QString &name) const
{
    if (m_path.isEmpty())
        return name;
    return m_path.endsWith(QLatin1Char('/')) ? m_path + name : m_path + QLatin1Char('/') + name;
}

bool SftpBrowser::contains(const QString &name) const
{
    for (const QVariant &entry : m_entries) {
        if (entry.toMap().value(QStringLiteral("name")).toString() == name)
            return true;
    }
    return false;
}

void SftpBrowser::download(const QString &remotePath, bool open)
{
    SftpRequest request { SftpRequest::Download, 0, remotePath, QString(), false };
    if (!send(&request))
        return;
    m_transfers.append(Transfer { request.id, remotePath.section(QLatin1Char('/'), -1, -1, QString::SectionSkipEmpty),
                                  false, 0, 0, open });
    emit transfersChanged();
}

void SftpBrowser::upload(const QString &localPath, const QString &remoteDirectory, bool overwrite)
{
    // Pickers hand over file URLs
    const QString local = localPath.startsWith(QLatin1String("file://")) ? QUrl(localPath).toLocalFile() : localPath;
    const QString name = QFileInfo(local).fileName();
    const QString directory = remoteDirectory.isEmpty() ? m_path : remoteDirectory;
    const QString remote = directory.isEmpty() ? name
                                               : directory.endsWith(QLatin1Char('/')) ? directory + name
                                                                                      : directory + QLatin1Char('/') + name;
    SftpRequest request { SftpRequest::Upload, 0, remote, local, overwrite };
    if (!send(&request))
        return;
    m_transfers.append(Transfer { request.id, name, true, 0, QFileInfo(local).isDir() ? 0 : QFileInfo(local).size(), false });
    emit transfersChanged();
}

void SftpBrowser::makeDirectory(const QString &name)
{
    SftpRequest request { SftpRequest::MakeDirectory, 0, childPath(name), QString(), false };
    if (send(&request))
        m_operations.insert(request.id, m_path);
}

void SftpBrowser::remove(const QString &name)
{
    SftpRequest request { SftpRequest::Remove, 0, childPath(name), QString(), false };
    if (send(&request))
        m_operations.insert(request.id, m_path);
}

void SftpBrowser::rename(const QString &name, const QString &newName)
{
    if (newName.isEmpty() || newName == name)
        return;
    SftpRequest request { SftpRequest::Rename, 0, childPath(name), childPath(newName), false };
    if (send(&request))
        m_operations.insert(request.id, m_path);
}

void SftpBrowser::cancel(int id)
{
    // Names the transfer by its id
    m_session->sendSftp(SftpRequest { SftpRequest::Cancel, id, QString(), QString(), false });
}

QString SftpBrowser::writeSharedText(const QString &name, const QString &text)
{
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/shared");
    QDir().mkpath(directory);
    QString fileName = QFileInfo(name).fileName();
    if (fileName.isEmpty())
        fileName = tr("shared.txt");
    QFile file(directory + QLatin1Char('/') + fileName);
    if (!file.open(QIODevice::WriteOnly) || file.write(text.toUtf8()) < 0)
        return QString();
    return file.fileName();
}

void SftpBrowser::onListed(int id, const QString &path, const QVariantList &entries, const QString &error)
{
    if (id != m_listId)
        return;
    setLoading(false);
    if (!error.isEmpty() && entries.isEmpty()) {
        setError(error);
        return;
    }
    setError(error);
    QVariantList sorted = entries;
    // Folders first, then by name
    std::sort(sorted.begin(), sorted.end(), [](const QVariant &a, const QVariant &b) {
        const QVariantMap left = a.toMap();
        const QVariantMap right = b.toMap();
        const bool leftDirectory = left.value(QStringLiteral("directory")).toBool();
        const bool rightDirectory = right.value(QStringLiteral("directory")).toBool();
        if (leftDirectory != rightDirectory)
            return leftDirectory;
        return QString::localeAwareCompare(left.value(QStringLiteral("name")).toString().toLower(),
                                           right.value(QStringLiteral("name")).toString().toLower()) < 0;
    });
    beginResetModel();
    m_entries = sorted;
    endResetModel();
    if (m_path != path) {
        m_path = path;
        emit pathChanged();
    }
}

void SftpBrowser::onDone(int id, const QString &error)
{
    if (!m_operations.contains(id))
        return;
    const QString directory = m_operations.take(id);
    if (!error.isEmpty())
        setError(error);
    if (directory == m_path)
        open(m_path);
}

void SftpBrowser::onProgress(int id, qint64 bytes, qint64 total)
{
    for (Transfer &transfer : m_transfers) {
        if (transfer.id != id)
            continue;
        transfer.bytes = bytes;
        transfer.total = total;
        emit transfersChanged();
        return;
    }
}

void SftpBrowser::onTransferred(int id, const QString &localPath, const QString &error)
{
    for (int i = 0; i < m_transfers.size(); ++i) {
        if (m_transfers.at(i).id != id)
            continue;
        const Transfer transfer = m_transfers.takeAt(i);
        emit transfersChanged();
        emit transferFinished(transfer.name, transfer.upload, localPath, error, transfer.open);
        // A new file in the folder shown
        if (transfer.upload && error.isEmpty())
            refresh();
        return;
    }
}

void SftpBrowser::onDisconnected()
{
    if (m_loading)
        setLoading(false);
    m_listId = 0;
    m_operations.clear();
    const QList<Transfer> transfers = m_transfers;
    m_transfers.clear();
    if (!transfers.isEmpty())
        emit transfersChanged();
    for (const Transfer &transfer : transfers)
        emit transferFinished(transfer.name, transfer.upload, QString(), tr("Disconnected"), false);
}

void SftpBrowser::setLoading(bool loading)
{
    if (m_loading == loading)
        return;
    m_loading = loading;
    emit loadingChanged();
}

void SftpBrowser::setError(const QString &error)
{
    if (m_errorString == error)
        return;
    m_errorString = error;
    emit errorStringChanged();
}

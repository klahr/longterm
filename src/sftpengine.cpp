#include "sftpengine.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVariantMap>

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <fcntl.h>
#include <sys/stat.h>

static const int MaxInFlight = 16;
static const quint32 ChunkSize = 32 * 1024;
// A damaged or hostile server can list any number of files
static const int MaxEntries = 10000;
static const int MaxLinksResolved = 500;
static const int ProgressIntervalMs = 250;

static QString remoteName(const QString &path)
{
    return path.section(QLatin1Char('/'), -1, -1, QString::SectionSkipEmpty);
}

SftpEngine::SftpEngine(ssh_session session, const Callbacks &callbacks)
    : m_session(session)
    , m_sftp(nullptr)
    , m_callbacks(callbacks)
{
}

SftpEngine::~SftpEngine()
{
    failAll(tr("Disconnected"));
    if (m_sftp)
        sftp_free(m_sftp);
}

void SftpEngine::add(const SftpRequest &request)
{
    m_requests.enqueue(request);
}

bool SftpEngine::ready() const
{
    if (!m_requests.isEmpty())
        return true;
    if (!m_transfers.isEmpty()) {
        const Transfer *transfer = m_transfers.first();
        if (!transfer->file || transfer->pending.isEmpty())
            return true;
        if (!transfer->eof && !transfer->restart && !transfer->cancelled && transfer->error.isEmpty()
                && transfer->pending.size() < MaxInFlight)
            return true;
    }
    return m_sftp && ssh_channel_poll(m_sftp->channel, 0) > 0;
}

bool SftpEngine::service()
{
    while (!m_requests.isEmpty())
        run(m_requests.dequeue());
    if (!m_transfers.isEmpty())
        serviceTransfer(m_transfers.first());
    return ssh_is_connected(m_session);
}

bool SftpEngine::open()
{
    if (m_sftp)
        return true;
    // Opens a channel and waits for the server, which takes a round trip or two
    m_sftp = sftp_new(m_session);
    if (m_sftp && sftp_init(m_sftp) == SSH_OK)
        return true;
    m_openError = tr("The server does not offer SFTP: %1").arg(QString::fromUtf8(ssh_get_error(m_session)));
    if (m_sftp)
        sftp_free(m_sftp);
    m_sftp = nullptr;
    return false;
}

QString SftpEngine::errorString(const QString &what) const
{
    QString reason;
    switch (m_sftp ? sftp_get_error(m_sftp) : -1) {
    case SSH_FX_NO_SUCH_FILE:
    case SSH_FX_NO_SUCH_PATH:
        reason = tr("it does not exist");
        break;
    case SSH_FX_PERMISSION_DENIED:
        reason = tr("permission denied");
        break;
    case SSH_FX_FILE_ALREADY_EXISTS:
        reason = tr("it already exists");
        break;
    case SSH_FX_OP_UNSUPPORTED:
        reason = tr("the server does not support it");
        break;
    default:
        reason = QString::fromUtf8(ssh_get_error(m_session));
        if (reason.isEmpty())
            reason = tr("the server refused");
    }
    return QStringLiteral("%1: %2").arg(what, reason);
}

QString SftpEngine::canonical(const QString &path, QString *error)
{
    const QByteArray encoded = (path.isEmpty() ? QStringLiteral(".") : path).toUtf8();
    char *resolved = sftp_canonicalize_path(m_sftp, encoded.constData());
    if (!resolved) {
        *error = errorString(tr("Could not open %1").arg(path.isEmpty() ? tr("the home folder") : path));
        return QString();
    }
    const QString result = QString::fromUtf8(resolved);
    ssh_string_free_char(resolved);
    return result;
}

void SftpEngine::run(const SftpRequest &request)
{
    if (request.type == SftpRequest::Cancel) {
        for (int i = m_transfers.size() - 1; i >= 0; --i) {
            Transfer *transfer = m_transfers.at(i);
            if (request.id != 0 && transfer->request.id != request.id)
                continue;
            transfer->cancelled = true;
            // Only the first has started, the rest just go
            if (i > 0) {
                m_callbacks.transferred(transfer->request.id, QString(), tr("Cancelled"));
                delete m_transfers.takeAt(i);
            }
        }
        return;
    }

    if (request.type == SftpRequest::Download || request.type == SftpRequest::Upload) {
        Transfer *transfer = new Transfer;
        transfer->request = request;
        transfer->total = 0;
        m_transfers.append(transfer);
        return;
    }

    if (!open()) {
        if (request.type == SftpRequest::List)
            m_callbacks.listed(request.id, request.path, QVariantList(), m_openError);
        else
            m_callbacks.done(request.id, m_openError);
        return;
    }

    const QByteArray path = request.path.toUtf8();
    QString error;
    switch (request.type) {
    case SftpRequest::List:
        list(request);
        return;
    case SftpRequest::MakeDirectory:
        if (sftp_mkdir(m_sftp, path.constData(), 0755) != SSH_OK)
            error = errorString(tr("Could not create %1").arg(remoteName(request.path)));
        break;
    case SftpRequest::Remove: {
        sftp_attributes attributes = sftp_lstat(m_sftp, path.constData());
        const bool directory = attributes && attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
        sftp_attributes_free(attributes);
        if ((directory ? sftp_rmdir(m_sftp, path.constData()) : sftp_unlink(m_sftp, path.constData())) != SSH_OK)
            error = errorString(directory ? tr("Could not delete %1, only empty folders can be deleted").arg(remoteName(request.path))
                                          : tr("Could not delete %1").arg(remoteName(request.path)));
        break;
    }
    case SftpRequest::Rename:
        if (sftp_rename(m_sftp, path.constData(), request.target.toUtf8().constData()) != SSH_OK)
            error = errorString(tr("Could not rename %1").arg(remoteName(request.path)));
        break;
    default:
        break;
    }
    m_callbacks.done(request.id, error);
}

void SftpEngine::list(const SftpRequest &request)
{
    QString error;
    const QString directory = canonical(request.path, &error);
    if (directory.isEmpty()) {
        m_callbacks.listed(request.id, request.path, QVariantList(), error);
        return;
    }
    sftp_dir dir = sftp_opendir(m_sftp, directory.toUtf8().constData());
    if (!dir) {
        m_callbacks.listed(request.id, directory, QVariantList(),
                           errorString(tr("Could not open %1").arg(directory)));
        return;
    }

    QVariantList entries;
    int linksResolved = 0;
    sftp_attributes attributes;
    while (entries.size() < MaxEntries && (attributes = sftp_readdir(m_sftp, dir))) {
        const QString name = QString::fromUtf8(attributes->name);
        if (name == QLatin1String(".") || name == QLatin1String("..")) {
            sftp_attributes_free(attributes);
            continue;
        }
        bool isDirectory = attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
        const bool isLink = attributes->type == SSH_FILEXFER_TYPE_SYMLINK;
        quint64 size = attributes->size;
        // A link is shown as what it points to
        if (isLink && linksResolved++ < MaxLinksResolved) {
            const QString target = directory == QLatin1String("/") ? QLatin1Char('/') + name
                                                                  : directory + QLatin1Char('/') + name;
            sftp_attributes targetAttributes = sftp_stat(m_sftp, target.toUtf8().constData());
            if (targetAttributes) {
                isDirectory = targetAttributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
                size = targetAttributes->size;
                sftp_attributes_free(targetAttributes);
            }
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("name"), name);
        entry.insert(QStringLiteral("directory"), isDirectory);
        entry.insert(QStringLiteral("link"), isLink);
        entry.insert(QStringLiteral("size"), qint64(size));
        entry.insert(QStringLiteral("modified"), QDateTime::fromTime_t(attributes->mtime));
        entry.insert(QStringLiteral("permissions"), int(attributes->permissions & 07777));
        entries.append(entry);
        sftp_attributes_free(attributes);
    }
    if (entries.size() < MaxEntries && !sftp_dir_eof(dir))
        error = errorString(tr("Could not read %1").arg(directory));
    sftp_closedir(dir);
    m_callbacks.listed(request.id, directory, entries, error);
}

QString SftpEngine::uniqueLocalPath(const QString &directory, const QString &name)
{
    const QFileInfo info(name);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    QString path = directory + QLatin1Char('/') + name;
    for (int i = 2; QFileInfo::exists(path); ++i)
        path = QStringLiteral("%1/%2 (%3)%4").arg(directory, base).arg(i).arg(suffix);
    return path;
}

bool SftpEngine::prepareTransfer(Transfer *transfer)
{
    transfer->prepared = true;
    if (!open()) {
        transfer->error = m_openError;
        return false;
    }
    const SftpRequest &request = transfer->request;
    const QString name = remoteName(request.path);

    if (request.type == SftpRequest::Download) {
        sftp_attributes attributes = sftp_stat(m_sftp, request.path.toUtf8().constData());
        if (!attributes) {
            transfer->error = errorString(tr("Could not open %1").arg(name));
            return false;
        }
        const bool directory = attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
        const qint64 size = qint64(attributes->size);
        sftp_attributes_free(attributes);
        QString directoryPath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        QString localName = name.isEmpty() ? tr("download") : name;
        if (!request.target.isEmpty()) {
            directoryPath = QFileInfo(request.target).path();
            localName = QFileInfo(request.target).fileName();
        }
        QDir().mkpath(directoryPath);
        transfer->localPath = request.target.isEmpty() ? uniqueLocalPath(directoryPath, localName) : request.target;
        if (!directory) {
            transfer->items.append(Item { request.path, transfer->localPath });
            transfer->total = size;
            return true;
        }
        if (!QDir().mkpath(transfer->localPath)) {
            transfer->error = tr("Could not make the folder %1").arg(transfer->localPath);
            return false;
        }
        return listRemoteFolder(transfer, request.path, transfer->localPath);
    }

    const QFileInfo local(request.target);
    transfer->localPath = request.target;
    if (!local.isDir()) {
        transfer->items.append(Item { request.path, request.target });
        transfer->total = local.size();
        return true;
    }
    // A folder goes in as a new folder, or into the one there when asked to replace
    if (sftp_mkdir(m_sftp, request.path.toUtf8().constData(), 0755) != SSH_OK) {
        sftp_attributes existing = request.overwrite ? sftp_stat(m_sftp, request.path.toUtf8().constData()) : nullptr;
        const bool folder = existing && existing->type == SSH_FILEXFER_TYPE_DIRECTORY;
        sftp_attributes_free(existing);
        if (!folder) {
            transfer->error = errorString(tr("Could not create %1").arg(name));
            return false;
        }
    }
    QDirIterator walk(request.target, QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
                      QDirIterator::Subdirectories);
    const QDir base(request.target);
    while (walk.hasNext() && transfer->items.size() < MaxEntries) {
        const QString path = walk.next();
        const QFileInfo info = walk.fileInfo();
        if (info.isSymLink())
            continue;
        const QString remote = request.path + QLatin1Char('/') + base.relativeFilePath(path);
        if (info.isDir()) {
            // There already when a sibling's file made it, which is fine
            sftp_mkdir(m_sftp, remote.toUtf8().constData(), 0755);
        } else {
            transfer->items.append(Item { remote, path });
            transfer->total += info.size();
        }
    }
    return true;
}

// Depth first, links are left out so a link to a parent cannot loop
bool SftpEngine::listRemoteFolder(Transfer *transfer, const QString &remote, const QString &local)
{
    sftp_dir dir = sftp_opendir(m_sftp, remote.toUtf8().constData());
    if (!dir) {
        transfer->error = errorString(tr("Could not open %1").arg(remote));
        return false;
    }
    QStringList folders;
    sftp_attributes attributes;
    while (transfer->items.size() < MaxEntries && (attributes = sftp_readdir(m_sftp, dir))) {
        const QString name = QString::fromUtf8(attributes->name);
        const bool directory = attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
        const bool regular = attributes->type == SSH_FILEXFER_TYPE_REGULAR;
        const qint64 size = qint64(attributes->size);
        sftp_attributes_free(attributes);
        if (name == QLatin1String(".") || name == QLatin1String("..") || name.contains(QLatin1Char('/')))
            continue;
        if (directory) {
            folders.append(name);
        } else if (regular) {
            transfer->items.append(Item { remote + QLatin1Char('/') + name, local + QLatin1Char('/') + name });
            transfer->total += size;
        }
    }
    sftp_closedir(dir);
    for (const QString &folder : folders) {
        const QString localFolder = local + QLatin1Char('/') + folder;
        if (!QDir().mkpath(localFolder)) {
            transfer->error = tr("Could not make the folder %1").arg(localFolder);
            return false;
        }
        if (!listRemoteFolder(transfer, remote + QLatin1Char('/') + folder, localFolder))
            return false;
    }
    return true;
}

bool SftpEngine::openItem(Transfer *transfer)
{
    const Item &item = transfer->items.first();
    const QString name = remoteName(item.remote);
    transfer->itemBytes = 0;
    transfer->requested = 0;
    transfer->eof = false;
    transfer->restart = false;
    if (transfer->request.type == SftpRequest::Download) {
        transfer->file = sftp_open(m_sftp, item.remote.toUtf8().constData(), O_RDONLY, 0);
        if (!transfer->file) {
            transfer->error = errorString(tr("Could not open %1").arg(name));
            return false;
        }
        transfer->download = new QSaveFile(item.local);
        if (!transfer->download->open(QIODevice::WriteOnly)) {
            transfer->error = tr("Could not save %1: %2").arg(item.local, transfer->download->errorString());
            return false;
        }
    } else {
        transfer->upload = new QFile(item.local);
        if (!transfer->upload->open(QIODevice::ReadOnly)) {
            transfer->error = tr("Could not read %1: %2").arg(item.local, transfer->upload->errorString());
            return false;
        }
        const int flags = O_WRONLY | O_CREAT | (transfer->request.overwrite ? O_TRUNC : O_EXCL);
        transfer->file = sftp_open(m_sftp, item.remote.toUtf8().constData(), flags, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (!transfer->file) {
            transfer->error = errorString(tr("Could not create %1").arg(name));
            return false;
        }
    }
    sftp_file_set_nonblocking(transfer->file);
    return true;
}

void SftpEngine::closeItem(Transfer *transfer)
{
    for (Read &read : transfer->pending)
        sftp_aio_free(read.aio);
    transfer->pending.clear();
    const QString remote = transfer->items.isEmpty() ? QString() : transfer->items.first().remote;
    if (transfer->file && sftp_close(transfer->file) != SSH_OK && transfer->error.isEmpty())
        transfer->error = errorString(tr("Could not write %1").arg(remoteName(remote)));
    const bool failed = !transfer->error.isEmpty() || transfer->cancelled;
    // Half an upload is no use, unless it replaced a file which is gone either way
    if (transfer->file && failed && transfer->upload && !transfer->request.overwrite && m_sftp
            && ssh_is_connected(m_session))
        sftp_unlink(m_sftp, remote.toUtf8().constData());
    transfer->file = nullptr;
    if (transfer->download) {
        if (!failed && !transfer->download->commit())
            transfer->error = tr("Could not save %1: %2").arg(transfer->download->fileName(),
                                                              transfer->download->errorString());
        else if (failed)
            transfer->download->cancelWriting();
        delete transfer->download;
        transfer->download = nullptr;
    }
    delete transfer->upload;
    transfer->upload = nullptr;
    if (!transfer->items.isEmpty())
        transfer->items.removeFirst();
}

void SftpEngine::serviceTransfer(Transfer *transfer)
{
    if (!transfer->prepared && !transfer->cancelled && !prepareTransfer(transfer)) {
        finishTransfer(transfer);
        return;
    }
    const bool stopping = transfer->cancelled || !transfer->error.isEmpty();
    if (!transfer->file && !stopping && !transfer->items.isEmpty() && !openItem(transfer)) {
        finishTransfer(transfer);
        return;
    }
    if (transfer->file) {
        if (transfer->request.type == SftpRequest::Download)
            serviceDownload(transfer);
        else
            serviceUpload(transfer);
        const bool itemDone = transfer->eof || transfer->cancelled || !transfer->error.isEmpty();
        if (transfer->pending.isEmpty() && itemDone)
            closeItem(transfer);
    }
    if (!transfer->file && (transfer->items.isEmpty() || transfer->cancelled || !transfer->error.isEmpty())) {
        finishTransfer(transfer);
        return;
    }
    if (!transfer->progressTime.isValid() || transfer->progressTime.hasExpired(ProgressIntervalMs)) {
        transfer->progressTime.start();
        m_callbacks.progress(transfer->request.id, transfer->bytes, transfer->total);
    }
}

void SftpEngine::serviceDownload(Transfer *transfer)
{
    // Answers come back in the order asked
    while (!transfer->pending.isEmpty()) {
        Read &read = transfer->pending.head();
        if (m_buffer.size() < int(read.length))
            m_buffer.resize(int(read.length));
        const ssize_t n = sftp_aio_wait_read(&read.aio, m_buffer.data(), read.length);
        if (n == SSH_AGAIN)
            break;
        const quint32 length = read.length;
        transfer->pending.dequeue();
        if (transfer->restart || transfer->cancelled || !transfer->error.isEmpty())
            continue;
        if (n < 0) {
            transfer->error = errorString(tr("Could not read %1").arg(remoteName(transfer->items.first().remote)));
            continue;
        }
        if (n == 0) {
            transfer->eof = true;
            continue;
        }
        if (transfer->download->write(m_buffer.constData(), n) != n) {
            transfer->error = tr("Could not save %1: %2").arg(transfer->download->fileName(),
                                                              transfer->download->errorString());
            continue;
        }
        transfer->bytes += n;
        transfer->itemBytes += n;
        // A short answer leaves a gap before the next ones, so they are
        // dropped and asked again from here. At the end of the file the
        // next answer says so.
        if (quint32(n) < length)
            transfer->restart = true;
    }
    if (transfer->restart && transfer->pending.isEmpty()) {
        transfer->restart = false;
        transfer->requested = transfer->itemBytes;
        sftp_seek64(transfer->file, quint64(transfer->itemBytes));
    }

    while (!transfer->eof && !transfer->restart && !transfer->cancelled && transfer->error.isEmpty()
           && transfer->pending.size() < MaxInFlight) {
        sftp_aio aio = nullptr;
        const ssize_t length = sftp_aio_begin_read(transfer->file, ChunkSize, &aio);
        if (length < 0) {
            transfer->error = errorString(tr("Could not read %1").arg(remoteName(transfer->items.first().remote)));
            break;
        }
        transfer->pending.enqueue(Read { aio, quint32(length) });
        transfer->requested += length;
    }
}

void SftpEngine::serviceUpload(Transfer *transfer)
{
    while (!transfer->pending.isEmpty()) {
        Read &write = transfer->pending.head();
        const ssize_t n = sftp_aio_wait_write(&write.aio);
        if (n == SSH_AGAIN)
            break;
        transfer->pending.dequeue();
        if (n < 0) {
            if (transfer->error.isEmpty())
                transfer->error = errorString(tr("Could not write %1").arg(remoteName(transfer->items.first().remote)));
            continue;
        }
        transfer->bytes += n;
        transfer->itemBytes += n;
    }

    if (m_buffer.size() < int(ChunkSize))
        m_buffer.resize(int(ChunkSize));
    while (!transfer->eof && !transfer->cancelled && transfer->error.isEmpty()
           && transfer->pending.size() < MaxInFlight) {
        const qint64 n = transfer->upload->read(m_buffer.data(), ChunkSize);
        if (n < 0) {
            transfer->error = tr("Could not read %1: %2").arg(transfer->upload->fileName(), transfer->upload->errorString());
            break;
        }
        if (n == 0) {
            transfer->eof = true;
            break;
        }
        sftp_aio aio = nullptr;
        const ssize_t taken = sftp_aio_begin_write(transfer->file, m_buffer.constData(), size_t(n), &aio);
        if (taken < 0) {
            transfer->error = errorString(tr("Could not write %1").arg(remoteName(transfer->items.first().remote)));
            break;
        }
        // The server's limit can be below the chunk size
        if (taken < n)
            transfer->upload->seek(transfer->upload->pos() - (n - taken));
        transfer->pending.enqueue(Read { aio, quint32(taken) });
    }
}

void SftpEngine::finishTransfer(Transfer *transfer)
{
    if (transfer->file || transfer->download || transfer->upload)
        closeItem(transfer);
    QString error = transfer->error;
    if (error.isEmpty() && transfer->cancelled)
        error = tr("Cancelled");
    m_transfers.removeOne(transfer);
    if (error.isEmpty())
        m_callbacks.progress(transfer->request.id, transfer->bytes, qMax(transfer->total, transfer->bytes));
    // An upload has nothing on the phone to show
    const bool download = transfer->request.type == SftpRequest::Download;
    m_callbacks.transferred(transfer->request.id, error.isEmpty() && download ? transfer->localPath : QString(), error);
    delete transfer;
}

void SftpEngine::failAll(const QString &error)
{
    while (!m_requests.isEmpty()) {
        const SftpRequest request = m_requests.dequeue();
        if (request.type == SftpRequest::List)
            m_callbacks.listed(request.id, request.path, QVariantList(), error);
        else if (request.type == SftpRequest::Download || request.type == SftpRequest::Upload)
            m_callbacks.transferred(request.id, QString(), error);
        else if (request.type != SftpRequest::Cancel)
            m_callbacks.done(request.id, error);
    }
    while (!m_transfers.isEmpty()) {
        Transfer *transfer = m_transfers.first();
        if (transfer->error.isEmpty())
            transfer->error = error;
        finishTransfer(transfer);
    }
}

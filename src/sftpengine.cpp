#include "sftpengine.h"

#include <QDateTime>
#include <QDir>
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

bool SftpEngine::startTransfer(Transfer *transfer)
{
    if (!open()) {
        transfer->error = m_openError;
        return false;
    }
    const SftpRequest &request = transfer->request;
    const QByteArray path = request.path.toUtf8();
    const QString name = remoteName(request.path);

    if (request.type == SftpRequest::Download) {
        transfer->file = sftp_open(m_sftp, path.constData(), O_RDONLY, 0);
        if (!transfer->file) {
            transfer->error = errorString(tr("Could not open %1").arg(name));
            return false;
        }
        sftp_attributes attributes = sftp_fstat(transfer->file);
        const bool directory = attributes && attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
        if (attributes)
            transfer->total = qint64(attributes->size);
        sftp_attributes_free(attributes);
        if (directory) {
            transfer->error = tr("%1 is a folder, only files can be downloaded").arg(name);
            return false;
        }
        const QString directoryPath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        QDir().mkpath(directoryPath);
        transfer->localPath = uniqueLocalPath(directoryPath, name.isEmpty() ? tr("download") : name);
        transfer->download = new QSaveFile(transfer->localPath);
        if (!transfer->download->open(QIODevice::WriteOnly)) {
            transfer->error = tr("Could not save %1: %2").arg(transfer->localPath, transfer->download->errorString());
            return false;
        }
    } else {
        transfer->upload = new QFile(request.target);
        if (!transfer->upload->open(QIODevice::ReadOnly)) {
            transfer->error = tr("Could not read %1: %2").arg(request.target, transfer->upload->errorString());
            return false;
        }
        transfer->total = transfer->upload->size();
        const int flags = O_WRONLY | O_CREAT | (request.overwrite ? O_TRUNC : O_EXCL);
        transfer->file = sftp_open(m_sftp, path.constData(), flags, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (!transfer->file) {
            transfer->error = errorString(tr("Could not create %1").arg(name));
            return false;
        }
    }
    sftp_file_set_nonblocking(transfer->file);
    transfer->progressTime.start();
    m_callbacks.progress(request.id, 0, transfer->total);
    return true;
}

void SftpEngine::serviceTransfer(Transfer *transfer)
{
    if (!transfer->file && !transfer->download && !transfer->upload && !transfer->cancelled) {
        if (!startTransfer(transfer)) {
            finishTransfer(transfer);
            return;
        }
    }
    if (transfer->file) {
        if (transfer->request.type == SftpRequest::Download)
            serviceDownload(transfer);
        else
            serviceUpload(transfer);
    }
    const bool stopping = transfer->cancelled || !transfer->error.isEmpty();
    if (transfer->pending.isEmpty() && (transfer->eof || stopping || !transfer->file)) {
        finishTransfer(transfer);
        return;
    }
    if (transfer->progressTime.hasExpired(ProgressIntervalMs)) {
        transfer->progressTime.restart();
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
            transfer->error = errorString(tr("Could not read %1").arg(remoteName(transfer->request.path)));
            continue;
        }
        if (n == 0) {
            transfer->eof = true;
            continue;
        }
        if (transfer->download->write(m_buffer.constData(), n) != n) {
            transfer->error = tr("Could not save %1: %2").arg(transfer->localPath, transfer->download->errorString());
            continue;
        }
        transfer->bytes += n;
        // A short answer leaves a gap before the next ones, so they are
        // dropped and asked again from here. At the end of the file the
        // next answer says so.
        if (quint32(n) < length)
            transfer->restart = true;
    }
    if (transfer->restart && transfer->pending.isEmpty()) {
        transfer->restart = false;
        transfer->requested = transfer->bytes;
        sftp_seek64(transfer->file, quint64(transfer->bytes));
    }

    while (!transfer->eof && !transfer->restart && !transfer->cancelled && transfer->error.isEmpty()
           && transfer->pending.size() < MaxInFlight) {
        sftp_aio aio = nullptr;
        const ssize_t length = sftp_aio_begin_read(transfer->file, ChunkSize, &aio);
        if (length < 0) {
            transfer->error = errorString(tr("Could not read %1").arg(remoteName(transfer->request.path)));
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
                transfer->error = errorString(tr("Could not write %1").arg(remoteName(transfer->request.path)));
            continue;
        }
        transfer->bytes += n;
    }

    if (m_buffer.size() < int(ChunkSize))
        m_buffer.resize(int(ChunkSize));
    while (!transfer->eof && !transfer->cancelled && transfer->error.isEmpty()
           && transfer->pending.size() < MaxInFlight) {
        const qint64 n = transfer->upload->read(m_buffer.data(), ChunkSize);
        if (n < 0) {
            transfer->error = tr("Could not read %1: %2").arg(transfer->request.target, transfer->upload->errorString());
            break;
        }
        if (n == 0) {
            transfer->eof = true;
            break;
        }
        sftp_aio aio = nullptr;
        const ssize_t taken = sftp_aio_begin_write(transfer->file, m_buffer.constData(), size_t(n), &aio);
        if (taken < 0) {
            transfer->error = errorString(tr("Could not write %1").arg(remoteName(transfer->request.path)));
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
    for (Read &read : transfer->pending)
        sftp_aio_free(read.aio);
    transfer->pending.clear();

    QString error = transfer->error;
    if (error.isEmpty() && transfer->cancelled)
        error = tr("Cancelled");
    const bool created = transfer->request.type == SftpRequest::Upload && !transfer->request.overwrite;
    if (transfer->file && sftp_close(transfer->file) != SSH_OK && error.isEmpty())
        error = errorString(tr("Could not write %1").arg(remoteName(transfer->request.path)));
    // Half an upload is no use, unless it replaced a file which is gone either way
    if (transfer->file && !error.isEmpty() && created && m_sftp && ssh_is_connected(m_session))
        sftp_unlink(m_sftp, transfer->request.path.toUtf8().constData());
    transfer->file = nullptr;

    QString localPath;
    if (transfer->download) {
        if (error.isEmpty() && !transfer->download->commit())
            error = tr("Could not save %1: %2").arg(transfer->localPath, transfer->download->errorString());
        else if (!error.isEmpty())
            transfer->download->cancelWriting();
        if (error.isEmpty())
            localPath = transfer->localPath;
        delete transfer->download;
    }
    delete transfer->upload;

    m_transfers.removeOne(transfer);
    if (error.isEmpty())
        m_callbacks.progress(transfer->request.id, transfer->bytes, qMax(transfer->total, transfer->bytes));
    m_callbacks.transferred(transfer->request.id, localPath, error);
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

#ifndef SFTPENGINE_H
#define SFTPENGINE_H

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QList>
#include <QQueue>
#include <QString>
#include <QVariantList>

#include <functional>

class QFile;
class QSaveFile;

typedef struct ssh_session_struct *ssh_session;
typedef struct sftp_session_struct *sftp_session;
typedef struct sftp_file_struct *sftp_file;
typedef struct sftp_aio_struct *sftp_aio;

struct SftpRequest {
    enum Type {
        List,
        Download,
        Upload,
        MakeDirectory,
        Remove,
        Rename,
        // Stops the transfer with this id, or all of them for id 0
        Cancel
    };

    Type type;
    int id;
    // Remote, an empty path for List is the home directory
    QString path;
    // The new remote path for Rename, the local file for Upload
    QString target;
    // Upload replaces a file that is already there
    bool overwrite;
};

// Runs SFTP requests on a connected SSH session. Lives in the SSH worker
// thread, which calls service() from its poll() loop. Transfers keep several
// requests in flight without waiting, so they neither block the terminal
// nor crawl along at one round trip per chunk.
class SftpEngine
{
    Q_DECLARE_TR_FUNCTIONS(SftpEngine)

public:
    struct Callbacks {
        // entries are maps with name, directory, link, size, modified and permissions
        std::function<void(int id, const QString &path, const QVariantList &entries, const QString &error)> listed;
        // For MakeDirectory, Remove and Rename
        std::function<void(int id, const QString &error)> done;
        std::function<void(int id, qint64 bytes, qint64 total)> progress;
        // localPath is where a download went
        std::function<void(int id, const QString &localPath, const QString &error)> transferred;
    };

    SftpEngine(ssh_session session, const Callbacks &callbacks);
    ~SftpEngine();

    void add(const SftpRequest &request);
    // Does what it can without waiting, false once the SSH connection failed
    bool service();
    // service() has something to do right away
    bool ready() const;
    // Ends everything with the error, such as after the connection dropped
    void failAll(const QString &error);

private:
    struct Read {
        sftp_aio aio;
        quint32 length;
    };

    struct Transfer {
        SftpRequest request;
        sftp_file file = nullptr;
        QSaveFile *download = nullptr;
        QFile *upload = nullptr;
        QString localPath;
        qint64 total = 0;
        qint64 bytes = 0;
        // Requested from the server, ahead of bytes
        qint64 requested = 0;
        QQueue<Read> pending;
        bool eof = false;
        // Waiting for the requests in flight to come back before going on from bytes
        bool restart = false;
        QString error;
        bool cancelled = false;
        QElapsedTimer progressTime;
    };

    bool open();
    void run(const SftpRequest &request);
    QString errorString(const QString &what) const;
    QString canonical(const QString &path, QString *error);
    void list(const SftpRequest &request);
    bool startTransfer(Transfer *transfer);
    void serviceTransfer(Transfer *transfer);
    void serviceDownload(Transfer *transfer);
    void serviceUpload(Transfer *transfer);
    void finishTransfer(Transfer *transfer);
    static QString uniqueLocalPath(const QString &directory, const QString &name);

    ssh_session m_session;
    sftp_session m_sftp;
    Callbacks m_callbacks;
    QString m_openError;
    QQueue<SftpRequest> m_requests;
    QList<Transfer *> m_transfers;
    QByteArray m_buffer;
};

#endif // SFTPENGINE_H

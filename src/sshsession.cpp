#include "sshsession.h"

#include <QAtomicInt>
#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QStandardPaths>
#include <QThread>

#include <libssh/libssh.h>

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "secretvault.h"
#include "terminal.h"

// Idle time before the worker sends traffic to keep servers and NAT routers
// from dropping the connection
static const int KeepAliveIntervalMs = 60 * 1000;

class SshWorker : public QThread
{
    Q_OBJECT

public:
    SshWorker(const QString &host, int port, const QString &user,
              const QString &password, const QByteArray &privateKey,
              const QString &knownHostsPath, int columns, int rows)
        : m_host(host.toUtf8())
        , m_port(port)
        , m_user(user.toUtf8())
        , m_password(password.toUtf8())
        , m_privateKey(privateKey)
        , m_knownHostsPath(QFile::encodeName(knownHostsPath))
        , m_stop(0)
        , m_columns(columns)
        , m_rows(rows)
        , m_resizePending(false)
    {
        // Lets other threads wake the worker out of poll()
        if (pipe2(m_wakePipe, O_CLOEXEC | O_NONBLOCK) != 0)
            m_wakePipe[0] = m_wakePipe[1] = -1;
    }

    ~SshWorker()
    {
        if (m_wakePipe[0] >= 0) {
            close(m_wakePipe[0]);
            close(m_wakePipe[1]);
        }
    }

    void write(const QByteArray &data)
    {
        {
            QMutexLocker locker(&m_writeMutex);
            m_pendingWrite.append(data);
        }
        wake();
    }

    void resize(int columns, int rows)
    {
        {
            QMutexLocker locker(&m_writeMutex);
            m_columns = columns;
            m_rows = rows;
            m_resizePending = true;
        }
        wake();
    }

    void stop()
    {
        m_stop.storeRelease(1);
        wake();
    }

signals:
    void connected();
    void dataReceived(const QByteArray &data);
    void info(const QString &message);
    void failed(const QString &message);
    void shellExited();
    void hostKeyChanged(const QString &fingerprint);

protected:
    void run() override
    {
        ssh_session session = ssh_new();
        if (!session) {
            emit failed(tr("Could not create SSH session"));
            return;
        }
        ssh_channel channel = nullptr;

        if (openShell(session, &channel)) {
            emit connected();
            readLoop(session, channel);
        }

        if (channel) {
            if (ssh_channel_is_open(channel)) {
                ssh_channel_send_eof(channel);
                ssh_channel_close(channel);
            }
            ssh_channel_free(channel);
        }
        ssh_disconnect(session);
        ssh_free(session);
    }

private:
    bool openShell(ssh_session session, ssh_channel *channelOut)
    {
        long timeout = 15;
        bool processConfig = false;
        ssh_options_set(session, SSH_OPTIONS_HOST, m_host.constData());
        ssh_options_set(session, SSH_OPTIONS_PORT, &m_port);
        ssh_options_set(session, SSH_OPTIONS_USER, m_user.constData());
        ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS, m_knownHostsPath.constData());
        ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);
        ssh_options_set(session, SSH_OPTIONS_PROCESS_CONFIG, &processConfig);

        if (ssh_connect(session) != SSH_OK)
            return fail(session, tr("Connection failed"));
        if (m_stop.loadAcquire())
            return false;

        if (!verifyHost(session))
            return false;

        if (!authenticate(session))
            return false;

        ssh_channel channel = ssh_channel_new(session);
        if (!channel)
            return fail(session, tr("Could not create channel"));
        *channelOut = channel;

        if (ssh_channel_open_session(channel) != SSH_OK)
            return fail(session, tr("Could not open session channel"));
        int columns;
        int rows;
        {
            QMutexLocker locker(&m_writeMutex);
            columns = m_columns;
            rows = m_rows;
            m_resizePending = false;
        }
        if (ssh_channel_request_pty_size(channel, "xterm-256color", columns, rows) != SSH_OK)
            return fail(session, tr("Could not allocate a terminal"));
        if (ssh_channel_request_shell(channel) != SSH_OK)
            return fail(session, tr("Could not start shell"));
        return true;
    }

    bool authenticate(ssh_session session)
    {
        if (m_privateKey.isEmpty()) {
            const int rc = ssh_userauth_password(session, nullptr, m_password.constData());
            m_password.fill('\0');
            if (rc != SSH_AUTH_SUCCESS)
                return fail(session, tr("Authentication failed"));
            return true;
        }

        ssh_key key = nullptr;
        int rc = ssh_pki_import_privkey_base64(m_privateKey.constData(), nullptr, nullptr, nullptr, &key);
        m_privateKey.fill('\0');
        if (rc != SSH_OK) {
            emit failed(tr("Could not load private key"));
            return false;
        }
        rc = ssh_userauth_publickey(session, nullptr, key);
        ssh_key_free(key);
        if (rc != SSH_AUTH_SUCCESS)
            return fail(session, tr("Key was not accepted"));
        return true;
    }

    bool verifyHost(ssh_session session)
    {
        ssh_key key = nullptr;
        if (ssh_get_server_publickey(session, &key) != SSH_OK)
            return fail(session, tr("Could not get server host key"));

        unsigned char *hash = nullptr;
        size_t hashLen = 0;
        int rc = ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hashLen);
        ssh_key_free(key);
        if (rc != SSH_OK)
            return fail(session, tr("Could not hash server host key"));
        char *hex = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hashLen);
        const QString fingerprint = QString::fromLatin1(hex);
        ssh_string_free_char(hex);
        ssh_clean_pubkey_hash(&hash);

        switch (ssh_session_is_known_server(session)) {
        case SSH_KNOWN_HOSTS_OK:
            return true;
        case SSH_KNOWN_HOSTS_UNKNOWN:
        case SSH_KNOWN_HOSTS_NOT_FOUND:
            if (ssh_session_update_known_hosts(session) != SSH_OK)
                return fail(session, tr("Could not save host key"));
            emit info(tr("Trusted new host key %1").arg(fingerprint));
            return true;
        case SSH_KNOWN_HOSTS_CHANGED:
        case SSH_KNOWN_HOSTS_OTHER:
            emit hostKeyChanged(fingerprint);
            emit failed(tr("Host key has changed"));
            return false;
        case SSH_KNOWN_HOSTS_ERROR:
        default:
            return fail(session, tr("Could not check host key"));
        }
    }

    void wake()
    {
        const char byte = 0;
        if (m_wakePipe[1] >= 0 && ::write(m_wakePipe[1], &byte, 1) < 0) {
            // A full pipe already guarantees a wakeup
        }
    }

    void drainWakePipe()
    {
        char bytes[64];
        while (m_wakePipe[0] >= 0 && ::read(m_wakePipe[0], bytes, sizeof(bytes)) > 0) {
        }
    }

    // Returns false on a read error, eof is set when the server closed the channel
    bool readAvailable(ssh_session session, ssh_channel channel, bool *eof)
    {
        char buffer[4096];
        for (int stream = 0; stream <= 1; ++stream) {
            for (;;) {
                const int n = ssh_channel_read_nonblocking(channel, buffer, sizeof(buffer), stream);
                if (n == SSH_EOF) {
                    *eof = true;
                    break;
                }
                if (n == SSH_ERROR)
                    return fail(session, tr("Read failed"));
                if (n == 0)
                    break;
                emit dataReceived(QByteArray(buffer, n));
            }
        }
        return true;
    }

    // Sleeps in poll() until the server sends something, another thread
    // wakes it, or a keepalive is due, so an idle connection costs no wakeups
    void readLoop(ssh_session session, ssh_channel channel)
    {
        const socket_t fd = ssh_get_fd(session);
        QElapsedTimer idle;
        idle.start();
        while (!m_stop.loadAcquire()) {
            drainWakePipe();

            QByteArray pending;
            bool resizePending;
            int columns;
            int rows;
            {
                QMutexLocker locker(&m_writeMutex);
                pending.swap(m_pendingWrite);
                resizePending = m_resizePending;
                m_resizePending = false;
                columns = m_columns;
                rows = m_rows;
            }
            if (resizePending)
                ssh_channel_change_pty_size(channel, columns, rows);
            if (!pending.isEmpty()) {
                if (ssh_channel_write(channel, pending.constData(), pending.size()) == SSH_ERROR) {
                    fail(session, tr("Write failed"));
                    return;
                }
                idle.restart();
            }

            bool eof = false;
            if (!readAvailable(session, channel, &eof))
                return;
            if (eof || !ssh_channel_is_open(channel) || ssh_channel_is_eof(channel))
                break;

            if (idle.hasExpired(KeepAliveIntervalMs)) {
                if (ssh_send_ignore(session, "keepalive") != SSH_OK) {
                    fail(session, tr("Connection lost"));
                    return;
                }
                idle.restart();
            }

            struct pollfd fds[2];
            fds[0].fd = fd;
            fds[0].events = POLLIN;
            fds[1].fd = m_wakePipe[0];
            fds[1].events = POLLIN;
            const int timeout = int(qMax<qint64>(0, KeepAliveIntervalMs - idle.elapsed()));
            const int ready = poll(fds, m_wakePipe[0] >= 0 ? 2 : 1, timeout);
            if (ready < 0 && errno != EINTR) {
                emit failed(tr("Connection lost"));
                return;
            }
            if (ready > 0 && (fds[0].revents & POLLIN))
                idle.restart();
        }
        // Failures return above, so the server ended the shell unless we were stopped
        if (!m_stop.loadAcquire())
            emit shellExited();
    }

    bool fail(ssh_session session, const QString &message)
    {
        emit failed(QStringLiteral("%1: %2").arg(message, QString::fromUtf8(ssh_get_error(session))));
        return false;
    }

    const QByteArray m_host;
    int m_port;
    const QByteArray m_user;
    QByteArray m_password;
    QByteArray m_privateKey;
    const QByteArray m_knownHostsPath;
    QAtomicInt m_stop;
    QMutex m_writeMutex;
    QByteArray m_pendingWrite;
    int m_columns;
    int m_rows;
    bool m_resizePending;
    int m_wakePipe[2];
};

SshSession::SshSession(const QString &name, const QString &host, int port, const QString &user,
                       QObject *parent)
    : QObject(parent)
    , m_name(name)
    , m_host(host)
    , m_port(port)
    , m_user(user)
    , m_state(Disconnected)
    , m_worker(nullptr)
    , m_terminal(new Terminal(this))
    , m_secretFetchPending(false)
    , m_hostKeyMismatch(false)
    , m_authVault(nullptr)
    , m_authKind(Password)
{
    connect(m_terminal, &Terminal::outputReady, this, &SshSession::onTerminalOutput);
    connect(m_terminal, &Terminal::sizeChanged, this, &SshSession::onTerminalSizeChanged);
}

SshSession::~SshSession()
{
    stopWorker();
}

void SshSession::connectToHost(const QString &password)
{
    if (m_state != Disconnected)
        return;
    m_authPassword = password;
    m_authVault = nullptr;
    m_authSecretId.clear();
    startWorker(password, QByteArray());
}

void SshSession::reconnect()
{
    if (m_state != Disconnected)
        return;
    m_terminal->write(QByteArrayLiteral("\r\n"));
    if (m_authVault)
        connectWithSecret(m_authVault, m_authSecretId, m_authKind);
    else
        connectToHost(m_authPassword);
}

void SshSession::trustNewHostKey()
{
    if (!m_hostKeyMismatch || m_state != Disconnected)
        return;

    // libssh writes plain entries, "host" for port 22 and "[host]:port" otherwise
    const QString pattern = m_port == 22 ? m_host : QStringLiteral("[%1]:%2").arg(m_host).arg(m_port);
    QFile file(knownHostsPath());
    if (file.open(QIODevice::ReadOnly)) {
        QByteArray kept;
        while (!file.atEnd()) {
            const QByteArray line = file.readLine();
            const QList<QByteArray> hosts = line.trimmed().split(' ').value(0).split(',');
            if (!hosts.contains(pattern.toUtf8()))
                kept.append(line);
        }
        file.close();
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write(kept);
    }
    reconnect();
}

void SshSession::connectWithSecret(SecretVault *vault, const QString &secretId, SecretKind kind)
{
    if (m_state != Disconnected || !vault)
        return;

    m_authVault = vault;
    m_authSecretId = secretId;
    m_authKind = kind;
    m_errorString.clear();
    emit errorStringChanged();
    m_secretFetchPending = true;
    setState(Connecting);
    vault->fetch(secretId, this, [this, kind](const QByteArray &secret, const QString &error) {
        // Disconnected while the secret was being fetched
        if (!m_secretFetchPending)
            return;
        m_secretFetchPending = false;
        if (!error.isEmpty()) {
            onWorkerFailed(kind == PrivateKey ? tr("Could not read private key: %1").arg(error)
                                              : tr("Could not read saved password: %1").arg(error));
            setState(Disconnected);
            return;
        }
        if (kind == PrivateKey)
            startWorker(QString(), secret);
        else
            startWorker(QString::fromUtf8(secret), QByteArray());
    });
}

QString SshSession::knownHostsPath() const
{
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    return dataDir + QStringLiteral("/known_hosts");
}

void SshSession::startWorker(const QString &password, const QByteArray &privateKey)
{
    m_errorString.clear();
    emit errorStringChanged();
    if (m_hostKeyMismatch) {
        m_hostKeyMismatch = false;
        emit hostKeyMismatchChanged();
    }

    m_worker = new SshWorker(m_host, m_port, m_user, password, privateKey, knownHostsPath(),
                             m_terminal->columns(), m_terminal->rows());
    connect(m_worker, &SshWorker::connected, this, &SshSession::onWorkerConnected);
    connect(m_worker, &SshWorker::dataReceived, this, &SshSession::onWorkerData);
    connect(m_worker, &SshWorker::info, this, &SshSession::onWorkerInfo);
    connect(m_worker, &SshWorker::failed, this, &SshSession::onWorkerFailed);
    connect(m_worker, &SshWorker::shellExited, this, &SshSession::shellExited);
    connect(m_worker, &SshWorker::hostKeyChanged, this, &SshSession::onHostKeyChanged);
    connect(m_worker, &QThread::finished, this, &SshSession::onWorkerFinished);
    setState(Connecting);
    m_worker->start();
}

void SshSession::disconnectFromHost()
{
    if (m_secretFetchPending) {
        m_secretFetchPending = false;
        setState(Disconnected);
    }
    if (m_worker)
        m_worker->stop();
}

void SshSession::onWorkerConnected()
{
    setState(Connected);
}

void SshSession::onWorkerData(const QByteArray &data)
{
    m_terminal->write(data);
}

void SshSession::onWorkerInfo(const QString &message)
{
    m_terminal->write(message.toUtf8() + "\r\n");
}

void SshSession::onWorkerFailed(const QString &message)
{
    m_errorString = message;
    emit errorStringChanged();
}

void SshSession::onHostKeyChanged(const QString &fingerprint)
{
    m_serverFingerprint = fingerprint;
    m_hostKeyMismatch = true;
    emit hostKeyMismatchChanged();
}

void SshSession::onWorkerFinished()
{
    m_worker->deleteLater();
    m_worker = nullptr;
    setState(Disconnected);
}

void SshSession::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged();
}

void SshSession::onTerminalOutput(const QByteArray &data)
{
    if (m_worker && m_state == Connected)
        m_worker->write(data);
}

void SshSession::onTerminalSizeChanged()
{
    if (m_worker)
        m_worker->resize(m_terminal->columns(), m_terminal->rows());
}

void SshSession::stopWorker()
{
    if (!m_worker)
        return;
    m_worker->disconnect(this);
    m_worker->stop();
    m_worker->wait();
    delete m_worker;
    m_worker = nullptr;
}

#include "sshsession.moc"

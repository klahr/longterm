#include "sshsession.h"

#include <QAtomicInt>
#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QMutex>
#include <QNetworkInterface>
#include <QStandardPaths>
#include <QThread>
#include <QVector>
#include <QWaitCondition>

#include <libssh/callbacks.h>
#include <libssh/libssh.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "keystore.h"
#include "secretvault.h"
#include "sshagent.h"
#include "terminal.h"

// Like OpenSSH's NumberOfPasswordPrompts
static const int PasswordAttempts = 3;

// Idle time before the worker sends traffic to keep servers and NAT routers
// from dropping the connection
static const int KeepAliveIntervalMs = 60 * 1000;

// Carries the target connection's bytes over a direct-tcpip channel of the
// jump host session. libssh's own ProxyJump stops every jump thread in the
// process when any session disconnects, so it cannot be used with several.
class JumpProxy : public QThread
{
public:
    // Takes ownership of the connected session, the channel and the socket
    JumpProxy(ssh_session session, ssh_channel channel, int fd)
        : m_session(session)
        , m_channel(channel)
        , m_fd(fd)
        , m_stop(0)
    {
    }

    void stop() { m_stop.storeRelease(1); }

protected:
    void run() override
    {
        ssh_event event = ssh_event_new();
        ssh_connector toChannel = ssh_connector_new(m_session);
        ssh_connector fromChannel = ssh_connector_new(m_session);
        if (event && toChannel && fromChannel) {
            ssh_connector_set_in_fd(toChannel, m_fd);
            ssh_connector_set_out_channel(toChannel, m_channel, SSH_CONNECTOR_STDINOUT);
            ssh_event_add_connector(event, toChannel);
            ssh_connector_set_in_channel(fromChannel, m_channel, SSH_CONNECTOR_STDINOUT);
            ssh_connector_set_out_fd(fromChannel, m_fd);
            ssh_event_add_connector(event, fromChannel);
            // Short polls so stop() takes effect quickly
            while (!m_stop.loadAcquire() && ssh_channel_is_open(m_channel) && !ssh_channel_is_eof(m_channel)) {
                if (ssh_event_dopoll(event, 500) == SSH_ERROR)
                    break;
            }
            ssh_event_remove_connector(event, toChannel);
            ssh_event_remove_connector(event, fromChannel);
        }
        if (toChannel)
            ssh_connector_free(toChannel);
        if (fromChannel)
            ssh_connector_free(fromChannel);
        if (event)
            ssh_event_free(event);

        // Lets the target session see the end of its connection
        shutdown(m_fd, SHUT_RDWR);
        close(m_fd);
        if (ssh_channel_is_open(m_channel))
            ssh_channel_close(m_channel);
        ssh_channel_free(m_channel);
        ssh_disconnect(m_session);
        ssh_free(m_session);
    }

private:
    ssh_session m_session;
    ssh_channel m_channel;
    int m_fd;
    QAtomicInt m_stop;
};

class SshWorker : public QThread
{
    Q_OBJECT

public:
    struct Forward {
        int localPort;
        QByteArray remoteHost;
        int remotePort;
    };

    struct Config {
        QByteArray host;
        int port;
        QByteArray user;
        SshCredentials credentials;
        bool hasJump;
        QByteArray jumpHost;
        int jumpPort;
        QByteArray jumpUser;
        SshCredentials jumpCredentials;
        bool forwardAgent;
        QList<Forward> forwards;
        QByteArray knownHostsPath;
    };

    SshWorker(const Config &config, int columns, int rows)
        : m_config(config)
        , m_stop(0)
        , m_columns(columns)
        , m_rows(rows)
        , m_resizePending(false)
        , m_promptAnswered(false)
        , m_agent(nullptr)
        , m_jumpProxy(nullptr)
    {
        std::memset(&m_callbacks, 0, sizeof(m_callbacks));
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
        wipe(&m_config.credentials);
        wipe(&m_config.jumpCredentials);
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

    void answerPrompt(const QByteArray &answer)
    {
        QMutexLocker locker(&m_promptMutex);
        m_promptAnswer = answer;
        m_promptAnswered = true;
        m_promptCondition.wakeAll();
    }

    void stop()
    {
        m_stop.storeRelease(1);
        wake();
        QMutexLocker locker(&m_promptMutex);
        m_promptCondition.wakeAll();
    }

signals:
    void connected(const QString &localAddress);
    void dataReceived(const QByteArray &data);
    void info(const QString &message);
    // network is set when the connection failed or dropped, as opposed to being refused
    void failed(const QString &message, bool network);
    void shellExited();
    void hostKeyChanged(const QString &fingerprint, const QString &knownHostsPattern);
    // canRemember is set when the answer is the account password
    void promptRequested(const QString &text, bool echo, bool canRemember);

protected:
    void run() override
    {
        ssh_session session = ssh_new();
        if (!session) {
            emit failed(tr("Could not create SSH session"), false);
            return;
        }
        ssh_channel channel = nullptr;

        if (openShell(session, &channel)) {
            emit connected(m_localAddress);
            listenForForwards();
            readLoop(session, channel);
        }

        for (Stream *stream : m_streams + m_newStreams)
            closeStream(stream);
        m_streams.clear();
        m_newStreams.clear();
        for (const Listener &listener : m_listeners)
            close(listener.fd);
        m_listeners.clear();
        if (channel) {
            if (ssh_channel_is_open(channel)) {
                ssh_channel_send_eof(channel);
                ssh_channel_close(channel);
            }
            ssh_channel_free(channel);
        }
        ssh_disconnect(session);
        ssh_free(session);
        if (m_jumpProxy) {
            m_jumpProxy->stop();
            m_jumpProxy->wait();
            delete m_jumpProxy;
            m_jumpProxy = nullptr;
        }
        delete m_agent;
        m_agent = nullptr;
    }

private:
    // A forwarded connection, or an agent channel when fd is -1
    struct Stream {
        ssh_channel channel;
        int fd;
        QByteArray toSocket;
        QByteArray agentInput;
        bool channelEof;
        bool socketEof;
        bool closed;
    };

    struct Listener {
        int fd;
        Forward forward;
    };

    static void wipe(SshCredentials *credentials)
    {
        credentials->password.fill('\0');
        credentials->privateKey.fill('\0');
    }

    static QString knownHostsPattern(const QByteArray &host, int port)
    {
        // libssh writes plain entries, "host" for port 22 and "[host]:port" otherwise
        const QString name = QString::fromUtf8(host);
        return port == 22 ? name : QStringLiteral("[%1]:%2").arg(name).arg(port);
    }

    static ssh_channel onAgentChannelRequest(ssh_session session, void *userdata)
    {
        SshWorker *worker = static_cast<SshWorker *>(userdata);
        ssh_channel channel = ssh_channel_new(session);
        if (channel)
            worker->m_newStreams.append(new Stream { channel, -1, QByteArray(), QByteArray(), false, false, false });
        return channel;
    }

    void setOptions(ssh_session session, const QByteArray &host, int port, const QByteArray &user)
    {
        long timeout = 15;
        bool processConfig = false;
        ssh_options_set(session, SSH_OPTIONS_HOST, host.constData());
        ssh_options_set(session, SSH_OPTIONS_PORT, &port);
        ssh_options_set(session, SSH_OPTIONS_USER, user.constData());
        ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS, m_config.knownHostsPath.constData());
        ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);
        ssh_options_set(session, SSH_OPTIONS_PROCESS_CONFIG, &processConfig);
    }

    // Logs in to the jump host and hands the target session a socket that
    // reaches the target through it
    bool connectJump(ssh_session session)
    {
        ssh_session jump = ssh_new();
        if (!jump) {
            emit failed(tr("Could not create SSH session"), false);
            return false;
        }
        setOptions(jump, m_config.jumpHost, m_config.jumpPort, m_config.jumpUser);
        if (ssh_connect(jump) != SSH_OK) {
            fail(jump, tr("Connection failed"), true, true);
            ssh_free(jump);
            return false;
        }
        m_localAddress = localAddress(jump);

        ssh_channel channel = nullptr;
        int fds[2] = { -1, -1 };
        if (m_stop.loadAcquire() || !verifyHost(jump, m_config.jumpHost, m_config.jumpPort, true)
                || !authenticate(jump, &m_config.jumpCredentials, true)) {
            // Already reported
        } else if (!(channel = ssh_channel_new(jump))
                   || ssh_channel_open_forward(channel, m_config.host.constData(), m_config.port, "127.0.0.1", 0) != SSH_OK) {
            fail(jump, tr("Could not reach %1 from the jump host").arg(QString::fromUtf8(m_config.host)), false, true);
        } else if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
            report(tr("Could not create socket"), false, true);
        } else {
            m_jumpProxy = new JumpProxy(jump, channel, fds[1]);
            m_jumpProxy->start();
            // libssh closes the socket with the session
            ssh_options_set(session, SSH_OPTIONS_FD, &fds[0]);
            return true;
        }
        if (channel)
            ssh_channel_free(channel);
        ssh_disconnect(jump);
        ssh_free(jump);
        return false;
    }

    bool openShell(ssh_session session, ssh_channel *channelOut)
    {
        setOptions(session, m_config.host, m_config.port, m_config.user);
        if (m_config.hasJump && !connectJump(session))
            return false;
        if (m_stop.loadAcquire())
            return false;

        if (ssh_connect(session) != SSH_OK)
            return fail(session, tr("Connection failed"), true);
        if (!m_config.hasJump)
            m_localAddress = localAddress(session);
        if (m_stop.loadAcquire())
            return false;

        if (!verifyHost(session, m_config.host, m_config.port, false))
            return false;

        if (!authenticate(session, &m_config.credentials, false))
            return false;

        ssh_channel channel = ssh_channel_new(session);
        if (!channel)
            return fail(session, tr("Could not create channel"), false);
        *channelOut = channel;

        if (ssh_channel_open_session(channel) != SSH_OK)
            return fail(session, tr("Could not open session channel"), false);

        if (m_agent) {
            ssh_callbacks_init(&m_callbacks);
            m_callbacks.userdata = this;
            m_callbacks.channel_open_request_auth_agent_function = &SshWorker::onAgentChannelRequest;
            ssh_set_callbacks(session, &m_callbacks);
            if (ssh_channel_request_auth_agent(channel) != SSH_OK)
                emit info(tr("The server refused agent forwarding"));
        }

        int columns;
        int rows;
        {
            QMutexLocker locker(&m_writeMutex);
            columns = m_columns;
            rows = m_rows;
            m_resizePending = false;
        }
        if (ssh_channel_request_pty_size(channel, "xterm-256color", columns, rows) != SSH_OK)
            return fail(session, tr("Could not allocate a terminal"), false);
        if (ssh_channel_request_shell(channel) != SSH_OK)
            return fail(session, tr("Could not start shell"), false);
        return true;
    }

    bool authenticate(ssh_session session, SshCredentials *credentials, bool jump)
    {
        int rc = ssh_userauth_none(session, nullptr);
        if (rc == SSH_AUTH_SUCCESS)
            return true;
        if (rc == SSH_AUTH_ERROR)
            return fail(session, tr("Authentication failed"), true, jump);
        int methods = ssh_userauth_list(session, nullptr);

        bool passwordLeft = !credentials->password.isEmpty();
        if (!credentials->privateKey.isEmpty()) {
            ssh_key key = nullptr;
            rc = ssh_pki_import_privkey_base64(credentials->privateKey.constData(), nullptr, nullptr, nullptr, &key);
            // Kept encrypted, so its passphrase is asked for on every connect
            const bool encrypted = rc != SSH_OK && KeyStore::isEncrypted(credentials->privateKey);
            if (encrypted) {
                const QString prompt = jump ? tr("Key passphrase for the jump host %1@%2").arg(QString::fromUtf8(m_config.jumpUser),
                                                                                              QString::fromUtf8(m_config.jumpHost))
                                            : tr("Key passphrase for %1@%2").arg(QString::fromUtf8(m_config.user),
                                                                                QString::fromUtf8(m_config.host));
                for (int attempt = 0; attempt < PasswordAttempts && rc != SSH_OK; ++attempt) {
                    QByteArray passphrase;
                    if (!ask(attempt > 0 ? tr("Wrong passphrase, try again.") + QLatin1Char('\n') + prompt : prompt,
                             false, &passphrase)) {
                        credentials->privateKey.fill('\0');
                        return false;
                    }
                    rc = ssh_pki_import_privkey_base64(credentials->privateKey.constData(), passphrase.constData(),
                                                       nullptr, nullptr, &key);
                    passphrase.fill('\0');
                }
            }
            credentials->privateKey.fill('\0');
            if (rc != SSH_OK) {
                // The session's own error is about the server, not the key
                report(encrypted ? tr("Wrong key passphrase") : tr("Could not load private key"), false, jump);
                return false;
            }
            rc = ssh_userauth_publickey(session, nullptr, key);
            if (!jump && m_config.forwardAgent && (rc == SSH_AUTH_SUCCESS || rc == SSH_AUTH_PARTIAL)) {
                m_agent = new SshAgent(key);
                if (!m_agent->isValid()) {
                    delete m_agent;
                    m_agent = nullptr;
                    emit info(tr("Agent forwarding is not available for this key"));
                }
            }
            ssh_key_free(key);
            if (rc == SSH_AUTH_SUCCESS)
                return true;
            if (rc != SSH_AUTH_PARTIAL)
                return fail(session, tr("Key was not accepted"), false, jump);
            methods = ssh_userauth_list(session, nullptr);
        } else if (methods & SSH_AUTH_METHOD_PASSWORD) {
            // Asks when no password is known or the known one is rejected
            const QString account = jump ? tr("Jump host password for %1@%2").arg(QString::fromUtf8(m_config.jumpUser),
                                                                                QString::fromUtf8(m_config.jumpHost))
                                         : tr("Password for %1@%2").arg(QString::fromUtf8(m_config.user),
                                                                        QString::fromUtf8(m_config.host));
            for (int attempt = 0; attempt < PasswordAttempts; ++attempt) {
                if (credentials->password.isEmpty()) {
                    const QString text = attempt > 0 ? tr("Wrong password, try again.") + QLatin1Char('\n') + account
                                                     : account;
                    if (!ask(text, false, &credentials->password, !jump))
                        return false;
                }
                rc = ssh_userauth_password(session, nullptr, credentials->password.constData());
                credentials->password.fill('\0');
                credentials->password.clear();
                if (rc == SSH_AUTH_SUCCESS)
                    return true;
                if (rc == SSH_AUTH_ERROR)
                    return fail(session, tr("Authentication failed"), true, jump);
                if (rc == SSH_AUTH_PARTIAL)
                    break;
            }
            if (rc != SSH_AUTH_PARTIAL)
                return fail(session, tr("Authentication failed"), false, jump);
            // Used up, keyboard-interactive has to ask for anything else
            passwordLeft = false;
            methods = ssh_userauth_list(session, nullptr);
        }

        if (methods & SSH_AUTH_METHOD_INTERACTIVE) {
            const bool ok = keyboardInteractive(session, passwordLeft ? credentials->password : QByteArray(), jump);
            credentials->password.fill('\0');
            return ok;
        }
        credentials->password.fill('\0');
        return fail(session, tr("Authentication failed"), false, jump);
    }

    bool keyboardInteractive(ssh_session session, QByteArray password, bool jump)
    {
        int rc = ssh_userauth_kbdint(session, nullptr, nullptr);
        while (rc == SSH_AUTH_INFO) {
            QStringList header;
            if (jump)
                header.append(tr("Jump host"));
            const QString name = QString::fromUtf8(ssh_userauth_kbdint_getname(session)).trimmed();
            const QString instruction = QString::fromUtf8(ssh_userauth_kbdint_getinstruction(session)).trimmed();
            if (!name.isEmpty())
                header.append(name);
            if (!instruction.isEmpty())
                header.append(instruction);

            const int prompts = ssh_userauth_kbdint_getnprompts(session);
            for (int i = 0; i < prompts; ++i) {
                char echo = 0;
                const QString prompt = QString::fromUtf8(ssh_userauth_kbdint_getprompt(session, unsigned(i), &echo)).trimmed();
                QByteArray answer;
                // The first hidden prompt is the password when one was given
                if (!echo && !password.isEmpty()) {
                    answer = password;
                    password.fill('\0');
                    password.clear();
                } else if (!ask(QStringList(header + QStringList(prompt)).join(QLatin1Char('\n')), echo, &answer,
                                !jump && !echo && prompt.contains(QLatin1String("password"), Qt::CaseInsensitive))) {
                    return false;
                }
                rc = ssh_userauth_kbdint_setanswer(session, unsigned(i), answer.constData());
                answer.fill('\0');
                if (rc < 0)
                    return fail(session, tr("Authentication failed"), false, jump);
            }
            rc = ssh_userauth_kbdint(session, nullptr, nullptr);
        }
        password.fill('\0');
        if (rc == SSH_AUTH_SUCCESS)
            return true;
        if (rc == SSH_AUTH_ERROR)
            return fail(session, tr("Authentication failed"), true, jump);
        return fail(session, tr("Authentication failed"), false, jump);
    }

    // Blocks until the user answers, returns false when the worker is stopped instead
    bool ask(const QString &text, bool echo, QByteArray *answer, bool canRemember = false)
    {
        QMutexLocker locker(&m_promptMutex);
        m_promptAnswered = false;
        emit promptRequested(text, echo, canRemember);
        while (!m_promptAnswered && !m_stop.loadAcquire())
            m_promptCondition.wait(&m_promptMutex);
        if (!m_promptAnswered)
            return false;
        *answer = m_promptAnswer;
        m_promptAnswer.fill('\0');
        m_promptAnswer.clear();
        return true;
    }

    bool verifyHost(ssh_session session, const QByteArray &host, int port, bool jump)
    {
        ssh_key key = nullptr;
        if (ssh_get_server_publickey(session, &key) != SSH_OK)
            return fail(session, tr("Could not get server host key"), false, jump);

        unsigned char *hash = nullptr;
        size_t hashLen = 0;
        int rc = ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hashLen);
        ssh_key_free(key);
        if (rc != SSH_OK)
            return fail(session, tr("Could not hash server host key"), false, jump);
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
                return fail(session, tr("Could not save host key"), false, jump);
            emit info(jump ? tr("Trusted new jump host key %1").arg(fingerprint)
                           : tr("Trusted new host key %1").arg(fingerprint));
            return true;
        case SSH_KNOWN_HOSTS_CHANGED:
        case SSH_KNOWN_HOSTS_OTHER:
            emit hostKeyChanged(fingerprint, knownHostsPattern(host, port));
            report(jump ? tr("Jump host key has changed") : tr("Host key has changed"), false, jump);
            return false;
        case SSH_KNOWN_HOSTS_ERROR:
        default:
            return fail(session, tr("Could not check host key"), false, jump);
        }
    }

    static QString localAddress(ssh_session session)
    {
        sockaddr_storage address;
        socklen_t length = sizeof(address);
        if (getsockname(ssh_get_fd(session), reinterpret_cast<sockaddr *>(&address), &length) != 0)
            return QString();
        return QHostAddress(reinterpret_cast<sockaddr *>(&address)).toString();
    }

    void listenForForwards()
    {
        for (const Forward &forward : m_config.forwards) {
            const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            const int reuse = 1;
            sockaddr_in address;
            std::memset(&address, 0, sizeof(address));
            address.sin_family = AF_INET;
            address.sin_port = htons(quint16(forward.localPort));
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0
                    || bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0
                    || listen(fd, 8) != 0) {
                if (fd >= 0)
                    close(fd);
                emit info(tr("Could not listen on local port %1: %2")
                          .arg(forward.localPort).arg(QString::fromLocal8Bit(strerror(errno))));
                continue;
            }
            m_listeners.append(Listener { fd, forward });
            emit info(tr("Forwarding local port %1 to %2:%3")
                      .arg(forward.localPort).arg(QString::fromUtf8(forward.remoteHost)).arg(forward.remotePort));
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
                    return fail(session, tr("Read failed"), true);
                if (n == 0)
                    break;
                emit dataReceived(QByteArray(buffer, n));
            }
        }
        return true;
    }

    // Moves what arrived on forwarded and agent channels along, false on a connection error
    bool serviceStreams(ssh_session session)
    {
        m_streams.append(m_newStreams);
        m_newStreams.clear();

        char buffer[16384];
        for (Stream *stream : m_streams) {
            while (!stream->channelEof) {
                const int n = ssh_channel_read_nonblocking(stream->channel, buffer, sizeof(buffer), 0);
                if (n == SSH_ERROR)
                    return fail(session, tr("Read failed"), true);
                if (n == SSH_EOF || (n == 0 && (!ssh_channel_is_open(stream->channel)
                                                || ssh_channel_is_eof(stream->channel)))) {
                    stream->channelEof = true;
                    break;
                }
                if (n == 0)
                    break;
                if (stream->fd >= 0)
                    stream->toSocket.append(buffer, n);
                else
                    stream->agentInput.append(buffer, n);
            }

            if (stream->fd < 0) {
                const QByteArray replies = m_agent ? m_agent->process(&stream->agentInput) : QByteArray();
                if (!replies.isEmpty() && ssh_channel_write(stream->channel, replies.constData(), uint32_t(replies.size())) == SSH_ERROR)
                    return fail(session, tr("Write failed"), true);
                if (stream->channelEof)
                    stream->closed = true;
                continue;
            }

            flushToSocket(stream);
            if (stream->channelEof && stream->toSocket.isEmpty())
                stream->closed = true;
        }
        sweepStreams();
        return true;
    }

    void flushToSocket(Stream *stream)
    {
        while (!stream->toSocket.isEmpty()) {
            const ssize_t n = ::send(stream->fd, stream->toSocket.constData(), size_t(stream->toSocket.size()), MSG_NOSIGNAL);
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                    stream->closed = true;
                return;
            }
            stream->toSocket.remove(0, int(n));
        }
    }

    // Returns false on a connection error
    bool readSocket(ssh_session session, Stream *stream)
    {
        char buffer[16384];
        const ssize_t n = ::recv(stream->fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            if (ssh_channel_write(stream->channel, buffer, uint32_t(n)) == SSH_ERROR)
                return fail(session, tr("Write failed"), true);
        } else if (n == 0) {
            stream->socketEof = true;
            ssh_channel_send_eof(stream->channel);
            if (stream->channelEof)
                stream->closed = true;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            stream->closed = true;
        }
        return true;
    }

    void acceptForward(ssh_session session, const Listener &listener)
    {
        const int fd = accept4(listener.fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0)
            return;
        ssh_channel channel = ssh_channel_new(session);
        if (!channel || ssh_channel_open_forward(channel, listener.forward.remoteHost.constData(),
                                                 listener.forward.remotePort, "127.0.0.1",
                                                 listener.forward.localPort) != SSH_OK) {
            emit info(tr("Could not forward to %1:%2: %3")
                      .arg(QString::fromUtf8(listener.forward.remoteHost)).arg(listener.forward.remotePort)
                      .arg(QString::fromUtf8(ssh_get_error(session))));
            if (channel)
                ssh_channel_free(channel);
            close(fd);
            return;
        }
        m_streams.append(new Stream { channel, fd, QByteArray(), QByteArray(), false, false, false });
    }

    void closeStream(Stream *stream)
    {
        if (stream->fd >= 0)
            close(stream->fd);
        if (ssh_channel_is_open(stream->channel)) {
            ssh_channel_send_eof(stream->channel);
            ssh_channel_close(stream->channel);
        }
        ssh_channel_free(stream->channel);
        delete stream;
    }

    void sweepStreams()
    {
        for (int i = m_streams.size() - 1; i >= 0; --i) {
            if (m_streams.at(i)->closed)
                closeStream(m_streams.takeAt(i));
        }
    }

    // Sleeps in poll() until the server sends something, another thread
    // wakes it, a forwarded socket is ready, or a keepalive is due, so an
    // idle connection costs no wakeups
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
                    fail(session, tr("Write failed"), true);
                    return;
                }
                idle.restart();
            }

            bool eof = false;
            if (!readAvailable(session, channel, &eof))
                return;
            if (eof || !ssh_channel_is_open(channel) || ssh_channel_is_eof(channel))
                break;
            if (!serviceStreams(session))
                return;

            if (idle.hasExpired(KeepAliveIntervalMs)) {
                if (ssh_send_ignore(session, "keepalive") != SSH_OK) {
                    fail(session, tr("Connection lost"), true);
                    return;
                }
                idle.restart();
            }

            QVector<pollfd> fds;
            fds.append(pollfd { fd, POLLIN, 0 });
            fds.append(pollfd { m_wakePipe[0], POLLIN, 0 });
            const int firstListener = fds.size();
            for (const Listener &listener : m_listeners)
                fds.append(pollfd { listener.fd, POLLIN, 0 });
            const int firstStream = fds.size();
            QList<Stream *> polledStreams;
            for (Stream *stream : m_streams) {
                if (stream->fd < 0)
                    continue;
                short events = stream->socketEof ? 0 : POLLIN;
                if (!stream->toSocket.isEmpty())
                    events |= POLLOUT;
                fds.append(pollfd { stream->fd, events, 0 });
                polledStreams.append(stream);
            }

            const int timeout = int(qMax<qint64>(0, KeepAliveIntervalMs - idle.elapsed()));
            const int ready = poll(fds.data(), nfds_t(fds.size()), timeout);
            if (ready < 0 && errno != EINTR) {
                emit failed(tr("Connection lost"), true);
                return;
            }
            if (ready <= 0)
                continue;
            if (fds.at(0).revents & POLLIN)
                idle.restart();

            for (int i = 0; i < polledStreams.size(); ++i) {
                Stream *stream = polledStreams.at(i);
                const short revents = fds.at(firstStream + i).revents;
                if (revents & POLLOUT)
                    flushToSocket(stream);
                if (revents & POLLIN) {
                    if (!readSocket(session, stream))
                        return;
                    idle.restart();
                } else if (revents & (POLLHUP | POLLERR)) {
                    stream->closed = true;
                }
            }
            sweepStreams();
            for (int i = 0; i < m_listeners.size(); ++i) {
                if (fds.at(firstListener + i).revents & POLLIN)
                    acceptForward(session, m_listeners.at(i));
            }
        }
        // Failures return above, so the server ended the shell unless we were stopped
        if (!m_stop.loadAcquire())
            emit shellExited();
    }

    void report(const QString &message, bool network, bool jump)
    {
        emit failed(jump ? tr("Jump host: %1").arg(message) : message, network);
    }

    bool fail(ssh_session session, const QString &message, bool network, bool jump = false)
    {
        report(QStringLiteral("%1: %2").arg(message, QString::fromUtf8(ssh_get_error(session))), network, jump);
        return false;
    }

    Config m_config;
    QAtomicInt m_stop;
    QMutex m_writeMutex;
    QByteArray m_pendingWrite;
    int m_columns;
    int m_rows;
    bool m_resizePending;
    int m_wakePipe[2];
    QMutex m_promptMutex;
    QWaitCondition m_promptCondition;
    bool m_promptAnswered;
    QByteArray m_promptAnswer;
    SshAgent *m_agent;
    JumpProxy *m_jumpProxy;
    QString m_localAddress;
    struct ssh_callbacks_struct m_callbacks;
    QList<Stream *> m_streams;
    // Agent channels opened from inside libssh while m_streams is being walked
    QList<Stream *> m_newStreams;
    QList<Listener> m_listeners;
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
    , m_prompting(false)
    , m_promptEcho(false)
    , m_promptCanRemember(false)
    , m_canRememberPassword(false)
    , m_lost(false)
    , m_reachedConnected(false)
    , m_hasJump(false)
    , m_jumpPort(22)
    , m_forwardAgent(false)
{
    connect(m_terminal, &Terminal::outputReady, this, &SshSession::onTerminalOutput);
    connect(m_terminal, &Terminal::sizeChanged, this, &SshSession::onTerminalSizeChanged);
}

SshSession::~SshSession()
{
    stopWorker();
}

bool SshSession::hasLocalAddress() const
{
    if (m_localAddress.isEmpty())
        return true;
    return QNetworkInterface::allAddresses().contains(QHostAddress(m_localAddress));
}

void SshSession::setJumpHost(const QString &host, int port, const QString &user,
                             SecretVault *vault, const QString &secretId, SecretKind kind)
{
    m_hasJump = true;
    m_jumpHost = host;
    m_jumpPort = port;
    m_jumpUser = user;
    m_jumpAuth = AuthSource();
    m_jumpAuth.vault = vault;
    m_jumpAuth.secretId = secretId;
    m_jumpAuth.kind = kind;
}

void SshSession::connectToHost(const QString &password)
{
    if (m_state != Disconnected)
        return;
    m_auth = AuthSource();
    m_auth.password = password;
    startConnection();
}

void SshSession::reconnect()
{
    if (m_state != Disconnected)
        return;
    m_terminal->write(QByteArrayLiteral("\r\n"));
    startConnection();
}

void SshSession::trustNewHostKey()
{
    if (!m_hostKeyMismatch || m_state != Disconnected)
        return;

    const QByteArray pattern = m_mismatchPattern.toUtf8();
    QFile file(knownHostsPath());
    if (file.open(QIODevice::ReadOnly)) {
        QByteArray kept;
        while (!file.atEnd()) {
            const QByteArray line = file.readLine();
            const QList<QByteArray> hosts = line.trimmed().split(' ').value(0).split(',');
            if (!hosts.contains(pattern))
                kept.append(line);
        }
        file.close();
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write(kept);
    }
    reconnect();
}

void SshSession::answerPrompt(const QString &answer, bool remember)
{
    if (!m_prompting || !m_worker)
        return;
    if (m_promptCanRemember && m_auth.kind == Password) {
        // Reconnects use it too, instead of asking again or retrying a rejected saved one
        m_auth = AuthSource();
        m_auth.password = answer;
        if (remember && m_canRememberPassword)
            emit passwordRemembered(answer);
    }
    setPrompt(false, QString(), false, false);
    m_worker->answerPrompt(answer.toUtf8());
}

void SshSession::dropConnection()
{
    if (m_state != Connected || !m_worker)
        return;
    onWorkerFailed(tr("Network changed"), true);
    m_worker->stop();
}

void SshSession::setSecret(SecretVault *vault, const QString &secretId, SecretKind kind)
{
    m_auth = AuthSource();
    m_auth.vault = vault;
    m_auth.secretId = secretId;
    m_auth.kind = kind;
}

void SshSession::clearStoredSecret()
{
    if (m_auth.vault)
        m_auth = AuthSource();
}

void SshSession::setEndpoint(const QString &host, int port, const QString &user)
{
    if (m_host == host && m_port == port && m_user == user)
        return;
    m_host = host;
    m_port = port;
    m_user = user;
    emit endpointChanged();
}

void SshSession::connectWithSecret(SecretVault *vault, const QString &secretId, SecretKind kind)
{
    if (m_state != Disconnected || !vault)
        return;
    setSecret(vault, secretId, kind);
    startConnection();
}

void SshSession::startConnection()
{
    if (m_state != Disconnected)
        return;
    m_errorString.clear();
    emit errorStringChanged();
    m_secretFetchPending = true;
    setState(Connecting);
    fetchCredentials(m_hasJump ? m_jumpAuth : AuthSource(), [this](const SshCredentials &jumpCredentials) {
        fetchCredentials(m_auth, [this, jumpCredentials](const SshCredentials &credentials) {
            m_secretFetchPending = false;
            startWorker(credentials, jumpCredentials);
        });
    });
}

void SshSession::fetchCredentials(const AuthSource &source, const std::function<void(const SshCredentials &)> &done)
{
    if (!source.vault) {
        SshCredentials credentials;
        credentials.password = source.password.toUtf8();
        done(credentials);
        return;
    }
    const SecretKind kind = source.kind;
    source.vault->fetch(source.secretId, this, [this, kind, done](const QByteArray &secret, const QString &error) {
        // Disconnected while the secret was being fetched
        if (!m_secretFetchPending)
            return;
        if (!error.isEmpty()) {
            m_secretFetchPending = false;
            onWorkerFailed(kind == PrivateKey ? tr("Could not read private key: %1").arg(error)
                                              : tr("Could not read saved password: %1").arg(error), false);
            setState(Disconnected);
            return;
        }
        SshCredentials credentials;
        if (kind == PrivateKey)
            credentials.privateKey = secret;
        else
            credentials.password = secret;
        done(credentials);
    });
}

QString SshSession::knownHostsPath()
{
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    return dataDir + QStringLiteral("/known_hosts");
}

void SshSession::startWorker(const SshCredentials &credentials, const SshCredentials &jumpCredentials)
{
    m_errorString.clear();
    emit errorStringChanged();
    if (m_hostKeyMismatch) {
        m_hostKeyMismatch = false;
        emit hostKeyMismatchChanged();
    }
    m_reachedConnected = false;
    m_localAddress.clear();

    SshWorker::Config config;
    config.host = m_host.toUtf8();
    config.port = m_port;
    config.user = m_user.toUtf8();
    config.credentials = credentials;
    config.hasJump = m_hasJump;
    config.jumpHost = m_jumpHost.toUtf8();
    config.jumpPort = m_jumpPort;
    config.jumpUser = m_jumpUser.toUtf8();
    config.jumpCredentials = jumpCredentials;
    config.forwardAgent = m_forwardAgent;
    config.knownHostsPath = QFile::encodeName(knownHostsPath());
    for (const QString &entry : m_localForwards) {
        // localPort:host:remotePort, where host may be a bracketed IPv6 address
        const int first = entry.indexOf(QLatin1Char(':'));
        const int last = entry.lastIndexOf(QLatin1Char(':'));
        if (first <= 0 || last <= first)
            continue;
        SshWorker::Forward forward;
        forward.localPort = entry.left(first).toInt();
        forward.remotePort = entry.mid(last + 1).toInt();
        QString host = entry.mid(first + 1, last - first - 1);
        if (host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']')))
            host = host.mid(1, host.size() - 2);
        forward.remoteHost = host.toUtf8();
        if (forward.localPort > 0 && forward.remotePort > 0 && !host.isEmpty())
            config.forwards.append(forward);
    }

    m_worker = new SshWorker(config, m_terminal->columns(), m_terminal->rows());
    config.credentials.password.fill('\0');
    config.credentials.privateKey.fill('\0');
    config.jumpCredentials.password.fill('\0');
    config.jumpCredentials.privateKey.fill('\0');
    connect(m_worker, &SshWorker::connected, this, &SshSession::onWorkerConnected);
    connect(m_worker, &SshWorker::dataReceived, this, &SshSession::onWorkerData);
    connect(m_worker, &SshWorker::info, this, &SshSession::onWorkerInfo);
    connect(m_worker, &SshWorker::failed, this, &SshSession::onWorkerFailed);
    connect(m_worker, &SshWorker::shellExited, this, &SshSession::shellExited);
    connect(m_worker, &SshWorker::hostKeyChanged, this, &SshSession::onHostKeyChanged);
    connect(m_worker, &SshWorker::promptRequested, this, &SshSession::onWorkerPrompt);
    connect(m_worker, &QThread::finished, this, &SshSession::onWorkerFinished);
    setState(Connecting);
    m_worker->start();
}

void SshSession::disconnectFromHost()
{
    m_lost = false;
    if (m_secretFetchPending) {
        m_secretFetchPending = false;
        setState(Disconnected);
    }
    if (m_worker)
        m_worker->stop();
}

void SshSession::onWorkerConnected(const QString &localAddress)
{
    m_localAddress = localAddress;
    m_reachedConnected = true;
    m_lost = false;
    setState(Connected);
    const QString script = m_startupScript.trimmed();
    if (!script.isEmpty())
        m_worker->write(QString(script + QLatin1Char('\n')).replace(QLatin1Char('\n'), QLatin1Char('\r')).toUtf8());
}

void SshSession::onWorkerData(const QByteArray &data)
{
    m_terminal->write(data);
}

void SshSession::onWorkerInfo(const QString &message)
{
    m_terminal->write(message.toUtf8() + "\r\n");
}

void SshSession::onWorkerFailed(const QString &message, bool network)
{
    // Only a connection that was up and then lost its network comes back by itself
    if (!network)
        m_lost = false;
    else if (m_state == Connected)
        m_lost = true;
    m_errorString = message;
    emit errorStringChanged();
}

void SshSession::onHostKeyChanged(const QString &fingerprint, const QString &knownHostsPattern)
{
    m_serverFingerprint = fingerprint;
    m_mismatchPattern = knownHostsPattern;
    m_hostKeyMismatch = true;
    emit hostKeyMismatchChanged();
}

void SshSession::onWorkerPrompt(const QString &text, bool echo, bool canRemember)
{
    setPrompt(true, text, echo, canRemember);
}

void SshSession::onWorkerFinished()
{
    const bool lost = m_lost && m_reachedConnected;
    m_worker->deleteLater();
    m_worker = nullptr;
    m_localAddress.clear();
    setPrompt(false, QString(), false, false);
    setState(Disconnected);
    if (lost)
        emit connectionLost();
}

void SshSession::setName(const QString &name)
{
    if (m_name == name)
        return;
    m_name = name;
    emit nameChanged();
}

void SshSession::setStartupScript(const QString &script)
{
    if (m_startupScript == script)
        return;
    m_startupScript = script;
    emit startupScriptChanged();
}

void SshSession::setColorScheme(const QString &colorScheme)
{
    if (m_colorScheme == colorScheme)
        return;
    m_colorScheme = colorScheme;
    emit colorSchemeChanged();
}

void SshSession::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged();
}

void SshSession::setPrompt(bool prompting, const QString &text, bool echo, bool canRemember)
{
    if (m_prompting == prompting && m_promptText == text && m_promptEcho == echo
            && m_promptCanRemember == canRemember)
        return;
    m_prompting = prompting;
    m_promptText = text;
    m_promptEcho = echo;
    m_promptCanRemember = canRemember;
    emit promptChanged();
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

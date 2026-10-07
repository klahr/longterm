#include "sshsession.h"

#include <QAtomicInt>
#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QMutex>
#include <QNetworkInterface>
#include <QRegExp>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>
#include <QVector>
#include <QWaitCondition>

#include <libssh/callbacks.h>
#include <libssh/libssh.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "keystore.h"
#include "moshclient.h"
#include "secretvault.h"
#include "sftpbrowser.h"
#include "sftpengine.h"
#include "sshagent.h"
#include "terminal.h"

// libssh keeps the key's struct private, so it goes through signals as an opaque pointer
Q_DECLARE_OPAQUE_POINTER(ssh_key_struct *)
Q_DECLARE_METATYPE(SharedSshKey)

// Like OpenSSH's NumberOfPasswordPrompts
static const int PasswordAttempts = 3;

// Output waiting for the terminal beyond which the worker stops reading, so a
// flood from the server is slowed down by SSH flow control instead of piling
// up in memory and freezing the UI thread
static const int MaxUnconsumedBytes = 256 * 1024;
// Servers asking this many keyboard-interactive rounds are going in circles
static const int MaxInteractiveRounds = 32;
static const int MaxForwardBuffer = 256 * 1024;
static const int DefaultConnectTimeout = 15;
static const int AddressTimeoutMs = 5 * 1000;
static const unsigned int UserTimeoutMs = 30 * 1000;

// Idle time before the worker sends traffic to keep servers and NAT routers
// from dropping the connection
static const int DefaultKeepAliveInterval = 60;
static const int StopWaitMs = 500;
// mosh-server prints its port and key right away
static const int MoshStartTimeoutMs = 20 * 1000;
static const int MaxMoshStartOutput = 16 * 1024;
// A shutdown the server does not answer gives up after this
static const int MoshShutdownMs = 1500;
// Silence after which the session shows how long the server has been quiet
static const int MoshSilenceMs = 5 * 1000;
static const int MoshNothingHeardMs = 10 * 1000;
// A resumed session that is still there answers within moments
static const int MoshResumeTimeoutMs = 15 * 1000;
// The largest SOCKS request, a domain name of 255 bytes
static const int MaxSocksRequest = 512;
// How long a notice shows, and how long until the SSH connection under mosh is tried again
static const int NoticeMs = 6000;
static const int SideRetryMs = 30 * 1000;
static const int SideFirstTryMs = 3000;
// Typing shown ahead that the server has not confirmed by then is likely not coming
static const int MaxPredictionAgeMs = 5000;
static const char SystemProbeCommand[] = "uname -sr; cat /etc/os-release";
static const int SystemProbeTimeoutMs = 10 * 1000;
static const int MaxSystemProbeOutput = 16 * 1024;

static QString cleanSystemId(const QString &id)
{
    return id.toLower().replace(QRegExp(QStringLiteral("[^a-z0-9._ -]")), QString()).simplified().left(64);
}

static QString cleanSystemName(const QString &name)
{
    return QString(name).replace(QRegExp(QStringLiteral("[\\x0000-\\x001f\\x007f]")), QStringLiteral(" "))
            .simplified().left(64);
}

// Single quotes for a POSIX shell, left out where nothing needs them
static QByteArray shellQuote(const QByteArray &text)
{
    if (!text.isEmpty() && QRegExp(QStringLiteral("[A-Za-z0-9_.,:@%+/=-]+")).exactMatch(QString::fromUtf8(text)))
        return text;
    return '\'' + QByteArray(text).replace('\'', "'\\''") + '\'';
}

// Attaches to the tmux session, creating it if needed, and falls back to a
// login shell where tmux is missing. Runs in the user's shell, which may be
// fish or csh as well as a POSIX one.
static QByteArray tmuxCommand(const QByteArray &session)
{
    return "tmux new-session -A -s " + shellQuote(session) + " || exec \"$SHELL\" -l";
}

static bool guessSystem(const QByteArray &banner, QString *id, QString *name)
{
    struct Pattern {
        const char *text;
        const char *id;
        const char *name;
    };
    static const Pattern patterns[] = {
        { "Ubuntu", "ubuntu", "Ubuntu" },
        { "Raspbian", "raspbian", "Raspbian" },
        { "Debian", "debian", "Debian" },
        { "FreeBSD", "freebsd", "FreeBSD" },
        { "NetBSD", "netbsd", "NetBSD" },
        { "for_Windows", "windows", "Windows" },
        { "ROSSSH", "routeros", "RouterOS" },
    };
    for (const Pattern &pattern : patterns) {
        if (!banner.contains(pattern.text))
            continue;
        *id = QString::fromLatin1(pattern.id);
        *name = QString::fromLatin1(pattern.name);
        QRegExp release(QStringLiteral("\\+deb(\\d+)u"));
        if (*id == QLatin1String("debian") && release.indexIn(QString::fromLatin1(banner)) >= 0)
            *name += QLatin1Char(' ') + release.cap(1);
        return true;
    }
    return false;
}

static bool describeSystem(const QByteArray &output, QString *id, QString *name)
{
    QString unameLine;
    QHash<QString, QString> release;
    for (const QByteArray &rawLine : output.split('\n')) {
        const QString line = QString::fromUtf8(rawLine).trimmed();
        const int equals = line.indexOf(QLatin1Char('='));
        if (equals > 0 && line.left(equals).contains(QRegExp(QStringLiteral("^[A-Z_]+$")))) {
            QString value = line.mid(equals + 1);
            if (value.size() >= 2 && (value.startsWith(QLatin1Char('"')) || value.startsWith(QLatin1Char('\'')))
                    && value.endsWith(value.at(0)))
                value = value.mid(1, value.size() - 2);
            release.insert(line.left(equals), value);
        } else if (unameLine.isEmpty() && !line.isEmpty()) {
            unameLine = line;
        }
    }

    if (release.contains(QStringLiteral("ID"))) {
        *id = release.value(QStringLiteral("ID")) + QLatin1Char(' ') + release.value(QStringLiteral("ID_LIKE"));
        *name = release.value(QStringLiteral("PRETTY_NAME"));
        if (name->isEmpty())
            *name = (release.value(QStringLiteral("NAME")) + QLatin1Char(' ') + release.value(QStringLiteral("VERSION_ID"))).trimmed();
    } else {
        const QString kernel = unameLine.section(QLatin1Char(' '), 0, 0);
        if (kernel == QLatin1String("Darwin")) {
            *id = QStringLiteral("macos");
            *name = QStringLiteral("macOS");
        } else if (kernel.startsWith(QLatin1String("CYGWIN")) || kernel.startsWith(QLatin1String("MINGW"))
                   || kernel.startsWith(QLatin1String("MSYS"))) {
            *id = QStringLiteral("windows");
            *name = QStringLiteral("Windows");
        } else if (kernel == QLatin1String("SunOS")) {
            *id = QStringLiteral("solaris");
            *name = unameLine;
        } else if (!kernel.isEmpty() && kernel.contains(QRegExp(QStringLiteral("^[A-Za-z][A-Za-z0-9_-]*$")))) {
            *id = kernel;
            *name = unameLine;
        }
    }
    *id = cleanSystemId(*id);
    *name = cleanSystemName(*name);
    return !id->isEmpty();
}

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

    ~JumpProxy() override
    {
        ssh_disconnect(m_session);
        ssh_free(m_session);
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

    // The server listens on remotePort and connections go on to host:port from here
    struct RemoteForward {
        int remotePort;
        QByteArray host;
        int port;
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
        QList<RemoteForward> remoteForwards;
        // Local ports with a SOCKS proxy
        QList<int> socksPorts;
        QList<QPair<QByteArray, QByteArray> > environment;
        QByteArray tmuxSession;
        QByteArray moshServer;
        // Runs this instead of a shell, empty for a shell
        QByteArray command;
        // An OpenSSH certificate for the key, "type base64 comment"
        QByteArray certificate;
        // Names the file programs report their status to under mosh, which
        // drops their escape sequences, empty for none
        QByteArray statusFile;
        // Only files, forwards and the status file, the terminal is elsewhere
        bool sideOnly = false;
        // Fails rather than asking, for connections the user did not start
        bool noPrompts = false;
        // Where a mosh session keeps what it needs to be picked up again after a restart
        QString journalPath;
        // Picks the mosh session of the journal up instead of connecting anew
        QByteArray resumeKey;
        int connectTimeout;
        int keepAliveInterval;
        bool mosh;
        QByteArray knownHostsPath;
    };

    SshWorker(const Config &config, int columns, int rows)
        : m_config(config)
        , m_stop(0)
        , m_columns(columns)
        , m_rows(rows)
        , m_resizePending(false)
        , m_roamPending(false)
        , m_promptAnswered(false)
        , m_agent(nullptr)
        , m_key(nullptr)
        , m_jumpKey(nullptr)
        , m_jumpProxy(nullptr)
        , m_systemProbe(nullptr)
        , m_sftp(nullptr)
        , m_statusChannel(nullptr)
        , m_mosh(nullptr)
        , m_moshSilence(0)
        , m_moshPort(0)
        , m_moshNothingHeardReported(false)
        , m_moshResumed(false)
        , m_moshGone(false)
        , m_moshEchoed(0)
        , m_keepMosh(0)
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

    void sftp(const SftpRequest &request)
    {
        {
            QMutexLocker locker(&m_writeMutex);
            m_sftpRequests.append(request);
        }
        wake();
    }

    // A new socket for mosh after the network changed
    void roam()
    {
        {
            QMutexLocker locker(&m_writeMutex);
            m_roamPending = true;
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

    // keepMosh leaves a mosh session running on the server, to be resumed after a restart
    void stop(bool keepMosh = false)
    {
        m_keepMosh.storeRelease(keepMosh ? 1 : 0);
        m_stop.storeRelease(1);
        wake();
        {
            QMutexLocker locker(&m_promptMutex);
            m_promptCondition.wakeAll();
        }
        // Breaks libssh out of whatever it is waiting for on the network
        QMutexLocker locker(&m_socketMutex);
        for (int fd : m_sockets)
            ::shutdown(fd, SHUT_RDWR);
    }

    // The terminal has taken this much of the output
    void consumed(int bytes)
    {
        const int before = m_unconsumed.fetchAndAddOrdered(-bytes);
        if (before > MaxUnconsumedBytes && before - bytes <= MaxUnconsumedBytes)
            wake();
    }

signals:
    void connected(const QString &localAddress);
    void dataReceived(const QByteArray &data);
    void info(const QString &message);
    // network is set when the connection failed or dropped, as opposed to being refused
    void failed(const QString &message, bool network);
    // status is the shell's or command's exit status, -1 when unknown
    void shellExited(int status);
    void hostKeyChanged(const QString &fingerprint, const QString &knownHostsPattern);
    // canRemember is set when the answer is the account password
    void promptRequested(const QString &text, bool echo, bool canRemember, const QString &label);
    void systemDetected(const QString &id, const QString &name, bool certain);
    // The terminal goes over mosh from here on
    void moshStarted();
    // Screen updates from mosh, see MoshClient::Output
    void moshData(const QByteArray &screen, const QByteArray &data, int skipLines);
    // Seconds the mosh server has been quiet for, 0 once it is heard again
    void moshSilence(int seconds);
    // The SSH connection under a mosh session closed, with it files and forwards
    void sshClosed();
    // The key of a new mosh session, to keep for resuming it
    void moshKey(const QByteArray &key);
    // The mosh session to resume was not there any more
    void moshGone();
    // How many typed bytes the mosh server has echoed, and the round trip in milliseconds
    void moshEcho(qint64 echoedBytes, int roundTrip);
    // A line a program wrote to the status file, "777;longterm-status;working" and the like
    void statusLine(const QByteArray &line);
    void sftpListed(int id, const QString &path, const QVariantList &entries, const QString &error);
    void sftpDone(int id, const QString &error);
    void sftpProgress(int id, qint64 bytes, qint64 total);
    void sftpTransferred(int id, const QString &localPath, const QString &error);
    // An encrypted key was decrypted with its passphrase, for later connections to reuse
    void keyUnlocked(const SharedSshKey &key, bool jump);

protected:
    void run() override
    {
        ssh_session session = ssh_new();
        if (!session) {
            emit failed(tr("Could not create SSH session"), false);
            return;
        }
        ssh_channel channel = nullptr;

        if (!m_config.resumeKey.isEmpty()) {
            if (resumeMosh()) {
                emit connected(QString());
                readLoop(session, &channel, false);
            }
        } else if (openShell(session, &channel)) {
            emit connected(m_localAddress);
            startSystemProbe(session);
            listenForForwards();
            requestRemoteForwards(session);
            readLoop(session, &channel);
        }

        finishMosh();
        closeSsh(session, &channel);
        ssh_free(session);
        if (m_jumpProxy) {
            m_jumpProxy->stop();
            m_jumpProxy->wait();
            delete m_jumpProxy;
            m_jumpProxy = nullptr;
        }
        delete m_agent;
        m_agent = nullptr;
        ssh_key_free(takeKey(&m_key));
        ssh_key_free(takeKey(&m_jumpKey));
    }

private:
    // A forwarded connection, or an agent channel when fd is -1
    struct Stream {
        Stream(ssh_channel channel, int fd)
            : channel(channel)
            , fd(fd)
            , channelEof(false)
            , socketEof(false)
            , closed(false)
            , writeShutdown(false)
            , socksPort(0)
            , socksGreeted(false)
        {
        }

        // Null while a SOCKS client has not yet said where to
        ssh_channel channel;
        int fd;
        QByteArray toSocket;
        QByteArray agentInput;
        bool channelEof;
        bool socketEof;
        bool closed;
        // The server's side ended, so the socket was told no more is coming
        bool writeShutdown;
        // The SOCKS proxy's local port, for a client still negotiating
        int socksPort;
        bool socksGreeted;
        QByteArray socksInput;
    };

    struct Listener {
        int fd;
        Forward forward;
        bool socks;
    };

    static void wipe(SshCredentials *credentials)
    {
        credentials->password.fill('\0');
        credentials->privateKey.fill('\0');
        credentials->unlockedKey.reset();
    }

    static QString knownHostsPattern(const QByteArray &host, int port)
    {
        // libssh writes plain entries, "host" for port 22 and "[host]:port" otherwise
        const QString name = QString::fromUtf8(host.toLower());
        return port == 22 ? name : QStringLiteral("[%1]:%2").arg(name).arg(port);
    }

    static ssh_channel onAgentChannelRequest(ssh_session session, void *userdata)
    {
        SshWorker *worker = static_cast<SshWorker *>(userdata);
        ssh_channel channel = ssh_channel_new(session);
        if (channel)
            worker->m_newStreams.append(new Stream(channel, -1));
        return channel;
    }

    // The server passes on a connection to a port it listens on for a remote forward
    static ssh_channel onForwardedChannelRequest(ssh_session session, const char *, int destinationPort,
                                                 const char *, int, void *userdata)
    {
        SshWorker *worker = static_cast<SshWorker *>(userdata);
        for (const RemoteForward &forward : worker->m_config.remoteForwards) {
            if (forward.remotePort != destinationPort)
                continue;
            const int fd = connectTcp(forward.host, forward.port);
            if (fd < 0) {
                emit worker->info(tr("Could not connect to %1:%2 for remote port %3")
                                  .arg(QString::fromUtf8(forward.host)).arg(forward.port).arg(forward.remotePort));
                return nullptr;
            }
            ssh_channel channel = ssh_channel_new(session);
            if (!channel) {
                close(fd);
                return nullptr;
            }
            worker->m_newStreams.append(new Stream(channel, fd));
            return channel;
        }
        return nullptr;
    }

    // Starts connecting without waiting, -1 when no address works at all
    static int connectTcp(const QByteArray &host, int port)
    {
        addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *addresses = nullptr;
        if (getaddrinfo(host.constData(), QByteArray::number(port).constData(), &hints, &addresses) != 0)
            return -1;
        int fd = -1;
        for (addrinfo *address = addresses; address && fd < 0; address = address->ai_next) {
            fd = socket(address->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (fd >= 0 && ::connect(fd, address->ai_addr, address->ai_addrlen) != 0 && errno != EINPROGRESS) {
                close(fd);
                fd = -1;
            }
        }
        freeaddrinfo(addresses);
        return fd;
    }

    void setOptions(ssh_session session, const QByteArray &host, int port, const QByteArray &user)
    {
        long timeout = m_config.connectTimeout;
        bool processConfig = false;
        ssh_options_set(session, SSH_OPTIONS_HOST, host.constData());
        ssh_options_set(session, SSH_OPTIONS_PORT, &port);
        ssh_options_set(session, SSH_OPTIONS_USER, user.constData());
        ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS, m_config.knownHostsPath.constData());
        ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);
        ssh_options_set(session, SSH_OPTIONS_PROCESS_CONFIG, &processConfig);
    }

    // Connects the TCP socket here rather than in libssh, so stop() can
    // interrupt the connect and anything libssh later waits for
    bool connectSocket(ssh_session session, const QByteArray &host, int port, bool jump)
    {
        if (port < 1 || port > 65535) {
            report(tr("Invalid port %1").arg(port), false, jump);
            return false;
        }
        addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *addresses = nullptr;
        const int lookup = getaddrinfo(host.constData(), QByteArray::number(port).constData(), &hints, &addresses);
        if (lookup != 0) {
            report(tr("Connection failed: %1").arg(QString::fromLocal8Bit(gai_strerror(lookup))), true, jump);
            return false;
        }

        QElapsedTimer elapsed;
        elapsed.start();
        int fd = -1;
        int error = ETIMEDOUT;
        for (addrinfo *address = addresses; address && fd < 0 && !m_stop.loadAcquire(); address = address->ai_next) {
            fd = socket(address->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (fd < 0)
                continue;
            if (::connect(fd, address->ai_addr, address->ai_addrlen) == 0)
                break;
            if (errno != EINPROGRESS) {
                error = errno;
                close(fd);
                fd = -1;
                continue;
            }
            pollfd fds[2] = { { fd, POLLOUT, 0 }, { m_wakePipe[0], POLLIN, 0 } };
            const int connectTimeoutMs = m_config.connectTimeout * 1000;
            QElapsedTimer addressElapsed;
            addressElapsed.start();
            socklen_t length = sizeof(error);
            int ready;
            do {
                qint64 timeout = qMax<qint64>(0, connectTimeoutMs - elapsed.elapsed());
                if (address->ai_next)
                    timeout = qMin<qint64>(timeout, qMax<qint64>(0, qMin(AddressTimeoutMs, connectTimeoutMs / 2)
                                                                    - addressElapsed.elapsed()));
                ready = poll(fds, 2, int(timeout));
                if (ready > 0 && !fds[0].revents)
                    drainWakePipe();
            } while (ready > 0 && !fds[0].revents && !m_stop.loadAcquire());
            if (ready == 0)
                error = ETIMEDOUT;
            if (ready <= 0 || !(fds[0].revents & (POLLOUT | POLLERR | POLLHUP))
                    || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) {
                close(fd);
                fd = -1;
            }
        }
        freeaddrinfo(addresses);
        if (fd < 0) {
            if (!m_stop.loadAcquire())
                report(tr("Connection failed: %1").arg(QString::fromLocal8Bit(strerror(error))), true, jump);
            return false;
        }

        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &UserTimeoutMs, sizeof(UserTimeoutMs));
        watchSocket(fd);
        ssh_options_set(session, SSH_OPTIONS_FD, &fd);
        return true;
    }

    static void handOverSocket(ssh_session session)
    {
        const socket_t none = SSH_INVALID_SOCKET;
        ssh_options_set(session, SSH_OPTIONS_FD, &none);
    }

    void watchSocket(int fd)
    {
        QMutexLocker locker(&m_socketMutex);
        m_sockets.append(fd);
        if (m_stop.loadAcquire())
            ::shutdown(fd, SHUT_RDWR);
    }

    // Before libssh closes them, so stop() never shuts down a reused descriptor
    void forgetSockets()
    {
        QMutexLocker locker(&m_socketMutex);
        m_sockets.clear();
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
        if (!connectSocket(jump, m_config.jumpHost, m_config.jumpPort, true)) {
            ssh_free(jump);
            return false;
        }
        if (ssh_connect(jump) != SSH_OK) {
            fail(jump, tr("Connection failed"), true, true);
            forgetSockets();
            ssh_free(jump);
            return false;
        }
        handOverSocket(jump);
        m_localAddress = localAddress(jump);

        ssh_channel channel = nullptr;
        int fds[2] = { -1, -1 };
        if (m_stop.loadAcquire() || !verifyHost(jump, m_config.jumpHost, m_config.jumpPort, true)
                || !authenticate(jump, &m_config.jumpCredentials, takeKey(&m_jumpKey), true)) {
            // Already reported
        } else if (!(channel = ssh_channel_new(jump))
                   || ssh_channel_open_forward(channel, m_config.host.constData(), m_config.port, "127.0.0.1", 0) != SSH_OK) {
            fail(jump, tr("Could not reach %1 from the jump host").arg(QString::fromUtf8(m_config.host)), false, true);
        } else if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
            report(tr("Could not create socket"), false, true);
        } else {
            m_jumpProxy = new JumpProxy(jump, channel, fds[1]);
            m_jumpProxy->start();
            watchSocket(fds[0]);
            ssh_options_set(session, SSH_OPTIONS_FD, &fds[0]);
            return true;
        }
        if (channel)
            ssh_channel_free(channel);
        forgetSockets();
        ssh_disconnect(jump);
        ssh_free(jump);
        return false;
    }

    bool openShell(ssh_session session, ssh_channel *channelOut)
    {
        if ((m_config.hasJump && !loadKey(&m_config.jumpCredentials, true, &m_jumpKey))
                || !loadKey(&m_config.credentials, false, &m_key))
            return false;
        setOptions(session, m_config.host, m_config.port, m_config.user);
        if (m_config.hasJump ? !connectJump(session)
                             : !connectSocket(session, m_config.host, m_config.port, false))
            return false;
        const bool connected = ssh_connect(session) == SSH_OK;
        if (!connected)
            fail(session, tr("Connection failed"), true);
        handOverSocket(session);
        if (!connected)
            return false;
        if (!m_config.hasJump)
            m_localAddress = localAddress(session);
        if (m_stop.loadAcquire())
            return false;

        if (!verifyHost(session, m_config.host, m_config.port, false))
            return false;
        QString systemId;
        QString systemName;
        if (guessSystem(QByteArray(ssh_get_serverbanner(session)), &systemId, &systemName))
            emit systemDetected(systemId, systemName, false);

        if (!authenticate(session, &m_config.credentials, takeKey(&m_key), false))
            return false;

        if (m_agent || !m_config.remoteForwards.isEmpty()) {
            ssh_callbacks_init(&m_callbacks);
            m_callbacks.userdata = this;
            if (m_agent)
                m_callbacks.channel_open_request_auth_agent_function = &SshWorker::onAgentChannelRequest;
            if (!m_config.remoteForwards.isEmpty())
                m_callbacks.channel_open_request_forwarded_tcpip_function = &SshWorker::onForwardedChannelRequest;
            ssh_set_callbacks(session, &m_callbacks);
        }

        if (m_config.sideOnly) {
            startStatusChannel(session);
            return true;
        }
        if (!openSessionChannel(session, channelOut, false))
            return false;
        if (m_config.mosh) {
            if (!startMosh(session, channelOut))
                return false;
            // Only when mosh-server ran, the fallback shell has its terminal for that
            if (m_mosh)
                startStatusChannel(session);
            return true;
        }
        return startShell(session, *channelOut);
    }

    // quiet leaves out what was already said for an earlier channel
    bool openSessionChannel(ssh_session session, ssh_channel *channelOut, bool quiet)
    {
        ssh_channel channel = ssh_channel_new(session);
        if (!channel)
            return fail(session, tr("Could not create channel"), false);
        *channelOut = channel;

        if (ssh_channel_open_session(channel) != SSH_OK)
            return fail(session, tr("Could not open session channel"), false);

        if (m_agent && ssh_channel_request_auth_agent(channel) != SSH_OK && !quiet)
            emit info(tr("The server refused agent forwarding"));
        requestEnvironment(channel, quiet);
        return true;
    }

    // A terminal with the shell, or tmux, on the session channel
    bool startShell(ssh_session session, ssh_channel channel)
    {
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
        if (!m_config.command.isEmpty()) {
            if (ssh_channel_request_exec(channel, m_config.command.constData()) != SSH_OK)
                return fail(session, tr("Could not run the command"), false);
        } else if (!m_config.tmuxSession.isEmpty()) {
            if (ssh_channel_request_exec(channel, tmuxCommand(m_config.tmuxSession).constData()) != SSH_OK)
                return fail(session, tr("Could not start tmux"), false);
        } else if (ssh_channel_request_shell(channel) != SSH_OK) {
            return fail(session, tr("Could not start shell"), false);
        }
        return true;
    }

    void requestEnvironment(ssh_channel channel, bool quiet)
    {
        QList<QByteArray> refused;
        for (const auto &variable : m_config.environment) {
            if (ssh_channel_request_env(channel, variable.first.constData(), variable.second.constData()) != SSH_OK)
                refused.append(variable.first);
        }
        if (!refused.isEmpty() && !quiet)
            emit info(tr("The server did not take %1, its AcceptEnv setting says which variables it takes")
                      .arg(QString::fromUtf8(refused.join(", "))));
    }

    // Runs mosh-server over the session channel and connects to it. The SSH
    // connection stays for files, forwards and the agent while it lasts.
    // Without a mosh-server that starts, the terminal goes over SSH instead.
    bool startMosh(ssh_session session, ssh_channel *channel)
    {
        QByteArray command = moshServerCommand() + " new -c 256";
        // Applied only where the server has no UTF-8 locale of its own
        bool locale = false;
        for (const auto &variable : m_config.environment) {
            if (variable.first == "LANG" || variable.first.startsWith("LC_")) {
                command += " -l " + shellQuote(variable.first + '=' + variable.second);
                locale = true;
            }
        }
        if (!locale)
            command += " -l LANG=C.UTF-8";
        // Listens where the SSH connection came in, which through a jump host is the wrong side
        if (!m_config.hasJump)
            command += " -s";
        // The shell learns where to report to, and inside tmux the session does
        // too, as its shells may have started under another connection
        QByteArray shell;
        if (!m_config.statusFile.isEmpty())
            shell += "export LONGTERM_STATUS_FILE=\"$HOME/.cache/longterm/" + m_config.statusFile + "\"; ";
        if (!m_config.tmuxSession.isEmpty()) {
            if (!m_config.statusFile.isEmpty())
                shell += "tmux set-environment -t " + shellQuote(m_config.tmuxSession)
                        + " LONGTERM_STATUS_FILE \"$LONGTERM_STATUS_FILE\" 2>/dev/null; ";
            shell += tmuxCommand(m_config.tmuxSession);
        } else {
            shell += "exec \"${SHELL:-/bin/sh}\" -l";
        }
        command += " -- sh -c " + shellQuote(shell);
        if (ssh_channel_request_exec(*channel, command.constData()) != SSH_OK)
            return fail(session, tr("Could not start mosh-server"), false);

        QByteArray output;
        QByteArray errors;
        int port = 0;
        QByteArray key;
        QElapsedTimer elapsed;
        elapsed.start();
        char buffer[4096];
        while (port == 0 && !m_stop.loadAcquire() && !elapsed.hasExpired(MoshStartTimeoutMs)) {
            const int n = ssh_channel_read_timeout(*channel, buffer, sizeof(buffer), 0, 500);
            if (n == SSH_ERROR)
                return fail(session, tr("Could not start mosh-server"), true);
            if (n > 0 && output.size() < MaxMoshStartOutput)
                output.append(buffer, n);
            int e;
            while ((e = ssh_channel_read_nonblocking(*channel, buffer, sizeof(buffer), 1)) > 0) {
                if (errors.size() < MaxMoshStartOutput)
                    errors.append(buffer, e);
            }
            // "MOSH CONNECT <port> <key>", only whole lines
            const QList<QByteArray> lines = output.split('\n');
            for (int i = 0; i + 1 < lines.size() && port == 0; ++i) {
                const QList<QByteArray> words = lines.at(i).trimmed().split(' ');
                if (words.size() == 4 && words.at(0) == "MOSH" && words.at(1) == "CONNECT") {
                    port = words.at(2).toInt();
                    key = words.at(3);
                }
            }
            if (n <= 0 && (ssh_channel_is_eof(*channel) || !ssh_channel_is_open(*channel)))
                break;
        }
        if (ssh_channel_is_open(*channel))
            ssh_channel_close(*channel);
        ssh_channel_free(*channel);
        *channel = nullptr;
        output.fill('\0');
        if (m_stop.loadAcquire())
            return false;
        if (port <= 0 || port > 65535) {
            QString said;
            for (const QByteArray &line : errors.split('\n')) {
                if (!line.trimmed().isEmpty())
                    said = cleanSystemName(QString::fromUtf8(line)).left(200);
            }
            emit info(said.isEmpty() ? tr("mosh-server did not start, is mosh installed on the server? Going on over SSH.")
                                     : tr("mosh-server did not start (%1), going on over SSH.").arg(said));
            return openSessionChannel(session, channel, true) && startShell(session, *channel);
        }

        sockaddr_storage address;
        socklen_t length = sizeof(address);
        std::memset(&address, 0, sizeof(address));
        const bool direct = !m_config.hasJump
                && getpeername(ssh_get_fd(session), reinterpret_cast<sockaddr *>(&address), &length) == 0
                && (address.ss_family == AF_INET || address.ss_family == AF_INET6);
        if (direct) {
            // The same address the SSH connection went to
            if (address.ss_family == AF_INET)
                reinterpret_cast<sockaddr_in *>(&address)->sin_port = htons(quint16(port));
            else
                reinterpret_cast<sockaddr_in6 *>(&address)->sin6_port = htons(quint16(port));
        } else {
            addrinfo hints;
            std::memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo *addresses = nullptr;
            const int lookup = getaddrinfo(m_config.host.constData(), QByteArray::number(port).constData(),
                                           &hints, &addresses);
            if (lookup != 0 || !addresses) {
                report(tr("Could not look up %1 for mosh: %2").arg(QString::fromUtf8(m_config.host),
                                                                     QString::fromLocal8Bit(gai_strerror(lookup))),
                       true, false);
                return false;
            }
            std::memcpy(&address, addresses->ai_addr, addresses->ai_addrlen);
            length = addresses->ai_addrlen;
            freeaddrinfo(addresses);
        }

        int columns;
        int rows;
        {
            QMutexLocker locker(&m_writeMutex);
            columns = m_columns;
            rows = m_rows;
            m_resizePending = false;
        }
        MoshClient *mosh = new MoshClient;
        const bool started = mosh->start(reinterpret_cast<sockaddr *>(&address), length, key, columns, rows);
        if (started && !m_config.journalPath.isEmpty() && mosh->setJournal(m_config.journalPath)) {
            mosh->setJournalNote(m_config.statusFile);
            emit moshKey(key);
        }
        key.fill('\0');
        if (!started) {
            report(mosh->errorString(), false, false);
            delete mosh;
            return false;
        }
        m_mosh = mosh;
        m_moshPort = port;
        emit moshStarted();
        return true;
    }

    // A path in the home folder keeps its ~ outside the quotes so the shell expands it
    QByteArray moshServerCommand() const
    {
        const QByteArray path = m_config.moshServer.trimmed();
        if (path.isEmpty())
            return "mosh-server";
        if (path.startsWith("~/"))
            return "~/" + shellQuote(path.mid(2));
        return shellQuote(path);
    }

    // Follows the status file, which the shell under mosh-server writes to, see longterm-status
    void startStatusChannel(ssh_session session)
    {
        if (m_config.statusFile.isEmpty())
            return;
        // Leftovers of connections long gone are cleared on the way
        const QByteArray script = "d=\"$HOME/.cache/longterm\"; mkdir -p \"$d\" || exit 1;"
                " find \"$d\" -name '*.status' -mtime +7 -exec rm -f {} + 2>/dev/null;"
                " f=\"$d/" + m_config.statusFile + "\"; touch \"$f\" && exec tail -n 0 -F \"$f\" 2>/dev/null";
        m_statusChannel = ssh_channel_new(session);
        if (m_statusChannel && ssh_channel_open_session(m_statusChannel) == SSH_OK
                && ssh_channel_request_exec(m_statusChannel, ("sh -c " + shellQuote(script)).constData()) == SSH_OK)
            return;
        finishStatusChannel();
    }

    // False when the connection failed, which is reported
    bool serviceStatusChannel(ssh_session session)
    {
        static const int MaxStatusLine = 8192;
        if (!m_statusChannel)
            return true;
        char buffer[4096];
        int n;
        while ((n = ssh_channel_read_nonblocking(m_statusChannel, buffer, sizeof(buffer), 0)) > 0) {
            m_statusInput.append(buffer, n);
            int end;
            while ((end = m_statusInput.indexOf('\n')) >= 0) {
                const QByteArray line = m_statusInput.left(end).trimmed();
                m_statusInput.remove(0, end + 1);
                if (!line.isEmpty())
                    emit statusLine(line);
            }
            // A line without an end is no status
            if (m_statusInput.size() > MaxStatusLine)
                m_statusInput.clear();
        }
        // Under mosh this may be the only thing reading the connection, so it notices it going
        if (n == SSH_ERROR && !ssh_is_connected(session))
            return fail(session, tr("Connection lost"), true);
        if (n == SSH_ERROR || !ssh_channel_is_open(m_statusChannel) || ssh_channel_is_eof(m_statusChannel))
            finishStatusChannel();
        return true;
    }

    void finishStatusChannel()
    {
        if (!m_statusChannel)
            return;
        if (ssh_channel_is_open(m_statusChannel))
            ssh_channel_close(m_statusChannel);
        ssh_channel_free(m_statusChannel);
        m_statusChannel = nullptr;
        m_statusInput.clear();
    }

    // Tells the server the session is over, waiting a moment for it to agree
    // Picks up the mosh session of the journal, with no SSH connection of its own
    bool resumeMosh()
    {
        int columns;
        int rows;
        {
            QMutexLocker locker(&m_writeMutex);
            columns = m_columns;
            rows = m_rows;
            m_resizePending = false;
        }
        MoshClient *mosh = new MoshClient;
        if (!mosh->resume(m_config.journalPath, m_config.resumeKey, columns, rows)) {
            report(mosh->errorString(), false, false);
            delete mosh;
            QFile::remove(m_config.journalPath);
            emit moshGone();
            return false;
        }
        m_mosh = mosh;
        m_moshResumed = true;
        emit moshStarted();
        return true;
    }

    void finishMosh()
    {
        if (!m_mosh)
            return;
        // Left running for the next start of the app, which resumes it
        if (m_keepMosh.loadAcquire() && !m_mosh->finished()) {
            delete m_mosh;
            m_mosh = nullptr;
            return;
        }
        if (!m_config.journalPath.isEmpty())
            QFile::remove(m_config.journalPath);
        m_mosh->shutdown();
        QElapsedTimer elapsed;
        elapsed.start();
        while (!m_mosh->finished() && !elapsed.hasExpired(MoshShutdownMs)) {
            m_mosh->tick();
            const QVector<int> moshFds = m_mosh->fds();
            QVector<pollfd> fds;
            for (int fd : moshFds)
                fds.append(pollfd { fd, POLLIN, 0 });
            const int timeout = qMin<qint64>(m_mosh->waitTime(), MoshShutdownMs - elapsed.elapsed());
            if (poll(fds.data(), nfds_t(fds.size()), qMax(0, timeout)) < 0 && errno != EINTR)
                break;
            for (int i = 0; i < fds.size(); ++i) {
                if (fds.at(i).revents & POLLIN)
                    m_mosh->readable(fds.at(i).fd);
            }
        }
        delete m_mosh;
        m_mosh = nullptr;
    }

    // Ends everything that goes over the SSH connection, then the connection
    void closeSsh(ssh_session session, ssh_channel *channel)
    {
        delete m_sftp;
        m_sftp = nullptr;
        for (Stream *stream : m_streams + m_newStreams)
            closeStream(stream);
        m_streams.clear();
        m_newStreams.clear();
        for (const Listener &listener : m_listeners)
            close(listener.fd);
        m_listeners.clear();
        finishSystemProbe();
        finishStatusChannel();
        if (*channel) {
            if (ssh_channel_is_open(*channel)) {
                ssh_channel_send_eof(*channel);
                ssh_channel_close(*channel);
            }
            ssh_channel_free(*channel);
            *channel = nullptr;
        }
        forgetSockets();
        ssh_disconnect(session);
    }

    void startSystemProbe(ssh_session session)
    {
        m_systemProbe = ssh_channel_new(session);
        if (m_systemProbe && ssh_channel_open_session(m_systemProbe) == SSH_OK
                && ssh_channel_request_exec(m_systemProbe, SystemProbeCommand) == SSH_OK) {
            m_systemProbeTime.start();
            return;
        }
        finishSystemProbe();
    }

    void serviceSystemProbe()
    {
        if (!m_systemProbe)
            return;
        char buffer[4096];
        for (;;) {
            const int n = ssh_channel_read_nonblocking(m_systemProbe, buffer, sizeof(buffer), 0);
            if (n > 0) {
                if (m_systemProbeOutput.size() < MaxSystemProbeOutput)
                    m_systemProbeOutput.append(buffer, n);
                continue;
            }
            if (n == 0 && ssh_channel_is_open(m_systemProbe) && !ssh_channel_is_eof(m_systemProbe)
                    && !m_systemProbeTime.hasExpired(SystemProbeTimeoutMs))
                return;
            break;
        }
        QString id;
        QString name;
        if (describeSystem(m_systemProbeOutput, &id, &name))
            emit systemDetected(id, name, true);
        finishSystemProbe();
    }

    void finishSystemProbe()
    {
        if (!m_systemProbe)
            return;
        if (ssh_channel_is_open(m_systemProbe))
            ssh_channel_close(m_systemProbe);
        ssh_channel_free(m_systemProbe);
        m_systemProbe = nullptr;
        m_systemProbeOutput.clear();
    }

    static ssh_key takeKey(ssh_key *key)
    {
        ssh_key taken = *key;
        *key = nullptr;
        return taken;
    }

    // Decrypts the private key, asking for its passphrase, before anything connects. A server
    // drops a connection still waiting for the passphrase after its LoginGraceTime, and newer
    // OpenSSH then refuses connections from the address for a while
    bool loadKey(SshCredentials *credentials, bool jump, ssh_key *keyOut)
    {
        *keyOut = nullptr;
        if (credentials->privateKey.isEmpty())
            return true;
        // Typed before, a reconnect must not wait for someone to type it again
        if (credentials->unlockedKey) {
            ssh_key key = ssh_key_dup(credentials->unlockedKey.data());
            credentials->unlockedKey.reset();
            if (key) {
                credentials->privateKey.fill('\0');
                *keyOut = key;
                return true;
            }
        }
        ssh_key key = nullptr;
        int rc = ssh_pki_import_privkey_base64(credentials->privateKey.constData(), nullptr, nullptr, nullptr, &key);
        // Kept encrypted, so its passphrase is asked for on every connect
        const bool encrypted = rc != SSH_OK && KeyStore::isEncrypted(credentials->privateKey);
        const QString problem = encrypted ? KeyStore::encryptionProblem(credentials->privateKey) : QString();
        if (!problem.isEmpty()) {
            credentials->privateKey.fill('\0');
            report(problem, false, jump);
            return false;
        }
        if (encrypted) {
            const QString prompt = jump ? tr("Key passphrase for the jump host %1@%2").arg(QString::fromUtf8(m_config.jumpUser),
                                                                                          QString::fromUtf8(m_config.jumpHost))
                                        : tr("Key passphrase for %1@%2").arg(QString::fromUtf8(m_config.user),
                                                                            QString::fromUtf8(m_config.host));
            for (int attempt = 0; attempt < PasswordAttempts && rc != SSH_OK; ++attempt) {
                QByteArray passphrase;
                if (!ask(attempt > 0 ? tr("Wrong passphrase, try again.") + QLatin1Char('\n') + prompt : prompt,
                         tr("Passphrase"), false, &passphrase)) {
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
            report(encrypted ? tr("Wrong key passphrase") : tr("Could not load private key"), false, jump);
            return false;
        }
        if (encrypted) {
            if (ssh_key copy = ssh_key_dup(key))
                emit keyUnlocked(SharedSshKey(copy, ssh_key_free), jump);
        }
        *keyOut = key;
        return true;
    }

    // Takes ownership of key, which is null for hosts without one
    bool authenticate(ssh_session session, SshCredentials *credentials, ssh_key key, bool jump)
    {
        int rc = ssh_userauth_none(session, nullptr);
        if (rc == SSH_AUTH_SUCCESS) {
            ssh_key_free(key);
            return true;
        }
        if (rc == SSH_AUTH_ERROR) {
            ssh_key_free(key);
            return fail(session, tr("Authentication failed"), true, jump);
        }
        int methods = ssh_userauth_list(session, nullptr);

        bool passwordLeft = !credentials->password.isEmpty();
        const bool hadKey = key != nullptr;
        if (key) {
            if (!jump && !m_config.certificate.isEmpty())
                attachCertificate(key);
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
                    if (!ask(text, tr("Password"), false, &credentials->password, !jump))
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
        if (!hadKey && (methods & SSH_AUTH_METHOD_PUBLICKEY)
                && !(methods & (SSH_AUTH_METHOD_PASSWORD | SSH_AUTH_METHOD_INTERACTIVE))) {
            report(tr("The server only accepts keys, choose one for this host"), false, jump);
            return false;
        }
        return fail(session, tr("Authentication failed"), false, jump);
    }

    // The key logs in with its certificate, which the server checks against its CA
    void attachCertificate(ssh_key key)
    {
        const QList<QByteArray> fields = m_config.certificate.trimmed().split(' ');
        ssh_key certificate = nullptr;
        if (fields.size() < 2
                || ssh_pki_import_cert_base64(fields.at(1).constData(), ssh_key_type_from_name(fields.at(0).constData()),
                                              &certificate) != SSH_OK
                || ssh_pki_copy_cert_to_privkey(certificate, key) != SSH_OK)
            emit info(tr("The key's certificate could not be used, logging in with the plain key"));
        ssh_key_free(certificate);
    }

    static QString promptLabel(const QString &prompt)
    {
        QString label = prompt.simplified();
        while (label.endsWith(QLatin1Char(':')) || label.endsWith(QLatin1Char('?')))
            label.chop(1);
        label = label.trimmed();
        if (label.isEmpty() || label.size() > 40)
            return tr("Answer");
        return label;
    }

    bool keyboardInteractive(ssh_session session, QByteArray password, bool jump)
    {
        int rc = ssh_userauth_kbdint(session, nullptr, nullptr);
        for (int round = 0; rc == SSH_AUTH_INFO; ++round) {
            if (m_stop.loadAcquire())
                return false;
            if (round >= MaxInteractiveRounds)
                return fail(session, tr("The server kept asking questions"), false, jump);
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
                } else if (!ask(QStringList(header + QStringList(prompt)).join(QLatin1Char('\n')), promptLabel(prompt), echo, &answer,
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
    bool ask(const QString &text, const QString &label, bool echo, QByteArray *answer, bool canRemember = false)
    {
        if (m_config.noPrompts) {
            report(tr("The login needs an answer"), false, false);
            return false;
        }
        QMutexLocker locker(&m_promptMutex);
        m_promptAnswered = false;
        emit promptRequested(text, echo, canRemember, label);
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
            if (listenOn(forward, false))
                emit info(tr("Forwarding local port %1 to %2:%3")
                          .arg(forward.localPort).arg(QString::fromUtf8(forward.remoteHost)).arg(forward.remotePort));
        }
        for (int port : m_config.socksPorts) {
            if (listenOn(Forward { port, QByteArray(), 0 }, true))
                emit info(tr("SOCKS proxy on local port %1").arg(port));
        }
    }

    bool listenOn(const Forward &forward, bool socks)
    {
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
            return false;
        }
        m_listeners.append(Listener { fd, forward, socks });
        return true;
    }

    void requestRemoteForwards(ssh_session session)
    {
        for (const RemoteForward &forward : m_config.remoteForwards) {
            // Only on the server's loopback, as OpenSSH does by default
            if (ssh_channel_listen_forward(session, "localhost", forward.remotePort, nullptr) == SSH_OK)
                emit info(tr("Forwarding remote port %1 to %2:%3")
                          .arg(forward.remotePort).arg(QString::fromUtf8(forward.host)).arg(forward.port));
            else
                emit info(tr("The server did not listen on port %1: %2")
                          .arg(forward.remotePort).arg(QString::fromUtf8(ssh_get_error(session))));
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
            while (!throttled()) {
                const int n = ssh_channel_read_nonblocking(channel, buffer, sizeof(buffer), stream);
                if (n == SSH_EOF) {
                    *eof = true;
                    break;
                }
                if (n == SSH_ERROR)
                    return fail(session, tr("Read failed"), true);
                if (n == 0)
                    break;
                m_unconsumed.fetchAndAddOrdered(n);
                emit dataReceived(QByteArray(buffer, n));
            }
        }
        return true;
    }

    bool throttled() const
    {
        return m_unconsumed.loadAcquire() > MaxUnconsumedBytes;
    }

    // Moves what arrived on forwarded and agent channels along, false on a connection error
    bool serviceStreams(ssh_session session)
    {
        m_streams.append(m_newStreams);
        m_newStreams.clear();

        char buffer[16384];
        for (Stream *stream : m_streams) {
            if (!stream->channel)
                continue;
            while (wantsChannelData(stream)) {
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
                if (m_agent && stream->toSocket.size() < MaxForwardBuffer)
                    stream->toSocket.append(m_agent->process(&stream->agentInput));
                const int length = int(qMin<quint32>(quint32(stream->toSocket.size()), ssh_channel_window_size(stream->channel)));
                if (length > 0 && ssh_channel_write(stream->channel, stream->toSocket.constData(), uint32_t(length)) == SSH_ERROR) {
                    if (!ssh_is_connected(session))
                        return fail(session, tr("Write failed"), true);
                    stream->closed = true;
                }
                stream->toSocket.remove(0, length);
                if (stream->channelEof)
                    stream->closed = true;
                continue;
            }

            flushToSocket(stream);
            // The other side may still answer after the server's side ended, as with nc
            if (stream->channelEof && stream->toSocket.isEmpty()) {
                if (stream->socketEof || !ssh_channel_is_open(stream->channel)) {
                    stream->closed = true;
                } else if (!stream->writeShutdown) {
                    ::shutdown(stream->fd, SHUT_WR);
                    stream->writeShutdown = true;
                }
            }
        }
        sweepStreams();
        return true;
    }

    static bool wantsChannelData(const Stream *stream)
    {
        if (stream->channelEof || stream->toSocket.size() >= MaxForwardBuffer)
            return false;
        return stream->fd >= 0 || stream->agentInput.size() < 2 * MaxForwardBuffer;
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
        if (!stream->channel) {
            readSocks(session, stream);
            return true;
        }
        char buffer[16384];
        const uint32_t window = ssh_channel_window_size(stream->channel);
        if (window == 0)
            return true;
        const ssize_t n = ::recv(stream->fd, buffer, qMin<size_t>(sizeof(buffer), window), 0);
        if (n > 0) {
            if (ssh_channel_write(stream->channel, buffer, uint32_t(n)) == SSH_ERROR) {
                if (!ssh_is_connected(session))
                    return fail(session, tr("Write failed"), true);
                stream->closed = true;
            }
        } else if (n == 0) {
            stream->socketEof = true;
            ssh_channel_send_eof(stream->channel);
            if (stream->channelEof && stream->toSocket.isEmpty())
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
        if (listener.socks) {
            // The channel opens once the client has said where to
            Stream *stream = new Stream(nullptr, fd);
            stream->socksPort = listener.forward.localPort;
            m_streams.append(stream);
            return;
        }
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
        m_streams.append(new Stream(channel, fd));
    }

    // Reads a SOCKS 4, 4a or 5 CONNECT request and opens the channel it asks for
    void readSocks(ssh_session session, Stream *stream)
    {
        char buffer[MaxSocksRequest];
        const ssize_t n = ::recv(stream->fd, buffer, sizeof(buffer), 0);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            stream->closed = true;
            return;
        }
        if (n < 0)
            return;
        QByteArray &input = stream->socksInput;
        input.append(buffer, int(n));
        const auto reply = [stream](const QByteArray &bytes) {
            if (::send(stream->fd, bytes.constData(), size_t(bytes.size()), MSG_NOSIGNAL) != bytes.size())
                stream->closed = true;
        };
        const auto refuse = [stream, &reply](const QByteArray &bytes) {
            reply(bytes);
            stream->closed = true;
        };

        QByteArray host;
        int port = 0;
        int consumed = 0;
        QByteArray success;
        QByteArray failure;
        if (input.at(0) == 4) {
            // VN CD DSTPORT DSTIP USERID NUL, then DOMAIN NUL for 4a's 0.0.0.x
            success = QByteArray("\x00\x5a\x00\x00\x00\x00\x00\x00", 8);
            failure = QByteArray("\x00\x5b\x00\x00\x00\x00\x00\x00", 8);
            const int userEnd = input.size() >= 9 ? input.indexOf('\0', 8) : -1;
            const bool domain = input.size() >= 8 && input.at(4) == 0 && input.at(5) == 0 && input.at(6) == 0
                    && input.at(7) != 0;
            const int domainEnd = userEnd >= 0 && domain ? input.indexOf('\0', userEnd + 1) : -1;
            if (userEnd < 0 || (domain && domainEnd < 0)) {
                if (input.size() >= MaxSocksRequest)
                    stream->closed = true;
                return;
            }
            if (input.at(1) != 1) {
                refuse(failure);
                return;
            }
            port = (quint8(input.at(2)) << 8) | quint8(input.at(3));
            if (domain) {
                host = input.mid(userEnd + 1, domainEnd - userEnd - 1);
                consumed = domainEnd + 1;
            } else {
                host = QByteArray::number(quint8(input.at(4))) + '.' + QByteArray::number(quint8(input.at(5))) + '.'
                        + QByteArray::number(quint8(input.at(6))) + '.' + QByteArray::number(quint8(input.at(7)));
                consumed = userEnd + 1;
            }
        } else if (input.at(0) == 5) {
            if (!stream->socksGreeted) {
                // VER NMETHODS METHODS, and only no authentication is offered
                if (input.size() < 2 || input.size() < 2 + quint8(input.at(1)))
                    return;
                const int methods = quint8(input.at(1));
                if (!input.mid(2, methods).contains('\0')) {
                    refuse(QByteArray("\x05\xff", 2));
                    return;
                }
                reply(QByteArray("\x05\x00", 2));
                input.remove(0, 2 + methods);
                stream->socksGreeted = true;
            }
            // VER CMD RSV ATYP DST.ADDR DST.PORT
            success = QByteArray("\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00", 10);
            failure = QByteArray("\x05\x01\x00\x01\x00\x00\x00\x00\x00\x00", 10);
            if (input.size() < 5)
                return;
            int addressEnd;
            switch (input.at(3)) {
            case 1:
                addressEnd = 8;
                break;
            case 3:
                addressEnd = 5 + quint8(input.at(4));
                break;
            case 4:
                addressEnd = 20;
                break;
            default:
                refuse(QByteArray("\x05\x08\x00\x01\x00\x00\x00\x00\x00\x00", 10));
                return;
            }
            if (input.size() < addressEnd + 2)
                return;
            if (input.at(1) != 1) {
                refuse(QByteArray("\x05\x07\x00\x01\x00\x00\x00\x00\x00\x00", 10));
                return;
            }
            if (input.at(3) == 1) {
                host = QByteArray::number(quint8(input.at(4))) + '.' + QByteArray::number(quint8(input.at(5))) + '.'
                        + QByteArray::number(quint8(input.at(6))) + '.' + QByteArray::number(quint8(input.at(7)));
            } else if (input.at(3) == 3) {
                host = input.mid(5, quint8(input.at(4)));
            } else {
                char text[INET6_ADDRSTRLEN];
                if (!inet_ntop(AF_INET6, input.constData() + 4, text, sizeof(text))) {
                    refuse(failure);
                    return;
                }
                host = text;
            }
            port = (quint8(input.at(addressEnd)) << 8) | quint8(input.at(addressEnd + 1));
            consumed = addressEnd + 2;
        } else {
            stream->closed = true;
            return;
        }

        ssh_channel channel = ssh_channel_new(session);
        if (!channel || host.isEmpty()
                || ssh_channel_open_forward(channel, host.constData(), port, "127.0.0.1", stream->socksPort) != SSH_OK) {
            if (channel)
                ssh_channel_free(channel);
            refuse(failure);
            return;
        }
        reply(success);
        stream->channel = channel;
        // Whatever the client sent right after the request
        const QByteArray rest = input.mid(consumed);
        input.clear();
        if (!rest.isEmpty() && ssh_channel_write(channel, rest.constData(), uint32_t(rest.size())) == SSH_ERROR)
            stream->closed = true;
    }

    void closeStream(Stream *stream)
    {
        if (stream->fd >= 0)
            close(stream->fd);
        if (stream->channel) {
            if (ssh_channel_is_open(stream->channel)) {
                ssh_channel_send_eof(stream->channel);
                ssh_channel_close(stream->channel);
            }
            ssh_channel_free(stream->channel);
        }
        delete stream;
    }

    void sweepStreams()
    {
        for (int i = m_streams.size() - 1; i >= 0; --i) {
            if (m_streams.at(i)->closed)
                closeStream(m_streams.takeAt(i));
        }
    }

    // The SSH side of the loop, false on a connection error, which is reported.
    // shellEnded is set when the server closed the shell.
    bool serviceSsh(ssh_session session, ssh_channel channel, QElapsedTimer *idle, bool *shellEnded)
    {
        if (channel) {
            bool eof = false;
            if (!readAvailable(session, channel, &eof))
                return false;
            if (eof || !ssh_channel_is_open(channel) || ssh_channel_is_eof(channel)) {
                *shellEnded = true;
                return true;
            }
        }
        if (!serviceStreams(session))
            return false;
        serviceSystemProbe();
        if (!serviceStatusChannel(session))
            return false;
        if (m_sftp && !m_sftp->service())
            return fail(session, tr("Connection lost"), true);
        if (idle->hasExpired(qint64(m_config.keepAliveInterval) * 1000)) {
            if (ssh_send_ignore(session, "keepalive") != SSH_OK)
                return fail(session, tr("Connection lost"), true);
            idle->restart();
        }
        return true;
    }

    void handleSftp(const QList<SftpRequest> &requests, ssh_session session, bool sshUp)
    {
        if (requests.isEmpty())
            return;
        if (!sshUp) {
            SftpEngine closed(session, sftpCallbacks());
            for (const SftpRequest &request : requests)
                closed.add(request);
            closed.failAll(tr("The SSH connection has closed, reconnect for files"));
            return;
        }
        if (!m_sftp)
            m_sftp = new SftpEngine(session, sftpCallbacks());
        for (const SftpRequest &request : requests)
            m_sftp->add(request);
    }

    SftpEngine::Callbacks sftpCallbacks()
    {
        SftpEngine::Callbacks callbacks;
        callbacks.listed = [this](int id, const QString &path, const QVariantList &entries, const QString &error) {
            emit sftpListed(id, path, entries, error);
        };
        callbacks.done = [this](int id, const QString &error) { emit sftpDone(id, error); };
        callbacks.progress = [this](int id, qint64 bytes, qint64 total) { emit sftpProgress(id, bytes, total); };
        callbacks.transferred = [this](int id, const QString &localPath, const QString &error) {
            emit sftpTransferred(id, localPath, error);
        };
        return callbacks;
    }

    // Passes on what mosh has for the terminal, false once the session is over
    bool serviceMosh()
    {
        // A resumed session that never answers is gone from the server
        if (m_moshResumed && !m_mosh->everHeard() && m_mosh->silence() >= MoshResumeTimeoutMs) {
            QFile::remove(m_config.journalPath);
            m_moshGone = true;
            emit moshGone();
            m_keepMosh.storeRelease(1);
            return false;
        }
        m_mosh->tick();
        for (const MoshClient::Output &output : m_mosh->takeOutput())
            emit moshData(output.replace ? output.screen : QByteArray(), output.bytes, output.skipLines);
        // After the screen it confirms
        if (m_mosh->echoedBytes() != m_moshEchoed) {
            m_moshEchoed = m_mosh->echoedBytes();
            emit moshEcho(m_moshEchoed, m_mosh->roundTrip());
        }
        const qint64 silence = m_mosh->silence();
        const int seconds = silence >= MoshSilenceMs ? int(silence / 1000) : 0;
        if (seconds != m_moshSilence) {
            m_moshSilence = seconds;
            emit moshSilence(seconds);
        }
        if (!m_mosh->everHeard() && silence >= MoshNothingHeardMs && !m_moshNothingHeardReported) {
            m_moshNothingHeardReported = true;
            emit info(tr("Nothing came back from mosh-server on UDP port %1, a firewall may be in the way")
                      .arg(m_moshPort));
        }
        return !m_mosh->finished();
    }

    // Sleeps in poll() until the server sends something, another thread
    // wakes it, a forwarded socket is ready, or a keepalive is due, so an
    // idle connection costs no wakeups
    // sshUp is false for a resumed mosh session, which has no SSH connection
    void readLoop(ssh_session session, ssh_channel *channel, bool sshUp = true)
    {
        const socket_t fd = sshUp ? ssh_get_fd(session) : -1;
        QElapsedTimer idle;
        idle.start();
        while (!m_stop.loadAcquire()) {
            drainWakePipe();

            QByteArray pending;
            bool resizePending;
            bool roamPending;
            int columns;
            int rows;
            QList<SftpRequest> sftpRequests;
            {
                QMutexLocker locker(&m_writeMutex);
                pending.swap(m_pendingWrite);
                resizePending = m_resizePending;
                m_resizePending = false;
                roamPending = m_roamPending;
                m_roamPending = false;
                columns = m_columns;
                rows = m_rows;
                sftpRequests.swap(m_sftpRequests);
            }
            if (m_mosh) {
                if (roamPending)
                    m_mosh->roam();
                if (resizePending)
                    m_mosh->resize(columns, rows);
                m_mosh->write(pending);
            } else if (*channel) {
                if (resizePending)
                    ssh_channel_change_pty_size(*channel, columns, rows);
                if (!pending.isEmpty()) {
                    if (ssh_channel_write(*channel, pending.constData(), pending.size()) == SSH_ERROR) {
                        fail(session, tr("Write failed"), true);
                        return;
                    }
                    idle.restart();
                }
            }

            handleSftp(sftpRequests, session, sshUp);
            if (sshUp) {
                bool shellEnded = false;
                if (!serviceSsh(session, *channel, &idle, &shellEnded)) {
                    // Without mosh the connection is over, with it only files and forwards are
                    if (!m_mosh)
                        return;
                    sshUp = false;
                    closeSsh(session, channel);
                    emit sshClosed();
                } else if (shellEnded) {
                    break;
                }
            }
            if (m_mosh && !serviceMosh()) {
                // Not an ending shell, the session reconnects anew
                if (m_moshGone)
                    return;
                if (!m_mosh->errorString().isEmpty()) {
                    emit failed(m_mosh->errorString(), false);
                    return;
                }
                break;
            }

            QVector<pollfd> fds;
            // While throttled consumed() wakes the loop, the socket would keep it spinning,
            // and so would it with no channel to read what comes. Then the keepalive notices
            // a connection that went.
            const bool reading = *channel || m_statusChannel || m_sftp || m_systemProbe || !m_streams.isEmpty();
            fds.append(pollfd { sshUp ? fd : -1, short(throttled() || !reading ? 0 : POLLIN), 0 });
            fds.append(pollfd { m_wakePipe[0], POLLIN, 0 });
            const int firstListener = fds.size();
            for (const Listener &listener : m_listeners)
                fds.append(pollfd { listener.fd, POLLIN, 0 });
            const int firstStream = fds.size();
            QList<Stream *> polledStreams;
            for (Stream *stream : m_streams) {
                if (stream->fd < 0)
                    continue;
                short events = stream->socketEof || (stream->channel && ssh_channel_window_size(stream->channel) == 0)
                        ? 0 : POLLIN;
                if (!stream->toSocket.isEmpty())
                    events |= POLLOUT;
                fds.append(pollfd { stream->fd, events, 0 });
                polledStreams.append(stream);
            }
            const int firstMosh = fds.size();
            const QVector<int> moshFds = m_mosh ? m_mosh->fds() : QVector<int>();
            for (int moshFd : moshFds)
                fds.append(pollfd { moshFd, POLLIN, 0 });

            bool buffered = false;
            if (sshUp) {
                buffered = *channel && !throttled() && (ssh_channel_poll(*channel, 0) > 0 || ssh_channel_poll(*channel, 1) > 0);
                for (int i = 0; i < m_streams.size() && !buffered; ++i) {
                    const Stream *stream = m_streams.at(i);
                    buffered = stream->channel && wantsChannelData(stream) && ssh_channel_poll(stream->channel, 0) > 0;
                }
                if (m_systemProbe)
                    buffered = buffered || ssh_channel_poll(m_systemProbe, 0) > 0;
                if (m_statusChannel)
                    buffered = buffered || ssh_channel_poll(m_statusChannel, 0) > 0;
                if (m_sftp)
                    buffered = buffered || m_sftp->ready();
            }
            qint64 timeout = buffered ? 0 : INT_MAX;
            if (sshUp)
                timeout = qMin<qint64>(timeout, qMax<qint64>(0, qint64(m_config.keepAliveInterval) * 1000 - idle.elapsed()));
            if (m_systemProbe)
                timeout = qMin<qint64>(timeout, qMax<qint64>(0, SystemProbeTimeoutMs - m_systemProbeTime.elapsed()) + 1);
            if (m_mosh) {
                timeout = qMin<qint64>(timeout, m_mosh->waitTime());
                // Counts the seconds while the server is quiet
                if (m_mosh->silence() >= MoshSilenceMs - 1000 || !m_mosh->everHeard())
                    timeout = qMin<qint64>(timeout, 1000);
            }
            const int ready = poll(fds.data(), nfds_t(fds.size()), int(timeout));
            if (ready < 0 && errno != EINTR) {
                report(tr("Connection lost"), true, false);
                return;
            }
            if (ready <= 0)
                continue;
            if (fds.at(0).revents & POLLIN)
                idle.restart();

            for (int i = 0; i < moshFds.size(); ++i) {
                if (fds.at(firstMosh + i).revents & POLLIN)
                    m_mosh->readable(moshFds.at(i));
            }
            bool sshFailed = false;
            for (int i = 0; i < polledStreams.size() && !sshFailed; ++i) {
                Stream *stream = polledStreams.at(i);
                const short revents = fds.at(firstStream + i).revents;
                if (revents & POLLOUT)
                    flushToSocket(stream);
                if (revents & POLLIN) {
                    sshFailed = !readSocket(session, stream);
                    idle.restart();
                } else if (revents & (POLLHUP | POLLERR)) {
                    stream->closed = true;
                }
            }
            if (sshFailed) {
                if (!m_mosh)
                    return;
                sshUp = false;
                closeSsh(session, channel);
                emit sshClosed();
                continue;
            }
            sweepStreams();
            for (int i = 0; i < m_listeners.size(); ++i) {
                if (fds.at(firstListener + i).revents & POLLIN)
                    acceptForward(session, m_listeners.at(i));
            }
        }
        // Failures return above, so the server ended the shell unless we were stopped
        if (m_stop.loadAcquire())
            return;
        uint32_t status = 0;
        char *signal = nullptr;
        int coreDumped = 0;
        const bool known = *channel && ssh_channel_get_exit_state(*channel, &status, &signal, &coreDumped) == SSH_OK
                && !signal;
        ssh_string_free_char(signal);
        emit shellExited(known ? int(status) : -1);
    }

    void report(const QString &message, bool network, bool jump)
    {
        if (m_stop.loadAcquire())
            return;
        // Under mosh only the files and forwards go with the SSH connection
        if (m_mosh) {
            emit info(tr("The SSH connection closed: %1. The terminal goes on over mosh, files and port forwards stop.")
                      .arg(message));
            return;
        }
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
    bool m_roamPending;
    QList<SftpRequest> m_sftpRequests;
    int m_wakePipe[2];
    QMutex m_promptMutex;
    QWaitCondition m_promptCondition;
    bool m_promptAnswered;
    QByteArray m_promptAnswer;
    SshAgent *m_agent;
    // Loaded before connecting, until authentication takes them
    ssh_key m_key;
    ssh_key m_jumpKey;
    JumpProxy *m_jumpProxy;
    ssh_channel m_systemProbe;
    QByteArray m_systemProbeOutput;
    QElapsedTimer m_systemProbeTime;
    QMutex m_socketMutex;
    QList<int> m_sockets;
    QAtomicInt m_unconsumed;
    QString m_localAddress;
    struct ssh_callbacks_struct m_callbacks;
    QList<Stream *> m_streams;
    // Agent channels opened from inside libssh while m_streams is being walked
    QList<Stream *> m_newStreams;
    QList<Listener> m_listeners;
    SftpEngine *m_sftp;
    ssh_channel m_statusChannel;
    QByteArray m_statusInput;
    MoshClient *m_mosh;
    int m_moshSilence;
    int m_moshPort;
    bool m_moshNothingHeardReported;
    bool m_moshResumed;
    bool m_moshGone;
    qint64 m_moshEchoed;
    QAtomicInt m_keepMosh;
};

SshSession::SshSession(const QString &name, const QString &host, int port, const QString &user,
                       QObject *parent)
    : QObject(parent)
    , m_name(name)
    , m_ownTmux(false)
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
    , m_usesMosh(false)
    , m_silentSeconds(0)
    , m_sshUp(false)
    , m_files(new SftpBrowser(this))
    , m_sideWorker(nullptr)
    , m_sideFetchPending(false)
    , m_sideUp(false)
    , m_moshVault(nullptr)
    , m_resuming(false)
    , m_connectAfterWorker(false)
    , m_log(nullptr)
    , m_typedBytes(0)
    , m_predictionTrusted(false)
    , m_predictionEnabled(true)
    , m_roundTrip(0)
    , m_predictionMinRoundTrip(50)
    , m_typedUnknown(false)
    , m_typedEscape(0)
    , m_typedPaste(false)
{
    qRegisterMetaType<SharedSshKey>();
    connect(m_terminal, &Terminal::outputReady, this, &SshSession::onTerminalOutput);
    connect(m_terminal, &Terminal::sizeChanged, this, &SshSession::onTerminalSizeChanged);
    connect(m_terminal, &Terminal::lineScrolled, this, &SshSession::writeLog);
    // Long enough to read, a notice is not a log
    m_noticeTimer.setSingleShot(true);
    m_noticeTimer.setInterval(NoticeMs);
    connect(&m_noticeTimer, &QTimer::timeout, this, [this]() { setNotice(QString()); });
    m_sideRetry.setSingleShot(true);
    connect(&m_sideRetry, &QTimer::timeout, this, &SshSession::startSideWorker);
}

SshSession::~SshSession()
{
    stopSideWorker();
    // The app is going, a mosh session stays for the next start to resume
    stopWorker(true);
    finishLog();
}

void SshSession::startLog()
{
    finishLog();
    if (!m_options.logging)
        return;
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
            + QStringLiteral("/Longterm");
    QDir().mkpath(directory);
    QString name = (m_name.isEmpty() ? m_user + QLatin1Char('@') + m_host : m_name);
    name.replace(QRegExp(QStringLiteral("[/\\\\:*?\"<>|\\x0000-\\x001f]")), QStringLiteral("_"));
    m_log = new QFile(QStringLiteral("%1/%2 %3.log").arg(directory, name,
                                                         QDate::currentDate().toString(Qt::ISODate)), this);
    if (!m_log->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        m_terminal->write(tr("Could not write the session log: %1").arg(m_log->errorString()).toUtf8() + "\r\n");
        delete m_log;
        m_log = nullptr;
        return;
    }
    writeLog(tr("--- Connected to %1@%2 at %3 ---").arg(m_user, m_host,
                                                         QDateTime::currentDateTime().toString(Qt::ISODate)));
    m_terminal->setReportScrolledLines(true);
    if (m_logPath != m_log->fileName()) {
        m_logPath = m_log->fileName();
        emit logPathChanged();
    }
}

void SshSession::finishLog()
{
    if (!m_log)
        return;
    // What is still on the screen never scrolled off
    QStringList lines;
    for (int row = 0; row < m_terminal->rows(); ++row)
        lines.append(m_terminal->text(row));
    while (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    for (const QString &line : lines)
        writeLog(line);
    writeLog(tr("--- Disconnected at %1 ---").arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
    m_terminal->setReportScrolledLines(false);
    delete m_log;
    m_log = nullptr;
}

void SshSession::writeLog(const QString &line)
{
    if (!m_log)
        return;
    m_log->write(line.toUtf8() + '\n');
    m_log->flush();
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
    // Hosts are configured again on every change to any of them, only another key forgets it
    if (!m_hasJump || m_jumpAuth.vault != vault || m_jumpAuth.secretId != secretId || m_jumpAuth.kind != kind)
        m_unlockedJumpKey.reset();
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
    m_unlockedKey.reset();
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
            const int space = line.indexOf(' ');
            QList<QByteArray> hosts = line.left(space).split(',');
            if (space <= 0 || !hosts.contains(pattern)) {
                kept.append(line);
                continue;
            }
            hosts.removeAll(pattern);
            if (!hosts.isEmpty())
                kept.append(hosts.join(',') + line.mid(space));
        }
        file.close();
        QSaveFile saved(file.fileName());
        if (saved.open(QIODevice::WriteOnly)) {
            saved.write(kept);
            saved.commit();
        }
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
    setPrompt(false, QString(), false, false, QString());
    m_worker->answerPrompt(answer.toUtf8());
}

void SshSession::roam()
{
    if (!m_worker || !m_usesMosh)
        return;
    m_worker->roam();
    // A new network is a good moment to get files and forwards back
    if (!m_sshUp && !m_sideUp) {
        stopSideWorker();
        startSideWorker();
    }
}

bool SshSession::sendSftp(const SftpRequest &request)
{
    if (m_worker && m_state == Connected && m_sshUp)
        m_worker->sftp(request);
    else if (m_sideWorker && m_sideUp)
        m_sideWorker->sftp(request);
    else
        return false;
    return true;
}

void SshSession::sendInput(const QString &text)
{
    if (m_worker && m_state == Connected)
        m_worker->write(text.toUtf8());
}

QString SshSession::scrollbackText() const
{
    QStringList lines;
    for (int row = -m_terminal->scrollbackLines(); row < m_terminal->rows(); ++row)
        lines.append(m_terminal->text(row));
    while (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

QString SshSession::saveScrollback()
{
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QDir().mkpath(directory);
    QString name = (m_name.isEmpty() ? m_user + QLatin1Char('@') + m_host : m_name);
    name.replace(QRegExp(QStringLiteral("[/\\\\:*?\"<>|\\x0000-\\x001f]")), QStringLiteral("_"));
    const QString path = QStringLiteral("%1/%2 %3.txt").arg(directory, name,
            QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH.mm.ss")));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(scrollbackText().toUtf8()) < 0 || !file.commit()) {
        m_errorString = tr("Could not save the scrollback: %1").arg(file.errorString());
        emit errorStringChanged();
        return QString();
    }
    return path;
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
    // Hosts are configured again on every change to any of them, only another key forgets it
    if (m_auth.vault != vault || m_auth.secretId != secretId || m_auth.kind != kind)
        m_unlockedKey.reset();
    m_auth = AuthSource();
    m_auth.vault = vault;
    m_auth.secretId = secretId;
    m_auth.kind = kind;
}

void SshSession::clearStoredSecret()
{
    if (m_auth.vault)
        m_auth = AuthSource();
    m_unlockedKey.reset();
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

QString SshSession::journalPath() const
{
    if (m_sessionId.isEmpty())
        return QString();
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/mosh");
    QDir().mkpath(directory);
    return directory + QLatin1Char('/') + m_sessionId + QStringLiteral(".journal");
}

bool SshSession::canResume() const
{
    return m_moshVault && m_options.mosh && !journalPath().isEmpty() && QFile::exists(journalPath());
}

void SshSession::forgetMosh()
{
    if (journalPath().isEmpty())
        return;
    QFile::remove(journalPath());
    if (m_moshVault)
        m_moshVault->remove(QStringLiteral("mosh") + m_sessionId, this, [](const QString &) {});
}

void SshSession::startConnection()
{
    if (m_state != Disconnected)
        return;
    m_errorString.clear();
    emit errorStringChanged();
    m_secretFetchPending = true;
    setState(Connecting);
    // The mosh session the app left running, no login needed
    if (canResume()) {
        m_moshVault->fetch(QStringLiteral("mosh") + m_sessionId, this, [this](const QByteArray &key, const QString &error) {
            if (!m_secretFetchPending)
                return;
            m_secretFetchPending = false;
            if (!error.isEmpty() || key.isEmpty()) {
                QFile::remove(journalPath());
                setState(Disconnected);
                startConnection();
                return;
            }
            SshCredentials none;
            m_resumeKey = key;
            startWorker(none, none);
            m_resumeKey.fill('\0');
            m_resumeKey.clear();
        });
        return;
    }
    fetchCredentials(m_hasJump ? m_jumpAuth : AuthSource(), [this](const SshCredentials &jumpCredentials) {
        fetchCredentials(m_auth, [this, jumpCredentials](const SshCredentials &credentials) {
            m_secretFetchPending = false;
            startWorker(credentials, jumpCredentials);
        });
    });
}

void SshSession::fetchCredentials(const AuthSource &source, const std::function<void(const SshCredentials &)> &done,
                                  bool side)
{
    if (!source.vault) {
        SshCredentials credentials;
        credentials.password = source.password.toUtf8();
        done(credentials);
        return;
    }
    const SecretKind kind = source.kind;
    source.vault->fetch(source.secretId, this, [this, kind, done, side](const QByteArray &secret, const QString &error) {
        if (side) {
            // Disconnected meanwhile, or the secret is not there to try with
            if (!m_sideFetchPending || !error.isEmpty()) {
                m_sideFetchPending = false;
                return;
            }
        } else if (!m_secretFetchPending) {
            // Disconnected while the secret was being fetched
            return;
        } else if (!error.isEmpty()) {
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

void SshSession::startWorker(const SshCredentials &credentials, const SshCredentials &jumpCredentials, bool side)
{
    if (!side) {
        m_errorString.clear();
        emit errorStringChanged();
        if (m_hostKeyMismatch) {
            m_hostKeyMismatch = false;
            emit hostKeyMismatchChanged();
        }
        m_reachedConnected = false;
        m_localAddress.clear();
        // A name of its own for each connection's status file, a resumed one keeps its own
        if (m_resumeKey.isEmpty())
            m_statusFile = m_options.mosh ? QUuid::createUuid().toRfc4122().toHex() + ".status" : QByteArray();
        else
            m_statusFile = MoshClient::journalNote(journalPath());
    }

    SshWorker::Config config;
    config.host = m_host.toUtf8();
    config.port = m_port;
    config.user = m_user.toUtf8();
    config.credentials = credentials;
    config.credentials.unlockedKey = m_unlockedKey;
    config.hasJump = m_hasJump;
    config.jumpHost = m_jumpHost.toUtf8();
    config.jumpPort = m_jumpPort;
    config.jumpUser = m_jumpUser.toUtf8();
    config.jumpCredentials = jumpCredentials;
    config.jumpCredentials.unlockedKey = m_unlockedJumpKey;
    config.forwardAgent = m_options.forwardAgent;
    config.knownHostsPath = QFile::encodeName(knownHostsPath());
    config.connectTimeout = m_options.connectTimeout > 0 ? m_options.connectTimeout : DefaultConnectTimeout;
    config.keepAliveInterval = m_options.keepAliveInterval > 0 ? m_options.keepAliveInterval : DefaultKeepAliveInterval;
    config.tmuxSession = (m_ownTmux ? m_tmuxSession : m_options.tmuxSession).toUtf8();
    config.moshServer = m_options.moshServer.toUtf8();
    config.command = m_options.command.toUtf8();
    config.certificate = m_certificate;
    config.mosh = m_options.mosh && !side;
    config.statusFile = m_statusFile;
    config.sideOnly = side;
    if (m_options.mosh && !side) {
        config.journalPath = journalPath();
        config.resumeKey = m_resumeKey;
    }
    config.noPrompts = side;
    for (const QString &entry : m_options.environment) {
        const int equals = entry.indexOf(QLatin1Char('='));
        if (equals > 0)
            config.environment.append(qMakePair(entry.left(equals).toUtf8(), entry.mid(equals + 1).toUtf8()));
    }
    for (const QString &entry : m_options.dynamicForwards) {
        const int port = entry.toInt();
        if (port > 0 && port <= 65535)
            config.socksPorts.append(port);
    }
    for (const QString &entry : m_options.remoteForwards) {
        // remotePort:host:localPort, where host may be a bracketed IPv6 address
        const int first = entry.indexOf(QLatin1Char(':'));
        const int last = entry.lastIndexOf(QLatin1Char(':'));
        if (first <= 0 || last <= first)
            continue;
        QString host = entry.mid(first + 1, last - first - 1);
        if (host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']')))
            host = host.mid(1, host.size() - 2);
        const SshWorker::RemoteForward forward { entry.left(first).toInt(), host.toUtf8(), entry.mid(last + 1).toInt() };
        if (forward.remotePort > 0 && forward.remotePort <= 65535 && forward.port > 0 && forward.port <= 65535
                && !host.isEmpty())
            config.remoteForwards.append(forward);
    }
    for (const QString &entry : m_options.localForwards) {
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
        if (forward.localPort > 0 && forward.localPort <= 65535 && forward.remotePort > 0
                && forward.remotePort <= 65535 && !host.isEmpty())
            config.forwards.append(forward);
    }

    SshWorker *worker = new SshWorker(config, m_terminal->columns(), m_terminal->rows());
    config.credentials.password.fill('\0');
    config.credentials.privateKey.fill('\0');
    config.jumpCredentials.password.fill('\0');
    config.jumpCredentials.privateKey.fill('\0');
    if (side) {
        // Quietly, the terminal goes on over mosh whether this works or not
        m_sideWorker = worker;
        connect(worker, &SshWorker::connected, this, [this]() { setSideUp(true); });
        connect(worker, &SshWorker::info, this, &SshSession::onWorkerInfo);
        connect(worker, &SshWorker::statusLine, this, &SshSession::onStatusLine);
        connect(worker, &SshWorker::sftpListed, m_files, &SftpBrowser::onListed);
        connect(worker, &SshWorker::sftpDone, m_files, &SftpBrowser::onDone);
        connect(worker, &SshWorker::sftpProgress, m_files, &SftpBrowser::onProgress);
        connect(worker, &SshWorker::sftpTransferred, m_files, &SftpBrowser::onTransferred);
        connect(worker, &QThread::finished, this, &SshSession::onSideWorkerFinished);
        worker->start();
        return;
    }
    m_worker = worker;
    connect(m_worker, &SshWorker::statusLine, this, &SshSession::onStatusLine);
    connect(m_worker, &SshWorker::moshKey, this, [this](const QByteArray &key) {
        if (m_moshVault)
            m_moshVault->store(QStringLiteral("mosh") + m_sessionId, key, this, [](const QString &) {});
    });
    connect(m_worker, &SshWorker::moshGone, this, [this]() { m_connectAfterWorker = true; });
    connect(m_worker, &SshWorker::moshEcho, this, &SshSession::onMoshEcho);
    connect(m_worker, &SshWorker::keyUnlocked, this, &SshSession::onKeyUnlocked);
    m_typedBytes = 0;
    clearPredictions(false);
    m_resuming = !config.resumeKey.isEmpty();
    connect(m_worker, &SshWorker::connected, this, &SshSession::onWorkerConnected);
    connect(m_worker, &SshWorker::dataReceived, this, &SshSession::onWorkerData);
    connect(m_worker, &SshWorker::info, this, &SshSession::onWorkerInfo);
    connect(m_worker, &SshWorker::failed, this, &SshSession::onWorkerFailed);
    connect(m_worker, &SshWorker::shellExited, this, &SshSession::onShellExited);
    connect(m_worker, &SshWorker::hostKeyChanged, this, &SshSession::onHostKeyChanged);
    connect(m_worker, &SshWorker::promptRequested, this, &SshSession::onWorkerPrompt);
    connect(m_worker, &SshWorker::systemDetected, this, &SshSession::onWorkerSystem);
    connect(m_worker, &SshWorker::moshStarted, this, &SshSession::onMoshStarted);
    connect(m_worker, &SshWorker::moshData, this, &SshSession::onMoshData);
    connect(m_worker, &SshWorker::moshSilence, this, &SshSession::onMoshSilence);
    connect(m_worker, &SshWorker::sshClosed, this, &SshSession::onSshClosed);
    connect(m_worker, &SshWorker::sftpListed, m_files, &SftpBrowser::onListed);
    connect(m_worker, &SshWorker::sftpDone, m_files, &SftpBrowser::onDone);
    connect(m_worker, &SshWorker::sftpProgress, m_files, &SftpBrowser::onProgress);
    connect(m_worker, &SshWorker::sftpTransferred, m_files, &SftpBrowser::onTransferred);
    connect(m_worker, &QThread::finished, this, &SshSession::onWorkerFinished);
    setState(Connecting);
    m_worker->start();
}

void SshSession::disconnectFromHost()
{
    m_lost = false;
    // Ended on purpose, the next connect asks for the passphrase again
    m_unlockedKey.reset();
    m_unlockedJumpKey.reset();
    // Ending the session ends its mosh session too, nothing is left to resume
    if (m_moshVault && !m_sessionId.isEmpty())
        m_moshVault->remove(QStringLiteral("mosh") + m_sessionId, this, [](const QString &) {});
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
    // A resumed mosh session has no SSH connection, one comes up next to it for files
    m_sshUp = !m_resuming;
    setState(Connected);
    startLog();
    if (m_resuming)
        m_sideRetry.start(SideFirstTryMs);
    const QString script = m_startupScript.trimmed();
    if (!script.isEmpty())
        m_worker->write(QString(script + QLatin1Char('\n')).replace(QLatin1Char('\n'), QLatin1Char('\r')).toUtf8());
}

void SshSession::onWorkerData(const QByteArray &data)
{
    m_terminal->write(data);
    // Output of a worker already replaced must not count for the new one
    if (m_worker && sender() == m_worker)
        m_worker->consumed(data.size());
}

void SshSession::onWorkerInfo(const QString &message)
{
    // mosh's screen updates assume the terminal shows exactly what they left,
    // so text written in between would garble it
    if (m_usesMosh) {
        setNotice(message);
        return;
    }
    m_terminal->write(message.toUtf8() + "\r\n");
}

void SshSession::setNotice(const QString &notice)
{
    m_notice = notice;
    emit noticeChanged();
    if (!notice.isEmpty())
        m_noticeTimer.start();
}

void SshSession::onStatusLine(const QByteArray &line)
{
    // "777;payload" or "9;payload", as OSC 777 and OSC 9 would carry it
    const int separator = line.indexOf(';');
    const int command = line.left(separator).toInt();
    if (separator > 0 && (command == 777 || command == 9))
        m_terminal->notifyFromOsc(command, line.mid(separator + 1));
}

void SshSession::startSideWorker()
{
    if (m_sideWorker || m_sideFetchPending || !m_worker || !m_usesMosh || m_state != Connected || m_sshUp)
        return;
    m_sideFetchPending = true;
    fetchCredentials(m_hasJump ? m_jumpAuth : AuthSource(), [this](const SshCredentials &jumpCredentials) {
        fetchCredentials(m_auth, [this, jumpCredentials](const SshCredentials &credentials) {
            if (!m_sideFetchPending)
                return;
            m_sideFetchPending = false;
            if (m_worker && m_usesMosh && !m_sideWorker)
                startWorker(credentials, jumpCredentials, true);
        }, true);
    }, true);
}

void SshSession::stopSideWorker()
{
    m_sideFetchPending = false;
    m_sideRetry.stop();
    setSideUp(false);
    if (!m_sideWorker)
        return;
    m_sideWorker->disconnect(this);
    m_sideWorker->stop();
    connect(m_sideWorker, &QThread::finished, m_sideWorker, &QObject::deleteLater);
    if (m_sideWorker->isFinished())
        m_sideWorker->deleteLater();
    m_sideWorker = nullptr;
}

void SshSession::onSideWorkerFinished()
{
    if (m_sideWorker)
        m_sideWorker->deleteLater();
    m_sideWorker = nullptr;
    setSideUp(false);
    // Tried again while the mosh session lasts
    if (m_worker && m_usesMosh && !m_sshUp)
        m_sideRetry.start(SideRetryMs);
}

void SshSession::setSideUp(bool up)
{
    if (m_sideUp == up)
        return;
    m_sideUp = up;
    emit filesAvailableChanged();
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

void SshSession::setSystem(const QString &id, const QString &name)
{
    if (m_systemId == id && m_systemName == name)
        return;
    m_systemId = id;
    m_systemName = name;
    emit systemChanged();
}

void SshSession::onWorkerSystem(const QString &id, const QString &name, bool certain)
{
    if (!certain && !m_systemId.isEmpty())
        return;
    setSystem(id, name);
}

void SshSession::onKeyUnlocked(const SharedSshKey &key, bool jump)
{
    // Not from a worker that was already replaced
    if (!m_worker || sender() != m_worker)
        return;
    if (jump)
        m_unlockedJumpKey = key;
    else
        m_unlockedKey = key;
}

void SshSession::onWorkerPrompt(const QString &text, bool echo, bool canRemember, const QString &label)
{
    setPrompt(true, text, echo, canRemember, label);
}

void SshSession::onWorkerFinished()
{
    const bool lost = m_lost && m_reachedConnected;
    m_worker->deleteLater();
    m_worker = nullptr;
    m_localAddress.clear();
    setPrompt(false, QString(), false, false, QString());
    setSshUp(false);
    stopSideWorker();
    setNotice(QString());
    m_files->onDisconnected();
    finishLog();
    setUsesMosh(false);
    setSilentSeconds(0);
    m_resuming = false;
    setState(Disconnected);
    if (m_connectAfterWorker) {
        m_connectAfterWorker = false;
        forgetMosh();
        m_terminal->write(tr("The mosh session was gone from the server, connecting again").toUtf8() + "\r\n");
        startConnection();
        return;
    }
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

void SshSession::setOwnTmux(bool ownTmux)
{
    if (m_ownTmux == ownTmux)
        return;
    m_ownTmux = ownTmux;
    emit tmuxChanged();
}

void SshSession::setTmuxSession(const QString &tmuxSession)
{
    if (m_tmuxSession == tmuxSession)
        return;
    m_tmuxSession = tmuxSession;
    emit tmuxChanged();
}

void SshSession::setOptions(const SshOptions &options)
{
    const bool tmux = m_options.tmuxSession != options.tmuxSession;
    m_options = options;
    if (tmux)
        emit tmuxChanged();
}

void SshSession::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    // A new connection starts a new shell, whatever ran in the old one is gone
    if (state == Connecting) {
        m_terminal->clearActivity();
        m_terminal->clearWorkingDirectory();
    }
    emit stateChanged();
    emit filesAvailableChanged();
}

void SshSession::onShellExited(int status)
{
    // A command's output stays on screen to read, a shell's session just disconnects
    if (m_options.command.isEmpty()) {
        emit shellExited();
        return;
    }
    m_terminal->write(QByteArrayLiteral("\r\n"));
    emit commandFinished(status);
}

void SshSession::onMoshStarted()
{
    setUsesMosh(true);
    // mosh-server's own terminal answers the programs, and the screen
    // starts out empty and in its default modes, as mosh-server assumes
    m_terminal->setAnswersQueries(false);
    QByteArray reset = "\033[r\033[0m\033[" + QByteArray::number(m_terminal->rows()) + ";1H";
    reset += QByteArray(m_terminal->rows(), '\n');
    reset += "\033[H\033[?25h\033[?5l\033[?2004l\033[?1000l\033[?1002l\033[?1003l\033[?1004l\033[?1006l";
    m_terminal->write(reset);
}

void SshSession::onMoshData(const QByteArray &screen, const QByteArray &data, int skipLines)
{
    // A screen started over would otherwise go into the scrollback, and so would
    // lines that are in it already
    if (!screen.isEmpty())
        m_terminal->write(screen, false);
    m_terminal->write(data, true, skipLines);
}

void SshSession::onMoshSilence(int seconds)
{
    setSilentSeconds(seconds);
}

void SshSession::onSshClosed()
{
    setSshUp(false);
    // Soon, as only the SSH connection may have gone and not the network
    m_sideRetry.start(SideFirstTryMs);
}

void SshSession::setUsesMosh(bool usesMosh)
{
    if (!usesMosh)
        m_terminal->setAnswersQueries(true);
    if (m_usesMosh == usesMosh)
        return;
    m_usesMosh = usesMosh;
    emit moshChanged();
}

void SshSession::setSilentSeconds(int seconds)
{
    if (m_silentSeconds == seconds)
        return;
    m_silentSeconds = seconds;
    emit silentSecondsChanged();
}

void SshSession::setSshUp(bool up)
{
    if (m_sshUp == up)
        return;
    m_sshUp = up;
    emit filesAvailableChanged();
}

void SshSession::setPrompt(bool prompting, const QString &text, bool echo, bool canRemember, const QString &label)
{
    if (m_prompting == prompting && m_promptText == text && m_promptEcho == echo
            && m_promptCanRemember == canRemember && m_promptLabel == label)
        return;
    m_prompting = prompting;
    m_promptText = text;
    m_promptEcho = echo;
    m_promptLabel = label;
    m_promptCanRemember = canRemember;
    emit promptChanged();
}

void SshSession::onTerminalOutput(const QByteArray &data)
{
    if (m_worker && m_state == Connected) {
        trackTyping(data);
        if (m_usesMosh)
            predict(data);
        m_worker->write(data);
    }
}

// Shows typing before the server echoes it, as mosh's own client does. Each
// line starts out unsure, so nothing typed at a password prompt shows: only
// once the screen confirmed earlier typing on the line do new keys show.
void SshSession::predict(const QByteArray &data)
{
    static const int MaxPredicted = 200;
    m_typedBytes += data.size();
    bool printable = true;
    for (const char byte : data)
        printable = printable && uchar(byte) >= 0x20 && uchar(byte) != 0x7f;
    const QString text = QString::fromUtf8(data);
    int column = m_predictions.isEmpty() ? m_terminal->cursorPosition().col
                                         : m_predictions.last().column + m_predictions.last().text.size();
    const int row = m_predictions.isEmpty() ? m_terminal->cursorPosition().row : m_predictions.last().row;
    // Enter, arrows and editing keys move things in ways only the server knows
    if (!printable || !m_predictionEnabled || m_terminal->altScreen()
            || column + text.size() >= m_terminal->columns() || m_predictions.size() >= MaxPredicted) {
        clearPredictions(false);
        return;
    }
    m_predictions.append(Prediction { text, row, column, m_typedBytes });
    m_predictionAge.start();
    showPredictions();
    // Takes the guesses down if the server never confirms them
    QTimer::singleShot(MaxPredictionAgeMs + 100, this, &SshSession::showPredictions);
}

void SshSession::onMoshEcho(qint64 echoedBytes, int roundTrip)
{
    m_roundTrip = roundTrip;
    while (!m_predictions.isEmpty() && m_predictions.first().endBytes <= echoedBytes) {
        const Prediction prediction = m_predictions.takeFirst();
        // The server has shown it, so the screen says whether it came out as guessed
        const QString shown = m_terminal->text(prediction.row).mid(prediction.column, prediction.text.size());
        if (shown != prediction.text) {
            clearPredictions(false);
            return;
        }
        m_predictionTrusted = true;
    }
    showPredictions();
}

void SshSession::showPredictions()
{
    if (!m_predictions.isEmpty() && m_predictionAge.hasExpired(MaxPredictionAgeMs)) {
        clearPredictions(false);
        return;
    }
    if (m_predictions.isEmpty() || !m_predictionTrusted || m_roundTrip < m_predictionMinRoundTrip) {
        m_terminal->setPrediction(-1, 0, QString());
        return;
    }
    QString text;
    for (const Prediction &prediction : m_predictions)
        text += prediction.text;
    m_terminal->setPrediction(m_predictions.first().row, m_predictions.first().column, text);
}

void SshSession::clearPredictions(bool trusted)
{
    m_predictions.clear();
    m_predictionTrusted = trusted;
    m_terminal->setPrediction(-1, 0, QString());
}

// Follows simple line editing. Arrow keys, Tab and the like change the line in
// ways only the shell knows, so such lines are not kept.
void SshSession::trackTyping(const QByteArray &data)
{
    static const int MaxHistory = 200;
    static const int MaxLine = 4096;
    for (const char byte : data) {
        const uchar c = uchar(byte);
        if (m_typedEscape == 1) {
            // ESC [ starts a CSI sequence, ESC O one more byte, anything else is Alt with a key
            m_typedEscape = c == '[' ? 2 : c == 'O' ? 3 : 0;
            if (m_typedEscape == 0)
                m_typedUnknown = true;
            continue;
        }
        if (m_typedEscape == 2) {
            if (c >= 0x40 && c <= 0x7e) {
                // Bracketed paste marks the text between as typed in one go
                if (c == '~' && (m_typedParameters == "200" || m_typedParameters == "201"))
                    m_typedPaste = m_typedParameters == "200";
                else
                    m_typedUnknown = true;
                m_typedParameters.clear();
                m_typedEscape = 0;
            } else if (m_typedParameters.size() < 16) {
                m_typedParameters.append(byte);
            }
            continue;
        }
        if (m_typedEscape == 3) {
            m_typedEscape = 0;
            m_typedUnknown = true;
            continue;
        }
        if (c == 0x1b) {
            m_typedEscape = 1;
        } else if (c == '\r' && !m_typedPaste) {
            const QString line = QString::fromUtf8(m_typed).trimmed();
            // Only what the screen shows on the cursor's line, typed with echo on
            const bool shown = !line.isEmpty() && !m_terminal->altScreen()
                    && m_terminal->text(m_terminal->cursorPosition().row).contains(line);
            if (!m_typedUnknown && shown) {
                m_history.removeAll(line);
                m_history.append(line);
                while (m_history.size() > MaxHistory)
                    m_history.removeFirst();
                emit historyChanged();
            }
            m_typed.clear();
            m_typedUnknown = false;
        } else if (c == 0x7f || c == 0x08) {
            // One character, which in UTF-8 may be several bytes
            while (!m_typed.isEmpty() && (uchar(m_typed.at(m_typed.size() - 1)) & 0xc0) == 0x80)
                m_typed.chop(1);
            m_typed.chop(1);
        } else if (c == 0x15 || c == 0x03) {
            // Ctrl+U and Ctrl+C start over
            m_typed.clear();
            m_typedUnknown = false;
        } else if (c == 0x17) {
            // Ctrl+W, the last word
            while (m_typed.endsWith(' '))
                m_typed.chop(1);
            while (!m_typed.isEmpty() && !m_typed.endsWith(' '))
                m_typed.chop(1);
        } else if (c < 0x20 && !(m_typedPaste && (c == '\r' || c == '\n' || c == '\t'))) {
            m_typedUnknown = true;
        } else if (m_typed.size() < MaxLine) {
            m_typed.append(byte);
        }
    }
}

void SshSession::onTerminalSizeChanged()
{
    if (m_worker)
        m_worker->resize(m_terminal->columns(), m_terminal->rows());
}

void SshSession::stopWorker(bool keepMosh)
{
    if (!m_worker)
        return;
    m_worker->disconnect(this);
    m_worker->stop(keepMosh);
    if (m_worker->wait(StopWaitMs)) {
        delete m_worker;
    } else {
        connect(m_worker, &QThread::finished, m_worker, &QObject::deleteLater);
        if (m_worker->isFinished())
            m_worker->deleteLater();
    }
    m_worker = nullptr;
}

#include "sshsession.moc"

// Talks to a real mosh-server, which has to be installed, through a relay
// that drops, delays and reorders datagrams. The arguments are the share of
// datagrams dropped, in percent, and a seed for which ones.
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QProcess>
#include <QStringList>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "moshclient.h"
#include "terminal.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

class Random
{
public:
    explicit Random(quint32 seed) : m_state(seed ? seed : 1) {}
    int bounded(int high) { return int(next() % quint32(high)); }

private:
    quint32 next()
    {
        m_state ^= m_state << 13;
        m_state ^= m_state >> 17;
        m_state ^= m_state << 5;
        return m_state;
    }
    quint32 m_state;
};

static QString rowText(const Terminal &t, int row)
{
    QString s;
    for (int c = 0; c < t.columns(); ++c) {
        VTermScreenCell cell = t.cell(row, c);
        if (cell.chars[0] == uint32_t(-1))
            continue;
        s += cell.chars[0] ? QString::fromUcs4(&cell.chars[0], 1) : QStringLiteral(" ");
    }
    while (s.endsWith(QLatin1Char(' ')))
        s.chop(1);
    return s;
}

static QStringList screen(const Terminal &t)
{
    QStringList lines;
    for (int r = 0; r < t.rows(); ++r)
        lines << rowText(t, r);
    return lines;
}

static sockaddr_in loopback(quint16 port)
{
    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

// Sits between client and server, the client sends to it as if it were the server
class Relay
{
public:
    Relay(quint16 serverPort, int dropPercent, quint32 seed)
        : m_random(seed)
        , m_drop(dropPercent)
        , m_haveClient(false)
    {
        m_server = loopback(serverPort);
        m_front = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        sockaddr_in any = loopback(0);
        bind(m_front, reinterpret_cast<sockaddr *>(&any), sizeof(any));
        m_back = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        m_clock.start();
    }

    ~Relay()
    {
        close(m_front);
        close(m_back);
    }

    sockaddr_in address() const
    {
        sockaddr_in address;
        socklen_t length = sizeof(address);
        getsockname(m_front, reinterpret_cast<sockaddr *>(&address), &length);
        return address;
    }

    void setDrop(int percent) { m_drop = percent; }

    void addFds(QVector<pollfd> *fds) const
    {
        fds->append(pollfd { m_front, POLLIN, 0 });
        fds->append(pollfd { m_back, POLLIN, 0 });
    }

    int waitTime() const
    {
        qint64 wait = 50;
        for (const Packet &packet : m_queue)
            wait = qMin(wait, qMax<qint64>(0, packet.due - m_clock.elapsed()));
        return int(wait);
    }

    void service()
    {
        char buffer[4096];
        sockaddr_in from;
        socklen_t length = sizeof(from);
        ssize_t n;
        while ((n = recvfrom(m_front, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr *>(&from), &length)) > 0) {
            // The client roams to new ports, replies go to the latest
            m_client = from;
            m_haveClient = true;
            queue(QByteArray(buffer, int(n)), true);
            length = sizeof(from);
        }
        while ((n = recv(m_back, buffer, sizeof(buffer), 0)) > 0)
            queue(QByteArray(buffer, int(n)), false);

        for (int i = m_queue.size() - 1; i >= 0; --i) {
            const Packet &packet = m_queue.at(i);
            if (packet.due > m_clock.elapsed())
                continue;
            if (packet.toServer)
                sendto(m_back, packet.data.constData(), size_t(packet.data.size()), 0,
                       reinterpret_cast<const sockaddr *>(&m_server), sizeof(m_server));
            else if (m_haveClient)
                sendto(m_front, packet.data.constData(), size_t(packet.data.size()), 0,
                       reinterpret_cast<const sockaddr *>(&m_client), sizeof(m_client));
            m_queue.removeAt(i);
        }
    }

private:
    struct Packet {
        QByteArray data;
        bool toServer;
        qint64 due;
    };

    void queue(const QByteArray &data, bool toServer)
    {
        if (m_random.bounded(100) < m_drop)
            return;
        // Some arrive late, after others sent later
        const int delay = m_drop > 0 ? 5 + m_random.bounded(m_random.bounded(10) == 0 ? 400 : 60) : 0;
        m_queue.append(Packet { data, toServer, m_clock.elapsed() + delay });
    }

    Random m_random;
    int m_drop;
    int m_front;
    int m_back;
    sockaddr_in m_server;
    sockaddr_in m_client;
    bool m_haveClient;
    QList<Packet> m_queue;
    QElapsedTimer m_clock;
};

struct Session {
    Terminal terminal;
    MoshClient client;
    Relay *relay = nullptr;
    int replaced = 0;

    // Runs the client until done() holds or the time is up
    bool runUntil(const std::function<bool()> &done, int timeoutMs)
    {
        QElapsedTimer elapsed;
        elapsed.start();
        while (!elapsed.hasExpired(timeoutMs)) {
            if (done())
                return true;
            QVector<pollfd> fds;
            const QVector<int> clientFds = client.fds();
            for (int fd : clientFds)
                fds.append(pollfd { fd, POLLIN, 0 });
            relay->addFds(&fds);
            const int timeout = qMin(qMin(client.waitTime(), relay->waitTime()), 50);
            poll(fds.data(), nfds_t(fds.size()), timeout);
            relay->service();
            for (int i = 0; i < clientFds.size(); ++i) {
                if (fds.at(i).revents & POLLIN)
                    client.readable(clientFds.at(i));
            }
            client.tick();
            for (const MoshClient::Output &output : client.takeOutput()) {
                if (output.replace)
                    ++replaced;
                terminal.write(output.bytes);
            }
        }
        return done();
    }

    bool screenContains(const QString &text) const
    {
        for (const QString &line : screen(terminal)) {
            if (line.contains(text))
                return true;
        }
        return false;
    }

    QString lastLine() const
    {
        const QStringList lines = screen(terminal);
        for (int i = lines.size() - 1; i >= 0; --i) {
            if (!lines.at(i).isEmpty())
                return lines.at(i);
        }
        return QString();
    }
};

// Starts a detached mosh-server running sh, false when it does not start
static bool startServer(quint16 *port, QByteArray *key)
{
    QProcess server;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PS1"), QStringLiteral("ready$ "));
    environment.insert(QStringLiteral("ENV"), QString());
    server.setProcessEnvironment(environment);
    server.start(QStringLiteral("mosh-server"),
                 { QStringLiteral("new"), QStringLiteral("-i"), QStringLiteral("127.0.0.1"), QStringLiteral("-c"),
                   QStringLiteral("256"), QStringLiteral("-l"), QStringLiteral("LANG=C.UTF-8"),
                   QStringLiteral("--"), QStringLiteral("/bin/sh") });
    if (!server.waitForFinished(10000))
        return false;
    for (const QByteArray &line : server.readAllStandardOutput().split('\n')) {
        const QList<QByteArray> words = line.trimmed().split(' ');
        if (words.size() == 4 && words.at(0) == "MOSH" && words.at(1) == "CONNECT") {
            *port = quint16(words.at(2).toUInt());
            *key = words.at(3);
            return true;
        }
    }
    std::printf("mosh-server said: %s\n", server.readAllStandardError().constData());
    return false;
}

static bool connectSession(Session *session, int drop, quint32 seed)
{
    quint16 port;
    QByteArray key;
    if (!startServer(&port, &key))
        return false;
    session->relay = new Relay(port, drop, seed);
    session->terminal.resize(24, 80);
    const sockaddr_in address = session->relay->address();
    if (!session->client.start(reinterpret_cast<const sockaddr *>(&address), sizeof(address), key, 80, 24)) {
        std::printf("start failed: %s\n", qPrintable(session->client.errorString()));
        return false;
    }
    return true;
}

static void typeLine(Session *session, const QByteArray &line)
{
    session->client.write(line + '\r');
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const int drop = argc > 1 ? QByteArray(argv[1]).toInt() : 25;
    const quint32 seed = argc > 2 ? QByteArray(argv[2]).toUInt() : 1;

    {
        Session session;
        CHECK(connectSession(&session, 0, seed), "could not start mosh-server");
        CHECK(session.runUntil([&]() { return session.lastLine().startsWith(QLatin1String("ready$")); }, 10000),
              "no prompt: %s", qPrintable(screen(session.terminal).join(QLatin1Char('|'))));
        CHECK(session.client.everHeard(), "never heard from the server");

        typeLine(&session, "echo hel''lo; stty size");
        CHECK(session.runUntil([&]() { return session.screenContains(QStringLiteral("24 80")); }, 10000),
              "no stty output: %s", qPrintable(screen(session.terminal).join(QLatin1Char('|'))));
        CHECK(session.screenContains(QStringLiteral("hello")), "no echo output");

        session.terminal.resize(20, 60);
        session.client.resize(60, 20);
        typeLine(&session, "stty size");
        CHECK(session.runUntil([&]() { return session.screenContains(QStringLiteral("20 60")); }, 10000),
              "resize not seen: %s", qPrintable(screen(session.terminal).join(QLatin1Char('|'))));

        // Lots of output while datagrams go missing, the screen still ends up right
        session.relay->setDrop(drop);
        typeLine(&session, "seq 1 3000; echo do''ne");
        CHECK(session.runUntil([&]() { return session.screenContains(QStringLiteral("done"))
                                                      && session.lastLine().startsWith(QLatin1String("ready$")); }, 60000),
              "lossy output did not arrive: %s", qPrintable(screen(session.terminal).join(QLatin1Char('|'))));
        const QStringList lines = screen(session.terminal);
        CHECK(lines.join(QLatin1Char('\n')).contains(QStringLiteral("2999\n3000\ndone")),
              "screen is wrong: %s", qPrintable(lines.join(QLatin1Char('|'))));
        std::printf("screen replaced %d times at %d%% loss\n", session.replaced, drop);

        // Typing while lossy, every key arrives once and in order
        typeLine(&session, "echo a1b2c3d4e5f6g7h8i9");
        CHECK(session.runUntil([&]() { return session.screenContains(QStringLiteral("\na1b2c3d4e5f6g7h8i9").mid(1))
                                                      && session.lastLine().startsWith(QLatin1String("ready$")); }, 30000),
              "typing lost: %s", qPrintable(screen(session.terminal).join(QLatin1Char('|'))));

        // A new socket, as after a network change
        session.relay->setDrop(0);
        session.client.roam();
        typeLine(&session, "echo ro''amed");
        CHECK(session.runUntil([&]() { return session.screenContains(QStringLiteral("roamed")); }, 15000),
              "no output after roaming: %s", qPrintable(screen(session.terminal).join(QLatin1Char('|'))));

        // The shell exits, so the server ends the session
        typeLine(&session, "exit");
        CHECK(session.runUntil([&]() { return session.client.finished(); }, 15000), "server did not end the session");
        CHECK(session.client.errorString().isEmpty(), "error: %s", qPrintable(session.client.errorString()));
        delete session.relay;
    }

    {
        // The client ends the session
        Session session;
        CHECK(connectSession(&session, 0, 2), "could not start mosh-server");
        CHECK(session.runUntil([&]() { return session.lastLine().startsWith(QLatin1String("ready$")); }, 10000), "no prompt");
        session.client.shutdown();
        CHECK(session.runUntil([&]() { return session.client.finished(); }, 15000), "shutdown not acknowledged");
        delete session.relay;
    }

    {
        // A key that is not the server's is never heard back
        Session session;
        quint16 port;
        QByteArray key;
        CHECK(startServer(&port, &key), "could not start mosh-server");
        session.relay = new Relay(port, 0, 3);
        const sockaddr_in address = session.relay->address();
        key[0] = key.at(0) == 'A' ? 'B' : 'A';
        CHECK(session.client.start(reinterpret_cast<const sockaddr *>(&address), sizeof(address), key, 80, 24), "start");
        session.runUntil([]() { return false; }, 2000);
        CHECK(!session.client.everHeard(), "heard with a wrong key");
        delete session.relay;
        std::system("pkill -f 'mosh-server new' >/dev/null 2>&1");
    }

    std::printf(failures ? "%d FAILURES\n" : "OK\n", failures);
    return failures ? 1 : 0;
}

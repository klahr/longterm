// Connects to a real SSH server on 127.0.0.1 port 22 as tester with the
// password secret. The server needs AcceptEnv LONGTERM_*, TCP forwarding,
// mosh-server, tmux and nc. Run it somewhere disposable, it changes the
// tester account's home folder and kills its tmux and mosh servers.
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <cstdio>
#include <functional>

#include "secretvault.h"
#include "sftpbrowser.h"
#include "sshsession.h"
#include "terminal.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); std::fflush(stdout); } } while (0)

static bool waitFor(const std::function<bool()> &done, int timeoutMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!done() && !elapsed.hasExpired(timeoutMs)) {
        QEventLoop loop;
        QTimer::singleShot(20, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return done();
}

static QString screen(SshSession *session)
{
    const Terminal *terminal = session->terminal();
    QStringList lines;
    for (int row = -terminal->scrollbackLines(); row < terminal->rows(); ++row) {
        QString line;
        for (int column = 0; column < terminal->columns(); ++column) {
            const VTermScreenCell cell = terminal->cell(row, column);
            if (cell.chars[0] == uint32_t(-1))
                continue;
            line += cell.chars[0] ? QString::fromUcs4(&cell.chars[0], 1) : QStringLiteral(" ");
        }
        while (line.endsWith(QLatin1Char(' ')))
            line.chop(1);
        lines << line;
    }
    return lines.join(QLatin1Char('\n'));
}

static bool shows(SshSession *session, const QString &text, int timeoutMs = 10000)
{
    return waitFor([&]() { return screen(session).contains(text); }, timeoutMs);
}

static SshSession *connectWith(const SshOptions &options)
{
    SshSession *session = new SshSession(QStringLiteral("test"), QStringLiteral("127.0.0.1"), 22,
                                         QStringLiteral("tester"));
    session->terminal()->resize(30, 100);
    session->setOptions(options);
    session->connectToHost(QStringLiteral("secret"));
    waitFor([&]() { return session->state() == SshSession::Connected || !session->errorString().isEmpty(); }, 20000);
    return session;
}

static void type(SshSession *session, const QByteArray &line)
{
    emit session->terminal()->outputReady(line + '\r');
}

static void disconnect(SshSession *session)
{
    session->disconnectFromHost();
    waitFor([&]() { return session->state() == SshSession::Disconnected; }, 10000);
    delete session;
}

static QString run(const QString &command)
{
    QProcess process;
    process.start(QStringLiteral("sh"), { QStringLiteral("-c"), command });
    process.waitForFinished(20000);
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

static void testShellAndEnvironment()
{
    SshOptions options;
    options.environment = QStringList { QStringLiteral("LONGTERM_TEST=it works"), QStringLiteral("REFUSED_VAR=x") };
    SshSession *session = connectWith(options);
    CHECK(session->state() == SshSession::Connected, "not connected: %s", qPrintable(session->errorString()));
    CHECK(!session->usesMosh(), "mosh without asking for it");
    CHECK(shows(session, QStringLiteral("did not take REFUSED_VAR")), "refused variable not reported");
    type(session, "echo \"[$LONGTERM_TEST]\"");
    CHECK(shows(session, QStringLiteral("[it works]")), "variable not set: %s", qPrintable(screen(session)));

    // The shell tells where it is, the file browser starts there
    type(session, "cd /tmp && printf '\\033]7;file://host/tmp\\007'");
    CHECK(waitFor([&]() { return session->terminal()->workingDirectory() == QLatin1String("/tmp"); }, 5000),
          "working directory: %s", qPrintable(session->terminal()->workingDirectory()));

    // Exiting the shell ends the session rather than failing it
    QSignalSpy exited(session, &SshSession::shellExited);
    type(session, "exit");
    CHECK(waitFor([&]() { return exited.count() > 0; }, 10000), "shell exit not seen");
    disconnect(session);
}

// Needs LONGTERM_TEST_SECRETS/key, a private key the tester account accepts
static void testAgentForwarding()
{
    if (qEnvironmentVariableIsEmpty("LONGTERM_TEST_SECRETS")) {
        std::printf("SKIP agent forwarding, no LONGTERM_TEST_SECRETS\n");
        return;
    }
    SecretVault vault;
    SshOptions options;
    options.forwardAgent = true;
    SshSession *session = new SshSession(QStringLiteral("test"), QStringLiteral("127.0.0.1"), 22, QStringLiteral("tester"));
    session->terminal()->resize(30, 100);
    session->setOptions(options);
    session->connectWithSecret(&vault, QStringLiteral("key"), SshSession::PrivateKey);
    waitFor([&]() { return session->state() == SshSession::Connected || !session->errorString().isEmpty(); }, 20000);
    CHECK(session->state() == SshSession::Connected, "key login: %s", qPrintable(session->errorString()));
    type(session, "ssh-add -l; echo agent-$?");
    CHECK(shows(session, QStringLiteral("agent-0")), "agent not forwarded: %s", qPrintable(screen(session)));
    CHECK(screen(session).contains(QLatin1String("ED25519")), "agent has no key: %s", qPrintable(screen(session)));
    disconnect(session);
}

static void testTmux()
{
    run(QStringLiteral("su tester -c 'tmux kill-server' 2>/dev/null"));
    SshOptions options;
    options.tmuxSession = QStringLiteral("main box");
    SshSession *first = connectWith(options);
    CHECK(first->state() == SshSession::Connected, "not connected: %s", qPrintable(first->errorString()));
    CHECK(shows(first, QStringLiteral("[main box]")), "no tmux status line: %s", qPrintable(screen(first)));
    type(first, "echo first-was-here");

    // A second connection attaches to the same session
    SshSession *second = connectWith(options);
    CHECK(shows(second, QStringLiteral("first-was-here")), "not the same tmux session: %s", qPrintable(screen(second)));
    disconnect(second);
    disconnect(first);
    CHECK(run(QStringLiteral("su tester -c 'tmux ls'")).startsWith(QLatin1String("main box:")), "tmux session gone");
    run(QStringLiteral("su tester -c 'tmux kill-server' 2>/dev/null"));
}

static void testForwards()
{
    // Something on this side for the server to reach
    QTcpServer target;
    target.listen(QHostAddress::LocalHost);
    QObject::connect(&target, &QTcpServer::newConnection, [&target]() {
        QTcpSocket *socket = target.nextPendingConnection();
        socket->write("reached-" + QByteArray::number(socket->localPort()) + "\n");
        socket->disconnectFromHost();
    });

    SshOptions options;
    options.remoteForwards = QStringList { QStringLiteral("2222:127.0.0.1:%1").arg(target.serverPort()) };
    options.dynamicForwards = QStringList { QStringLiteral("10800") };
    options.localForwards = QStringList { QStringLiteral("10801:127.0.0.1:22") };
    SshSession *session = connectWith(options);
    CHECK(session->state() == SshSession::Connected, "not connected: %s", qPrintable(session->errorString()));
    CHECK(shows(session, QStringLiteral("SOCKS proxy on local port 10800")), "no SOCKS proxy: %s", qPrintable(screen(session)));
    CHECK(shows(session, QStringLiteral("Forwarding remote port 2222")), "no remote forward: %s", qPrintable(screen(session)));

    // Remote forward, from the server back to here
    type(session, "nc -q 2 127.0.0.1 2222 < /dev/null");
    CHECK(shows(session, QStringLiteral("reached-%1").arg(target.serverPort())), "remote forward: %s", qPrintable(screen(session)));

    // Local forward to the server's own SSH port
    {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, 10801);
        CHECK(waitFor([&]() { return socket.canReadLine(); }, 10000), "local forward silent");
        CHECK(socket.readLine().startsWith("SSH-2.0"), "local forward reached something else");
    }

    // SOCKS 5 with a host name, the server looks it up
    {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, 10800);
        CHECK(socket.waitForConnected(5000), "SOCKS port closed");
        socket.write(QByteArray("\x05\x01\x00", 3));
        CHECK(waitFor([&]() { return socket.bytesAvailable() >= 2; }, 5000), "no SOCKS greeting");
        CHECK(socket.read(2) == QByteArray("\x05\x00", 2), "SOCKS method refused");
        QByteArray request("\x05\x01\x00\x03", 4);
        request += char(9) + QByteArray("localhost");
        request += char(target.serverPort() >> 8);
        request += char(target.serverPort() & 0xff);
        socket.write(request);
        CHECK(waitFor([&]() { return socket.bytesAvailable() >= 10; }, 10000), "no SOCKS reply");
        CHECK(socket.read(10).at(1) == 0, "SOCKS connect failed");
        CHECK(waitFor([&]() { return socket.canReadLine(); }, 10000), "nothing through SOCKS");
        CHECK(socket.readLine().startsWith("reached"), "SOCKS reached something else");
    }

    // SOCKS 4 with an address
    {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, 10800);
        CHECK(socket.waitForConnected(5000), "SOCKS port closed");
        QByteArray request("\x04\x01", 2);
        request += char(target.serverPort() >> 8);
        request += char(target.serverPort() & 0xff);
        request += QByteArray("\x7f\x00\x00\x01", 4);
        request += QByteArray("user\0", 5);
        socket.write(request);
        CHECK(waitFor([&]() { return socket.bytesAvailable() >= 8; }, 10000), "no SOCKS 4 reply");
        CHECK(socket.read(8).at(1) == 0x5a, "SOCKS 4 connect failed");
        CHECK(waitFor([&]() { return socket.canReadLine(); }, 10000), "nothing through SOCKS 4");
        CHECK(socket.readLine().startsWith("reached"), "SOCKS 4 reached something else");
    }

    // A refused target gets a SOCKS failure, not a hang
    {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, 10800);
        socket.waitForConnected(5000);
        socket.write(QByteArray("\x05\x01\x00\x05\x01\x00\x01\x7f\x00\x00\x01\x00\x01", 13));
        CHECK(waitFor([&]() { return socket.bytesAvailable() >= 12; }, 10000), "no SOCKS failure reply");
        const QByteArray reply = socket.readAll();
        CHECK(reply.size() >= 12 && reply.at(3) != 0, "SOCKS to a closed port did not fail");
    }
    disconnect(session);
}

static QByteArray fileHash(const QString &path)
{
    QFile file(path);
    file.open(QIODevice::ReadOnly);
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

static void testFiles(const SshOptions &options)
{
    run(QStringLiteral("rm -rf /home/tester/upload-test"));
    SshSession *session = connectWith(options);
    CHECK(session->state() == SshSession::Connected, "not connected: %s", qPrintable(session->errorString()));
    CHECK(session->filesAvailable(), "files not available");
    SftpBrowser *files = session->files();

    files->open(QString());
    CHECK(waitFor([&]() { return !files->loading(); }, 10000) && files->path() == QLatin1String("/home/tester"),
          "home folder: '%s' %s", qPrintable(files->path()), qPrintable(files->errorString()));

    files->makeDirectory(QStringLiteral("upload-test"));
    CHECK(waitFor([&]() { return files->contains(QStringLiteral("upload-test")); }, 10000),
          "folder not created: %s", qPrintable(files->errorString()));
    files->open(files->childPath(QStringLiteral("upload-test")));
    CHECK(waitFor([&]() { return files->path().endsWith(QLatin1String("/upload-test")) && !files->loading(); }, 10000),
          "folder not opened");

    // Big enough for many requests in flight, not a multiple of the chunk size
    const QString local = QDir::tempPath() + QStringLiteral("/longterm upload.bin");
    {
        QFile file(local);
        file.open(QIODevice::WriteOnly);
        QByteArray data(5 * 1024 * 1024 + 12345, Qt::Uninitialized);
        for (int i = 0; i < data.size(); ++i)
            data[i] = char((quint32(i) * 7919u) >> 3);
        file.write(data);
    }
    QSignalSpy finished(files, &SftpBrowser::transferFinished);
    files->upload(local, QString(), false);
    CHECK(waitFor([&]() { return finished.count() == 1; }, 60000), "upload did not finish");
    CHECK(finished.value(0).value(3).toString().isEmpty(), "upload failed: %s", qPrintable(finished.value(0).value(3).toString()));
    CHECK(run(QStringLiteral("sha256sum '/home/tester/upload-test/longterm upload.bin' | cut -d' ' -f1"))
          == QString::fromLatin1(fileHash(local).toHex()), "uploaded file differs");
    CHECK(waitFor([&]() { return files->contains(QStringLiteral("longterm upload.bin")); }, 10000), "listing not refreshed");

    // Uploading again without replacing is refused, and leaves the file alone
    files->upload(local, QString(), false);
    CHECK(waitFor([&]() { return finished.count() == 2; }, 30000), "second upload did not finish");
    CHECK(!finished.value(1).value(3).toString().isEmpty(), "existing file replaced without asking");
    CHECK(run(QStringLiteral("stat -c %s '/home/tester/upload-test/longterm upload.bin'")).toInt() == 5 * 1024 * 1024 + 12345,
          "existing file damaged");

    files->rename(QStringLiteral("longterm upload.bin"), QStringLiteral("renamed.bin"));
    CHECK(waitFor([&]() { return files->contains(QStringLiteral("renamed.bin")); }, 10000), "rename: %s", qPrintable(files->errorString()));

    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QFile::remove(downloads + QStringLiteral("/renamed.bin"));
    QFile::remove(downloads + QStringLiteral("/renamed (2).bin"));
    files->download(files->childPath(QStringLiteral("renamed.bin")));
    CHECK(waitFor([&]() { return finished.count() == 3; }, 60000), "download did not finish");
    const QString downloaded = finished.value(2).value(2).toString();
    CHECK(downloaded == downloads + QStringLiteral("/renamed.bin"), "downloaded to %s: %s",
          qPrintable(downloaded), qPrintable(finished.value(2).value(3).toString()));
    CHECK(fileHash(downloaded) == fileHash(local), "downloaded file differs");

    // A second download of the same name does not overwrite the first
    files->download(files->childPath(QStringLiteral("renamed.bin")));
    CHECK(waitFor([&]() { return finished.count() == 4; }, 60000), "second download did not finish");
    CHECK(finished.value(3).value(2).toString() == downloads + QStringLiteral("/renamed (2).bin"),
          "second download went to %s", qPrintable(finished.value(3).value(2).toString()));

    // Cancelled halfway, nothing is left behind
    run(QStringLiteral("head -c 200000000 /dev/zero > /home/tester/upload-test/big.bin"));
    files->download(files->childPath(QStringLiteral("big.bin")));
    CHECK(waitFor([&]() { return !files->transfers().isEmpty()
                                 && files->transfers().first().toMap().value(QStringLiteral("bytes")).toLongLong() > 0; }, 30000),
          "big download not under way");
    files->cancel(0);
    CHECK(waitFor([&]() { return finished.count() == 5; }, 30000), "cancel did not finish");
    CHECK(!finished.value(4).value(3).toString().isEmpty(), "cancelled download succeeded");
    CHECK(!QFile::exists(downloads + QStringLiteral("/big.bin")), "cancelled download left a file");

    // The terminal still works after all that
    type(session, "echo after-files");
    CHECK(shows(session, QStringLiteral("after-files")), "terminal stuck after transfers");

    files->remove(QStringLiteral("renamed.bin"));
    files->remove(QStringLiteral("big.bin"));
    CHECK(waitFor([&]() { return !files->contains(QStringLiteral("renamed.bin")) && !files->contains(QStringLiteral("big.bin")); }, 10000),
          "remove: %s", qPrintable(files->errorString()));
    files->up();
    CHECK(waitFor([&]() { return files->path() == QLatin1String("/home/tester"); }, 10000), "up");
    files->remove(QStringLiteral("upload-test"));
    CHECK(waitFor([&]() { return !files->contains(QStringLiteral("upload-test")); }, 10000),
          "remove folder: %s", qPrintable(files->errorString()));
    disconnect(session);
    QFile::remove(local);
}

static void testMosh()
{
    run(QStringLiteral("pkill mosh-server"));
    SshOptions options;
    options.mosh = true;
    options.environment = QStringList { QStringLiteral("LONGTERM_TEST=through mosh") };
    SshSession *session = connectWith(options);
    CHECK(session->state() == SshSession::Connected, "not connected: %s", qPrintable(session->errorString()));
    CHECK(session->usesMosh(), "not over mosh");
    type(session, "echo \"[$LONGTERM_TEST]\" $(ps -o comm= -p $PPID)");
    CHECK(shows(session, QStringLiteral("[through mosh] mosh-server")), "mosh shell: %s", qPrintable(screen(session)));

    // mosh-server answers the programs itself, an answer from this side would come in twice
    type(session, "printf '\\033[c'; read -rs -t 2 -d c x; read -rs -t 1 -d c y; echo \"<${x#?}|${y#?}>\"");
    CHECK(shows(session, QStringLiteral("<[?62|>")), "terminal answered too: %s", qPrintable(screen(session)));

    // A resize goes through mosh
    session->terminal()->resize(25, 90);
    type(session, "stty size");
    CHECK(shows(session, QStringLiteral("25 90")), "resize: %s", qPrintable(screen(session)));

    // Files go over the SSH connection that started mosh-server
    session->files()->open(QString());
    CHECK(waitFor([&]() { return session->files()->path() == QLatin1String("/home/tester"); }, 10000),
          "files under mosh: %s", qPrintable(session->files()->errorString()));

    // A network change only moves mosh to a new socket
    session->roam();
    type(session, "echo still-here");
    CHECK(shows(session, QStringLiteral("still-here")), "after roaming: %s", qPrintable(screen(session)));

    disconnect(session);
    CHECK(waitFor([]() { return run(QStringLiteral("pgrep -c mosh-server")) == QLatin1String("0"); }, 5000),
          "mosh-server still running after disconnecting");

    // Exiting the shell ends the session
    session = connectWith(options);
    QSignalSpy exited(session, &SshSession::shellExited);
    type(session, "exit");
    CHECK(waitFor([&]() { return exited.count() > 0; }, 15000), "mosh shell exit not seen");
    disconnect(session);

    // With tmux inside
    run(QStringLiteral("su tester -c 'tmux kill-server' 2>/dev/null"));
    options.tmuxSession = QStringLiteral("moshed");
    session = connectWith(options);
    CHECK(shows(session, QStringLiteral("[moshed]")), "no tmux under mosh: %s", qPrintable(screen(session)));
    disconnect(session);
    run(QStringLiteral("su tester -c 'tmux kill-server' 2>/dev/null"));

    // Without mosh on the server the terminal goes over SSH, with the same settings
    run(QStringLiteral("chmod -x /usr/bin/mosh-server"));
    options = SshOptions();
    options.mosh = true;
    options.environment = QStringList { QStringLiteral("LONGTERM_TEST=fell back") };
    session = connectWith(options);
    CHECK(session->state() == SshSession::Connected, "no fallback: %s", qPrintable(session->errorString()));
    CHECK(!session->usesMosh(), "claims mosh without mosh-server");
    CHECK(shows(session, QStringLiteral("going on over SSH")), "fallback not reported: %s", qPrintable(screen(session)));
    type(session, "echo \"[$LONGTERM_TEST]\" $(tty)");
    CHECK(shows(session, QStringLiteral("[fell back] /dev/pts/")), "fallback shell: %s", qPrintable(screen(session)));
    CHECK(screen(session).count(QStringLiteral("did not take")) == 0, "repeated messages: %s", qPrintable(screen(session)));
    disconnect(session);
    run(QStringLiteral("chmod +x /usr/bin/mosh-server"));
}

static void testConnectTimeout()
{
    SshOptions options;
    options.connectTimeout = 2;
    SshSession *session = new SshSession(QStringLiteral("test"), QStringLiteral("10.255.255.1"), 22, QStringLiteral("tester"));
    session->setOptions(options);
    QElapsedTimer elapsed;
    elapsed.start();
    session->connectToHost(QStringLiteral("secret"));
    waitFor([&]() { return !session->errorString().isEmpty(); }, 20000);
    CHECK(elapsed.elapsed() < 5000, "gave up after %lld ms", elapsed.elapsed());
    disconnect(session);
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QDir().mkpath(QFileInfo(SshSession::knownHostsPath()).path());
    // Names of tests to run, all of them without any
    const QStringList only = app.arguments().mid(1);
    const auto wanted = [&only](const char *name) { return only.isEmpty() || only.contains(QLatin1String(name)); };
    if (wanted("shell"))
        testShellAndEnvironment();
    if (wanted("agent"))
        testAgentForwarding();
    if (wanted("tmux"))
        testTmux();
    if (wanted("forwards"))
        testForwards();
    if (wanted("files"))
        testFiles(SshOptions());
    if (wanted("mosh"))
        testMosh();
    if (wanted("timeout"))
        testConnectTimeout();
    std::printf(failures ? "%d FAILURES\n" : "OK\n", failures);
    return failures ? 1 : 0;
}

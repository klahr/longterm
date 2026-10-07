// Connects to a real SSH server on 127.0.0.1 port 22 as tester with the
// password secret, set up by run.sh in the image of the Dockerfile next to
// this. Run it somewhere disposable, it changes the tester account's home
// folder and kills its tmux and mosh servers.
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

#include "appsettings.h"
#include "backup.h"
#include "colorschemes.h"
#include "hoststore.h"
#include "keystore.h"
#include "secretvault.h"
#include "sessionmanager.h"
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

static QString secretsDir()
{
    return QString::fromLocal8Bit(qgetenv("LONGTERM_TEST_SECRETS"));
}

static QByteArray readFile(const QString &path)
{
    QFile file(path);
    file.open(QIODevice::ReadOnly);
    return file.readAll();
}

static void testAgentStatus()
{
    // A question with answers, through the terminal of a plain connection
    SshSession *session = connectWith(SshOptions());
    QSignalSpy asked(session->terminal(), &Terminal::questionAsked);
    type(session, "longterm-status ask Claude 'May I; please' 'Yes=1' 'No=\\e'");
    CHECK(waitFor([&]() { return asked.count() == 1; }, 10000), "no question");
    const QVariantList replies = asked.value(0).value(2).toList();
    CHECK(asked.value(0).value(0).toString() == QLatin1String("Claude") && asked.value(0).value(1).toString() == QLatin1String("May I; please"),
          "question: %s / %s", qPrintable(asked.value(0).value(0).toString()), qPrintable(asked.value(0).value(1).toString()));
    CHECK(replies.size() == 2 && replies.at(0).toMap().value(QStringLiteral("keys")).toString() == QLatin1String("1")
          && replies.at(1).toMap().value(QStringLiteral("keys")).toString() == QLatin1String("\x1b"), "replies wrong");
    disconnect(session);

    // Under mosh the status goes through the file the app follows
    SshOptions options;
    options.mosh = true;
    session = connectWith(options);
    CHECK(session->usesMosh(), "not over mosh");
    type(session, "longterm-status working 'via the file'");
    CHECK(waitFor([&]() { return session->terminal()->activity() == QLatin1String("working")
                                 && session->terminal()->activityDetail() == QLatin1String("via the file"); }, 15000),
          "no status under mosh: '%s'", qPrintable(session->terminal()->activity()));
    disconnect(session);

    // And inside tmux under mosh, which finds the file in the tmux session
    run(QStringLiteral("su tester -c 'tmux kill-server' 2>/dev/null"));
    options.tmuxSession = QStringLiteral("agents");
    session = connectWith(options);
    CHECK(shows(session, QStringLiteral("[agents]")), "no tmux: %s", qPrintable(screen(session)));
    type(session, "longterm-status waiting 'in tmux'");
    CHECK(waitFor([&]() { return session->terminal()->activity() == QLatin1String("waiting"); }, 15000),
          "no status from tmux under mosh");
    disconnect(session);
    run(QStringLiteral("su tester -c 'tmux kill-server' 2>/dev/null"));
}

static void testSideConnection()
{
    // The SSH connection under mosh goes, the terminal stays and files come back
    SshOptions options;
    options.mosh = true;
    SshSession *session = connectWith(options);
    CHECK(session->filesAvailable(), "no files at first");
    run(QStringLiteral("pkill -f 'sshd-session: tester'"));
    CHECK(waitFor([&]() { return !session->filesAvailable(); }, 120000), "SSH connection did not drop");
    type(session, "echo still-typing");
    CHECK(shows(session, QStringLiteral("still-typing")), "terminal gone with SSH: %s", qPrintable(screen(session)));
    CHECK(waitFor([&]() { return session->filesAvailable(); }, 30000), "files did not come back");
    session->files()->open(QString());
    CHECK(waitFor([&]() { return session->files()->path() == QLatin1String("/home/tester"); }, 10000),
          "files after reconnecting: %s", qPrintable(session->files()->errorString()));
    disconnect(session);
}

static void testResume()
{
    run(QStringLiteral("pkill mosh-server"));
    SecretVault vault;
    SshOptions options;
    options.mosh = true;
    const auto open = [&]() {
        SshSession *session = new SshSession(QStringLiteral("test"), QStringLiteral("127.0.0.1"), 22, QStringLiteral("tester"));
        session->terminal()->resize(30, 100);
        session->setOptions(options);
        session->setSessionId(QStringLiteral("resumetest"));
        session->setMoshVault(&vault);
        session->connectToHost(QStringLiteral("secret"));
        waitFor([&]() { return session->state() == SshSession::Connected || !session->errorString().isEmpty(); }, 30000);
        return session;
    };

    SshSession *session = open();
    CHECK(session->usesMosh(), "not over mosh");
    type(session, "echo before-the-re''start");
    CHECK(shows(session, QStringLiteral("before-the-restart")), "no output");
    waitFor([]() { return false; }, 500);
    // The app quits, the mosh session stays on the server
    delete session;
    CHECK(QFile::exists(secretsDir() + QStringLiteral("/moshresumetest")), "mosh key not kept");
    CHECK(run(QStringLiteral("pgrep -c mosh-server")) == QLatin1String("1"), "mosh-server ended with the app");

    session = open();
    CHECK(session->usesMosh() && session->state() == SshSession::Connected, "not resumed: %s", qPrintable(session->errorString()));
    CHECK(shows(session, QStringLiteral("before-the-restart")), "screen not restored: %s", qPrintable(screen(session)));
    type(session, "echo after-the-re''start");
    CHECK(shows(session, QStringLiteral("after-the-restart")), "typing after resuming: %s", qPrintable(screen(session)));
    CHECK(run(QStringLiteral("pgrep -c mosh-server")) == QLatin1String("1"), "a second mosh-server started");
    CHECK(waitFor([&]() { return session->filesAvailable(); }, 30000), "no files next to the resumed session");
    disconnect(session);
    CHECK(waitFor([]() { return run(QStringLiteral("pgrep -c mosh-server")) == QLatin1String("0"); }, 5000),
          "mosh-server left after disconnecting");
    CHECK(!QFile::exists(secretsDir() + QStringLiteral("/moshresumetest")), "mosh key left after disconnecting");

    // A session gone from the server, as after a reboot, gets a new connection instead
    session = open();
    delete session;
    run(QStringLiteral("pkill -9 mosh-server"));
    session = open();
    CHECK(waitFor([&]() { return session->state() == SshSession::Connected && session->usesMosh()
                                 && screen(session).contains(QLatin1String("gone from the server"))
                                 && run(QStringLiteral("pgrep -c mosh-server")) == QLatin1String("1"); }, 60000),
          "no new connection after the session was gone: %s", qPrintable(screen(session)));
    disconnect(session);
}

static void testFolders()
{
    run(QStringLiteral("rm -rf /home/tester/tree /home/tester/uploaded && su tester -c 'mkdir -p ~/tree/sub/deeper && echo one > ~/tree/a.txt"
                       " && echo two > ~/tree/sub/b.txt && head -c 300000 /dev/urandom > ~/tree/sub/deeper/c.bin && : > ~/tree/empty'"));
    SshSession *session = connectWith(SshOptions());
    SftpBrowser *files = session->files();
    files->open(QString());
    waitFor([&]() { return !files->loading(); }, 10000);
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QDir(downloads + QStringLiteral("/tree")).removeRecursively();
    QSignalSpy finished(files, &SftpBrowser::transferFinished);
    files->download(QStringLiteral("/home/tester/tree"));
    CHECK(waitFor([&]() { return finished.count() == 1; }, 60000), "folder download did not finish");
    CHECK(finished.value(0).value(3).toString().isEmpty(), "folder download: %s", qPrintable(finished.value(0).value(3).toString()));
    const QString local = finished.value(0).value(2).toString();
    CHECK(local == downloads + QStringLiteral("/tree"), "went to %s", qPrintable(local));
    CHECK(readFile(local + QStringLiteral("/a.txt")) == "one\n" && readFile(local + QStringLiteral("/sub/b.txt")) == "two\n",
          "small files differ");
    CHECK(QCryptographicHash::hash(readFile(local + QStringLiteral("/sub/deeper/c.bin")), QCryptographicHash::Sha256).toHex()
          == run(QStringLiteral("sha256sum /home/tester/tree/sub/deeper/c.bin | cut -d' ' -f1")).toLatin1(), "big file differs");
    CHECK(QFile::exists(local + QStringLiteral("/empty")), "empty file missing");

    // The same folder back up under another name
    QDir(local).rename(local, downloads + QStringLiteral("/uploaded"));
    files->upload(downloads + QStringLiteral("/uploaded"), QStringLiteral("/home/tester"), false);
    CHECK(waitFor([&]() { return finished.count() == 2; }, 60000), "folder upload did not finish");
    CHECK(finished.value(1).value(3).toString().isEmpty(), "folder upload: %s", qPrintable(finished.value(1).value(3).toString()));
    CHECK(run(QStringLiteral("cd /home/tester && diff -r tree uploaded && echo same")) == QLatin1String("same"), "uploaded folder differs");
    QDir(downloads + QStringLiteral("/uploaded")).removeRecursively();
    disconnect(session);
}

static void testInstallKeyAndBackup()
{
    AppSettings settings;
    SecretVault vault;
    HostStore hosts(&vault);
    KeyStore keys(&vault);
    ColorSchemes schemes;
    SessionManager manager(&vault, &hosts, &keys, &settings);
    for (int row = hosts.count() - 1; row >= 0; --row)
        hosts.removeHost(hosts.hostIdAt(row));

    const QString hostId = hosts.saveHost(QString(), QStringLiteral("installhost"), QStringLiteral("127.0.0.1"), 22,
                                          QStringLiteral("tester"), QString(), QStringLiteral("secret"), true, QVariantMap());
    CHECK(waitFor([&]() { return hosts.host(hostId).value(QStringLiteral("hasPassword")).toBool(); }, 5000), "password not kept");
    const int keysBefore = keys.count();
    keys.generateKey(QStringLiteral("phone key"), QStringLiteral("ed25519"));
    CHECK(waitFor([&]() { return keys.count() == keysBefore + 1; }, 20000), "no key made: %s", qPrintable(keys.errorString()));
    const QString keyId = keys.keyIdAt(keys.count() - 1);

    for (int round = 0; round < 2; ++round) {
        SshSession *session = manager.installKey(hostId, keyId, true);
        QSignalSpy finished(session, &SshSession::commandFinished);
        CHECK(waitFor([&]() { return finished.count() == 1; }, 30000), "install did not finish: %s", qPrintable(screen(session)));
        CHECK(finished.value(0).value(0).toInt() == 0, "install failed: %s", qPrintable(screen(session)));
        CHECK(shows(session, QStringLiteral("The key is installed")), "%s", qPrintable(screen(session)));
        manager.closeSession(session);
    }
    CHECK(run(QStringLiteral("grep -c 'phone key' /home/tester/.ssh/authorized_keys")) == QLatin1String("1"),
          "key installed %s times", qPrintable(run(QStringLiteral("grep -c 'phone key' /home/tester/.ssh/authorized_keys"))));
    CHECK(hosts.host(hostId).value(QStringLiteral("keyId")).toString() == keyId, "host does not use the key");
    CHECK(!hosts.host(hostId).value(QStringLiteral("hasPassword")).toBool(), "password kept with a key");
    SshSession *session = manager.openHost(hostId);
    CHECK(waitFor([&]() { return session->state() == SshSession::Connected; }, 20000), "key login: %s", qPrintable(session->errorString()));
    manager.closeSession(session);

    // A certificate signed by the CA the server trusts logs in with a key it does not list
    const QString certificate = QString::fromLatin1(readFile(secretsDir() + QStringLiteral("/key2-cert.pub")));
    keys.importKey(QStringLiteral("ca key"), QString::fromLatin1(readFile(secretsDir() + QStringLiteral("/key2"))), QString());
    CHECK(waitFor([&]() { return keys.count() == keysBefore + 2; }, 20000), "key not imported: %s", qPrintable(keys.errorString()));
    const QString caKeyId = keys.keyIdAt(keys.count() - 1);
    CHECK(!keys.checkCertificate(keyId, certificate).isEmpty(), "certificate taken for another key");
    CHECK(keys.setCertificate(caKeyId, certificate).isEmpty(), "certificate refused: %s", qPrintable(keys.checkCertificate(caKeyId, certificate)));
    const QString caHostId = hosts.saveHost(QString(), QStringLiteral("cahost"), QStringLiteral("127.0.0.1"), 22,
                                            QStringLiteral("tester"), caKeyId, QString(), false, QVariantMap());
    session = manager.openHost(caHostId);
    CHECK(waitFor([&]() { return session->state() == SshSession::Connected; }, 20000), "certificate login: %s", qPrintable(session->errorString()));
    manager.closeSession(session);
    keys.setCertificate(caKeyId, QString());
    session = manager.openHost(caHostId);
    waitFor([&]() { return session->state() == SshSession::Connected || !session->errorString().isEmpty(); }, 20000);
    CHECK(session->state() != SshSession::Connected, "logged in without the certificate");
    manager.closeSession(session);
    keys.setCertificate(caKeyId, certificate);

    // Everything into a backup, gone, and back
    Backup backup(&vault, &hosts, &keys, &settings, &schemes);
    QSignalSpy exported(&backup, &Backup::exported);
    backup.exportBackup(QStringLiteral("correct horse"));
    CHECK(waitFor([&]() { return exported.count() == 1; }, 30000), "backup did not finish");
    const QString path = exported.value(0).value(0).toString();
    CHECK(!path.isEmpty(), "backup: %s", qPrintable(exported.value(0).value(1).toString()));
    const QByteArray contents = readFile(path);
    CHECK(!contents.contains("installhost") && !contents.contains("BEGIN OPENSSH"), "backup not encrypted");
    hosts.removeHost(hostId);
    hosts.removeHost(caHostId);
    keys.removeKey(keyId);
    keys.removeKey(caKeyId);
    waitFor([]() { return false; }, 500);

    QSignalSpy imported(&backup, &Backup::imported);
    backup.importBackup(path, QStringLiteral("wrong horse"));
    CHECK(waitFor([&]() { return imported.count() == 1; }, 30000), "wrong passphrase did not finish");
    CHECK(!imported.value(0).value(1).toString().isEmpty() && hosts.indexOf(hostId) < 0, "wrong passphrase accepted");
    backup.importBackup(path, QStringLiteral("correct horse"));
    CHECK(waitFor([&]() { return imported.count() == 2; }, 30000), "restore did not finish");
    CHECK(imported.value(1).value(1).toString().isEmpty(), "restore: %s", qPrintable(imported.value(1).value(1).toString()));
    CHECK(waitFor([&]() { return hosts.indexOf(hostId) >= 0 && hosts.indexOf(caHostId) >= 0
                                 && keys.indexOf(keyId) >= 0 && keys.indexOf(caKeyId) >= 0; }, 10000), "not restored");
    CHECK(keys.certificateText(caKeyId) == certificate.simplified(), "certificate not restored");
    session = manager.openHost(caHostId);
    CHECK(waitFor([&]() { return session->state() == SshSession::Connected; }, 20000), "login after restore: %s", qPrintable(session->errorString()));
    manager.closeSession(session);
    QFile::remove(path);
}

static void testPrediction()
{
    SshOptions options;
    options.mosh = true;
    SshSession *session = connectWith(options);
    // The loopback echoes at once, too quick to show anything otherwise
    session->setPredictionMinRoundTrip(0);
    CHECK(session->usesMosh(), "not over mosh");
    CHECK(shows(session, QStringLiteral("~$")), "no prompt");
    const auto typeSlowly = [&](const QByteArray &text, bool *predicted) {
        for (const char c : text) {
            emit session->terminal()->outputReady(QByteArray(1, c));
            // Before the echo can come back
            *predicted = *predicted || !session->terminal()->prediction().isEmpty();
            for (int i = 0; i < 10; ++i) {
                waitFor([]() { return false; }, 30);
                *predicted = *predicted || !session->terminal()->prediction().isEmpty();
            }
        }
    };
    bool predicted = false;
    typeSlowly("echo predicted-text", &predicted);
    CHECK(predicted, "typing never showed ahead");
    type(session, QByteArray());
    CHECK(shows(session, QStringLiteral("\npredicted-text").mid(1)), "%s", qPrintable(screen(session)));
    CHECK(waitFor([&]() { return session->terminal()->prediction().isEmpty(); }, 5000), "prediction left behind");

    // Nothing typed at a prompt that does not echo may show
    type(session, "read -s secret; echo \"got-$secret\"");
    waitFor([]() { return false; }, 1000);
    predicted = false;
    typeSlowly("hunter2", &predicted);
    CHECK(!predicted, "a password showed while typed");
    type(session, QByteArray());
    CHECK(shows(session, QStringLiteral("got-hunter2")), "%s", qPrintable(screen(session)));
    disconnect(session);
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
    if (wanted("status"))
        testAgentStatus();
    if (wanted("side"))
        testSideConnection();
    if (wanted("resume"))
        testResume();
    if (wanted("folders"))
        testFolders();
    if (wanted("install"))
        testInstallKeyAndBackup();
    if (wanted("prediction"))
        testPrediction();
    std::printf(failures ? "%d FAILURES\n" : "OK\n", failures);
    return failures ? 1 : 0;
}

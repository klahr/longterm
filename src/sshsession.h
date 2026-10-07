#ifndef SSHSESSION_H
#define SSHSESSION_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "sftpengine.h"
#include "terminal.h"

#include <functional>

class SecretVault;
class SftpBrowser;
class SshWorker;

struct SshCredentials {
    QByteArray password;
    QByteArray privateKey;
};

// How a saved host connects, see HostStore::Host
struct SshOptions {
    bool forwardAgent = false;
    QStringList localForwards;
    QStringList remoteForwards;
    QStringList dynamicForwards;
    QStringList environment;
    QString tmuxSession;
    int keepAliveInterval = 0;
    int connectTimeout = 0;
    bool mosh = false;
    // Empty for mosh-server on the PATH
    QString moshServer;
    // Writes what scrolls off the terminal to a file in Documents
    bool logging = false;
    // Runs this instead of a shell, its output stays to read once it ends
    QString command;
};

class SshSession : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)
    // Typed into the shell each time it starts
    Q_PROPERTY(QString startupScript READ startupScript WRITE setStartupScript NOTIFY startupScriptChanged)
    // Empty follows the app's color scheme
    Q_PROPERTY(QString colorScheme READ colorScheme WRITE setColorScheme NOTIFY colorSchemeChanged)
    Q_PROPERTY(bool ownTmux READ ownTmux WRITE setOwnTmux NOTIFY tmuxChanged)
    Q_PROPERTY(QString tmuxSession READ tmuxSession WRITE setTmuxSession NOTIFY tmuxChanged)
    Q_PROPERTY(QString hostTmuxSession READ hostTmuxSession NOTIFY tmuxChanged)
    Q_PROPERTY(QString host READ host NOTIFY endpointChanged)
    Q_PROPERTY(int port READ port NOTIFY endpointChanged)
    Q_PROPERTY(QString user READ user NOTIFY endpointChanged)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    Q_PROPERTY(Terminal *terminal READ terminal CONSTANT)
    Q_PROPERTY(bool hostKeyMismatch READ hostKeyMismatch NOTIFY hostKeyMismatchChanged)
    Q_PROPERTY(QString serverFingerprint READ serverFingerprint NOTIFY hostKeyMismatchChanged)
    // The server asks a keyboard-interactive question, such as a one-time code
    Q_PROPERTY(bool prompting READ prompting NOTIFY promptChanged)
    Q_PROPERTY(QString promptText READ promptText NOTIFY promptChanged)
    Q_PROPERTY(bool promptEcho READ promptEcho NOTIFY promptChanged)
    Q_PROPERTY(QString promptLabel READ promptLabel NOTIFY promptChanged)
    // The prompt asks for the password, which can be saved with the host
    Q_PROPERTY(bool promptCanRemember READ promptCanRemember NOTIFY promptChanged)
    Q_PROPERTY(QString systemId READ systemId NOTIFY systemChanged)
    Q_PROPERTY(QString systemName READ systemName NOTIFY systemChanged)
    // The terminal of this connection goes over mosh
    Q_PROPERTY(bool mosh READ usesMosh NOTIFY moshChanged)
    // How long a mosh server has been quiet, 0 while it is heard from
    Q_PROPERTY(int silentSeconds READ silentSeconds NOTIFY silentSecondsChanged)
    // Files and transfers need the SSH connection, which under mosh can close while the terminal goes on
    Q_PROPERTY(bool filesAvailable READ filesAvailable NOTIFY filesAvailableChanged)
    Q_PROPERTY(SftpBrowser *files READ files CONSTANT)
    // The saved host it belongs to, empty for a one-off connection
    Q_PROPERTY(QString hostId READ hostId CONSTANT)
    // Lines typed into the shell, the newest last. Only those that showed on
    // screen, so passwords typed without echo stay out.
    Q_PROPERTY(QStringList history READ history NOTIFY historyChanged)
    // A message to show for a moment, under mosh where the terminal cannot take it
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    // Where the session log goes, empty when not logging
    Q_PROPERTY(QString logPath READ logPath NOTIFY logPathChanged)

public:
    enum State {
        Disconnected,
        Connecting,
        Connected
    };
    Q_ENUM(State)

    enum SecretKind {
        Password,
        PrivateKey
    };

    SshSession(const QString &name, const QString &host, int port, const QString &user,
               QObject *parent = nullptr);
    ~SshSession();

    QString name() const { return m_name; }
    void setName(const QString &name);
    QString startupScript() const { return m_startupScript; }
    void setStartupScript(const QString &script);
    QString colorScheme() const { return m_colorScheme; }
    void setColorScheme(const QString &colorScheme);
    bool ownTmux() const { return m_ownTmux; }
    void setOwnTmux(bool ownTmux);
    QString tmuxSession() const { return m_tmuxSession; }
    void setTmuxSession(const QString &tmuxSession);
    QString hostTmuxSession() const { return m_options.tmuxSession; }
    QString host() const { return m_host; }
    int port() const { return m_port; }
    QString user() const { return m_user; }
    State state() const { return m_state; }
    QString errorString() const { return m_errorString; }
    Terminal *terminal() const { return m_terminal; }
    bool hostKeyMismatch() const { return m_hostKeyMismatch; }
    QString serverFingerprint() const { return m_serverFingerprint; }
    bool prompting() const { return m_prompting; }
    QString promptText() const { return m_promptText; }
    bool promptEcho() const { return m_promptEcho; }
    QString promptLabel() const { return m_promptLabel; }
    QString systemId() const { return m_systemId; }
    QString systemName() const { return m_systemName; }
    void setSystem(const QString &id, const QString &name);
    bool usesMosh() const { return m_usesMosh; }
    int silentSeconds() const { return m_silentSeconds; }
    bool filesAvailable() const { return m_state == Connected && (m_sshUp || m_sideUp); }
    SftpBrowser *files() const { return m_files; }
    QString hostId() const { return m_hostId; }
    // Names what the session keeps between starts of the app, such as a mosh session to resume
    QString sessionId() const { return m_sessionId; }
    void setSessionId(const QString &id) { m_sessionId = id; }
    // Where the keys of mosh sessions to resume are kept
    void setMoshVault(SecretVault *vault) { m_moshVault = vault; }
    // A mosh session left running when the app went, which connecting picks up
    bool canResume() const;
    // Ends what is kept for resuming, for a session that goes away
    void forgetMosh();
    void setHostId(const QString &hostId) { m_hostId = hostId; }
    QString logPath() const { return m_logPath; }
    QString notice() const { return m_notice; }
    QStringList history() const { return m_history; }
    bool promptCanRemember() const { return m_promptCanRemember && m_canRememberPassword && m_auth.kind == Password; }
    // Set for sessions of a saved host, which has somewhere to keep the password
    void setCanRememberPassword(bool canRemember) { m_canRememberPassword = canRemember; }
    // The connection dropped for network reasons after it was up
    bool isLost() const { return m_lost; }
    // False once the address the connection went out from has left the device
    bool hasLocalAddress() const;

    // A vault of null means no stored secret, only what the server prompts for
    void setJumpHost(const QString &host, int port, const QString &user,
                     SecretVault *vault, const QString &secretId, SecretKind kind);
    void clearJumpHost() { m_hasJump = false; }
    // Takes effect on the next connect, a live connection keeps its own
    void setEndpoint(const QString &host, int port, const QString &user);
    // Takes effect on the next connect
    void setOptions(const SshOptions &options);
    const SshOptions &options() const { return m_options; }
    // An OpenSSH certificate for the session's key, "type base64 comment"
    void setCertificate(const QByteArray &certificate) { m_certificate = certificate; }

    static QString knownHostsPath();

    Q_INVOKABLE void connectToHost(const QString &password);
    void connectWithSecret(SecretVault *vault, const QString &secretId, SecretKind kind);
    // Sets up reconnect() for a session that was never connected
    void setSecret(SecretVault *vault, const QString &secretId, SecretKind kind);
    // Forgets a stored key or password, keeping one typed while connecting
    void clearStoredSecret();
    Q_INVOKABLE void disconnectFromHost();
    // Connects again the same way as the last attempt
    Q_INVOKABLE void reconnect();
    // Forgets the stored host key after the server's key changed, then reconnects
    Q_INVOKABLE void trustNewHostKey();
    // remember only counts for promptCanRemember prompts
    Q_INVOKABLE void answerPrompt(const QString &answer, bool remember = false);
    // Stops a connection the network has gone from under, reporting it as lost
    void dropConnection();
    // Lets a mosh connection know the network changed, it goes on from the new one
    void roam();
    // False when there is no connection to send it over
    bool sendSftp(const SftpRequest &request);
    // Shows typing over mosh before the server echoes it
    void setPredictionEnabled(bool enabled) { m_predictionEnabled = enabled; }
    // Shows it from this round trip on, quicker links echo before anyone could tell. Tests use 0.
    void setPredictionMinRoundTrip(int milliseconds) { m_predictionMinRoundTrip = milliseconds; }
    // Types the text into the session, as an answer from a notification
    Q_INVOKABLE void sendInput(const QString &text);
    // The scrollback and the screen as plain text
    Q_INVOKABLE QString scrollbackText() const;
    // Into the Downloads folder, returns the file or an empty string with errorString set
    Q_INVOKABLE QString saveScrollback();

signals:
    void nameChanged();
    void startupScriptChanged();
    void colorSchemeChanged();
    void tmuxChanged();
    void stateChanged();
    void errorStringChanged();
    // The remote shell exited normally, as opposed to the connection failing
    void shellExited();
    // The command of SshOptions::command ended, status is -1 when not known
    void commandFinished(int status);
    void hostKeyMismatchChanged();
    void promptChanged();
    void systemChanged();
    void endpointChanged();
    void moshChanged();
    void silentSecondsChanged();
    void filesAvailableChanged();
    void logPathChanged();
    void noticeChanged();
    void historyChanged();
    // The user asked to save the password they typed
    void passwordRemembered(const QString &password);
    // An established connection dropped for network reasons and has wound down
    void connectionLost();

private slots:
    void onWorkerConnected(const QString &localAddress);
    void onWorkerData(const QByteArray &data);
    void onWorkerInfo(const QString &message);
    void onWorkerFailed(const QString &message, bool network);
    void onHostKeyChanged(const QString &fingerprint, const QString &knownHostsPattern);
    void onWorkerPrompt(const QString &text, bool echo, bool canRemember, const QString &label);
    void onWorkerSystem(const QString &id, const QString &name, bool certain);
    void onWorkerFinished();
    void onTerminalOutput(const QByteArray &data);
    void onTerminalSizeChanged();
    void onShellExited(int status);
    void onMoshStarted();
    void onMoshData(const QByteArray &screen, const QByteArray &data, int skipLines);
    void onMoshSilence(int seconds);
    void onSshClosed();
    void onStatusLine(const QByteArray &line);
    void onSideWorkerFinished();
    void onMoshEcho(qint64 echoedBytes, int roundTrip);

private:
    // Where the secrets for a login come from
    struct AuthSource {
        QString password;
        SecretVault *vault = nullptr;
        QString secretId;
        SecretKind kind = Password;
    };

    void setState(State state);
    void startConnection();
    // side fetches quietly for the extra SSH connection under mosh
    void fetchCredentials(const AuthSource &source, const std::function<void(const SshCredentials &)> &done,
                          bool side = false);
    // side starts the SSH connection for files and forwards under mosh, after its own dropped
    void startWorker(const SshCredentials &credentials, const SshCredentials &jumpCredentials, bool side = false);
    void startSideWorker();
    void stopSideWorker();
    void setSideUp(bool up);
    void setNotice(const QString &notice);
    void setPrompt(bool prompting, const QString &text, bool echo, bool canRemember, const QString &label);
    // keepMosh leaves a mosh session on the server to resume after a restart
    void stopWorker(bool keepMosh = false);
    QString journalPath() const;
    void setUsesMosh(bool usesMosh);
    void setSilentSeconds(int seconds);
    void setSshUp(bool up);
    void startLog();
    void finishLog();
    void writeLog(const QString &line);
    void trackTyping(const QByteArray &data);
    void predict(const QByteArray &data);
    void showPredictions();
    void clearPredictions(bool trusted);

    QString m_name;
    QString m_startupScript;
    QString m_colorScheme;
    bool m_ownTmux;
    QString m_tmuxSession;
    QString m_host;
    int m_port;
    QString m_user;
    State m_state;
    QString m_errorString;
    SshWorker *m_worker;
    Terminal *m_terminal;
    bool m_secretFetchPending;
    bool m_hostKeyMismatch;
    QString m_serverFingerprint;
    QString m_mismatchPattern;
    bool m_prompting;
    QString m_promptText;
    bool m_promptEcho;
    QString m_promptLabel;
    bool m_promptCanRemember;
    bool m_canRememberPassword;
    bool m_lost;
    bool m_reachedConnected;
    QString m_systemId;
    QString m_systemName;
    QString m_localAddress;
    // How the last connection authenticated, for reconnecting
    AuthSource m_auth;
    bool m_hasJump;
    QString m_jumpHost;
    int m_jumpPort;
    QString m_jumpUser;
    AuthSource m_jumpAuth;
    SshOptions m_options;
    QByteArray m_certificate;
    bool m_usesMosh;
    int m_silentSeconds;
    bool m_sshUp;
    SftpBrowser *m_files;
    SshWorker *m_sideWorker;
    bool m_sideFetchPending;
    bool m_sideUp;
    QTimer m_sideRetry;
    QByteArray m_statusFile;
    QString m_notice;
    QTimer m_noticeTimer;
    QString m_hostId;
    QString m_sessionId;
    SecretVault *m_moshVault;
    QByteArray m_resumeKey;
    // The connection picks up a mosh session rather than logging in
    bool m_resuming;
    // The mosh session to resume was gone, so a new connection follows
    bool m_connectAfterWorker;
    QString m_logPath;
    QFile *m_log;
    QStringList m_history;
    // Typing shown ahead of the mosh server, each part with the typed bytes up to its end
    struct Prediction {
        QString text;
        int row;
        int column;
        qint64 endBytes;
    };
    QList<Prediction> m_predictions;
    qint64 m_typedBytes;
    bool m_predictionTrusted;
    bool m_predictionEnabled;
    int m_roundTrip;
    int m_predictionMinRoundTrip;
    QElapsedTimer m_predictionAge;
    // The line being typed, unknown once editing keys moved around in it
    QByteArray m_typed;
    bool m_typedUnknown;
    // Inside an escape sequence the terminal sent, 0 when not, 2 inside CSI
    int m_typedEscape;
    bool m_typedPaste;
    QByteArray m_typedParameters;
};

#endif // SSHSESSION_H

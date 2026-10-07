#ifndef SSHSESSION_H
#define SSHSESSION_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

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
};

class SshSession : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)
    // Typed into the shell each time it starts
    Q_PROPERTY(QString startupScript READ startupScript WRITE setStartupScript NOTIFY startupScriptChanged)
    // Empty follows the app's color scheme
    Q_PROPERTY(QString colorScheme READ colorScheme WRITE setColorScheme NOTIFY colorSchemeChanged)
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
    bool filesAvailable() const { return m_state == Connected && m_sshUp; }
    SftpBrowser *files() const { return m_files; }
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
    void setOptions(const SshOptions &options) { m_options = options; }

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

signals:
    void nameChanged();
    void startupScriptChanged();
    void colorSchemeChanged();
    void stateChanged();
    void errorStringChanged();
    // The remote shell exited normally, as opposed to the connection failing
    void shellExited();
    void hostKeyMismatchChanged();
    void promptChanged();
    void systemChanged();
    void endpointChanged();
    void moshChanged();
    void silentSecondsChanged();
    void filesAvailableChanged();
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
    void onMoshStarted();
    void onMoshData(const QByteArray &data, bool replace);
    void onMoshSilence(int seconds);
    void onSshClosed();

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
    void fetchCredentials(const AuthSource &source, const std::function<void(const SshCredentials &)> &done);
    void startWorker(const SshCredentials &credentials, const SshCredentials &jumpCredentials);
    void setPrompt(bool prompting, const QString &text, bool echo, bool canRemember, const QString &label);
    void stopWorker();
    void setUsesMosh(bool usesMosh);
    void setSilentSeconds(int seconds);
    void setSshUp(bool up);

    QString m_name;
    QString m_startupScript;
    QString m_colorScheme;
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
    bool m_usesMosh;
    int m_silentSeconds;
    bool m_sshUp;
    SftpBrowser *m_files;
};

#endif // SSHSESSION_H

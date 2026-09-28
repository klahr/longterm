#ifndef SSHSESSION_H
#define SSHSESSION_H

#include <QObject>
#include <QString>

#include "terminal.h"

class SecretVault;
class SshWorker;

class SshSession : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)
    // Typed into the shell each time it starts
    Q_PROPERTY(QString startupScript READ startupScript WRITE setStartupScript NOTIFY startupScriptChanged)
    Q_PROPERTY(QString host READ host CONSTANT)
    Q_PROPERTY(int port READ port CONSTANT)
    Q_PROPERTY(QString user READ user CONSTANT)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    Q_PROPERTY(Terminal *terminal READ terminal CONSTANT)
    Q_PROPERTY(bool hostKeyMismatch READ hostKeyMismatch NOTIFY hostKeyMismatchChanged)
    Q_PROPERTY(QString serverFingerprint READ serverFingerprint NOTIFY hostKeyMismatchChanged)

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
    QString host() const { return m_host; }
    int port() const { return m_port; }
    QString user() const { return m_user; }
    State state() const { return m_state; }
    QString errorString() const { return m_errorString; }
    Terminal *terminal() const { return m_terminal; }
    bool hostKeyMismatch() const { return m_hostKeyMismatch; }
    QString serverFingerprint() const { return m_serverFingerprint; }

    Q_INVOKABLE void connectToHost(const QString &password);
    void connectWithSecret(SecretVault *vault, const QString &secretId, SecretKind kind);
    // Sets up reconnect() for a session that was never connected
    void setSecret(SecretVault *vault, const QString &secretId, SecretKind kind);
    Q_INVOKABLE void disconnectFromHost();
    // Connects again the same way as the last attempt
    Q_INVOKABLE void reconnect();
    // Forgets the stored host key after the server's key changed, then reconnects
    Q_INVOKABLE void trustNewHostKey();

signals:
    void nameChanged();
    void startupScriptChanged();
    void stateChanged();
    void errorStringChanged();
    // The remote shell exited normally, as opposed to the connection failing
    void shellExited();
    void hostKeyMismatchChanged();

private slots:
    void onWorkerConnected();
    void onWorkerData(const QByteArray &data);
    void onWorkerInfo(const QString &message);
    void onWorkerFailed(const QString &message);
    void onHostKeyChanged(const QString &fingerprint);
    void onWorkerFinished();
    void onTerminalOutput(const QByteArray &data);
    void onTerminalSizeChanged();

private:
    void setState(State state);
    void startWorker(const QString &password, const QByteArray &privateKey);
    QString knownHostsPath() const;
    void stopWorker();

    QString m_name;
    QString m_startupScript;
    const QString m_host;
    const int m_port;
    const QString m_user;
    State m_state;
    QString m_errorString;
    SshWorker *m_worker;
    Terminal *m_terminal;
    bool m_secretFetchPending;
    bool m_hostKeyMismatch;
    QString m_serverFingerprint;
    // How the last connection authenticated, for reconnecting
    QString m_authPassword;
    SecretVault *m_authVault;
    QString m_authSecretId;
    SecretKind m_authKind;
};

#endif // SSHSESSION_H

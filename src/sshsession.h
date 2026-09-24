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
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(QString host READ host CONSTANT)
    Q_PROPERTY(int port READ port CONSTANT)
    Q_PROPERTY(QString user READ user CONSTANT)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    Q_PROPERTY(Terminal *terminal READ terminal CONSTANT)

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
    QString host() const { return m_host; }
    int port() const { return m_port; }
    QString user() const { return m_user; }
    State state() const { return m_state; }
    QString errorString() const { return m_errorString; }
    Terminal *terminal() const { return m_terminal; }

    Q_INVOKABLE void connectToHost(const QString &password);
    void connectWithSecret(SecretVault *vault, const QString &secretId, SecretKind kind);
    Q_INVOKABLE void disconnectFromHost();

signals:
    void stateChanged();
    void errorStringChanged();
    // The remote shell exited normally, as opposed to the connection failing
    void shellExited();

private slots:
    void onWorkerConnected();
    void onWorkerData(const QByteArray &data);
    void onWorkerInfo(const QString &message);
    void onWorkerFailed(const QString &message);
    void onWorkerFinished();
    void onTerminalOutput(const QByteArray &data);
    void onTerminalSizeChanged();

private:
    void setState(State state);
    void startWorker(const QString &password, const QByteArray &privateKey);
    void stopWorker();

    const QString m_name;
    const QString m_host;
    const int m_port;
    const QString m_user;
    State m_state;
    QString m_errorString;
    SshWorker *m_worker;
    Terminal *m_terminal;
    bool m_secretFetchPending;
};

#endif // SSHSESSION_H

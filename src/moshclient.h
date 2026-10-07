#ifndef MOSHCLIENT_H
#define MOSHCLIENT_H

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QList>
#include <QString>
#include <QVector>

#include <sys/socket.h>

typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;

// The client side of the mosh protocol, for one connection to a mosh-server
// started over SSH. Not thread-safe, the SSH worker drives it from its poll()
// loop: wait for readable() on fds() or for waitTime(), then call tick().
//
// The server sends its screen as escape sequences that turn one screen state
// into another. They are written to the terminal as they are, so the
// terminal shows what the server's terminal shows.
class MoshClient
{
    Q_DECLARE_TR_FUNCTIONS(MoshClient)

public:
    // What the terminal is to be given. With replace set, the terminal
    // starts over from these bytes instead of continuing from what it shows.
    struct Output {
        QByteArray bytes;
        bool replace;
    };

    MoshClient();
    ~MoshClient();

    // key is the 22 character key printed by mosh-server. False with errorString set on failure.
    bool start(const sockaddr *address, socklen_t length, const QByteArray &key, int columns, int rows);
    QString errorString() const { return m_errorString; }

    // The sockets to poll for reading, several for a while after roam()
    QVector<int> fds() const;
    void readable(int fd);
    // Milliseconds until tick() has something to send
    int waitTime();
    void tick();
    QList<Output> takeOutput();

    void write(const QByteArray &keys);
    void resize(int columns, int rows);
    // Sends from a new socket, so the server learns the address after the network changed
    void roam();
    // Asks the server to end the session, finished() once it agreed or gave no answer
    void shutdown();
    // The session is over, ended by either side, or broken with errorString set
    bool finished() const;
    // Milliseconds since the server was last heard, or since start() before that
    qint64 silence() const;
    bool everHeard() const { return m_heard; }

private:
    struct UserEvent {
        QByteArray keys;
        int columns;
        int rows;
    };

    struct SentState {
        quint64 num;
        // Events up to here, counted from the start
        qint64 events;
        quint64 timestamp;
    };

    // A screen state the server may send changes from. Up to the checkpoint
    // the terminal is described by the checkpoint's bytes, every later state
    // by the changes that led to it.
    struct ReceivedState {
        quint64 num;
        QByteArray diff;
        int columns;
        int rows;
    };

    quint64 now() const;
    quint16 timestamp16() const;
    quint64 timeout() const;
    quint64 sendInterval() const;

    bool newSocket();
    void sendPacket(const QByteArray &payload);
    void receiveFragment(const QByteArray &fragment);
    void receiveInstruction(const QByteArray &instruction);
    void applyDiff(quint64 oldNum, quint64 newNum, const QByteArray &diff);
    void throwAwayUntil(quint64 num);

    QByteArray encrypt(quint64 nonce, const QByteArray &plaintext);
    bool decrypt(const QByteArray &packet, quint64 *nonce, QByteArray *plaintext);

    void calculateTimers();
    void updateAssumedReceiverState();
    QByteArray userDiff(qint64 from) const;
    void sendEmptyAck();
    void sendToReceiver(const QByteArray &diff);
    void addSentState(quint64 timestamp, quint64 num, qint64 events);
    void sendInstruction(const QByteArray &diff, quint64 newNum);
    void processAcknowledgment(quint64 ackNum);
    void fail(const QString &message);

    QString m_errorString;
    QElapsedTimer m_clock;
    EVP_CIPHER_CTX *m_encrypt;
    EVP_CIPHER_CTX *m_decrypt;
    unsigned char m_key[16];

    sockaddr_storage m_remote;
    socklen_t m_remoteLength;
    struct Socket {
        int fd;
        quint64 created;
    };
    // The newest last, the one packets go out from
    QList<Socket> m_sockets;
    quint64 m_lastPortChoice;
    quint64 m_lastRoundtripSuccess;
    quint64 m_nextSeq;
    quint64 m_expectedSeq;
    quint16 m_savedTimestamp;
    quint64 m_savedTimestampReceivedAt;
    bool m_rttHit;
    double m_srtt;
    double m_rttvar;

    quint64 m_fragmentId;
    quint64 m_assemblyId;
    QVector<QByteArray> m_fragments;
    QVector<bool> m_fragmentArrived;
    int m_fragmentsArrived;
    int m_fragmentsTotal;

    // Sender, the user's keystrokes and resizes
    QList<UserEvent> m_events;
    // Events before m_events, which the server has
    qint64 m_eventBase;
    QList<SentState> m_sent;
    int m_assumed;
    quint64 m_nextAckTime;
    quint64 m_nextSendTime;
    quint64 m_mindelayClock;
    quint64 m_lastHeard;
    bool m_pendingDataAck;
    bool m_shutdownInProgress;
    int m_shutdownTries;
    quint64 m_shutdownStart;
    quint64 m_lastAckSent;

    // Receiver, the server's screen
    quint64 m_ackNum;
    QByteArray m_checkpoint;
    quint64 m_checkpointNum;
    int m_checkpointColumns;
    int m_checkpointRows;
    QList<ReceivedState> m_received;
    QList<Output> m_output;
    bool m_heard;
    bool m_finished;
};

#endif // MOSHCLIENT_H

#ifndef MOSHCLIENT_H
#define MOSHCLIENT_H

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QList>
#include <QMap>
#include <QPair>
#include <QSize>
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
    // What the terminal is to be given. With replace set, the terminal starts
    // over from screen, without adding to its scrollback, before taking bytes.
    // Of the lines bytes scroll off, the first skipLines are in the scrollback
    // already, from the changes the screen started over from.
    struct Output {
        QByteArray bytes;
        bool replace;
        QByteArray screen;
        int skipLines;
    };

    MoshClient();
    ~MoshClient();

    // key is the 22 character key printed by mosh-server. False with errorString set on failure.
    bool start(const sockaddr *address, socklen_t length, const QByteArray &key, int columns, int rows);
    // Picks up a session from its journal, after the app was gone for a while.
    // The terminal gets the screen it had back first.
    bool resume(const QString &journalPath, const QByteArray &key, int columns, int rows);
    // Keeps what is needed to resume in the file from now on, see resume()
    bool setJournal(const QString &path);
    // Opaque text kept in the journal for whoever resumes, such as the status file's name
    void setJournalNote(const QByteArray &note);
    QByteArray journalNote() const { return m_journalNote; }
    // The note of a journal on disk, before resuming it
    static QByteArray journalNote(const QString &journalPath);
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
    // Bytes typed so far that the server has taken in and shown whatever they show
    qint64 echoedBytes() const;
    // The smoothed round trip in milliseconds
    int roundTrip() const { return int(m_srtt); }

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


    // A screen state of the server's, which it may send changes from. It is
    // the changes from its parent state, or for a root a whole screen drawn
    // from a cleared one. The states form a tree, as changes can come from
    // any state the server thinks this side has.
    struct ReceivedState {
        quint64 num;
        // NoParent for a root
        quint64 parent;
        QByteArray diff;
        int columns;
        int rows;
        // Lines scrolled off since the first screen, along the way it was reached
        qint64 scrollIndex;
    };

    quint64 now() const;
    quint16 timestamp16() const;
    quint64 timeout() const;
    quint64 sendInterval() const;

    // The key and the ciphers, false with errorString set
    bool setKey(const QByteArray &key);
    bool newSocket();
    void sendPacket(const QByteArray &payload);
    void receiveFragment(const QByteArray &fragment);
    void receiveInstruction(const QByteArray &instruction);
    void applyDiff(quint64 oldNum, quint64 newNum, const QByteArray &diff);
    void throwAwayUntil(quint64 num);
    // The changes from a root that make the state's screen, empty when some state on the way is gone
    QList<QPair<QByteArray, QSize> > stepsTo(quint64 num) const;

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
    // Typing and resizes go in the journal too: the server takes its newest
    // input as continuing the one it had, so a restart has to go on with it
    void journalEvent(qint64 index, const UserEvent &event);
    // A copy of the terminal at the newest state, which counts the lines each change scrolls off
    void resetMirror(const QByteArray &screen, int columns, int rows);
    int writeMirror(const QByteArray &bytes, int columns, int rows);
    // Appends a record, or writes the journal anew when rewrite is set
    void journal(char type, const QByteArray &data);
    void rewriteJournal();
    bool readJournal(const QString &path);

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
    QMap<quint64, ReceivedState> m_states;
    // The state the terminal shows, the newest
    quint64 m_latest;
    // Lines the terminal has scrolled into its scrollback, in scrollIndex terms
    qint64 m_shownScroll;
    QList<Output> m_output;
    bool m_heard;
    bool m_finished;

    // Resuming needs the states the server may send changes from, the newest
    // state it knows was acknowledged, and nonces never used before
    QFile *m_journal;
    QByteArray m_journalNote;
    quint64 m_seqReserved;
    quint64 m_journaledFront;
    // State numbers for what is typed, which must not repeat either: the
    // server takes a number it has as a state it has
    quint64 m_stateReserved;
    quint64 m_stateFloor;

    // Typed bytes up to each state sent, for the server's echo acknowledgments
    QMap<quint64, qint64> m_sentBytes;
    qint64 m_bytesWritten;
    quint64 m_echoAck;
    struct VTerm *m_mirror;
    int m_mirrorScrolled;
};

#endif // MOSHCLIENT_H

#include "moshclient.h"

#include <QPair>
#include <QSize>
#include <QtEndian>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <vterm.h>
#include <zlib.h>

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstring>
#include <netinet/in.h>
#include <unistd.h>

// See mosh's src/network and src/statesync, which this follows
static const quint64 DirectionMask = quint64(1) << 63;
static const quint64 SequenceMask = ~DirectionMask;
static const quint64 Shutdown = quint64(-1);
static const unsigned ProtocolVersion = 2;
// Nonce, timestamps, OCB tag and fragment header around the payload in each
// datagram, which stays below the smallest IPv6 MTU
static const int MaxDatagram = 1200;
static const int FragmentHeader = 10;
static const int FragmentPayload = MaxDatagram - 8 - 4 - 16 - FragmentHeader;
static const int ReceiveBuffer = 4096;
static const int MaxFragments = 8192;
static const int MaxInstruction = 16 * 1024 * 1024;

static const quint64 MinRto = 50;
static const quint64 MaxRto = 1000;
static const quint64 SendIntervalMin = 20;
static const quint64 SendIntervalMax = 250;
static const quint64 AckInterval = 3000;
static const quint64 AckDelay = 100;
// Keystrokes go out at once, as in mosh-client
static const quint64 SendMindelay = 1;
static const quint64 ActiveRetryTimeout = 10000;
static const int ShutdownRetries = 16;
static const int MaxSentStates = 32;
static const quint64 PortHopInterval = 10000;
static const quint64 MaxOldSocketAge = 60000;
static const int MaxSockets = 10;
// Past these the received states before the server's oldest are folded into one
static const int MaxReceivedStates = 64;
static const int MaxReceivedBytes = 256 * 1024;

// --- Protocol buffers, only the few messages mosh uses ---

static void putVarint(QByteArray *out, quint64 value)
{
    while (value >= 0x80) {
        out->append(char((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out->append(char(value));
}

static void putUint(QByteArray *out, int field, quint64 value)
{
    putVarint(out, quint64(field) << 3);
    putVarint(out, value);
}

static void putBytes(QByteArray *out, int field, const QByteArray &bytes)
{
    putVarint(out, (quint64(field) << 3) | 2);
    putVarint(out, quint64(bytes.size()));
    out->append(bytes);
}

class ProtoReader
{
public:
    explicit ProtoReader(const QByteArray &data)
        : m_data(data)
        , m_pos(0)
        , m_ok(true)
    {
    }

    bool ok() const { return m_ok; }

    // False at the end or on damaged input, see ok()
    bool next(int *field, int *wireType)
    {
        if (!m_ok || m_pos >= m_data.size())
            return false;
        quint64 key;
        if (!varint(&key) || (key >> 3) == 0 || (key >> 3) > INT_MAX) {
            m_ok = false;
            return false;
        }
        *field = int(key >> 3);
        *wireType = int(key & 7);
        return true;
    }

    bool varint(quint64 *value)
    {
        *value = 0;
        for (int shift = 0; shift < 64 && m_pos < m_data.size(); shift += 7) {
            const quint8 byte = quint8(m_data.at(m_pos++));
            *value |= quint64(byte & 0x7f) << shift;
            if (!(byte & 0x80))
                return true;
        }
        m_ok = false;
        return false;
    }

    bool bytes(QByteArray *value)
    {
        quint64 length;
        if (!varint(&length) || length > quint64(m_data.size() - m_pos)) {
            m_ok = false;
            return false;
        }
        *value = m_data.mid(m_pos, int(length));
        m_pos += int(length);
        return true;
    }

    bool skip(int wireType)
    {
        quint64 ignored;
        QByteArray ignoredBytes;
        switch (wireType) {
        case 0: return varint(&ignored);
        case 1: return advance(8);
        case 2: return bytes(&ignoredBytes);
        case 5: return advance(4);
        default:
            m_ok = false;
            return false;
        }
    }

private:
    bool advance(int count)
    {
        if (m_data.size() - m_pos < count) {
            m_ok = false;
            return false;
        }
        m_pos += count;
        return true;
    }

    QByteArray m_data;
    int m_pos;
    bool m_ok;
};

static QByteArray compress(const QByteArray &data)
{
    uLongf length = compressBound(uLong(data.size()));
    QByteArray out(int(length), Qt::Uninitialized);
    if (::compress(reinterpret_cast<Bytef *>(out.data()), &length,
                   reinterpret_cast<const Bytef *>(data.constData()), uLong(data.size())) != Z_OK)
        return QByteArray();
    out.truncate(int(length));
    return out;
}

static bool uncompress(const QByteArray &data, QByteArray *out)
{
    z_stream stream;
    std::memset(&stream, 0, sizeof(stream));
    if (inflateInit(&stream) != Z_OK)
        return false;
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    stream.avail_in = uInt(data.size());
    out->clear();
    char buffer[16384];
    int rc;
    do {
        stream.next_out = reinterpret_cast<Bytef *>(buffer);
        stream.avail_out = sizeof(buffer);
        rc = inflate(&stream, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END)
            break;
        out->append(buffer, int(sizeof(buffer) - stream.avail_out));
    } while (rc != Z_STREAM_END && out->size() <= MaxInstruction);
    inflateEnd(&stream);
    return rc == Z_STREAM_END;
}

// --- Screen states rebuilt from their bytes ---

// Follows a libvterm screen to describe it again as escape sequences
struct ScreenModes {
    bool cursorVisible = true;
    bool reverse = false;
    int mouse = VTERM_PROP_MOUSE_NONE;
    QByteArray title;
    QByteArray pendingTitle;
};

static int onModeProp(VTermProp prop, VTermValue *value, void *user)
{
    ScreenModes *modes = static_cast<ScreenModes *>(user);
    switch (prop) {
    case VTERM_PROP_CURSORVISIBLE:
        modes->cursorVisible = value->boolean;
        return 1;
    case VTERM_PROP_REVERSE:
        modes->reverse = value->boolean;
        return 1;
    case VTERM_PROP_MOUSE:
        modes->mouse = value->number;
        return 1;
    case VTERM_PROP_TITLE:
        if (value->string.initial)
            modes->pendingTitle.clear();
        modes->pendingTitle.append(value->string.str, int(value->string.len));
        if (value->string.final)
            modes->title = modes->pendingTitle;
        return 1;
    default:
        return 0;
    }
}

static const VTermScreenCallbacks s_modeCallbacks = {
    nullptr, // damage
    nullptr, // moverect
    nullptr, // movecursor
    &onModeProp,
    nullptr, // bell
    nullptr, // resize
    nullptr, // sb_pushline
    nullptr, // sb_popline
    nullptr, // sb_clear
};

static void appendColor(QByteArray *sgr, const VTermColor &color, bool background)
{
    if (background ? VTERM_COLOR_IS_DEFAULT_BG(&color) : VTERM_COLOR_IS_DEFAULT_FG(&color))
        return;
    if (VTERM_COLOR_IS_INDEXED(&color)) {
        const int index = color.indexed.idx;
        if (index < 8)
            *sgr += ';' + QByteArray::number((background ? 40 : 30) + index);
        else if (index < 16)
            *sgr += ';' + QByteArray::number((background ? 100 : 90) + index - 8);
        else
            *sgr += (background ? ";48;5;" : ";38;5;") + QByteArray::number(index);
    } else {
        *sgr += (background ? ";48;2;" : ";38;2;") + QByteArray::number(color.rgb.red) + ';'
                + QByteArray::number(color.rgb.green) + ';' + QByteArray::number(color.rgb.blue);
    }
}

static QByteArray sgr(bool bold, int underline, bool italic, bool blink, bool reverse, bool conceal,
                      bool strike, const VTermColor &foreground, const VTermColor &background)
{
    QByteArray sgr("\033[0");
    if (bold)
        sgr += ";1";
    if (italic)
        sgr += ";3";
    if (underline == VTERM_UNDERLINE_DOUBLE)
        sgr += ";21";
    else if (underline)
        sgr += ";4";
    if (blink)
        sgr += ";5";
    if (reverse)
        sgr += ";7";
    if (conceal)
        sgr += ";8";
    if (strike)
        sgr += ";9";
    appendColor(&sgr, foreground, false);
    appendColor(&sgr, background, true);
    return sgr + 'm';
}

// The private modes mosh sets with fixed sequences, which libvterm keeps to itself
static QByteArray privateModes(const QByteArray &bytes)
{
    static const int modes[] = { 2004, 1004, 1005, 1006, 1015 };
    bool set[5] = { false, false, false, false, false };
    for (int at = bytes.indexOf("\033[?"); at >= 0; at = bytes.indexOf("\033[?", at + 3)) {
        int end = at + 3;
        while (end < bytes.size() && bytes.at(end) >= '0' && bytes.at(end) <= '9')
            ++end;
        if (end >= bytes.size() || (bytes.at(end) != 'h' && bytes.at(end) != 'l'))
            continue;
        const int mode = bytes.mid(at + 3, end - at - 3).toInt();
        for (int i = 0; i < 5; ++i) {
            if (modes[i] == mode)
                set[i] = bytes.at(end) == 'h';
        }
    }
    QByteArray out;
    for (int i = 0; i < 5; ++i)
        out += "\033[?" + QByteArray::number(modes[i]) + (set[i] ? 'h' : 'l');
    return out;
}

// Bytes that put a terminal into the state the steps leave a fresh one in,
// each step being bytes for a screen of the given size
static QByteArray describeScreen(const QList<QPair<QByteArray, QSize> > &steps)
{
    if (steps.isEmpty())
        return QByteArray();
    VTerm *vterm = vterm_new(steps.first().second.height(), steps.first().second.width());
    vterm_set_utf8(vterm, 1);
    VTermScreen *screen = vterm_obtain_screen(vterm);
    ScreenModes modes;
    vterm_screen_set_callbacks(screen, &s_modeCallbacks, &modes);
    vterm_screen_enable_altscreen(screen, 1);
    vterm_screen_enable_reflow(screen, false);
    vterm_screen_reset(screen, 1);
    for (const auto &step : steps) {
        int rows;
        int columns;
        vterm_get_size(vterm, &rows, &columns);
        if (rows != step.second.height() || columns != step.second.width())
            vterm_set_size(vterm, step.second.height(), step.second.width());
        vterm_input_write(vterm, step.first.constData(), size_t(step.first.size()));
    }
    vterm_screen_flush_damage(screen);

    int rows;
    int columns;
    vterm_get_size(vterm, &rows, &columns);
    QByteArray out("\033[r\033[0m\033[H\033[2J");
    QByteArray pen = "\033[0m";
    for (int row = 0; row < rows; ++row) {
        out += "\033[" + QByteArray::number(row + 1) + ";1H";
        for (int column = 0; column < columns; ++column) {
            VTermScreenCell cell;
            VTermPos pos;
            pos.row = row;
            pos.col = column;
            if (!vterm_screen_get_cell(screen, pos, &cell))
                continue;
            if (cell.chars[0] == uint32_t(-1))
                continue;
            const QByteArray cellPen = sgr(cell.attrs.bold, cell.attrs.underline, cell.attrs.italic, cell.attrs.blink,
                                           cell.attrs.reverse, cell.attrs.conceal, cell.attrs.strike, cell.fg, cell.bg);
            if (cellPen != pen) {
                out += cellPen;
                pen = cellPen;
            }
            if (cell.chars[0] == 0) {
                out += ' ';
                continue;
            }
            QString text;
            for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i)
                text += QString::fromUcs4(&cell.chars[i], 1);
            out += text.toUtf8();
            // Wide characters cover the next cell too
            if (cell.width > 1)
                column += cell.width - 1;
        }
    }

    VTermState *state = vterm_obtain_state(vterm);
    VTermValue bold, underline, italic, blink, reverse, conceal, strike, foreground, background;
    vterm_state_get_penattr(state, VTERM_ATTR_BOLD, &bold);
    vterm_state_get_penattr(state, VTERM_ATTR_UNDERLINE, &underline);
    vterm_state_get_penattr(state, VTERM_ATTR_ITALIC, &italic);
    vterm_state_get_penattr(state, VTERM_ATTR_BLINK, &blink);
    vterm_state_get_penattr(state, VTERM_ATTR_REVERSE, &reverse);
    vterm_state_get_penattr(state, VTERM_ATTR_CONCEAL, &conceal);
    vterm_state_get_penattr(state, VTERM_ATTR_STRIKE, &strike);
    vterm_state_get_penattr(state, VTERM_ATTR_FOREGROUND, &foreground);
    vterm_state_get_penattr(state, VTERM_ATTR_BACKGROUND, &background);
    out += sgr(bold.boolean, underline.number, italic.boolean, blink.boolean, reverse.boolean,
               conceal.boolean, strike.boolean, foreground.color, background.color);
    VTermPos cursor;
    vterm_state_get_cursorpos(state, &cursor);
    out += "\033[" + QByteArray::number(cursor.row + 1) + ';' + QByteArray::number(cursor.col + 1) + 'H';
    out += modes.cursorVisible ? "\033[?25h" : "\033[?25l";
    out += modes.reverse ? "\033[?5h" : "\033[?5l";
    out += "\033[?1003l\033[?1002l\033[?1001l\033[?1000l";
    if (modes.mouse == VTERM_PROP_MOUSE_CLICK)
        out += "\033[?1000h";
    else if (modes.mouse == VTERM_PROP_MOUSE_DRAG)
        out += "\033[?1002h";
    else if (modes.mouse == VTERM_PROP_MOUSE_MOVE)
        out += "\033[?1003h";
    QByteArray all;
    for (const auto &step : steps)
        all += step.first;
    out += privateModes(all);
    if (!modes.title.isEmpty())
        out += "\033]0;" + modes.title + '\007';
    vterm_free(vterm);
    return out;
}

// --- The client ---

MoshClient::MoshClient()
    : m_encrypt(nullptr)
    , m_decrypt(nullptr)
    , m_remoteLength(0)
    , m_lastPortChoice(0)
    , m_lastRoundtripSuccess(0)
    , m_nextSeq(0)
    , m_expectedSeq(0)
    , m_savedTimestamp(quint16(-1))
    , m_savedTimestampReceivedAt(0)
    , m_rttHit(false)
    , m_srtt(1000)
    , m_rttvar(500)
    , m_fragmentId(0)
    , m_assemblyId(quint64(-1))
    , m_fragmentsArrived(0)
    , m_fragmentsTotal(-1)
    , m_eventBase(0)
    , m_assumed(0)
    , m_nextAckTime(0)
    , m_nextSendTime(0)
    , m_mindelayClock(quint64(-1))
    , m_lastHeard(0)
    , m_pendingDataAck(false)
    , m_shutdownInProgress(false)
    , m_shutdownTries(0)
    , m_shutdownStart(0)
    , m_lastAckSent(0)
    , m_ackNum(0)
    , m_checkpointNum(0)
    , m_checkpointColumns(80)
    , m_checkpointRows(24)
    , m_heard(false)
    , m_finished(false)
{
    std::memset(&m_remote, 0, sizeof(m_remote));
    std::memset(m_key, 0, sizeof(m_key));
    m_clock.start();
}

MoshClient::~MoshClient()
{
    for (const Socket &socket : m_sockets)
        close(socket.fd);
    if (m_encrypt)
        EVP_CIPHER_CTX_free(m_encrypt);
    if (m_decrypt)
        EVP_CIPHER_CTX_free(m_decrypt);
    std::memset(m_key, 0, sizeof(m_key));
}

bool MoshClient::start(const sockaddr *address, socklen_t length, const QByteArray &key, int columns, int rows)
{
    // 22 characters of base64 without the padding, 16 bytes
    const QByteArray decoded = QByteArray::fromBase64(key + "==");
    if (key.size() != 22 || decoded.size() != 16 || decoded.toBase64() != key + "==") {
        fail(tr("The server sent an invalid mosh key"));
        return false;
    }
    std::memcpy(m_key, decoded.constData(), 16);
    if (length > sizeof(m_remote)) {
        fail(tr("Invalid server address"));
        return false;
    }
    std::memcpy(&m_remote, address, length);
    m_remoteLength = length;

    m_encrypt = EVP_CIPHER_CTX_new();
    m_decrypt = EVP_CIPHER_CTX_new();
    if (!m_encrypt || !m_decrypt
            || EVP_CipherInit_ex(m_encrypt, EVP_aes_128_ocb(), nullptr, m_key, nullptr, 1) != 1
            || EVP_CIPHER_CTX_ctrl(m_encrypt, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) != 1
            || EVP_CIPHER_CTX_ctrl(m_encrypt, EVP_CTRL_AEAD_SET_TAG, 16, nullptr) != 1
            || EVP_CipherInit_ex(m_decrypt, EVP_aes_128_ocb(), nullptr, m_key, nullptr, 0) != 1
            || EVP_CIPHER_CTX_ctrl(m_decrypt, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) != 1
            || EVP_CIPHER_CTX_ctrl(m_decrypt, EVP_CTRL_AEAD_SET_TAG, 16, nullptr) != 1) {
        fail(tr("AES-OCB encryption is not available"));
        return false;
    }

    m_lastHeard = now();
    m_lastRoundtripSuccess = now();
    m_sent.append(SentState { 0, 0, now() });
    m_nextAckTime = now();
    m_nextSendTime = now();
    if (!newSocket())
        return false;
    // The server starts out with an 80x24 screen and learns the real size from the first packet
    resize(columns, rows);
    return true;
}

QVector<int> MoshClient::fds() const
{
    QVector<int> fds;
    for (const Socket &socket : m_sockets)
        fds.append(socket.fd);
    return fds;
}

quint64 MoshClient::now() const
{
    return quint64(m_clock.elapsed());
}

quint16 MoshClient::timestamp16() const
{
    quint16 timestamp = quint16(now() % 65536);
    // -1 means no timestamp
    if (timestamp == quint16(-1))
        ++timestamp;
    return timestamp;
}

quint64 MoshClient::timeout() const
{
    const quint64 rto = quint64(std::ceil(m_srtt + 4 * m_rttvar));
    return qBound(MinRto, rto, MaxRto);
}

quint64 MoshClient::sendInterval() const
{
    return qBound(SendIntervalMin, quint64(std::ceil(m_srtt / 2.0)), SendIntervalMax);
}

qint64 MoshClient::silence() const
{
    return qint64(now() - m_lastHeard);
}

bool MoshClient::finished() const
{
    return m_finished;
}

void MoshClient::fail(const QString &message)
{
    if (m_errorString.isEmpty())
        m_errorString = message;
    m_finished = true;
}

bool MoshClient::newSocket()
{
    const int fd = socket(m_remote.ss_family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fail(tr("Could not create socket: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        return false;
    }
#ifdef IP_MTU_DISCOVER
    if (m_remote.ss_family == AF_INET) {
        const int flag = IP_PMTUDISC_DONT;
        setsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER, &flag, sizeof(flag));
    }
#endif
    m_sockets.append(Socket { fd, now() });
    m_lastPortChoice = now();
    // Old sockets stay open a while for packets the server sent to them
    while (m_sockets.size() > MaxSockets) {
        close(m_sockets.first().fd);
        m_sockets.removeFirst();
    }
    return true;
}

void MoshClient::roam()
{
    if (!m_finished && !m_sockets.isEmpty())
        newSocket();
}

QByteArray MoshClient::encrypt(quint64 nonce, const QByteArray &plaintext)
{
    unsigned char iv[12] = { 0 };
    qToBigEndian(nonce, iv + 4);
    QByteArray out(8 + plaintext.size() + 16, Qt::Uninitialized);
    unsigned char *data = reinterpret_cast<unsigned char *>(out.data());
    std::memcpy(data, iv + 4, 8);
    int length = 0;
    int finalLength = 0;
    if (EVP_EncryptInit_ex(m_encrypt, nullptr, nullptr, nullptr, iv) != 1
            || EVP_EncryptUpdate(m_encrypt, data + 8, &length,
                                 reinterpret_cast<const unsigned char *>(plaintext.constData()), plaintext.size()) != 1
            || EVP_EncryptFinal_ex(m_encrypt, data + 8 + length, &finalLength) != 1
            || length + finalLength != plaintext.size()
            || EVP_CIPHER_CTX_ctrl(m_encrypt, EVP_CTRL_AEAD_GET_TAG, 16, data + 8 + plaintext.size()) != 1)
        return QByteArray();
    return out;
}

bool MoshClient::decrypt(const QByteArray &packet, quint64 *nonce, QByteArray *plaintext)
{
    if (packet.size() < 8 + 16)
        return false;
    const unsigned char *data = reinterpret_cast<const unsigned char *>(packet.constData());
    unsigned char iv[12] = { 0 };
    std::memcpy(iv + 4, data, 8);
    *nonce = qFromBigEndian<quint64>(data);
    const int length = packet.size() - 8 - 16;
    plaintext->resize(length);
    unsigned char tag[16];
    std::memcpy(tag, data + 8 + length, 16);
    int outLength = 0;
    int finalLength = 0;
    return EVP_DecryptInit_ex(m_decrypt, nullptr, nullptr, nullptr, iv) == 1
            && EVP_CIPHER_CTX_ctrl(m_decrypt, EVP_CTRL_AEAD_SET_TAG, 16, tag) == 1
            && EVP_DecryptUpdate(m_decrypt, reinterpret_cast<unsigned char *>(plaintext->data()), &outLength,
                                 data + 8, length) == 1
            && EVP_DecryptFinal_ex(m_decrypt, reinterpret_cast<unsigned char *>(plaintext->data()) + outLength,
                                   &finalLength) == 1
            && outLength + finalLength == length;
}

void MoshClient::sendPacket(const QByteArray &payload)
{
    if (m_sockets.isEmpty())
        return;
    quint16 timestampReply = quint16(-1);
    if (now() - m_savedTimestampReceivedAt < 1000) {
        // Held for a while, which the server must not count as network time
        timestampReply = quint16(m_savedTimestamp + (now() - m_savedTimestampReceivedAt));
        m_savedTimestamp = quint16(-1);
        m_savedTimestampReceivedAt = 0;
    }
    QByteArray plaintext(4, Qt::Uninitialized);
    qToBigEndian(timestamp16(), reinterpret_cast<uchar *>(plaintext.data()));
    qToBigEndian(timestampReply, reinterpret_cast<uchar *>(plaintext.data()) + 2);
    plaintext += payload;
    const QByteArray packet = encrypt(m_nextSeq++ & SequenceMask, plaintext);
    if (packet.isEmpty())
        return;
    // Lost like any datagram when the network is down, the timers send again
    ::sendto(m_sockets.last().fd, packet.constData(), size_t(packet.size()), MSG_DONTWAIT | MSG_NOSIGNAL,
             reinterpret_cast<const sockaddr *>(&m_remote), m_remoteLength);

    if (now() - m_lastPortChoice > PortHopInterval && now() - m_lastRoundtripSuccess > PortHopInterval)
        newSocket();
}

void MoshClient::readable(int fd)
{
    for (int i = 0; i < 64 && !m_finished; ++i) {
        char buffer[ReceiveBuffer];
        const ssize_t length = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT | MSG_TRUNC);
        if (length < 0)
            break;
        if (length > ssize_t(sizeof(buffer)))
            continue;
        QByteArray packet(buffer, int(length));
        quint64 nonce;
        QByteArray plaintext;
        // Anything that is not from the server is dropped
        if (!decrypt(packet, &nonce, &plaintext) || !(nonce & DirectionMask) || plaintext.size() < 4)
            continue;
        const quint64 seq = nonce & SequenceMask;
        const uchar *data = reinterpret_cast<const uchar *>(plaintext.constData());
        const quint16 timestamp = qFromBigEndian<quint16>(data);
        const quint16 timestampReply = qFromBigEndian<quint16>(data + 2);
        // Late packets still carry state, but not timing
        if (seq >= m_expectedSeq) {
            m_expectedSeq = seq + 1;
            if (timestamp != quint16(-1)) {
                m_savedTimestamp = timestamp;
                m_savedTimestampReceivedAt = now();
            }
            if (timestampReply != quint16(-1)) {
                const double rtt = quint16(timestamp16() - timestampReply);
                // Large values are a server that was suspended
                if (rtt < 5000) {
                    if (!m_rttHit) {
                        m_srtt = rtt;
                        m_rttvar = rtt / 2;
                        m_rttHit = true;
                    } else {
                        m_rttvar = 0.75 * m_rttvar + 0.25 * std::fabs(m_srtt - rtt);
                        m_srtt = 0.875 * m_srtt + 0.125 * rtt;
                    }
                }
            }
        }
        m_heard = true;
        receiveFragment(plaintext.mid(4));
    }
    // Old sockets go once the newest one has been in use for long enough
    while (m_sockets.size() > 1 && now() - m_lastPortChoice > MaxOldSocketAge) {
        close(m_sockets.first().fd);
        m_sockets.removeFirst();
    }
}

void MoshClient::receiveFragment(const QByteArray &fragment)
{
    if (fragment.size() < FragmentHeader)
        return;
    const uchar *data = reinterpret_cast<const uchar *>(fragment.constData());
    const quint64 id = qFromBigEndian<quint64>(data);
    const quint16 combined = qFromBigEndian<quint16>(data + 8);
    const bool final = combined & 0x8000;
    const int number = combined & 0x7fff;
    if (number >= MaxFragments)
        return;

    if (id != m_assemblyId) {
        m_assemblyId = id;
        m_fragments = QVector<QByteArray>(number + 1);
        m_fragmentArrived = QVector<bool>(number + 1, false);
        m_fragmentsArrived = 0;
        m_fragmentsTotal = -1;
    }
    if (m_fragments.size() < number + 1) {
        m_fragments.resize(number + 1);
        m_fragmentArrived.resize(number + 1);
    }
    if (!m_fragmentArrived.at(number)) {
        m_fragments[number] = fragment.mid(FragmentHeader);
        m_fragmentArrived[number] = true;
        ++m_fragmentsArrived;
    }
    if (final) {
        m_fragmentsTotal = number + 1;
        m_fragments.resize(m_fragmentsTotal);
        m_fragmentArrived.resize(m_fragmentsTotal);
        m_fragmentsArrived = int(m_fragmentArrived.count(true));
    }
    if (m_fragmentsArrived != m_fragmentsTotal)
        return;

    QByteArray compressed;
    for (const QByteArray &part : m_fragments)
        compressed += part;
    m_fragments.clear();
    m_fragmentArrived.clear();
    m_fragmentsArrived = 0;
    m_fragmentsTotal = -1;
    QByteArray instruction;
    if (uncompress(compressed, &instruction))
        receiveInstruction(instruction);
}

void MoshClient::receiveInstruction(const QByteArray &instruction)
{
    quint64 version = 0;
    quint64 oldNum = 0;
    quint64 newNum = 0;
    quint64 ackNum = 0;
    quint64 throwawayNum = 0;
    QByteArray diff;
    ProtoReader reader(instruction);
    int field;
    int wireType;
    while (reader.next(&field, &wireType)) {
        if (wireType == 0 && field >= 1 && field <= 5) {
            quint64 value;
            reader.varint(&value);
            switch (field) {
            case 1: version = value; break;
            case 2: oldNum = value; break;
            case 3: newNum = value; break;
            case 4: ackNum = value; break;
            case 5: throwawayNum = value; break;
            }
        } else if (wireType == 2 && field == 6) {
            reader.bytes(&diff);
        } else {
            reader.skip(wireType);
        }
    }
    if (!reader.ok())
        return;
    if (version != ProtocolVersion) {
        fail(tr("The server speaks mosh protocol version %1 instead of %2").arg(version).arg(ProtocolVersion));
        return;
    }

    processAcknowledgment(ackNum);
    m_lastRoundtripSuccess = m_sent.first().timestamp;
    applyDiff(oldNum, newNum, diff);
    throwAwayUntil(throwawayNum);
}

void MoshClient::applyDiff(quint64 oldNum, quint64 newNum, const QByteArray &diff)
{
    // Already have it
    if (newNum == m_checkpointNum)
        return;
    for (const ReceivedState &state : m_received) {
        if (state.num == newNum)
            return;
    }
    // Changes from a state that is gone or never came are dropped, the
    // server sends them again from one acknowledged
    int base = -2;
    if (oldNum == m_checkpointNum)
        base = -1;
    for (int i = 0; i < m_received.size() && base == -2; ++i) {
        if (m_received.at(i).num == oldNum)
            base = i;
    }
    if (base == -2)
        return;

    // A HostMessage: repeated Instruction { HostBytes hostbytes = 2; ResizeMessage resize = 3; EchoAck echoack = 7 }
    // The server's screen size, which its resizes change
    QByteArray bytes;
    int columns = base < 0 ? m_checkpointColumns : m_received.at(base).columns;
    int rows = base < 0 ? m_checkpointRows : m_received.at(base).rows;
    ProtoReader message(diff);
    int field;
    int wireType;
    while (message.next(&field, &wireType)) {
        QByteArray instruction;
        if (field != 1 || wireType != 2 || !message.bytes(&instruction)) {
            message.skip(wireType);
            continue;
        }
        ProtoReader inner(instruction);
        int innerField;
        int innerType;
        while (inner.next(&innerField, &innerType)) {
            QByteArray extension;
            if (innerType != 2 || (innerField != 2 && innerField != 3) || !inner.bytes(&extension)) {
                inner.skip(innerType);
                continue;
            }
            ProtoReader values(extension);
            int valueField;
            int valueType;
            while (values.next(&valueField, &valueType)) {
                quint64 number;
                if (innerField == 2 && valueField == 4 && valueType == 2) {
                    QByteArray hostBytes;
                    values.bytes(&hostBytes);
                    bytes += hostBytes;
                } else if (innerField == 3 && (valueField == 5 || valueField == 6) && valueType == 0) {
                    values.varint(&number);
                    const int size = int(qBound<quint64>(1, number, 4096));
                    if (valueField == 5)
                        columns = size;
                    else
                        rows = size;
                } else {
                    values.skip(valueType);
                }
            }
        }
    }
    if (!message.ok())
        return;

    if (base == m_received.size() - 1) {
        m_output.append(Output { bytes, false });
    } else {
        // The terminal shows a later state than these changes start from, so
        // it is put back into that state first
        QList<QPair<QByteArray, QSize> > steps;
        steps.append(qMakePair(m_checkpoint, QSize(m_checkpointColumns, m_checkpointRows)));
        for (int i = 0; i <= base; ++i)
            steps.append(qMakePair(m_received.at(i).diff, QSize(m_received.at(i).columns, m_received.at(i).rows)));
        m_received.erase(m_received.begin() + base + 1, m_received.end());
        m_output.append(Output { describeScreen(steps) + bytes, true });
    }
    m_received.append(ReceivedState { newNum, bytes, columns, rows });

    m_ackNum = newNum;
    m_lastHeard = now();
    if (!diff.isEmpty())
        m_pendingDataAck = true;
}

void MoshClient::throwAwayUntil(quint64 num)
{
    // The server no longer sends changes from states before num, so they
    // fold into the checkpoint. Only once there are many, as it costs a replay.
    int last = -1;
    int bytes = 0;
    for (int i = 0; i < m_received.size() && m_received.at(i).num < num; ++i) {
        last = i;
        bytes += m_received.at(i).diff.size();
    }
    // The newest state stays, the terminal shows it
    last = qMin(last, m_received.size() - 2);
    if (last < 0 || (last + 1 < MaxReceivedStates && bytes < MaxReceivedBytes))
        return;
    QList<QPair<QByteArray, QSize> > steps;
    steps.append(qMakePair(m_checkpoint, QSize(m_checkpointColumns, m_checkpointRows)));
    for (int i = 0; i <= last; ++i)
        steps.append(qMakePair(m_received.at(i).diff, QSize(m_received.at(i).columns, m_received.at(i).rows)));
    m_checkpoint = describeScreen(steps);
    m_checkpointNum = m_received.at(last).num;
    m_checkpointColumns = m_received.at(last).columns;
    m_checkpointRows = m_received.at(last).rows;
    m_received.erase(m_received.begin(), m_received.begin() + last + 1);
}

QList<MoshClient::Output> MoshClient::takeOutput()
{
    QList<Output> output;
    output.swap(m_output);
    return output;
}

// --- Sending the user's input, as mosh's TransportSender ---

void MoshClient::write(const QByteArray &keys)
{
    if (m_shutdownInProgress || keys.isEmpty())
        return;
    if (!m_events.isEmpty() && m_events.last().columns == 0
            && m_eventBase + m_events.size() > m_sent.last().events) {
        // Not sent yet, so it can still grow
        m_events.last().keys += keys;
        return;
    }
    m_events.append(UserEvent { keys, 0, 0 });
}

void MoshClient::resize(int columns, int rows)
{
    if (m_shutdownInProgress || columns <= 0 || rows <= 0)
        return;
    m_events.append(UserEvent { QByteArray(), columns, rows });
}

QByteArray MoshClient::userDiff(qint64 from) const
{
    // A UserMessage: repeated Instruction { Keystroke keystroke = 2; ResizeMessage resize = 3 }
    QByteArray message;
    for (qint64 i = qMax<qint64>(from, m_eventBase) - m_eventBase; i < m_events.size(); ++i) {
        const UserEvent &event = m_events.at(int(i));
        QByteArray value;
        QByteArray instruction;
        if (event.columns > 0) {
            putUint(&value, 5, quint64(event.columns));
            putUint(&value, 6, quint64(event.rows));
            putBytes(&instruction, 3, value);
        } else {
            putBytes(&value, 4, event.keys);
            putBytes(&instruction, 2, value);
        }
        putBytes(&message, 1, instruction);
    }
    return message;
}

void MoshClient::updateAssumedReceiverState()
{
    // Gives the benefit of the doubt to states sent recently enough
    m_assumed = 0;
    for (int i = 1; i < m_sent.size(); ++i) {
        if (now() - m_sent.at(i).timestamp >= timeout() + AckDelay)
            return;
        m_assumed = i;
    }
}

void MoshClient::calculateTimers()
{
    updateAssumedReceiverState();
    // Events the server has are no longer needed
    while (m_eventBase < m_sent.first().events && !m_events.isEmpty()) {
        m_events.removeFirst();
        ++m_eventBase;
    }

    const quint64 time = now();
    const qint64 events = m_eventBase + m_events.size();
    if (m_pendingDataAck && m_nextAckTime > time + AckDelay)
        m_nextAckTime = time + AckDelay;
    if (events != m_sent.last().events) {
        if (m_mindelayClock == quint64(-1))
            m_mindelayClock = time;
        m_nextSendTime = qMax(m_mindelayClock + SendMindelay, m_sent.last().timestamp + sendInterval());
    } else if (events != m_sent.at(m_assumed).events && m_lastHeard + ActiveRetryTimeout > time) {
        m_nextSendTime = m_sent.last().timestamp + sendInterval();
        if (m_mindelayClock != quint64(-1))
            m_nextSendTime = qMax(m_nextSendTime, m_mindelayClock + SendMindelay);
    } else if (events != m_sent.first().events && m_lastHeard + ActiveRetryTimeout > time) {
        m_nextSendTime = m_sent.last().timestamp + timeout() + AckDelay;
    } else {
        m_nextSendTime = quint64(-1);
    }
    // Speeds up the shutdown either way
    if (m_shutdownInProgress || m_ackNum == Shutdown)
        m_nextAckTime = m_sent.last().timestamp + sendInterval();
}

int MoshClient::waitTime()
{
    if (m_finished)
        return INT_MAX;
    calculateTimers();
    const quint64 next = qMin(m_nextAckTime, m_nextSendTime);
    const quint64 time = now();
    return next > time ? int(qMin<quint64>(next - time, INT_MAX)) : 0;
}

void MoshClient::tick()
{
    if (m_finished)
        return;
    calculateTimers();
    const quint64 time = now();
    if (time >= m_nextAckTime || time >= m_nextSendTime) {
        const qint64 events = m_eventBase + m_events.size();
        if (events == m_sent.at(m_assumed).events) {
            if (time >= m_nextAckTime) {
                sendEmptyAck();
                m_mindelayClock = quint64(-1);
            }
            if (time >= m_nextSendTime) {
                m_nextSendTime = quint64(-1);
                m_mindelayClock = quint64(-1);
            }
        } else {
            sendToReceiver(userDiff(m_sent.at(m_assumed).events));
            m_mindelayClock = quint64(-1);
        }
    }

    if (m_shutdownInProgress && (m_sent.first().num == Shutdown || m_shutdownTries >= ShutdownRetries
                                 || now() - m_shutdownStart >= ActiveRetryTimeout))
        m_finished = true;
    // The server ended the session and has been told it was heard
    if (m_lastAckSent == Shutdown)
        m_finished = true;
}

void MoshClient::shutdown()
{
    if (m_shutdownInProgress || m_finished)
        return;
    m_shutdownInProgress = true;
    m_shutdownStart = now();
}

void MoshClient::sendEmptyAck()
{
    const quint64 time = now();
    const quint64 num = m_shutdownInProgress ? Shutdown : m_sent.last().num + 1;
    addSentState(time, num, m_eventBase + m_events.size());
    sendInstruction(QByteArray(), num);
    m_nextAckTime = time + AckInterval;
    m_nextSendTime = quint64(-1);
}

void MoshClient::sendToReceiver(const QByteArray &diff)
{
    const qint64 events = m_eventBase + m_events.size();
    quint64 num = events == m_sent.last().events ? m_sent.last().num : m_sent.last().num + 1;
    if (m_shutdownInProgress)
        num = Shutdown;
    if (num == m_sent.last().num)
        m_sent.last().timestamp = now();
    else
        addSentState(now(), num, events);
    sendInstruction(diff, num);
    m_assumed = m_sent.size() - 1;
    m_nextAckTime = now() + AckInterval;
    m_nextSendTime = quint64(-1);
}

void MoshClient::addSentState(quint64 timestamp, quint64 num, qint64 events)
{
    m_sent.append(SentState { num, events, timestamp });
    if (m_sent.size() > MaxSentStates)
        m_sent.removeAt(m_sent.size() - 16);
}

void MoshClient::sendInstruction(const QByteArray &diff, quint64 newNum)
{
    // A TransportBuffers.Instruction
    unsigned char chaffBytes[16];
    unsigned char chaffLength = 0;
    RAND_bytes(&chaffLength, 1);
    chaffLength %= 17;
    RAND_bytes(chaffBytes, chaffLength);
    QByteArray instruction;
    putUint(&instruction, 1, ProtocolVersion);
    putUint(&instruction, 2, m_sent.at(m_assumed).num);
    putUint(&instruction, 3, newNum);
    putUint(&instruction, 4, m_ackNum);
    putUint(&instruction, 5, m_sent.first().num);
    putBytes(&instruction, 6, diff);
    putBytes(&instruction, 7, QByteArray(reinterpret_cast<const char *>(chaffBytes), chaffLength));
    if (newNum == Shutdown)
        ++m_shutdownTries;

    QByteArray payload = compress(instruction);
    // A new id for each instruction, the server keeps fragments of one id together
    const quint64 id = m_fragmentId++;
    quint16 number = 0;
    do {
        const QByteArray part = payload.left(FragmentPayload);
        payload.remove(0, part.size());
        QByteArray fragment(FragmentHeader, Qt::Uninitialized);
        qToBigEndian(id, reinterpret_cast<uchar *>(fragment.data()));
        qToBigEndian(quint16((payload.isEmpty() ? 0x8000 : 0) | number++), reinterpret_cast<uchar *>(fragment.data()) + 8);
        sendPacket(fragment + part);
    } while (!payload.isEmpty() && number < 0x7fff);
    m_lastAckSent = m_ackNum;
    m_pendingDataAck = false;
}

void MoshClient::processAcknowledgment(quint64 ackNum)
{
    // Ignored for a state already culled
    bool known = false;
    for (const SentState &state : m_sent)
        known = known || state.num == ackNum;
    if (!known)
        return;
    while (m_sent.first().num < ackNum)
        m_sent.removeFirst();
}

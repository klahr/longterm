#include "terminal.h"

#include "colorschemes.h"

#include <QUrl>

#include <cstring>

static const int MaxScrollbackLines = 5000;
// Larger clipboard writes from the server are dropped
static const int MaxClipboardBytes = 1024 * 1024;
// Longer titles and notifications are cut
static const int MaxTitleBytes = 4096;
// Longer activity details are cut, they share a line with the connection name
static const int MaxActivityDetailCharacters = 200;

const VTermScreenCallbacks Terminal::s_screenCallbacks = {
    &Terminal::onDamage,
    nullptr, // moverect, let libvterm report damage instead
    &Terminal::onMoveCursor,
    &Terminal::onSetTermProp,
    &Terminal::onBell,
    nullptr, // resize
    &Terminal::onPushLine,
    &Terminal::onPopLine,
    &Terminal::onClearScrollback,
};

const VTermSelectionCallbacks Terminal::s_selectionCallbacks = {
    &Terminal::onSelectionSet,
    nullptr, // query, the server is not allowed to read the clipboard
};

const VTermStateFallbacks Terminal::s_fallbacks = {
    nullptr, // control
    nullptr, // csi
    &Terminal::onOsc,
    nullptr, // dcs
    nullptr, // apc
    nullptr, // pm
    nullptr, // sos
};

Terminal::Terminal(QObject *parent)
    : QObject(parent)
    , m_rows(24)
    , m_columns(80)
    , m_cursorVisible(true)
    , m_altScreen(false)
    , m_mouseMode(VTERM_PROP_MOUSE_NONE)
    , m_answersQueries(true)
    , m_writing(false)
    , m_keepScrolledLines(true)
    , m_clipboardTooLarge(false)
    , m_scrolledLines(0)
    , m_droppedLines(0)
{
    m_cursor.row = 0;
    m_cursor.col = 0;

    m_vterm = vterm_new(m_rows, m_columns);
    vterm_set_utf8(m_vterm, 1);
    vterm_output_set_callback(m_vterm, &Terminal::onOutput, this);

    VTermColor foreground;
    VTermColor background;
    vterm_color_rgb(&foreground, 230, 230, 230);
    vterm_color_rgb(&background, 0, 0, 0);
    vterm_state_set_default_colors(vterm_obtain_state(m_vterm), &foreground, &background);

    m_screen = vterm_obtain_screen(m_vterm);
    vterm_screen_set_callbacks(m_screen, &s_screenCallbacks, this);
    vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_SCROLL);
    vterm_screen_enable_altscreen(m_screen, 1);
    // Rewrapping long lines on resize reads past its buffers in libvterm 0.3.3
    vterm_screen_enable_reflow(m_screen, false);
    vterm_screen_reset(m_screen, 1);
    vterm_state_set_selection_callbacks(vterm_obtain_state(m_vterm), &s_selectionCallbacks, this,
                                        m_clipboardBuffer, sizeof(m_clipboardBuffer));
    vterm_screen_set_unrecognised_fallbacks(m_screen, &s_fallbacks, this);
}

Terminal::~Terminal()
{
    vterm_free(m_vterm);
}

VTermScreenCell Terminal::cell(int row, int column) const
{
    if (row >= 0) {
        VTermScreenCell cell;
        VTermPos pos;
        pos.row = row;
        pos.col = column;
        if (vterm_screen_get_cell(m_screen, pos, &cell))
            return cell;
        return blankCell();
    }

    const int index = m_scrollback.size() + row;
    if (index < 0 || column >= m_scrollback.at(index).size())
        return blankCell();
    return m_scrollback.at(index).at(column);
}

QColor Terminal::color(VTermColor color) const
{
    vterm_screen_convert_color_to_rgb(m_screen, &color);
    return QColor(color.rgb.red, color.rgb.green, color.rgb.blue);
}

void Terminal::resize(int rows, int columns)
{
    if (rows <= 0 || columns <= 0 || (rows == m_rows && columns == m_columns))
        return;
    m_rows = rows;
    m_columns = columns;
    vterm_set_size(m_vterm, rows, columns);
    vterm_screen_flush_damage(m_screen);
    emit sizeChanged();
}

void Terminal::write(const QByteArray &data, bool keepScrolledLines)
{
    // Output while writing answers the server, the rest is typing
    m_writing = true;
    m_keepScrolledLines = keepScrolledLines;
    vterm_input_write(m_vterm, data.constData(), data.size());
    vterm_screen_flush_damage(m_screen);
    m_keepScrolledLines = true;
    m_writing = false;
}

void Terminal::sendKey(VTermKey key, VTermModifier modifiers)
{
    vterm_keyboard_key(m_vterm, key, modifiers);
}

void Terminal::sendChar(uint ucs4, VTermModifier modifiers)
{
    // libvterm encodes most Ctrl combinations as CSI u, which few servers
    // understand, so send the classic control bytes instead
    if (modifiers & VTERM_MOD_CTRL) {
        uint c = ucs4;
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        if (c == ' ' || (c >= '@' && c <= '_')) {
            QByteArray bytes;
            if (modifiers & VTERM_MOD_ALT)
                bytes.append('\x1b');
            bytes.append(char(c == ' ' ? 0 : c & 0x1f));
            emit outputReady(bytes);
            return;
        }
    }
    // The character is already shifted, and Shift would force CSI u
    vterm_keyboard_unichar(m_vterm, ucs4, VTermModifier(modifiers & ~VTERM_MOD_SHIFT));
}

void Terminal::sendWheel(bool up, int row, int column)
{
    vterm_mouse_move(m_vterm, row, column, VTERM_MOD_NONE);
    vterm_mouse_button(m_vterm, up ? 4 : 5, true, VTERM_MOD_NONE);
}

void Terminal::paste(const QString &text)
{
    // Terminals send carriage returns for Enter, and brackets let the remote
    // program tell pasted text from typing when it has asked for that
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\r"));
    normalized.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    normalized.remove(QStringLiteral("\x1b[201~"));
    vterm_keyboard_start_paste(m_vterm);
    emit outputReady(normalized.toUtf8());
    vterm_keyboard_end_paste(m_vterm);
}

void Terminal::setColors(const ColorScheme &scheme)
{
    VTermState *state = vterm_obtain_state(m_vterm);
    VTermColor foreground;
    VTermColor background;
    vterm_color_rgb(&foreground, qRed(scheme.foreground), qGreen(scheme.foreground), qBlue(scheme.foreground));
    vterm_color_rgb(&background, qRed(scheme.background), qGreen(scheme.background), qBlue(scheme.background));
    vterm_state_set_default_colors(state, &foreground, &background);
    for (int i = 0; i < 16; ++i) {
        VTermColor color;
        vterm_color_rgb(&color, qRed(scheme.palette[i]), qGreen(scheme.palette[i]), qBlue(scheme.palette[i]));
        vterm_state_set_palette_color(state, i, &color);
    }
    // Cells keep palette indices, so existing text picks up the new colors
    emit contentChanged();
}

VTermScreenCell Terminal::blankCell() const
{
    VTermScreenCell cell;
    std::memset(&cell, 0, sizeof(cell));
    cell.width = 1;
    vterm_state_get_default_colors(vterm_obtain_state(m_vterm), &cell.fg, &cell.bg);
    return cell;
}

void Terminal::onOutput(const char *bytes, size_t length, void *user)
{
    Terminal *terminal = static_cast<Terminal *>(user);
    if (terminal->m_writing && !terminal->m_answersQueries)
        return;
    emit terminal->outputReady(QByteArray(bytes, int(length)));
}

int Terminal::onDamage(VTermRect, void *user)
{
    emit static_cast<Terminal *>(user)->contentChanged();
    return 1;
}

int Terminal::onMoveCursor(VTermPos pos, VTermPos, int visible, void *user)
{
    Terminal *terminal = static_cast<Terminal *>(user);
    terminal->m_cursor = pos;
    terminal->m_cursorVisible = visible;
    emit terminal->contentChanged();
    return 1;
}

int Terminal::onSetTermProp(VTermProp prop, VTermValue *value, void *user)
{
    Terminal *terminal = static_cast<Terminal *>(user);
    switch (prop) {
    case VTERM_PROP_CURSORVISIBLE:
        terminal->m_cursorVisible = value->boolean;
        emit terminal->contentChanged();
        return 1;
    case VTERM_PROP_ALTSCREEN:
        terminal->m_altScreen = value->boolean;
        emit terminal->contentChanged();
        return 1;
    case VTERM_PROP_MOUSE:
        terminal->m_mouseMode = value->number;
        return 1;
    case VTERM_PROP_TITLE:
        if (value->string.initial)
            terminal->m_pendingTitle.clear();
        // An unterminated title would otherwise grow for as long as the server sends
        if (terminal->m_pendingTitle.size() < MaxTitleBytes)
            terminal->m_pendingTitle.append(value->string.str, qMin(int(value->string.len),
                                                                    MaxTitleBytes - terminal->m_pendingTitle.size()));
        if (value->string.final) {
            terminal->m_title = QString::fromUtf8(terminal->m_pendingTitle);
            emit terminal->titleChanged();
        }
        return 1;
    default:
        return 0;
    }
}

int Terminal::onBell(void *user)
{
    emit static_cast<Terminal *>(user)->bell();
    return 1;
}

int Terminal::onPushLine(int columns, const VTermScreenCell *cells, void *user)
{
    Terminal *terminal = static_cast<Terminal *>(user);
    if (!terminal->m_keepScrolledLines)
        return 1;
    QVector<VTermScreenCell> line(columns);
    std::memcpy(line.data(), cells, columns * sizeof(VTermScreenCell));
    terminal->m_scrollback.append(line);
    ++terminal->m_scrolledLines;
    if (terminal->m_scrollback.size() > MaxScrollbackLines) {
        terminal->m_scrollback.removeFirst();
        ++terminal->m_droppedLines;
    }
    return 1;
}

int Terminal::onPopLine(int columns, VTermScreenCell *cells, void *user)
{
    Terminal *terminal = static_cast<Terminal *>(user);
    if (terminal->m_scrollback.isEmpty())
        return 0;

    const QVector<VTermScreenCell> line = terminal->m_scrollback.takeLast();
    --terminal->m_scrolledLines;
    const VTermScreenCell blank = terminal->blankCell();
    for (int column = 0; column < columns; ++column)
        cells[column] = column < line.size() ? line.at(column) : blank;
    return 1;
}

int Terminal::onClearScrollback(void *user)
{
    Terminal *terminal = static_cast<Terminal *>(user);
    terminal->m_droppedLines += terminal->m_scrollback.size();
    terminal->m_scrollback.clear();
    emit terminal->contentChanged();
    return 1;
}

int Terminal::onSelectionSet(VTermSelectionMask, VTermStringFragment fragment, void *user)
{
    Terminal *terminal = static_cast<Terminal *>(user);
    if (fragment.initial) {
        terminal->m_pendingClipboard.clear();
        terminal->m_clipboardTooLarge = false;
    }
    if (terminal->m_pendingClipboard.size() + int(fragment.len) > MaxClipboardBytes) {
        terminal->m_pendingClipboard.clear();
        terminal->m_clipboardTooLarge = true;
    }
    if (!terminal->m_clipboardTooLarge)
        terminal->m_pendingClipboard.append(fragment.str, int(fragment.len));
    if (fragment.final) {
        // Empty for invalid base64 as well as for clearing, neither should wipe the clipboard
        if (!terminal->m_pendingClipboard.isEmpty())
            emit terminal->clipboardRequested(QString::fromUtf8(terminal->m_pendingClipboard));
        terminal->m_pendingClipboard.clear();
    }
    return 1;
}

int Terminal::onOsc(int command, VTermStringFragment fragment, void *user)
{
    if (command != 7 && command != 9 && command != 777)
        return 0;
    Terminal *terminal = static_cast<Terminal *>(user);
    if (fragment.initial)
        terminal->m_pendingOsc.clear();
    if (terminal->m_pendingOsc.size() < MaxTitleBytes)
        terminal->m_pendingOsc.append(fragment.str, qMin(int(fragment.len),
                                                         MaxTitleBytes - terminal->m_pendingOsc.size()));
    if (fragment.final) {
        terminal->notifyFromOsc(command, terminal->m_pendingOsc);
        terminal->m_pendingOsc.clear();
    }
    return 1;
}

void Terminal::notifyFromOsc(int command, const QByteArray &payload)
{
    if (command == 7) {
        // "file://host/path" with the path percent-encoded
        const QUrl url(QString::fromUtf8(payload.trimmed()));
        const QString path = url.scheme() == QLatin1String("file") ? url.path() : QString();
        if (path.startsWith(QLatin1Char('/')) && path != m_workingDirectory) {
            m_workingDirectory = path;
            emit workingDirectoryChanged();
        }
        return;
    }
    if (command == 9) {
        // iTerm2 takes the whole payload as the message, ConEmu uses "9;<number>;..." for
        // progress and other commands, which are no notifications
        int digits = 0;
        while (digits < payload.size() && payload.at(digits) >= '0' && payload.at(digits) <= '9')
            ++digits;
        if (digits > 0 && (digits == payload.size() || payload.at(digits) == ';'))
            return;
        if (!payload.trimmed().isEmpty())
            emit notificationRequested(QString(), QString::fromUtf8(payload).trimmed());
        return;
    }

    const QList<QByteArray> parts = payload.split(';');
    // "777;longterm-status;<working|waiting|done>[;<detail>]", anything else clears it,
    // the detail may contain semicolons
    if (parts.at(0) == "longterm-status") {
        const QByteArray status = parts.size() > 1 ? parts.at(1).trimmed() : QByteArray();
        if (status == "working" || status == "waiting" || status == "done") {
            QString detail;
            if (parts.size() > 2)
                detail = QString::fromUtf8(payload.mid(parts.at(0).size() + parts.at(1).size() + 2))
                             .simplified().left(MaxActivityDetailCharacters);
            setActivity(QString::fromLatin1(status), detail);
        } else {
            setActivity(QString());
        }
        return;
    }

    // "777;notify;<title>;<body>", the body may contain semicolons
    if (parts.size() < 3 || parts.at(0) != "notify")
        return;
    const QString title = QString::fromUtf8(parts.at(1)).trimmed();
    const QString body = QString::fromUtf8(payload.mid(parts.at(0).size() + parts.at(1).size() + 2)).trimmed();
    if (!title.isEmpty() || !body.isEmpty())
        emit notificationRequested(title, body);
}

void Terminal::clearActivity()
{
    setActivity(QString());
}

void Terminal::clearWorkingDirectory()
{
    if (m_workingDirectory.isEmpty())
        return;
    m_workingDirectory.clear();
    emit workingDirectoryChanged();
}

void Terminal::setActivity(const QString &activity, const QString &detail)
{
    if (m_activity == activity && m_activityDetail == detail)
        return;
    m_activity = activity;
    m_activityDetail = detail;
    emit activityChanged();
}

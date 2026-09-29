#include "terminal.h"

#include "colorschemes.h"

#include <cstring>

static const int MaxScrollbackLines = 5000;
// Larger clipboard writes from the server are dropped
static const int MaxClipboardBytes = 1024 * 1024;
// Longer titles are cut
static const int MaxTitleBytes = 4096;

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

Terminal::Terminal(QObject *parent)
    : QObject(parent)
    , m_rows(24)
    , m_columns(80)
    , m_cursorVisible(true)
    , m_clipboardTooLarge(false)
    , m_scrolledLines(0)
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

void Terminal::write(const QByteArray &data)
{
    vterm_input_write(m_vterm, data.constData(), data.size());
    vterm_screen_flush_damage(m_screen);
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

void Terminal::paste(const QString &text)
{
    // Terminals send carriage returns for Enter, and brackets let the remote
    // program tell pasted text from typing when it has asked for that
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\r"));
    normalized.replace(QLatin1Char('\n'), QLatin1Char('\r'));
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
    emit static_cast<Terminal *>(user)->outputReady(QByteArray(bytes, int(length)));
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
    QVector<VTermScreenCell> line(columns);
    std::memcpy(line.data(), cells, columns * sizeof(VTermScreenCell));
    terminal->m_scrollback.append(line);
    ++terminal->m_scrolledLines;
    if (terminal->m_scrollback.size() > MaxScrollbackLines)
        terminal->m_scrollback.removeFirst();
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
    static_cast<Terminal *>(user)->m_scrollback.clear();
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

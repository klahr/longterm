#include "terminalview.h"

#include "colorschemes.h"

#include <QFontMetricsF>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QStringList>

#include <cmath>

namespace {

struct KeyMapping {
    int qtKey;
    VTermKey vtermKey;
};

const KeyMapping keyMappings[] = {
    { Qt::Key_Return, VTERM_KEY_ENTER },
    { Qt::Key_Enter, VTERM_KEY_ENTER },
    { Qt::Key_Tab, VTERM_KEY_TAB },
    { Qt::Key_Backspace, VTERM_KEY_BACKSPACE },
    { Qt::Key_Escape, VTERM_KEY_ESCAPE },
    { Qt::Key_Up, VTERM_KEY_UP },
    { Qt::Key_Down, VTERM_KEY_DOWN },
    { Qt::Key_Left, VTERM_KEY_LEFT },
    { Qt::Key_Right, VTERM_KEY_RIGHT },
    { Qt::Key_Insert, VTERM_KEY_INS },
    { Qt::Key_Delete, VTERM_KEY_DEL },
    { Qt::Key_Home, VTERM_KEY_HOME },
    { Qt::Key_End, VTERM_KEY_END },
    { Qt::Key_PageUp, VTERM_KEY_PAGEUP },
    { Qt::Key_PageDown, VTERM_KEY_PAGEDOWN },
};

VTermKey toVTermKey(int key)
{
    for (const KeyMapping &mapping : keyMappings) {
        if (mapping.qtKey == key)
            return mapping.vtermKey;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
        return VTermKey(VTERM_KEY_FUNCTION(key - Qt::Key_F1 + 1));
    return VTERM_KEY_NONE;
}

bool sameStyle(const VTermScreenCell &a, const VTermScreenCell &b)
{
    return vterm_color_is_equal(&a.fg, &b.fg) && vterm_color_is_equal(&a.bg, &b.bg)
            && a.attrs.bold == b.attrs.bold && a.attrs.italic == b.attrs.italic
            && a.attrs.underline == b.attrs.underline && a.attrs.strike == b.attrs.strike
            && a.attrs.reverse == b.attrs.reverse && a.attrs.conceal == b.attrs.conceal;
}

void appendCellText(QString &text, const VTermScreenCell &cell)
{
    // Right half of a wide character, already covered by the cell before it
    if (cell.chars[0] == uint32_t(-1))
        return;
    if (cell.chars[0] == 0) {
        text.append(QLatin1Char(' '));
        return;
    }
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i)
        text.append(QString::fromUcs4(&cell.chars[i], 1));
}

// Paths, URLs and options should select as one word
bool isWordCell(const VTermScreenCell &cell)
{
    const uint32_t c = cell.chars[0];
    if (c == uint32_t(-1))
        return true;
    if (c == 0 || c == ' ' || c == '\t')
        return false;
    static const QString delimiters = QStringLiteral("\"'`()[]{}<>|;,");
    return c > 0xffff || !delimiters.contains(QChar(ushort(c)));
}

}

TerminalView::TerminalView(QQuickItem *parent)
    : QQuickPaintedItem(parent)
    , m_font(QStringLiteral("Monospace"))
    , m_cellWidth(1)
    , m_cellHeight(1)
    , m_ascent(0)
    , m_cursorColor(255, 255, 255, 150)
    , m_scrollOffset(0)
    , m_ctrlLatched(false)
    , m_altLatched(false)
    , m_selectionColor(255, 255, 255, 90)
    , m_hasSelection(false)
    , m_anchorLine(0)
    , m_anchorColumn(0)
    , m_endLine(0)
    , m_endColumn(0)
{
    setFlag(ItemAcceptsInputMethod, true);
    setFlag(ItemIsFocusScope, false);
    setOpaquePainting(true);
    m_font.setStyleHint(QFont::TypeWriter);
    m_font.setPixelSize(24);
    updateCellSize();
}

void TerminalView::setTerminal(Terminal *terminal)
{
    if (m_terminal == terminal)
        return;
    if (m_terminal)
        m_terminal->disconnect(this);
    m_terminal = terminal;
    if (m_terminal) {
        connect(m_terminal.data(), &Terminal::contentChanged, this, &TerminalView::onContentChanged);
        connect(m_terminal.data(), &Terminal::sizeChanged, this, &TerminalView::onSizeChanged);
    }
    clearSelection();
    setScrollOffset(0);
    applyColorScheme();
    updateTerminalSize();
    update();
    emit terminalChanged();
}

void TerminalView::setFontFamily(const QString &family)
{
    if (family.isEmpty() || family == m_font.family())
        return;
    m_font.setFamily(family);
    updateCellSize();
    updateTerminalSize();
    update();
    emit fontChanged();
}

void TerminalView::setFontPixelSize(int size)
{
    if (size <= 0 || size == m_font.pixelSize())
        return;
    m_font.setPixelSize(size);
    updateCellSize();
    updateTerminalSize();
    update();
    emit fontChanged();
}

void TerminalView::setCursorColor(const QColor &color)
{
    if (m_cursorColor == color)
        return;
    m_cursorColor = color;
    update();
    emit cursorColorChanged();
}

void TerminalView::setScrollOffset(int offset)
{
    const int maximum = m_terminal ? m_terminal->scrollbackLines() : 0;
    offset = qBound(0, offset, maximum);
    if (m_scrollOffset == offset)
        return;
    m_scrollOffset = offset;
    update();
    emit scrollOffsetChanged();
}

void TerminalView::setCtrlLatched(bool latched)
{
    if (m_ctrlLatched == latched)
        return;
    m_ctrlLatched = latched;
    emit latchedChanged();
}

void TerminalView::setAltLatched(bool latched)
{
    if (m_altLatched == latched)
        return;
    m_altLatched = latched;
    emit latchedChanged();
}

void TerminalView::setSelectionColor(const QColor &color)
{
    if (m_selectionColor == color)
        return;
    m_selectionColor = color;
    update();
    emit selectionColorChanged();
}

void TerminalView::setColorScheme(const QString &colorScheme)
{
    if (m_colorScheme == colorScheme)
        return;
    m_colorScheme = colorScheme;
    applyColorScheme();
    emit colorSchemeChanged();
}

void TerminalView::sendKey(int key)
{
    if (!m_terminal)
        return;
    const VTermKey vtermKey = toVTermKey(key);
    if (vtermKey == VTERM_KEY_NONE)
        return;
    clearSelection();
    setScrollOffset(0);
    m_terminal->sendKey(vtermKey, takeModifiers(Qt::NoModifier));
}

void TerminalView::sendText(const QString &text)
{
    if (!m_terminal)
        return;
    clearSelection();
    setScrollOffset(0);
    sendCharacters(text, takeModifiers(Qt::NoModifier));
}

void TerminalView::paste(const QString &text)
{
    if (!m_terminal || text.isEmpty())
        return;
    clearSelection();
    setScrollOffset(0);
    m_terminal->paste(text);
}

void TerminalView::startSelection(qreal x, qreal y)
{
    if (!m_terminal)
        return;
    cellAt(x, y, &m_anchorLine, &m_anchorColumn);
    m_endLine = m_anchorLine;
    m_endColumn = m_anchorColumn;
    if (!m_hasSelection) {
        m_hasSelection = true;
        emit selectionChanged();
    }
    update();
}

void TerminalView::updateSelection(qreal x, qreal y)
{
    if (!m_terminal || !m_hasSelection)
        return;
    cellAt(x, y, &m_endLine, &m_endColumn);
    update();
}

bool TerminalView::selectWordAt(qreal x, qreal y)
{
    if (!m_terminal)
        return false;

    int line, column;
    cellAt(x, y, &line, &column);
    const int row = line - m_terminal->scrollbackLines();
    const int columns = m_terminal->columns();

    // Start from the left half when the right half of a wide character was hit
    if (column > 0 && m_terminal->cell(row, column).chars[0] == uint32_t(-1))
        --column;
    if (!isWordCell(m_terminal->cell(row, column)))
        return false;

    int first = column;
    while (first > 0 && isWordCell(m_terminal->cell(row, first - 1)))
        --first;
    int last = column;
    while (last < columns - 1 && isWordCell(m_terminal->cell(row, last + 1)))
        ++last;

    m_anchorLine = line;
    m_anchorColumn = first;
    m_endLine = line;
    m_endColumn = last;
    if (!m_hasSelection) {
        m_hasSelection = true;
        emit selectionChanged();
    }
    update();
    return true;
}

void TerminalView::clearSelection()
{
    if (!m_hasSelection)
        return;
    m_hasSelection = false;
    update();
    emit selectionChanged();
}

QString TerminalView::selectedText() const
{
    if (!m_terminal || !m_hasSelection)
        return QString();

    int startLine, startColumn, endLine, endColumn;
    orderedSelection(&startLine, &startColumn, &endLine, &endColumn);
    const int scrollback = m_terminal->scrollbackLines();
    const int columns = m_terminal->columns();

    QStringList lines;
    for (int line = startLine; line <= endLine; ++line) {
        const int first = line == startLine ? startColumn : 0;
        const int last = line == endLine ? endColumn : columns - 1;
        QString text;
        for (int column = first; column <= last;) {
            const VTermScreenCell cell = m_terminal->cell(line - scrollback, column);
            appendCellText(text, cell);
            column += qMax(1, int(cell.width));
        }
        // Blank cells past the end of the text are not part of the line
        while (text.endsWith(QLatin1Char(' ')))
            text.chop(1);
        lines.append(text);
    }
    return lines.join(QLatin1Char('\n'));
}

void TerminalView::paint(QPainter *painter)
{
    if (!m_terminal) {
        painter->fillRect(boundingRect(), Qt::black);
        return;
    }

    painter->fillRect(boundingRect(), m_terminal->color(m_terminal->blankCell().bg));

    const int rows = m_terminal->rows();
    const int columns = m_terminal->columns();

    for (int row = 0; row < rows; ++row) {
        const int terminalRow = row - m_scrollOffset;
        const qreal y = row * m_cellHeight;

        int column = 0;
        while (column < columns) {
            const VTermScreenCell first = m_terminal->cell(terminalRow, column);
            const int start = column;
            QString text;
            appendCellText(text, first);
            column += qMax(1, int(first.width));

            // Wide characters are drawn on their own so the grid stays aligned
            if (first.width == 1) {
                while (column < columns) {
                    const VTermScreenCell next = m_terminal->cell(terminalRow, column);
                    if (next.width != 1 || !sameStyle(first, next))
                        break;
                    appendCellText(text, next);
                    ++column;
                }
            }

            QColor foreground = m_terminal->color(first.fg);
            QColor background = m_terminal->color(first.bg);
            bool defaultBackground = VTERM_COLOR_IS_DEFAULT_BG(&first.bg);
            if (first.attrs.reverse) {
                qSwap(foreground, background);
                defaultBackground = false;
            }

            const QRectF rect(start * m_cellWidth, y, (column - start) * m_cellWidth, m_cellHeight);
            if (!defaultBackground)
                painter->fillRect(rect, background);
            if (first.attrs.conceal || text.trimmed().isEmpty())
                continue;

            QFont font = first.attrs.bold ? m_boldFont : m_font;
            font.setItalic(first.attrs.italic);
            font.setUnderline(first.attrs.underline != VTERM_UNDERLINE_OFF);
            font.setStrikeOut(first.attrs.strike);
            painter->setFont(font);
            painter->setPen(foreground);
            painter->drawText(QPointF(rect.x(), y + m_ascent), text);
        }
    }

    if (m_hasSelection) {
        int startLine, startColumn, endLine, endColumn;
        orderedSelection(&startLine, &startColumn, &endLine, &endColumn);
        const int topLine = m_terminal->scrollbackLines() - m_scrollOffset;
        for (int row = 0; row < rows; ++row) {
            const int line = topLine + row;
            if (line < startLine || line > endLine)
                continue;
            const int first = line == startLine ? startColumn : 0;
            const int last = line == endLine ? endColumn : columns - 1;
            painter->fillRect(QRectF(first * m_cellWidth, row * m_cellHeight,
                                     (last - first + 1) * m_cellWidth, m_cellHeight), m_selectionColor);
        }
    }

    const VTermPos cursor = m_terminal->cursorPosition();
    const int cursorRow = cursor.row + m_scrollOffset;
    if (m_terminal->cursorVisible() && cursorRow < rows) {
        painter->fillRect(QRectF(cursor.col * m_cellWidth, cursorRow * m_cellHeight,
                                 m_cellWidth, m_cellHeight), m_cursorColor);
    }
}

QVariant TerminalView::inputMethodQuery(Qt::InputMethodQuery query) const
{
    switch (query) {
    case Qt::ImEnabled:
        return true;
    case Qt::ImHints:
        return int(Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase | Qt::ImhPreferLowercase);
    case Qt::ImCursorRectangle:
        if (m_terminal) {
            const VTermPos cursor = m_terminal->cursorPosition();
            return QRectF(cursor.col * m_cellWidth, cursor.row * m_cellHeight, m_cellWidth, m_cellHeight);
        }
        return QRectF();
    case Qt::ImSurroundingText:
        return QString();
    case Qt::ImCursorPosition:
    case Qt::ImAnchorPosition:
        return 0;
    default:
        return QQuickPaintedItem::inputMethodQuery(query);
    }
}

void TerminalView::geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChanged(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        updateTerminalSize();
}

void TerminalView::keyPressEvent(QKeyEvent *event)
{
    if (!m_terminal) {
        event->ignore();
        return;
    }
    clearSelection();
    setScrollOffset(0);

    const VTermModifier modifiers = takeModifiers(event->modifiers());
    const VTermKey key = toVTermKey(event->key());
    if (key != VTERM_KEY_NONE) {
        m_terminal->sendKey(key, modifiers);
    } else if ((modifiers & VTERM_MOD_CTRL) && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z) {
        // With Ctrl held, text() is already a control character
        m_terminal->sendChar('a' + (event->key() - Qt::Key_A), modifiers);
    } else if (!event->text().isEmpty()) {
        sendCharacters(event->text(), modifiers);
    } else {
        event->ignore();
        return;
    }
    event->accept();
}

void TerminalView::inputMethodEvent(QInputMethodEvent *event)
{
    if (m_terminal && !event->commitString().isEmpty()) {
        clearSelection();
        setScrollOffset(0);
        sendCharacters(event->commitString(), takeModifiers(Qt::NoModifier));
    }
    event->accept();
}

void TerminalView::updateTerminalSize()
{
    if (!m_terminal || width() <= 0 || height() <= 0)
        return;
    m_terminal->resize(qMax(1, int(height() / m_cellHeight)), qMax(1, int(width() / m_cellWidth)));
}

void TerminalView::updateCellSize()
{
    m_boldFont = m_font;
    m_boldFont.setBold(true);
    const QFontMetricsF metrics(m_font);
    m_cellWidth = metrics.width(QLatin1Char('M'));
    m_cellHeight = std::ceil(metrics.height());
    m_ascent = metrics.ascent();
}

VTermModifier TerminalView::takeModifiers(Qt::KeyboardModifiers modifiers)
{
    int result = VTERM_MOD_NONE;
    if (modifiers & Qt::ShiftModifier)
        result |= VTERM_MOD_SHIFT;
    if ((modifiers & Qt::ControlModifier) || m_ctrlLatched)
        result |= VTERM_MOD_CTRL;
    if ((modifiers & Qt::AltModifier) || m_altLatched)
        result |= VTERM_MOD_ALT;
    setCtrlLatched(false);
    setAltLatched(false);
    return VTermModifier(result);
}

void TerminalView::sendCharacters(const QString &text, VTermModifier modifiers)
{
    const QVector<uint> characters = text.toUcs4();
    for (uint c : characters) {
        if (c == '\n' || c == '\r')
            m_terminal->sendKey(VTERM_KEY_ENTER, modifiers);
        else
            m_terminal->sendChar(c, modifiers);
    }
}

void TerminalView::onSizeChanged()
{
    // Reflow moves text around, so an old selection would point at the wrong cells
    clearSelection();
    onContentChanged();
}

void TerminalView::cellAt(qreal x, qreal y, int *line, int *column) const
{
    const int row = qBound(0, int(y / m_cellHeight), m_terminal->rows() - 1);
    *column = qBound(0, int(x / m_cellWidth), m_terminal->columns() - 1);
    *line = m_terminal->scrollbackLines() - m_scrollOffset + row;
}

void TerminalView::orderedSelection(int *startLine, int *startColumn, int *endLine, int *endColumn) const
{
    const bool anchorFirst = m_anchorLine < m_endLine
            || (m_anchorLine == m_endLine && m_anchorColumn <= m_endColumn);
    *startLine = anchorFirst ? m_anchorLine : m_endLine;
    *startColumn = anchorFirst ? m_anchorColumn : m_endColumn;
    *endLine = anchorFirst ? m_endLine : m_anchorLine;
    *endColumn = anchorFirst ? m_endColumn : m_anchorColumn;
}

void TerminalView::applyColorScheme()
{
    const ColorScheme &scheme = ColorSchemes::find(m_colorScheme);
    QColor cursor(scheme.cursor);
    cursor.setAlpha(160);
    setCursorColor(cursor);
    QColor selection(scheme.selection);
    selection.setAlpha(140);
    setSelectionColor(selection);
    if (m_terminal)
        m_terminal->setColors(scheme);
    update();
}

void TerminalView::onContentChanged()
{
    // Keep the scrolled-back view clamped as the scrollback grows or shrinks
    setScrollOffset(m_scrollOffset);
    update();
}

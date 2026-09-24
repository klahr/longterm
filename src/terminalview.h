#ifndef TERMINALVIEW_H
#define TERMINALVIEW_H

#include <QColor>
#include <QFont>
#include <QPointer>
#include <QQuickPaintedItem>

#include "terminal.h"

class TerminalView : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(Terminal *terminal READ terminal WRITE setTerminal NOTIFY terminalChanged)
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY fontChanged)
    Q_PROPERTY(int fontPixelSize READ fontPixelSize WRITE setFontPixelSize NOTIFY fontChanged)
    Q_PROPERTY(qreal cellHeight READ cellHeight NOTIFY fontChanged)
    Q_PROPERTY(QColor cursorColor READ cursorColor WRITE setCursorColor NOTIFY cursorColorChanged)
    Q_PROPERTY(int scrollOffset READ scrollOffset WRITE setScrollOffset NOTIFY scrollOffsetChanged)
    Q_PROPERTY(bool ctrlLatched READ ctrlLatched WRITE setCtrlLatched NOTIFY latchedChanged)
    Q_PROPERTY(bool altLatched READ altLatched WRITE setAltLatched NOTIFY latchedChanged)
    Q_PROPERTY(QColor selectionColor READ selectionColor WRITE setSelectionColor NOTIFY selectionColorChanged)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    Q_PROPERTY(QString colorScheme READ colorScheme WRITE setColorScheme NOTIFY colorSchemeChanged)

public:
    explicit TerminalView(QQuickItem *parent = nullptr);

    Terminal *terminal() const { return m_terminal; }
    void setTerminal(Terminal *terminal);
    QString fontFamily() const { return m_font.family(); }
    void setFontFamily(const QString &family);
    int fontPixelSize() const { return m_font.pixelSize(); }
    void setFontPixelSize(int size);
    qreal cellHeight() const { return m_cellHeight; }
    QColor cursorColor() const { return m_cursorColor; }
    void setCursorColor(const QColor &color);
    int scrollOffset() const { return m_scrollOffset; }
    void setScrollOffset(int offset);
    bool ctrlLatched() const { return m_ctrlLatched; }
    void setCtrlLatched(bool latched);
    bool altLatched() const { return m_altLatched; }
    void setAltLatched(bool latched);
    QColor selectionColor() const { return m_selectionColor; }
    void setSelectionColor(const QColor &color);
    bool hasSelection() const { return m_hasSelection; }
    QString colorScheme() const { return m_colorScheme; }
    void setColorScheme(const QString &colorScheme);

    // For on-screen extra keys, takes Qt::Key values
    Q_INVOKABLE void sendKey(int key);
    Q_INVOKABLE void sendText(const QString &text);
    Q_INVOKABLE void paste(const QString &text);

    // Selection points are in item coordinates
    Q_INVOKABLE void startSelection(qreal x, qreal y);
    Q_INVOKABLE void updateSelection(qreal x, qreal y);
    // Returns false when there is no word at the point
    Q_INVOKABLE bool selectWordAt(qreal x, qreal y);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE QString selectedText() const;

    void paint(QPainter *painter) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

signals:
    void terminalChanged();
    void fontChanged();
    void cursorColorChanged();
    void scrollOffsetChanged();
    void latchedChanged();
    void selectionColorChanged();
    void selectionChanged();
    void colorSchemeChanged();

protected:
    void geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;

private:
    void updateTerminalSize();
    void updateCellSize();
    VTermModifier takeModifiers(Qt::KeyboardModifiers modifiers);
    void sendCharacters(const QString &text, VTermModifier modifiers);
    void onContentChanged();
    void onSizeChanged();
    void applyColorScheme();
    // Lines count from the oldest scrollback line so they survive scrolling
    void cellAt(qreal x, qreal y, int *line, int *column) const;
    void orderedSelection(int *startLine, int *startColumn, int *endLine, int *endColumn) const;

    QPointer<Terminal> m_terminal;
    QFont m_font;
    QFont m_boldFont;
    qreal m_cellWidth;
    qreal m_cellHeight;
    qreal m_ascent;
    QColor m_cursorColor;
    int m_scrollOffset;
    bool m_ctrlLatched;
    bool m_altLatched;
    QColor m_selectionColor;
    bool m_hasSelection;
    int m_anchorLine;
    int m_anchorColumn;
    int m_endLine;
    int m_endColumn;
    QString m_colorScheme;
};

#endif // TERMINALVIEW_H

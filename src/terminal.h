#ifndef TERMINAL_H
#define TERMINAL_H

#include <QByteArray>
#include <QColor>
#include <QList>
#include <QObject>
#include <QString>
#include <QVector>

#include <vterm.h>

struct ColorScheme;

class Terminal : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int rows READ rows NOTIFY sizeChanged)
    Q_PROPERTY(int columns READ columns NOTIFY sizeChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)

public:
    explicit Terminal(QObject *parent = nullptr);
    ~Terminal();

    int rows() const { return m_rows; }
    int columns() const { return m_columns; }
    int scrollbackLines() const { return m_scrollback.size(); }
    // Lines moved into the scrollback minus those taken back, which keeps
    // counting when the oldest lines are dropped
    qint64 scrolledLines() const { return m_scrolledLines; }
    qint64 droppedLines() const { return m_droppedLines; }
    QString title() const { return m_title; }
    VTermPos cursorPosition() const { return m_cursor; }
    bool cursorVisible() const { return m_cursorVisible; }
    bool altScreen() const { return m_altScreen; }
    bool mouseReporting() const { return m_mouseMode != VTERM_PROP_MOUSE_NONE; }

    // Rows below zero address the scrollback, -1 being the most recent line
    VTermScreenCell cell(int row, int column) const;
    VTermScreenCell blankCell() const;
    QColor color(VTermColor color) const;

    void resize(int rows, int columns);
    void write(const QByteArray &data);
    void sendKey(VTermKey key, VTermModifier modifiers);
    void sendChar(uint ucs4, VTermModifier modifiers);
    void sendWheel(bool up, int row, int column);
    void paste(const QString &text);
    void setColors(const ColorScheme &scheme);

signals:
    void sizeChanged();
    void titleChanged();
    void contentChanged();
    void outputReady(const QByteArray &data);
    void bell();
    // A program asked to put text on the clipboard with OSC 52
    void clipboardRequested(const QString &text);

private:
    static void onOutput(const char *bytes, size_t length, void *user);
    static int onDamage(VTermRect rect, void *user);
    static int onMoveCursor(VTermPos pos, VTermPos oldPos, int visible, void *user);
    static int onSetTermProp(VTermProp prop, VTermValue *value, void *user);
    static int onBell(void *user);
    static int onPushLine(int columns, const VTermScreenCell *cells, void *user);
    static int onPopLine(int columns, VTermScreenCell *cells, void *user);
    static int onClearScrollback(void *user);
    static int onSelectionSet(VTermSelectionMask mask, VTermStringFragment fragment, void *user);

    static const VTermScreenCallbacks s_screenCallbacks;
    static const VTermSelectionCallbacks s_selectionCallbacks;

    VTerm *m_vterm;
    VTermScreen *m_screen;
    int m_rows;
    int m_columns;
    VTermPos m_cursor;
    bool m_cursorVisible;
    bool m_altScreen;
    int m_mouseMode;
    QString m_title;
    QByteArray m_pendingTitle;
    QByteArray m_pendingClipboard;
    // libvterm decodes OSC 52 into this, it would leak a buffer of its own
    char m_clipboardBuffer[4096];
    bool m_clipboardTooLarge;
    QList<QVector<VTermScreenCell> > m_scrollback;
    qint64 m_scrolledLines;
    qint64 m_droppedLines;
};

#endif // TERMINAL_H

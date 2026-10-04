#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QStringList>
#include <cstdio>

#include "terminal.h"
#include "terminalview.h"

class Random
{
public:
    explicit Random(quint32 seed) : m_state(seed ? seed : 1) {}
    int bounded(int high) { return int(next() % quint32(high)); }
    int bounded(int low, int high) { return low + bounded(high - low); }

private:
    quint32 next()
    {
        m_state ^= m_state << 13;
        m_state ^= m_state >> 17;
        m_state ^= m_state << 5;
        return m_state;
    }
    quint32 m_state;
};

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static QString rowText(const Terminal &t, int row)
{
    QString s;
    for (int c = 0; c < t.columns(); ++c) {
        VTermScreenCell cell = t.cell(row, c);
        if (cell.chars[0] == uint32_t(-1))
            continue;
        if (cell.chars[0] == 0)
            s += QLatin1Char(' ');
        else
            s += QString::fromUcs4(&cell.chars[0], 1);
    }
    while (s.endsWith(QLatin1Char(' ')))
        s.chop(1);
    return s;
}

static QStringList allLines(const Terminal &t)
{
    QStringList l;
    for (int r = -t.scrollbackLines(); r < t.rows(); ++r)
        l << rowText(t, r);
    return l;
}

static QString lineName(int i) { return QString::asprintf("line %05d", i); }

static bool contiguous(const QStringList &lines, int last, QString *why)
{
    int expected = -1;
    for (const QString &s : lines) {
        if (!s.startsWith(QLatin1String("line ")))
            continue;
        const int n = s.mid(5).toInt();
        if (expected >= 0 && n != expected) {
            *why = QString::asprintf("got %d expected %d", n, expected);
            return false;
        }
        expected = n + 1;
    }
    if (expected != last + 1) {
        *why = QString::asprintf("ends at %d expected %d", expected - 1, last);
        return false;
    }
    return true;
}

static void writeLines(Terminal &t, int from, int to)
{
    QByteArray b = "cmd\r\n";
    for (int i = from; i <= to; ++i)
        b += lineName(i).toLatin1() + "\r\n";
    t.write(b);
}

static void testResizeContiguity()
{
    const QList<QPair<int, int>> sizes = { {40, 80}, {24, 80}, {12, 80}, {40, 80}, {5, 80}, {24, 60}, {24, 80}, {60, 100}, {24, 80} };
    for (int scenario = 0; scenario < 3; ++scenario) {
        Terminal t;
        t.resize(24, 80);
        int last = scenario == 0 ? 10 : 300;
        writeLines(t, 1, last);
        t.write("$ ");
        if (scenario == 2) {
            t.write("\x1b[H\x1b[2J$ ");
        }
        int step = 0;
        for (auto s : sizes) {
            t.resize(s.first, s.second);
            ++step;
            QString why;
            const QStringList lines = allLines(t);
            if (scenario == 2) {
                for (int r = 0; step <= 2 && r < t.rows(); ++r) {
                    const QString text = rowText(t, r);
                    CHECK(!text.startsWith(QLatin1String("line ")) || text.mid(5).toInt() > 300,
                          "scenario 2 resize %dx%d: cleared line back on screen row %d: '%s'", s.first, s.second, r, qPrintable(text));
                }
            } else {
                CHECK(contiguous(lines, last, &why), "scenario %d resize %dx%d: %s", scenario, s.first, s.second, qPrintable(why));
            }
            const VTermPos cur = t.cursorPosition();
            CHECK(rowText(t, cur.row) == QLatin1String("$"), "scenario %d resize %dx%d: cursor row %d is '%s'",
                  scenario, s.first, s.second, cur.row, qPrintable(rowText(t, cur.row)));
            writeLines(t, last + 1, last + 3);
            last += 3;
            t.write("$ ");
        }
    }
}

static void testAltScreen()
{
    Terminal t;
    t.resize(24, 80);
    writeLines(t, 1, 100);
    t.write("$ ");
    t.write("\x1b[?1049h\x1b[H\x1b[2J");
    for (int i = 0; i < 24; ++i)
        t.write(QByteArray("ALT ") + QByteArray::number(i) + (i < 23 ? "\r\n" : ""));
    const QList<QPair<int, int>> sizes = { {40, 80}, {12, 80}, {30, 70}, {24, 80} };
    for (auto s : sizes) {
        t.resize(s.first, s.second);
        for (int r = -t.scrollbackLines(); r < 0; ++r)
            CHECK(!rowText(t, r).startsWith(QLatin1String("ALT")), "alt screen line in scrollback after resize %dx%d", s.first, s.second);
    }
    t.write("\x1b[?1049l");
    QString why;
    CHECK(contiguous(allLines(t), 100, &why), "after alt screen: %s", qPrintable(why));
    CHECK(rowText(t, t.cursorPosition().row) == QLatin1String("$"), "after alt screen cursor row '%s'",
          qPrintable(rowText(t, t.cursorPosition().row)));
}

static void testScrollbackOverflow()
{
    Terminal t;
    t.resize(24, 80);
    int last = 6000;
    writeLines(t, 1, last);
    t.write("$ ");
    CHECK(t.scrollbackLines() == 5000, "scrollback %d", t.scrollbackLines());
    for (int i = 0; i < 20; ++i) {
        t.resize(i % 2 ? 40 : 20, 80);
        writeLines(t, last + 1, last + 50);
        last += 50;
        t.write("$ ");
        QString why;
        CHECK(contiguous(allLines(t), last, &why), "overflow step %d: %s", i, qPrintable(why));
    }
}

static QString topVisible(TerminalView &v, Terminal &t)
{
    return rowText(t, -v.scrollOffset());
}

static void testViewScroll()
{
    Terminal t;
    TerminalView v;
    v.setFontPixelSize(10);
    v.setTerminal(&t);
    v.setSize(QSizeF(v.cellHeight() * 10, v.cellHeight() * 24));
    writeLines(t, 1, 500);
    t.write("$ ");
    v.setScrollOffset(100);
    const QString top = topVisible(v, t);
    writeLines(t, 501, 520);
    t.write("$ ");
    CHECK(topVisible(v, t) == top, "output moved scrolled view: '%s' -> '%s'", qPrintable(top), qPrintable(topVisible(v, t)));

    for (int i = 0; i < 6; ++i) {
        const QString before = topVisible(v, t);
        t.resize(i % 2 ? 24 : 40, t.columns());
        CHECK(topVisible(v, t) == before, "resize %d moved scrolled view: '%s' -> '%s'", i, qPrintable(before), qPrintable(topVisible(v, t)));
    }

    QImage image(int(v.width()), int(v.height()), QImage::Format_ARGB32);
    for (int offset = 0; offset <= t.scrollbackLines(); offset += 37) {
        v.setScrollOffset(offset);
        QPainter p(&image);
        v.paint(&p);
    }
}

static void testFuzz(int iterations)
{
    Random rng(1234);
    const QByteArray pieces[] = {
        "\x1b[?1049h", "\x1b[?1049l", "\x1b[2J", "\x1b[3J", "\x1b[H", "\x1b[10;5H", "\x1b[r", "\x1b[5;10r",
        "\x1b[L", "\x1b[M", "\x1b[S", "\x1b[T", "\x1b[4h", "\x1b[4l", "\x1b[@", "\x1b[P", "\x1b[K", "\x1b[J",
        "\x1b[?7l", "\x1b[?7h", "\x1b[999C", "\x1b[3b", "\r\n", "\n", "\r", "\t", "\b",
        "\xe4\xb8\xad\xe6\x96\x87", "\xf0\x9f\x98\x80", "e\xcc\x81", "\x1b]0;title\x07", "\x1b]52;c;aGVsbG8=\x07",
        "\x1b[38;5;200m", "\x1b[0m", "\x1b#8", "\x1b[?6h", "\x1b[?6l", "\x1b[1;1H\x1b[2K", "\x1bD", "\x1bM", "\x1bc",
    };
    const int count = sizeof(pieces) / sizeof(pieces[0]);
    for (int i = 0; i < iterations; ++i) {
        Terminal t;
        TerminalView v;
        v.setFontPixelSize(10);
        v.setTerminal(&t);
        QImage image(200, 200, QImage::Format_ARGB32);
        for (int step = 0; step < 400; ++step) {
            const int r = rng.bounded(100);
            if (r < 60) {
                t.write(pieces[rng.bounded(count)]);
            } else if (r < 80) {
                QByteArray text;
                for (int k = rng.bounded(1, 120); k > 0; --k)
                    text += char(rng.bounded(0x20, 0x7f));
                t.write(text);
            } else if (r < 85) {
                QByteArray junk;
                for (int k = rng.bounded(1, 30); k > 0; --k)
                    junk += char(rng.bounded(256));
                t.write(junk);
            } else if (r < 92) {
                t.resize(rng.bounded(1, 60), rng.bounded(1, 120));
            } else if (r < 96) {
                v.setScrollOffset(rng.bounded(0, t.scrollbackLines() + 2));
            } else {
                QPainter p(&image);
                v.paint(&p);
            }
            if (step % 50 == 0)
                allLines(t);
        }
    }
}


static void testDragWithOutput()
{
    Terminal t;
    TerminalView v;
    v.setFontPixelSize(10);
    v.setTerminal(&t);
    v.setSize(QSizeF(v.cellHeight() * 10, v.cellHeight() * 24));
    int last = 500;
    writeLines(t, 1, last);
    t.write("$ ");
    v.scroll(50);
    for (int i = 0; i < 30; ++i) {
        const QString before = topVisible(v, t);
        const int beforeLine = before.mid(5).toInt();
        writeLines(t, last + 1, last + 7);
        last += 7;
        t.write("$ ");
        v.scroll(2);
        const int after = topVisible(v, t).mid(5).toInt();
        CHECK(after == beforeLine - 2, "drag with output step %d: top line %d -> %d", i, beforeLine, after);
    }
}

static void testAltScreenScroll()
{
    Terminal t;
    TerminalView v;
    v.setFontPixelSize(10);
    v.setTerminal(&t);
    v.setSize(QSizeF(v.cellHeight() * 10, v.cellHeight() * 24));
    writeLines(t, 1, 300);
    t.write("$ ");
    v.scroll(20);
    CHECK(v.scrollOffset() == 20, "offset %d", v.scrollOffset());
    QByteArray sent;
    QObject::connect(&t, &Terminal::outputReady, [&](const QByteArray &d) { sent += d; });
    t.write("\x1b[?1049h\x1b[H\x1b[2Jless");
    CHECK(v.scrollOffset() == 0, "alt screen keeps offset %d", v.scrollOffset());
    v.scroll(3);
    CHECK(v.scrollOffset() == 0, "alt screen scrolled into scrollback %d", v.scrollOffset());
    CHECK(sent == "\x1b[A\x1b[A\x1b[A", "alt screen scroll sent '%s'", sent.toHex().constData());
    sent.clear();
    v.scroll(-2);
    CHECK(sent == "\x1b[B\x1b[B", "alt screen scroll down sent '%s'", sent.toHex().constData());
    t.write("\x1b[?1000h\x1b[?1006h");
    const QByteArray at = QByteArray::number(t.columns() / 2 + 1) + ";" + QByteArray::number(t.rows() / 2 + 1) + "M";
    sent.clear();
    v.scroll(2);
    CHECK(sent == "\x1b[<64;" + at + "\x1b[<64;" + at, "mouse wheel up sent '%s'", sent.constData());
    sent.clear();
    v.scroll(-1);
    CHECK(sent == "\x1b[<65;" + at, "mouse wheel down sent '%s'", sent.constData());
    t.write("\x1b[?1000l");
    sent.clear();
    v.scroll(1);
    CHECK(sent == "\x1b[A", "after mouse off sent '%s'", sent.toHex().constData());
    t.write("\x1b[?1049l");
    v.scroll(5);
    CHECK(v.scrollOffset() == 5, "after alt screen offset %d", v.scrollOffset());
}

static void testRandomUse(int iterations)
{
    Random rng(99);
    for (int i = 0; i < iterations; ++i) {
        Terminal t;
        t.resize(24, 80);
        t.write("$ ");
        int last = 0;
        bool alt = false;
        for (int step = 0; step < 300; ++step) {
            const int r = rng.bounded(100);
            if (r < 30 && !alt) {
                const int n = rng.bounded(0, 60);
                writeLines(t, last + 1, last + n);
                last += n;
                t.write("$ ");
            } else if (r < 60) {
                const int rows = rng.bounded(3) == 0 ? rng.bounded(3, 60) : (rng.bounded(2) ? 24 : 40);
                t.resize(rows, rng.bounded(4) == 0 ? rng.bounded(80, 120) : 80);
            } else if (r < 70 && !alt) {
                t.write("\x1b[?1049h\x1b[H\x1b[2J");
                alt = true;
            } else if (r < 85 && alt) {
                QByteArray screen = "\x1b[H";
                for (int k = 0; k < t.rows(); ++k)
                    screen += "ALT " + QByteArray::number(k) + "\x1b[K" + (k + 1 < t.rows() ? "\r\n" : "");
                t.write(screen);
            } else if (r < 92 && alt) {
                t.write("\x1b[?1049l");
                alt = false;
            } else if (r < 94 && !alt) {
                t.write("\x1b[H\x1b[2J\x1b[3J$ ");
                last = 0;
                writeLines(t, 1, 0);
                t.write("$ ");
            }
            if (alt)
                continue;
            QString why;
            const QStringList lines = allLines(t);
            for (const QString &l : lines)
                CHECK(!l.startsWith(QLatin1String("ALT")), "iteration %d step %d: altscreen line in primary", i, step);
            if (last > 0)
                CHECK(contiguous(lines, last, &why), "iteration %d step %d: %s", i, step, qPrintable(why));
            const QString cursorRow = rowText(t, t.cursorPosition().row);
            CHECK(cursorRow == QLatin1String("$"), "iteration %d step %d: cursor row '%s'",
                  i, step, qPrintable(cursorRow));
            if (failures > 20)
                return;
        }
    }
}

static void testNotifications()
{
    Terminal t;
    QStringList seen;
    QObject::connect(&t, &Terminal::notificationRequested, [&](const QString &title, const QString &body) {
        seen.append(title + QLatin1Char('|') + body);
    });

    t.write("\x1b]777;notify;Claude;Done; all good\x07");
    t.write("\x1b]9;Waiting for input\x1b\\");
    // ConEmu progress and other numbered commands are no notifications
    t.write("\x1b]9;4;1;50\x07\x1b]9;4\x07");
    t.write("\x1b]777;preexec\x07");
    // Split across writes, as it arrives from the network
    t.write("\x1b]777;notify;Cla");
    t.write("ude;Needs approval\x07");
    const QByteArray huge(100000, 'x');
    t.write("\x1b]9;" + huge + "\x07");
    t.write("after");

    CHECK(seen.size() == 4, "%d notifications", seen.size());
    if (seen.size() == 4) {
        CHECK(seen.at(0) == QLatin1String("Claude|Done; all good"), "'%s'", qPrintable(seen.at(0)));
        CHECK(seen.at(1) == QLatin1String("|Waiting for input"), "'%s'", qPrintable(seen.at(1)));
        CHECK(seen.at(2) == QLatin1String("Claude|Needs approval"), "'%s'", qPrintable(seen.at(2)));
        CHECK(seen.at(3).size() == 4096 + 1, "huge notification %d long", seen.at(3).size());
    }
    CHECK(rowText(t, 0) == QLatin1String("after"), "screen '%s'", qPrintable(rowText(t, 0)));

    t.write("\x1b]777;longterm-status;working\x07");
    CHECK(t.activity() == QLatin1String("working"), "activity '%s'", qPrintable(t.activity()));
    t.write("\x1b]777;longterm-status;waiting\x07");
    CHECK(t.activity() == QLatin1String("waiting"), "activity '%s'", qPrintable(t.activity()));
    t.write("\x1b]777;longterm-status;done\x07");
    CHECK(t.activity() == QLatin1String("done"), "activity '%s'", qPrintable(t.activity()));
    CHECK(t.activityDetail().isEmpty(), "detail '%s' without one", qPrintable(t.activityDetail()));
    t.write("\x1b]777;longterm-status;working;Running a; b\x07");
    CHECK(t.activity() == QLatin1String("working") && t.activityDetail() == QLatin1String("Running a; b"),
          "activity '%s' detail '%s'", qPrintable(t.activity()), qPrintable(t.activityDetail()));
    t.write("\x1b]777;longterm-status;working;" + QByteArray(1000, 'y') + "\x07");
    CHECK(t.activityDetail().size() == 200, "long detail %d characters", t.activityDetail().size());
    t.write("\x1b]777;longterm-status;working\x07");
    CHECK(t.activityDetail().isEmpty(), "detail '%s' kept", qPrintable(t.activityDetail()));
    t.write("\x1b]777;longterm-status;bogus\x07");
    CHECK(t.activity().isEmpty(), "activity '%s' after unknown status", qPrintable(t.activity()));
    t.write("\x1b]777;longterm-status;done\x07");
    t.clearActivity();
    CHECK(t.activity().isEmpty(), "activity '%s' after clear", qPrintable(t.activity()));
    CHECK(seen.size() == 4, "status changes made notifications");
}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    testResizeContiguity();
    testAltScreen();
    testScrollbackOverflow();
    testViewScroll();
    testDragWithOutput();
    testAltScreenScroll();
    testNotifications();
    testRandomUse(argc > 2 ? atoi(argv[2]) : 200);
    testFuzz(argc > 1 ? atoi(argv[1]) : 200);
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}

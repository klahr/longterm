#include "colorschemes.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegExp>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>
#include <QUuid>
#include <QVariantList>
#include <QXmlStreamReader>

#include <algorithm>

namespace {

struct BuiltinScheme {
    const char *id;
    const char *name;
    QRgb foreground;
    QRgb background;
    QRgb cursor;
    QRgb selection;
    QRgb palette[16];
};

static const BuiltinScheme builtinSchemes[] = {
    {
        // libvterm's own palette
        "default", "Default",
        0xe6e6e6, 0x000000, 0xffffff, 0xffffff,
        {
            0x000000, 0xe00000, 0x00e000, 0xe0e000, 0x0000e0, 0xe000e0, 0x00e0e0, 0xe0e0e0,
            0x808080, 0xff4040, 0x40ff40, 0xffff40, 0x4040ff, 0xff40ff, 0x40ffff, 0xffffff,
        }
    },
    {
        // https://github.com/catppuccin/catppuccin
        "catppuccin-mocha", "Catppuccin Mocha",
        0xcdd6f4, 0x1e1e2e, 0xf5e0dc, 0x585b70,
        {
            0x45475a, 0xf38ba8, 0xa6e3a1, 0xf9e2af, 0x89b4fa, 0xf5c2e7, 0x94e2d5, 0xbac2de,
            0x585b70, 0xf38ba8, 0xa6e3a1, 0xf9e2af, 0x89b4fa, 0xf5c2e7, 0x94e2d5, 0xa6adc8,
        }
    },
    {
        // https://draculatheme.com/contribute
        "dracula", "Dracula",
        0xf8f8f2, 0x282a36, 0xf8f8f2, 0x44475a,
        {
            0x21222c, 0xff5555, 0x50fa7b, 0xf1fa8c, 0xbd93f9, 0xff79c6, 0x8be9fd, 0xf8f8f2,
            0x6272a4, 0xff6e6e, 0x69ff94, 0xffffa5, 0xd6acff, 0xff92df, 0xa4ffff, 0xffffff,
        }
    },
    {
        // https://github.com/morhetz/gruvbox
        "gruvbox-dark", "Gruvbox Dark",
        0xebdbb2, 0x282828, 0xebdbb2, 0x504945,
        {
            0x282828, 0xcc241d, 0x98971a, 0xd79921, 0x458588, 0xb16286, 0x689d6a, 0xa89984,
            0x928374, 0xfb4934, 0xb8bb26, 0xfabd2f, 0x83a598, 0xd3869b, 0x8ec07c, 0xebdbb2,
        }
    },
    {
        // https://www.nordtheme.com/docs/ports/
        "nord", "Nord",
        0xd8dee9, 0x2e3440, 0xd8dee9, 0x434c5e,
        {
            0x3b4252, 0xbf616a, 0xa3be8c, 0xebcb8b, 0x81a1c1, 0xb48ead, 0x88c0d0, 0xe5e9f0,
            0x4c566a, 0xbf616a, 0xa3be8c, 0xebcb8b, 0x81a1c1, 0xb48ead, 0x8fbcbb, 0xeceff4,
        }
    },
    {
        // https://ethanschoonover.com/solarized/
        "solarized-dark", "Solarized Dark",
        0x839496, 0x002b36, 0x93a1a1, 0x073642,
        {
            0x073642, 0xdc322f, 0x859900, 0xb58900, 0x268bd2, 0xd33682, 0x2aa198, 0xeee8d5,
            0x002b36, 0xcb4b16, 0x586e75, 0x657b83, 0x839496, 0x6c71c4, 0x93a1a1, 0xfdf6e3,
        }
    },
    {
        // Same palette as the dark variant, with the base tones swapped
        "solarized-light", "Solarized Light",
        0x657b83, 0xfdf6e3, 0x586e75, 0xeee8d5,
        {
            0x073642, 0xdc322f, 0x859900, 0xb58900, 0x268bd2, 0xd33682, 0x2aa198, 0xeee8d5,
            0x002b36, 0xcb4b16, 0x586e75, 0x657b83, 0x839496, 0x6c71c4, 0x93a1a1, 0xfdf6e3,
        }
    },
};

// Built in first, then the imported ones in the order they came
QList<ColorScheme> &schemes()
{
    static QList<ColorScheme> list;
    if (list.isEmpty()) {
        for (const BuiltinScheme &builtin : builtinSchemes) {
            ColorScheme scheme;
            scheme.id = QString::fromLatin1(builtin.id);
            scheme.name = QString::fromLatin1(builtin.name);
            scheme.foreground = builtin.foreground;
            scheme.background = builtin.background;
            scheme.cursor = builtin.cursor;
            scheme.selection = builtin.selection;
            std::copy(builtin.palette, builtin.palette + 16, scheme.palette);
            scheme.custom = false;
            list.append(scheme);
        }
    }
    return list;
}

QString settingsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/colorschemes.conf");
}

// #rrggbb, #rgb, 0xrrggbb or rrggbb
bool parseColor(QString text, QRgb *color)
{
    text = text.trimmed();
    text.remove(QLatin1Char('"'));
    text.remove(QLatin1Char('\''));
    if (text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        text = text.mid(2);
    else if (text.startsWith(QLatin1Char('#')))
        text = text.mid(1);
    if (text.size() == 3)
        text = QString(text.at(0)) + text.at(0) + text.at(1) + text.at(1) + text.at(2) + text.at(2);
    if (!QRegExp(QStringLiteral("[0-9A-Fa-f]{6}")).exactMatch(text))
        return false;
    *color = 0xff000000 | text.toUInt(nullptr, 16);
    return true;
}

QRgb mix(QRgb a, QRgb b)
{
    return qRgb((qRed(a) + qRed(b)) / 2, (qGreen(a) + qGreen(b)) / 2, (qBlue(a) + qBlue(b)) / 2);
}

// Colors by name, filled in by each format's reader
struct Found {
    QRgb palette[16];
    bool havePalette[16] = { false };
    QRgb foreground = 0;
    bool haveForeground = false;
    QRgb background = 0;
    bool haveBackground = false;
    QRgb cursor = 0;
    bool haveCursor = false;
    QRgb selection = 0;
    bool haveSelection = false;

    void set(int index, QRgb color)
    {
        if (index >= 0 && index < 16) {
            palette[index] = color;
            havePalette[index] = true;
        }
    }
};

// <key>Ansi 0 Color</key><dict><key>Red Component</key><real>0.1</real>...</dict>
bool readITerm(const QString &text, Found *found)
{
    QXmlStreamReader xml(text);
    QString key;
    int depth = 0;
    QString colorKey;
    double components[3] = { 0, 0, 0 };
    QString componentKey;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("dict")) {
            ++depth;
            if (depth == 2) {
                colorKey = key;
                components[0] = components[1] = components[2] = 0;
            }
        } else if (xml.isEndElement() && xml.name() == QLatin1String("dict")) {
            if (depth == 2) {
                const QRgb color = qRgb(qBound(0, int(components[0] * 255 + 0.5), 255),
                                        qBound(0, int(components[1] * 255 + 0.5), 255),
                                        qBound(0, int(components[2] * 255 + 0.5), 255));
                QRegExp ansi(QStringLiteral("Ansi (\\d+) Color"));
                if (ansi.exactMatch(colorKey)) {
                    found->set(ansi.cap(1).toInt(), color);
                } else if (colorKey == QLatin1String("Foreground Color")) {
                    found->foreground = color;
                    found->haveForeground = true;
                } else if (colorKey == QLatin1String("Background Color")) {
                    found->background = color;
                    found->haveBackground = true;
                } else if (colorKey == QLatin1String("Cursor Color")) {
                    found->cursor = color;
                    found->haveCursor = true;
                } else if (colorKey == QLatin1String("Selection Color")) {
                    found->selection = color;
                    found->haveSelection = true;
                }
            }
            --depth;
        } else if (xml.isStartElement() && xml.name() == QLatin1String("key")) {
            const QString name = xml.readElementText();
            if (depth == 1)
                key = name;
            else
                componentKey = name;
        } else if (xml.isStartElement() && (xml.name() == QLatin1String("real") || xml.name() == QLatin1String("integer"))) {
            const double value = xml.readElementText().toDouble();
            if (depth == 2) {
                if (componentKey == QLatin1String("Red Component"))
                    components[0] = value;
                else if (componentKey == QLatin1String("Green Component"))
                    components[1] = value;
                else if (componentKey == QLatin1String("Blue Component"))
                    components[2] = value;
            }
        }
    }
    return !xml.hasError();
}

// A Windows Terminal scheme, as in its settings.json
bool readWindowsTerminal(const QString &text, Found *found)
{
    const QJsonObject object = QJsonDocument::fromJson(text.toUtf8()).object();
    if (object.isEmpty())
        return false;
    static const char *const names[16] = { "black", "red", "green", "yellow", "blue", "purple", "cyan", "white",
                                           "brightBlack", "brightRed", "brightGreen", "brightYellow",
                                           "brightBlue", "brightPurple", "brightCyan", "brightWhite" };
    QRgb color;
    for (int i = 0; i < 16; ++i) {
        if (parseColor(object.value(QLatin1String(names[i])).toString(), &color))
            found->set(i, color);
    }
    found->haveForeground = parseColor(object.value(QStringLiteral("foreground")).toString(), &found->foreground);
    found->haveBackground = parseColor(object.value(QStringLiteral("background")).toString(), &found->background);
    found->haveCursor = parseColor(object.value(QStringLiteral("cursorColor")).toString(), &found->cursor);
    found->haveSelection = parseColor(object.value(QStringLiteral("selectionBackground")).toString(), &found->selection);
    return true;
}

// base00 to base0F, mapped the way base16-shell does
void readBase16(const QString &text, Found *found)
{
    QRgb base[16];
    bool have[16] = { false };
    QRegExp line(QStringLiteral("\\s*base0([0-9A-Fa-f])\\s*:\\s*[\"']?#?([0-9A-Fa-f]{6})[\"']?.*"));
    for (const QString &row : text.split(QLatin1Char('\n'))) {
        if (!line.exactMatch(row))
            continue;
        const int index = line.cap(1).toInt(nullptr, 16);
        have[index] = parseColor(line.cap(2), &base[index]);
    }
    static const int mapping[16] = { 0x0, 0x8, 0xb, 0xa, 0xd, 0xe, 0xc, 0x5, 0x3, 0x8, 0xb, 0xa, 0xd, 0xe, 0xc, 0x7 };
    for (int i = 0; i < 16; ++i) {
        if (have[mapping[i]])
            found->set(i, base[mapping[i]]);
    }
    if ((found->haveBackground = have[0x0]))
        found->background = base[0x0];
    if ((found->haveForeground = have[0x5]))
        found->foreground = base[0x5];
    if ((found->haveCursor = have[0x5]))
        found->cursor = base[0x5];
    if ((found->haveSelection = have[0x2]))
        found->selection = base[0x2];
}

// "*.color0: #000000", "*foreground: #ffffff" and the like
void readXresources(const QString &text, Found *found)
{
    QRegExp line(QStringLiteral("\\s*[^:!]*[.*]?(color(\\d+)|foreground|background|cursorColor)\\s*:\\s*(\\S+).*"));
    for (const QString &row : text.split(QLatin1Char('\n'))) {
        if (!line.exactMatch(row))
            continue;
        QRgb color;
        if (!parseColor(line.cap(3), &color))
            continue;
        if (!line.cap(2).isEmpty()) {
            found->set(line.cap(2).toInt(), color);
        } else if (line.cap(1) == QLatin1String("foreground")) {
            found->foreground = color;
            found->haveForeground = true;
        } else if (line.cap(1) == QLatin1String("background")) {
            found->background = color;
            found->haveBackground = true;
        } else {
            found->cursor = color;
            found->haveCursor = true;
        }
    }
}

// Alacritty's TOML ([colors.normal] then black = '#000000') or its older YAML
// (normal: then black: '0x000000'), told apart only by the separators
void readAlacritty(const QString &text, Found *found)
{
    static const char *const names[8] = { "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white" };
    QString section;
    int sectionIndent = -1;
    QRegExp header(QStringLiteral("\\s*\\[\\s*colors\\.([a-z_]+)\\s*\\]\\s*"));
    QRegExp yamlSection(QStringLiteral("(\\s*)([a-z_]+)\\s*:\\s*(#.*)?"));
    QRegExp entry(QStringLiteral("(\\s*)([a-z_]+)\\s*[=:]\\s*['\"]?(#|0x)([0-9A-Fa-f]{3,6})['\"]?.*"));
    for (const QString &row : text.split(QLatin1Char('\n'))) {
        if (header.exactMatch(row)) {
            section = header.cap(1);
            continue;
        }
        if (row.trimmed().startsWith(QLatin1Char('['))) {
            section.clear();
            continue;
        }
        if (yamlSection.exactMatch(row)) {
            const int indent = yamlSection.cap(1).size();
            if (yamlSection.cap(2) != QLatin1String("colors")) {
                section = yamlSection.cap(2);
                sectionIndent = indent;
            }
            continue;
        }
        if (!entry.exactMatch(row))
            continue;
        if (sectionIndent >= 0 && entry.cap(1).size() <= sectionIndent)
            section.clear();
        QRgb color;
        if (!parseColor(entry.cap(3) + entry.cap(4), &color))
            continue;
        const QString key = entry.cap(2);
        for (int i = 0; i < 8; ++i) {
            if (key != QLatin1String(names[i]))
                continue;
            if (section == QLatin1String("normal"))
                found->set(i, color);
            else if (section == QLatin1String("bright"))
                found->set(i + 8, color);
        }
        if (section == QLatin1String("primary") && key == QLatin1String("foreground")) {
            found->foreground = color;
            found->haveForeground = true;
        } else if (section == QLatin1String("primary") && key == QLatin1String("background")) {
            found->background = color;
            found->haveBackground = true;
        } else if (section == QLatin1String("cursor") && key == QLatin1String("cursor")) {
            found->cursor = color;
            found->haveCursor = true;
        } else if (section == QLatin1String("selection") && key == QLatin1String("background")) {
            found->selection = color;
            found->haveSelection = true;
        }
    }
}

}

ColorSchemes::ColorSchemes(QObject *parent)
    : QAbstractListModel(parent)
{
    QSettings settings(settingsPath(), QSettings::IniFormat);
    const int size = qMin(settings.beginReadArray(QStringLiteral("schemes")), 200);
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);
        ColorScheme scheme;
        scheme.id = settings.value(QStringLiteral("id")).toString();
        scheme.name = settings.value(QStringLiteral("name")).toString();
        scheme.custom = true;
        const QStringList palette = settings.value(QStringLiteral("palette")).toStringList();
        bool ok = !scheme.id.isEmpty() && palette.size() == 16
                && parseColor(settings.value(QStringLiteral("foreground")).toString(), &scheme.foreground)
                && parseColor(settings.value(QStringLiteral("background")).toString(), &scheme.background)
                && parseColor(settings.value(QStringLiteral("cursor")).toString(), &scheme.cursor)
                && parseColor(settings.value(QStringLiteral("selection")).toString(), &scheme.selection);
        for (int c = 0; ok && c < 16; ++c)
            ok = parseColor(palette.at(c), &scheme.palette[c]);
        if (ok && find(scheme.id).id != scheme.id)
            schemes().append(scheme);
    }
    settings.endArray();
}

int ColorSchemes::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : schemes().size();
}

QVariant ColorSchemes::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= schemes().size())
        return QVariant();
    const ColorScheme &scheme = schemes().at(index.row());
    switch (role) {
    case SchemeIdRole: return scheme.id;
    case NameRole: return scheme.name;
    case ForegroundRole: return QColor(scheme.foreground);
    case BackgroundRole: return QColor(scheme.background);
    case PaletteRole: {
        QVariantList palette;
        for (QRgb color : scheme.palette)
            palette.append(QColor(color));
        return palette;
    }
    case CustomRole: return scheme.custom;
    default: return QVariant();
    }
}

QHash<int, QByteArray> ColorSchemes::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[SchemeIdRole] = "schemeId";
    roles[NameRole] = "name";
    roles[ForegroundRole] = "foreground";
    roles[BackgroundRole] = "background";
    roles[PaletteRole] = "palette";
    roles[CustomRole] = "custom";
    return roles;
}

QString ColorSchemes::name(const QString &id) const
{
    return find(id).name;
}

QColor ColorSchemes::foreground(const QString &id) const
{
    return QColor(find(id).foreground);
}

QColor ColorSchemes::background(const QString &id) const
{
    return QColor(find(id).background);
}

ColorScheme ColorSchemes::find(const QString &id)
{
    for (const ColorScheme &scheme : schemes()) {
        if (scheme.id == id)
            return scheme;
    }
    return schemes().first();
}

bool ColorSchemes::parse(const QString &text, ColorScheme *scheme, QString *error)
{
    Found found;
    if (text.contains(QLatin1String("<plist")) || text.contains(QLatin1String("Ansi 0 Color"))) {
        if (!readITerm(text, &found)) {
            *error = tr("The iTerm2 scheme is damaged");
            return false;
        }
    } else if (text.trimmed().startsWith(QLatin1Char('{'))) {
        if (!readWindowsTerminal(text, &found)) {
            *error = tr("The Windows Terminal scheme is damaged");
            return false;
        }
    } else if (text.contains(QRegExp(QStringLiteral("base0[0-9A-Fa-f]\\s*:")))) {
        readBase16(text, &found);
    } else if (text.contains(QRegExp(QStringLiteral("color\\d+\\s*:"))) || text.contains(QLatin1String("*foreground"))) {
        readXresources(text, &found);
    } else {
        readAlacritty(text, &found);
    }

    QStringList missing;
    for (int i = 0; i < 16; ++i) {
        if (!found.havePalette[i])
            missing.append(QString::number(i));
    }
    if (!found.haveForeground)
        missing.append(tr("foreground"));
    if (!found.haveBackground)
        missing.append(tr("background"));
    if (missing.size() > 8) {
        *error = tr("No color scheme found, use an iTerm2, Alacritty, Windows Terminal, Xresources or base16 one");
        return false;
    }
    if (!missing.isEmpty()) {
        *error = tr("The scheme lacks colors: %1").arg(missing.join(QStringLiteral(", ")));
        return false;
    }
    error->clear();
    if (!scheme)
        return true;
    std::copy(found.palette, found.palette + 16, scheme->palette);
    scheme->foreground = found.foreground;
    scheme->background = found.background;
    scheme->cursor = found.haveCursor ? found.cursor : found.foreground;
    scheme->selection = found.haveSelection ? found.selection : mix(found.foreground, found.background);
    return true;
}

QString ColorSchemes::check(const QString &text) const
{
    ColorScheme scheme;
    QString error;
    parse(text, &scheme, &error);
    return error;
}

QString ColorSchemes::checkFile(const QString &path) const
{
    QString text;
    QString error;
    if (readFile(path, &text, &error))
        parse(text, nullptr, &error);
    return error;
}

bool ColorSchemes::readFile(const QString &path, QString *text, QString *error)
{
    const QString local = path.startsWith(QLatin1String("file://")) ? QUrl(path).toLocalFile() : path;
    QFile file(local);
    // Scheme files are small, a large one is something else
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
        *error = tr("Could not read %1").arg(QFileInfo(local).fileName());
        return false;
    }
    *text = QString::fromUtf8(file.readAll());
    return true;
}

QString ColorSchemes::importScheme(const QString &name, const QString &text)
{
    ColorScheme scheme;
    QString error;
    if (!parse(text, &scheme, &error))
        return QString();
    scheme.id = QStringLiteral("custom-") + QUuid::createUuid().toString().remove(QRegExp(QStringLiteral("[{}-]")));
    scheme.name = name.simplified().isEmpty() ? tr("Imported") : name.simplified().left(64);
    scheme.custom = true;
    beginInsertRows(QModelIndex(), schemes().size(), schemes().size());
    schemes().append(scheme);
    endInsertRows();
    save();
    emit schemesChanged();
    return scheme.id;
}

QString ColorSchemes::importSchemeFile(const QString &name, const QString &path)
{
    QString text;
    QString error;
    if (!readFile(path, &text, &error))
        return QString();
    const QString local = path.startsWith(QLatin1String("file://")) ? QUrl(path).toLocalFile() : path;
    return importScheme(name.trimmed().isEmpty() ? QFileInfo(local).completeBaseName() : name, text);
}

void ColorSchemes::removeScheme(const QString &id)
{
    for (int row = 0; row < schemes().size(); ++row) {
        if (schemes().at(row).id != id || !schemes().at(row).custom)
            continue;
        beginRemoveRows(QModelIndex(), row, row);
        schemes().removeAt(row);
        endRemoveRows();
        save();
        emit schemesChanged();
        return;
    }
}

QVariantList ColorSchemes::exportSchemes() const
{
    QVariantList list;
    for (const ColorScheme &scheme : schemes()) {
        if (!scheme.custom)
            continue;
        QVariantMap map;
        map.insert(QStringLiteral("id"), scheme.id);
        map.insert(QStringLiteral("name"), scheme.name);
        map.insert(QStringLiteral("foreground"), QColor(scheme.foreground).name());
        map.insert(QStringLiteral("background"), QColor(scheme.background).name());
        map.insert(QStringLiteral("cursor"), QColor(scheme.cursor).name());
        map.insert(QStringLiteral("selection"), QColor(scheme.selection).name());
        QStringList palette;
        for (QRgb color : scheme.palette)
            palette.append(QColor(color).name());
        map.insert(QStringLiteral("palette"), palette);
        list.append(map);
    }
    return list;
}

void ColorSchemes::restoreSchemes(const QVariantList &list)
{
    bool changed = false;
    for (const QVariant &item : list) {
        const QVariantMap map = item.toMap();
        ColorScheme scheme;
        scheme.id = map.value(QStringLiteral("id")).toString();
        scheme.name = map.value(QStringLiteral("name")).toString();
        scheme.custom = true;
        const QStringList palette = map.value(QStringLiteral("palette")).toStringList();
        bool ok = scheme.id.startsWith(QLatin1String("custom-")) && palette.size() == 16 && find(scheme.id).id != scheme.id
                && parseColor(map.value(QStringLiteral("foreground")).toString(), &scheme.foreground)
                && parseColor(map.value(QStringLiteral("background")).toString(), &scheme.background)
                && parseColor(map.value(QStringLiteral("cursor")).toString(), &scheme.cursor)
                && parseColor(map.value(QStringLiteral("selection")).toString(), &scheme.selection);
        for (int c = 0; ok && c < 16; ++c)
            ok = parseColor(palette.at(c), &scheme.palette[c]);
        if (!ok)
            continue;
        beginInsertRows(QModelIndex(), schemes().size(), schemes().size());
        schemes().append(scheme);
        endInsertRows();
        changed = true;
    }
    if (changed) {
        save();
        emit schemesChanged();
    }
}

void ColorSchemes::save()
{
    const auto hex = [](QRgb color) { return QColor(color).name(); };
    QSettings settings(settingsPath(), QSettings::IniFormat);
    settings.remove(QStringLiteral("schemes"));
    settings.beginWriteArray(QStringLiteral("schemes"));
    int index = 0;
    for (const ColorScheme &scheme : schemes()) {
        if (!scheme.custom)
            continue;
        settings.setArrayIndex(index++);
        settings.setValue(QStringLiteral("id"), scheme.id);
        settings.setValue(QStringLiteral("name"), scheme.name);
        settings.setValue(QStringLiteral("foreground"), hex(scheme.foreground));
        settings.setValue(QStringLiteral("background"), hex(scheme.background));
        settings.setValue(QStringLiteral("cursor"), hex(scheme.cursor));
        settings.setValue(QStringLiteral("selection"), hex(scheme.selection));
        QStringList palette;
        for (QRgb color : scheme.palette)
            palette.append(hex(color));
        settings.setValue(QStringLiteral("palette"), palette);
    }
    settings.endArray();
}

#include "colorschemes.h"

#include <QVariantList>

static const ColorScheme schemes[] = {
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

static const int schemeCount = sizeof(schemes) / sizeof(schemes[0]);

ColorSchemes::ColorSchemes(QObject *parent)
    : QAbstractListModel(parent)
{
}

int ColorSchemes::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : schemeCount;
}

QVariant ColorSchemes::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= schemeCount)
        return QVariant();
    const ColorScheme &scheme = schemes[index.row()];
    switch (role) {
    case SchemeIdRole: return QString::fromLatin1(scheme.id);
    case NameRole: return QString::fromLatin1(scheme.name);
    case ForegroundRole: return QColor(scheme.foreground);
    case BackgroundRole: return QColor(scheme.background);
    case PaletteRole: {
        QVariantList palette;
        for (QRgb color : scheme.palette)
            palette.append(QColor(color));
        return palette;
    }
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
    return roles;
}

QString ColorSchemes::name(const QString &id) const
{
    return QString::fromLatin1(find(id).name);
}

const ColorScheme &ColorSchemes::find(const QString &id)
{
    for (const ColorScheme &scheme : schemes) {
        if (id == QLatin1String(scheme.id))
            return scheme;
    }
    return schemes[0];
}

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

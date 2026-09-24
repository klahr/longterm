#ifndef COLORSCHEMES_H
#define COLORSCHEMES_H

#include <QAbstractListModel>
#include <QColor>
#include <QString>

struct ColorScheme {
    const char *id;
    const char *name;
    QRgb foreground;
    QRgb background;
    QRgb cursor;
    QRgb selection;
    QRgb palette[16];
};

class ColorSchemes : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        SchemeIdRole = Qt::UserRole + 1,
        NameRole,
        ForegroundRole,
        BackgroundRole,
        PaletteRole
    };

    explicit ColorSchemes(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE QString name(const QString &id) const;

    // Falls back to the default scheme for unknown ids
    static const ColorScheme &find(const QString &id);
};

#endif // COLORSCHEMES_H

#ifndef COLORSCHEMES_H
#define COLORSCHEMES_H

#include <QAbstractListModel>
#include <QColor>
#include <QList>
#include <QString>

struct ColorScheme {
    QString id;
    QString name;
    QRgb foreground;
    QRgb background;
    QRgb cursor;
    QRgb selection;
    QRgb palette[16];
    // Imported by the user, as opposed to built in
    bool custom;
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
        PaletteRole,
        CustomRole
    };

    // Loads the imported schemes, so find() knows them from then on
    explicit ColorSchemes(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE QString name(const QString &id) const;
    Q_INVOKABLE QColor foreground(const QString &id) const;
    Q_INVOKABLE QColor background(const QString &id) const;
    // Reads an iTerm2, Alacritty, Windows Terminal, Xresources or base16
    // scheme. Returns the new scheme's id, or an empty string with error set.
    Q_INVOKABLE QString importScheme(const QString &name, const QString &text);
    // The same from a file, named after it when name is empty
    Q_INVOKABLE QString importSchemeFile(const QString &name, const QString &path);
    // Why the text or file is not a scheme that can be imported, empty when it is
    Q_INVOKABLE QString check(const QString &text) const;
    Q_INVOKABLE QString checkFile(const QString &path) const;
    // Only imported schemes can go
    Q_INVOKABLE void removeScheme(const QString &id);

    // Falls back to the default scheme for unknown ids
    static ColorScheme find(const QString &id);
    // The imported schemes, for backups
    QVariantList exportSchemes() const;
    // Adds schemes from exportSchemes() that are not here yet
    void restoreSchemes(const QVariantList &list);

    // Fills in scheme from a scheme file's text, false with error set when it is not one
    static bool parse(const QString &text, ColorScheme *scheme, QString *error);

signals:
    void schemesChanged();

private:
    void save();
    static bool readFile(const QString &path, QString *text, QString *error);
};

#endif // COLORSCHEMES_H

#ifndef QRIMAGEPROVIDER_H
#define QRIMAGEPROVIDER_H

#include <QQuickImageProvider>

// "image://qr/<text>" draws the text as a QR code, for comparing a host key's
// fingerprint with another device
class QrImageProvider : public QQuickImageProvider
{
public:
    QrImageProvider();

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};

#endif // QRIMAGEPROVIDER_H

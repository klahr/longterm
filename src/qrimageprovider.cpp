#include "qrimageprovider.h"

#include <QPainter>
#include <QUrl>

#include "qrcodegen.hpp"

// Blank modules around the code, which readers need to find it
static const int QuietZone = 4;

QrImageProvider::QrImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage QrImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    const QByteArray text = QUrl::fromPercentEncoding(id.toUtf8()).toUtf8();
    const qrcodegen::QrCode code = qrcodegen::QrCode::encodeText(text.constData(), qrcodegen::QrCode::Ecc::MEDIUM);
    const int modules = code.getSize() + 2 * QuietZone;
    const int wanted = qMax(requestedSize.width(), requestedSize.height());
    const int scale = qMax(1, wanted > 0 ? wanted / modules : 8);
    QImage image(modules * scale, modules * scale, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    for (int y = 0; y < code.getSize(); ++y) {
        for (int x = 0; x < code.getSize(); ++x) {
            if (code.getModule(x, y))
                painter.fillRect((x + QuietZone) * scale, (y + QuietZone) * scale, scale, scale, Qt::black);
        }
    }
    if (size)
        *size = image.size();
    return image;
}

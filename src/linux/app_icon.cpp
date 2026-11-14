#include "app_icon.h"

#include <QFont>
#include <QPainter>
#include <QPixmap>

namespace vtt {

QIcon create_app_icon(const QColor& background) {
    QIcon icon;
    for (int size : {16, 22, 24, 32, 48, 64, 128}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(background);
        qreal radius = size * 0.18;
        painter.drawRoundedRect(QRectF(0, 0, size, size), radius, radius);

        QFont font = painter.font();
        font.setBold(true);
        font.setPixelSize(static_cast<int>(size * 0.72));
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(QRect(0, 0, size, size), Qt::AlignCenter,
                         QStringLiteral("V"));
        painter.end();

        icon.addPixmap(pixmap);
    }
    return icon;
}

} // namespace vtt

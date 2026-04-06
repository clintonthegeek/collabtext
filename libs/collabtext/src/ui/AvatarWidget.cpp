#include "ui/AvatarWidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QByteArray>

namespace CollabText::Ui {

static constexpr int kSize = 40;

AvatarWidget::AvatarWidget(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(kSize, kSize);
}

void AvatarWidget::setIdentity(const std::string &display_name, const std::string &color)
{
    m_displayName = QString::fromStdString(display_name);
    m_color = QColor(QString::fromStdString(color));
    if (!m_color.isValid())
        m_color = Qt::gray;
    update();
}

void AvatarWidget::setImage(const std::vector<uint8_t> &data)
{
    QByteArray bytes(reinterpret_cast<const char *>(data.data()),
                     static_cast<qsizetype>(data.size()));
    QPixmap pm;
    if (pm.loadFromData(bytes)) {
        m_image = pm.scaled(kSize, kSize, Qt::KeepAspectRatioByExpanding,
                            Qt::SmoothTransformation);
        m_hasImage = true;
    } else {
        m_hasImage = false;
    }
    update();
}

void AvatarWidget::clearImage()
{
    m_image = QPixmap();
    m_hasImage = false;
    update();
}

QSize AvatarWidget::sizeHint() const
{
    return QSize(kSize, kSize);
}

void AvatarWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Clip to circle
    QPainterPath clip;
    clip.addEllipse(rect());
    painter.setClipPath(clip);

    if (m_hasImage && !m_image.isNull()) {
        // Center the scaled image (may be larger than widget on one axis)
        int x = (width() - m_image.width()) / 2;
        int y = (height() - m_image.height()) / 2;
        painter.drawPixmap(x, y, m_image);
    } else {
        // Colored circle background
        painter.fillRect(rect(), m_color);

        // White initials
        QString initials = extractInitials(m_displayName);
        QFont font = painter.font();
        font.setPixelSize(kSize * 4 / 10);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(rect(), Qt::AlignCenter, initials);
    }
}

QString AvatarWidget::extractInitials(const QString &name) const
{
    if (name.trimmed().isEmpty())
        return QStringLiteral("?");

    QStringList parts = name.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return QStringLiteral("?");

    if (parts.size() == 1)
        return parts.first().at(0).toUpper();

    return QString(parts.first().at(0).toUpper()) +
           QString(parts.last().at(0).toUpper());
}

} // namespace CollabText::Ui

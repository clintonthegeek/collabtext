#include "ui/PresenceIndicator.h"

#include <QPainter>

namespace CollabText::Ui {

static constexpr int kSize = 12;

PresenceIndicator::PresenceIndicator(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(kSize, kSize);
}

void PresenceIndicator::setActivity(const std::string &activity)
{
    m_activity = activity;
    update();
}

void PresenceIndicator::setStale(bool stale)
{
    m_stale = stale;
    update();
}

QSize PresenceIndicator::sizeHint() const
{
    return QSize(kSize, kSize);
}

QColor PresenceIndicator::currentColor() const
{
    if (m_stale)
        return QColor(QStringLiteral("#9ca3af"));

    if (m_activity == "typing" || m_activity == "selecting")
        return QColor(QStringLiteral("#22c55e"));

    if (m_activity == "idle")
        return QColor(QStringLiteral("#eab308"));

    // away, or any unknown activity
    return QColor(QStringLiteral("#9ca3af"));
}

void PresenceIndicator::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(currentColor());
    painter.drawEllipse(rect());
}

} // namespace CollabText::Ui

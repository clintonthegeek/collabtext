#include "ui/CursorLabelWidget.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

namespace CollabText::Ui {

static constexpr int kPadH = 5;
static constexpr int kPadV = 2;
static constexpr int kRadius = 3;
static constexpr int kFontSize = 10;
static constexpr int kFadeDelayMs = 2000;
static constexpr int kFadeDurationMs = 500;

CursorLabelWidget::CursorLabelWidget(QWidget *viewport)
    : QWidget(viewport)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);

    m_fadeTimer.setSingleShot(true);
    m_fadeTimer.setInterval(kFadeDelayMs);
    connect(&m_fadeTimer, &QTimer::timeout,
            this, &CursorLabelWidget::onFadeTimerTimeout);
}

void CursorLabelWidget::setLabel(const QString &name, const QColor &color) {
    m_name = name;
    m_color = color;
    updateGeometry();
    update();
}

void CursorLabelWidget::showAtPosition(const QPoint &pos, quint64 cursorVersion,
                                        bool flipBelow) {
    bool cursorMoved = (cursorVersion != m_lastCursorVersion);
    m_lastCursorVersion = cursorVersion;

    QSize sz = sizeHint();
    resize(sz);

    int x = pos.x();
    int y = flipBelow ? pos.y() + 2 : pos.y() - sz.height() - 2;

    if (parentWidget()) {
        int maxX = parentWidget()->width() - sz.width();
        if (x > maxX) x = qMax(0, maxX);
    }

    move(x, y);

    if (cursorMoved) {
        // Remote user actually moved their cursor — show and restart fade
        cancelFade();
        show();
        raise();
        scheduleFade();
    }
    // Position shifted due to local edits — reposition but don't reset fade
}

void CursorLabelWidget::scheduleFade() {
    m_fadeTimer.start();
}

void CursorLabelWidget::cancelFade() {
    m_fadeTimer.stop();
    if (m_fadeAnim) {
        m_fadeAnim->stop();
        delete m_fadeAnim;
        m_fadeAnim = nullptr;
    }
    setOpacity(1.0);
}

QSize CursorLabelWidget::sizeHint() const {
    QFont font;
    font.setPixelSize(kFontSize);
    font.setBold(true);
    QFontMetrics fm(font);
    int w = fm.horizontalAdvance(m_name) + kPadH * 2;
    int h = fm.height() + kPadV * 2;
    return {w, h};
}

void CursorLabelWidget::setOpacity(qreal opacity) {
    m_opacity = opacity;
    update();
}

void CursorLabelWidget::paintEvent(QPaintEvent *) {
    if (m_name.isEmpty()) return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(m_opacity);

    QColor bg = m_color;
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()), kRadius, kRadius);
    p.fillPath(path, bg);

    p.setPen(Qt::white);
    QFont font = p.font();
    font.setPixelSize(kFontSize);
    font.setBold(true);
    p.setFont(font);
    p.drawText(rect(), Qt::AlignCenter, m_name);
}

void CursorLabelWidget::onFadeTimerTimeout() {
    m_fadeAnim = new QPropertyAnimation(this, "opacity", this);
    m_fadeAnim->setDuration(kFadeDurationMs);
    m_fadeAnim->setStartValue(1.0);
    m_fadeAnim->setEndValue(0.0);
    connect(m_fadeAnim, &QPropertyAnimation::finished,
            this, &CursorLabelWidget::onFadeFinished);
    m_fadeAnim->start(QAbstractAnimation::DeleteWhenStopped);
    m_fadeAnim = nullptr;
}

void CursorLabelWidget::onFadeFinished() {
    hide();
    m_opacity = 1.0;
}

} // namespace CollabText::Ui

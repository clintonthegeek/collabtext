#include "ui/ChatPanelWidget.h"

#include <QLabel>
#include <QListWidgetItem>
#include <QScrollBar>
#include <QVBoxLayout>

namespace CollabText::Ui {

ChatPanelWidget::ChatPanelWidget(QWidget *parent)
    : QWidget(parent)
    , m_list(new QListWidget(this))
    , m_input(new QLineEdit(this))
{
    m_list->setWordWrap(true);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setFocusPolicy(Qt::NoFocus);

    m_input->setPlaceholderText("Type a message...");

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_input);

    connect(m_input, &QLineEdit::returnPressed, this, [this]() {
        const QString text = m_input->text().trimmed();
        if (!text.isEmpty()) {
            emit messageSent(text);
            m_input->clear();
        }
    });
}

void ChatPanelWidget::addMessage(const QString &authorName, const QString &body,
                                  const QString &timestamp, const QColor &authorColor)
{
    QScrollBar *bar = m_list->verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum();

    auto *item = new QListWidgetItem(m_list);

    auto *label = new QLabel(this);
    label->setTextFormat(Qt::RichText);
    label->setText(
        QString("<b style=\"color:%1\">%2:</b> %3")
            .arg(authorColor.name(), authorName.toHtmlEscaped(), body.toHtmlEscaped()));
    label->setWordWrap(true);
    label->setToolTip(timestamp);

    item->setSizeHint(label->sizeHint());
    m_list->setItemWidget(item, label);

    if (atBottom) {
        m_list->scrollToBottom();
    }
}

} // namespace CollabText::Ui

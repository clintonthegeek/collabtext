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
                                  const QString &timestamp, const QColor &authorColor,
                                  int anchorLine)
{
    QScrollBar *bar = m_list->verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum();

    auto *item = new QListWidgetItem(m_list);

    QString html = QString("<b style=\"color:%1\">%2:</b> %3")
        .arg(authorColor.name(), authorName.toHtmlEscaped(), body.toHtmlEscaped());

    if (anchorLine >= 1) {
        html += QString(" <a href=\"line:%1\" style=\"color:#888; font-size:small\">(line %1)</a>")
            .arg(anchorLine);
    }

    auto *label = new QLabel(this);
    label->setTextFormat(Qt::RichText);
    label->setText(html);
    label->setWordWrap(true);
    label->setToolTip(timestamp);

    if (anchorLine >= 1) {
        connect(label, &QLabel::linkActivated, this, [this](const QString &link) {
            if (link.startsWith("line:")) {
                bool ok = false;
                int line = link.mid(5).toInt(&ok);
                if (ok) emit anchorClicked(line);
            }
        });
    }

    item->setSizeHint(label->sizeHint());
    m_list->setItemWidget(item, label);

    if (atBottom) {
        m_list->scrollToBottom();
    }
}

} // namespace CollabText::Ui
